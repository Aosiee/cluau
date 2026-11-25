// cluau's runner: script-mode translation, Luau's compiler and VM, `require`
// for .cluau / .luau files, the REPL and `repr`.

#include "cluau/runtime.h"
#include "Resolve.h"
#include "Std.h"

#include "Luau/Compiler.h"
#include "lua.h"
#include "lualib.h"

#ifdef CLUAU_HAS_ISOCLINE
#include "isocline.h"
#endif

#include <algorithm>
#include <csignal>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <io.h>
#define CLUAU_ISATTY(fd) _isatty(fd)
#else
#include <unistd.h>
#define CLUAU_ISATTY(fd) isatty(fd)
#endif

namespace fs = std::filesystem;

namespace cluau::runtime {

	namespace {

		constexpr const char* kRuntimeRegistry = "_cluau_runtime";
		constexpr const char* kModulesRegistry = "_cluau_modules";

		// The metatable every script class's table has (the script profile's
		// class line): calling a class constructs an instance, whose
		// metatable is the class table; a class's missing members are its
		// base's (`__base`, from `extends`).
		constexpr const char* kPrelude = R"LUAU(
local kClassKeys = { __index = true, __name = true, __base = true, Fields = true }

__cluau_meta = {
	__index = function(class, key)
		local base = rawget(class, "__base")
		if base then
			return base[key]
		end
		return nil
	end,
	__call = function(class, ...)
		if rawget(class, "__index") == nil then
			-- The first instance finishes the class: instances look their
			-- methods up in it, and Luau finds metamethods without __index,
			-- so the inherited ones are copied in.
			rawset(class, "__index", class)
			local base = rawget(class, "__base")
			while base do
				-- `next`: a class is callable, and Luau iterates a callable
				-- table by calling it.
				for key, value in next, base do
					if type(key) == "string" and string.sub(key, 1, 2) == "__" and not kClassKeys[key] and rawget(class, key) == nil then
						rawset(class, key, value)
					end
				end
				base = rawget(base, "__base")
			end
		end
		local object = setmetatable({}, class)
		-- The fields, a base's first (a subclass's default wins).
		local chain = {}
		local c = class
		while c do
			table.insert(chain, 1, c)
			c = rawget(c, "__base")
		end
		for _, each in chain do
			local fields = rawget(each, "Fields")
			if fields then
				for field, make in fields do
					object[field] = make()
				end
			end
		end
		local init = class.Init
		if init then
			init(object, ...)
		end
		return object
	end,
	__tostring = function(class)
		return "class " .. tostring(rawget(class, "__name"))
	end,
}
)LUAU";

		volatile std::sig_atomic_t g_Interrupt = 0;
		bool g_Interrupted = false; // the last run ended in an interrupt

		// Luau's interrupt callback: a pending Ctrl-C becomes an error in
		// the running script (once: the callback is off until the next run).
		void Interrupt(lua_State* L, int gc)
		{
			if (gc >= 0 || !g_Interrupt)
				return;
			g_Interrupt = 0;
			g_Interrupted = true;
			lua_callbacks(L)->interrupt = nullptr;
			lua_rawcheckstack(L, 1);
			luaL_error(L, "interrupted");
		}

		void OnSigint(int)
		{
			// A second Ctrl-C while the first is pending ends the process.
			if (g_Interrupt)
				std::_Exit(130);
			g_Interrupt = 1;
	#ifdef _WIN32
			std::signal(SIGINT, OnSigint);
	#endif
		}

		void CatchSigint()
		{
	#ifdef _WIN32
			std::signal(SIGINT, OnSigint);
	#else
			// Without SA_RESTART, so a script blocked reading standard input
			// gets its read back and stops at the first Ctrl-C.
			struct sigaction action = {};
			action.sa_handler = OnSigint;
			sigemptyset(&action.sa_mask);
			action.sa_flags = 0;
			sigaction(SIGINT, &action, nullptr);
	#endif
		}

		std::string ToUtf8(const fs::path& p) { return detail::PathText(p); }

		bool ReadFile(const fs::path& path, std::string& text)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in)
				return false;
			std::stringstream buffer;
			buffer << in.rdbuf();
			text = buffer.str();
			return true;
		}

		bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

		enum class LoadStatus { Ok, Incomplete, Error };

	} // namespace

	struct Runtime::Impl
	{
		Options Opts;
		lua_State* L = nullptr;
		fs::path StartDir; // the working directory at start (relative chunk names resolve from it)
		fs::path MainDir;  // the main script's directory (bare requires search it first)
		std::set<std::string> Loading; // modules whose chunk is running (a require cycle)
		std::map<std::string, std::string> Sources; // each chunk's source, by the name errors give it

		// Line `line` (1-based) of a chunk's source, its indentation removed
		// (how much in `indent`); "" when there is no such line.
		std::string SourceLine(const std::string& name, int line, size_t* indent = nullptr) const
		{
			auto it = Sources.find(name);
			if (it == Sources.end() || line < 1)
				return {};
			const std::string& text = it->second;
			size_t start = 0;
			for (int i = 1; i < line; i++)
			{
				start = text.find('\n', start);
				if (start == std::string::npos)
					return {};
				start++;
			}
			size_t end = text.find('\n', start);
			std::string out = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
			if (!out.empty() && out.back() == '\r')
				out.pop_back();
			const size_t first = out.find_first_not_of(" \t");
			if (indent)
				*indent = first == std::string::npos ? 0 : first;
			return first == std::string::npos ? std::string() : out.substr(first);
		}

		// The source line under an error (and a caret under its column, when
		// it has one): what the message is about.
		std::string Excerpt(const std::string& name, int line, int col = -1) const
		{
			size_t indent = 0;
			const std::string text = SourceLine(name, line, &indent);
			if (text.empty())
				return {};
			std::string out = "\n    " + text;
			if (col >= (int)indent)
			{
				std::string caret = "\n    ";
				for (size_t i = indent; i < (size_t)col && i - indent < text.size(); i++)
					caret += text[i - indent] == '\t' ? '\t' : ' ';
				out += caret + "^";
			}
			return out;
		}

		// Luau's stack (`file:line function name` per frame) with each frame's
		// source line under it.
		std::string Backtrace(const std::string& trace) const
		{
			std::string out;
			size_t at = 0;
			while (at < trace.size())
			{
				size_t end = trace.find('\n', at);
				if (end == std::string::npos)
					end = trace.size();
				const std::string frame = trace.substr(at, end - at);
				at = end + 1;
				if (frame.empty())
					continue;
				out += frame + "\n";
				const size_t fn = frame.find(" function ");
				const std::string where = frame.substr(0, fn);
				const size_t colon = where.rfind(':');
				if (colon == std::string::npos || colon + 1 >= where.size() ||
					where.find_first_not_of("0123456789", colon + 1) != std::string::npos)
					continue;
				const std::string excerpt = Excerpt(where.substr(0, colon), std::atoi(where.c_str() + colon + 1));
				if (!excerpt.empty())
					out += excerpt.substr(1) + "\n";
			}
			while (!out.empty() && out.back() == '\n')
				out.pop_back();
			return out;
		}

		static Impl& Get(lua_State* L)
		{
			lua_getfield(L, LUA_REGISTRYINDEX, kRuntimeRegistry);
			Impl* impl = static_cast<Impl*>(lua_tolightuserdata(L, -1));
			lua_pop(L, 1);
			return *impl;
		}

		// A path as errors show it: relative to the start directory when
		// it is under it.
		std::string Display(const fs::path& path) const
		{
			const fs::path rel = path.lexically_relative(StartDir);
			if (!rel.empty() && *rel.begin() != "..")
				return ToUtf8(rel);
			return ToUtf8(path);
		}

		// Translates (script mode) and compiles `source`; pushes the chunk
		// on L's stack on success. A .luau file (`luau`) is compiled as it is.
		LoadStatus Load(lua_State* L, const std::string& source, const std::string& name, bool repl, bool quiet, std::string& error,
			bool luau = false, const std::string* shown = nullptr)
		{
			// What errors quote: the source as written (the prompt's input
			// without the `return ` it tries first).
			Sources[name] = shown ? *shown : source;
			TranslateOptions options;
			options.Script = true;
			options.Repl = repl;
			options.Profile = &ScriptProfile();
			TranslateResult result;
			if (luau)
				result.Luau = source;
			else
				result = Translate(source, options);
			std::string warnings;
			bool incomplete = false;
			error.clear();
			for (const Diagnostic& d : result.Diagnostics)
			{
				std::string line = name + ":" + std::to_string(d.At.Begin.Line + 1) + ":" + std::to_string(d.At.Begin.Col + 1) + ": " +
					(d.Warning ? "warning: " : "") + d.Message + Excerpt(name, d.At.Begin.Line + 1, d.At.Begin.Col) + "\n";
				if (d.Warning)
					warnings += line;
				else
					error += line;
			}
			incomplete = result.Unfinished;
			if (!error.empty())
			{
				error.pop_back();
				return incomplete ? LoadStatus::Incomplete : LoadStatus::Error;
			}
			if (!quiet && !warnings.empty())
				std::fputs(warnings.c_str(), stderr);

			Luau::CompileOptions copts;
			copts.optimizationLevel = 1;
			copts.debugLevel = 1;
			const std::string bytecode = Luau::compile(result.Luau, copts);
			const std::string chunkname = "@" + name;
			if (luau_load(L, chunkname.c_str(), bytecode.data(), bytecode.size(), 0) != 0)
			{
				size_t len = 0;
				const char* msg = lua_tolstring(L, -1, &len);
				error.assign(msg, len);
				lua_pop(L, 1);
				// `name:line: message`: the line under it.
				const std::string prefix = name + ":";
				if (error.compare(0, prefix.size(), prefix) == 0)
					error += Excerpt(name, std::atoi(error.c_str() + prefix.size()));
				return error.find("<eof>") != std::string::npos ? LoadStatus::Incomplete : LoadStatus::Error;
			}
			return LoadStatus::Ok;
		}

		// Runs the chunk on top of L's stack in a thread (so an error keeps
		// its stack for the backtrace). On success the thread is left on
		// L's stack with the results on its own; returns it, or nullptr
		// after reporting the error.
		lua_State* Execute()
		{
			lua_State* T = lua_newthread(L);
			lua_pushvalue(L, -2);
			lua_remove(L, -3);
			lua_xmove(L, T, 1);
			g_Interrupt = 0;
			g_Interrupted = false;
			lua_callbacks(L)->interrupt = Interrupt;
			const int status = lua_resume(T, nullptr, 0);
			if (status == LUA_OK)
				return T;
			std::string message;
			if (status == LUA_YIELD)
				message = "the script yielded outside a coroutine";
			else if (lua_isstring(T, -1))
				message = lua_tostring(T, -1);
			else
				message = std::string("(an error value of type ") + luaL_typename(T, -1) + ")";
			std::fflush(stdout);
			std::string trace = lua_debugtrace(T);
			while (!trace.empty() && trace.back() == '\n')
				trace.pop_back();
			if (trace.empty())
				std::fprintf(stderr, "%s\n", message.c_str());
			else
				std::fprintf(stderr, "%s\nstack backtrace:\n%s\n", message.c_str(), Backtrace(trace).c_str());
			lua_pop(L, 1); // the thread
			return nullptr;
		}

		int RunChunk(const std::string& source, const std::string& name, bool luau = false)
		{
			std::string error;
			if (Load(L, source, name, false, false, error, luau) != LoadStatus::Ok)
			{
				std::fprintf(stderr, "%s\n", error.c_str());
				return 1;
			}
			lua_State* T = Execute();
			std::fflush(stdout);
			if (!T)
				return g_Interrupted ? 130 : 1;
			lua_pop(L, 1);
			return 0;
		}
	};

	namespace {


		int Require(lua_State* L)
		{
			Runtime::Impl& rt = Runtime::Impl::Get(L);
			const std::string name = luaL_checkstring(L, 1);
			if (StartsWith(name, "@std/"))
			{
				lua_getfield(L, LUA_REGISTRYINDEX, detail::kStdRegistry);
				lua_getfield(L, -1, name.c_str() + 5);
				if (!lua_isnil(L, -1))
					return 1;
				lua_pop(L, 1);
				// A module written in cluau: loaded once, then kept with the others.
				auto source = detail::StdSources().find(name.substr(5));
				if (source == detail::StdSources().end())
					luaL_error(L, "no standard module '%s' (the standard modules: %s)", name.c_str(), detail::StdNames());
				std::string error;
				if (rt.Load(L, source->second, name, false, true, error) != LoadStatus::Ok)
				{
					lua_pushlstring(L, error.data(), error.size());
					lua_error(L);
				}
				lua_call(L, 0, 1);
				lua_pushvalue(L, -1);
				lua_setfield(L, -3, name.c_str() + 5);
				return 1;
			}
			if (StartsWith(name, "@"))
				luaL_error(L, "unknown alias in '%s' (the standard modules are @std/<name>)", name.c_str());

			// The requiring chunk's directory (past C functions such as
			// pcall: `pcall(require, "./x")` is the caller's).
			fs::path from = rt.StartDir;
			lua_Debug ar;
			for (int level = 1; lua_getinfo(L, level, "s", &ar); level++)
			{
				if (!ar.source || std::strcmp(ar.source, "=[C]") == 0)
					continue;
				if (ar.source[0] == '@')
					from = (rt.StartDir / fs::u8path(ar.source + 1)).parent_path();
				break;
			}

			std::vector<fs::path> searchDirs;
			for (const std::string& dir : rt.Opts.SearchPath)
				searchDirs.push_back(rt.StartDir / fs::u8path(dir));
			std::vector<fs::path> looked;
			const fs::path found = detail::ResolveRequire(name, from, rt.MainDir, searchDirs, &looked);
			if (found.empty())
			{
				std::string tried;
				for (const fs::path& base : looked)
					tried += (tried.empty() ? "" : ", ") + rt.Display(base) + "[.cluau|.luau]";
				luaL_error(L, "module '%s' not found (looked for %s)", name.c_str(), tried.c_str());
			}
			const std::string key = detail::ModuleKey(found);

			lua_getfield(L, LUA_REGISTRYINDEX, kModulesRegistry);
			lua_getfield(L, -1, key.c_str());
			if (!lua_isnil(L, -1))
				return 1;
			lua_pop(L, 2);
			if (rt.Loading.count(key))
				luaL_error(L, "require cycle: '%s' is still loading (it requires itself through the modules it requires)", rt.Display(found).c_str());

			std::string text;
			if (!ReadFile(found, text))
				luaL_error(L, "cannot read module '%s'", rt.Display(found).c_str());
			std::string error;
			if (rt.Load(L, text, rt.Display(found), false, false, error, detail::IsLuauFile(found)) != LoadStatus::Ok)
			{
				lua_pushlstring(L, error.data(), error.size());
				lua_error(L);
			}
			struct Guard
			{
				std::set<std::string>& Set;
				std::string Key;
				~Guard() { Set.erase(Key); }
			} guard{ rt.Loading, key };
			rt.Loading.insert(key);
			lua_call(L, 0, 1);
			if (lua_isnil(L, -1))
			{
				lua_pop(L, 1);
				lua_pushboolean(L, 1);
			}
			lua_getfield(L, LUA_REGISTRYINDEX, kModulesRegistry);
			lua_pushvalue(L, -2);
			lua_setfield(L, -2, key.c_str());
			lua_pop(L, 1);
			return 1;
		}

		// ---- repr ---------------------------------------------------------

		bool IsIdentifier(const char* s, size_t len)
		{
			if (len == 0 || !(std::isalpha((unsigned char)s[0]) || s[0] == '_'))
				return false;
			for (size_t i = 1; i < len; i++)
				if (!(std::isalnum((unsigned char)s[i]) || s[i] == '_'))
					return false;
			static const char* kKeywords[] = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in", "local", "nil",
				"not", "or", "repeat", "return", "then", "true", "until", "while" };
			for (const char* k : kKeywords)
				if (std::strlen(k) == len && std::memcmp(k, s, len) == 0)
					return false;
			return true;
		}

		void Quote(const char* s, size_t len, std::string& out)
		{
			out += '"';
			for (size_t i = 0; i < len; i++)
			{
				const unsigned char c = (unsigned char)s[i];
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20 || c == 0x7f)
					{
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\%d", c);
						out += buf;
					}
					else
						out += (char)c;
				}
			}
			out += '"';
		}

		void ReprAt(lua_State* L, int index, int depth, std::vector<const void*>& seen, std::string& out)
		{
			index = lua_absindex(L, index);
			lua_checkstack(L, 8);
			switch (lua_type(L, index))
			{
			case LUA_TSTRING:
			{
				size_t len = 0;
				const char* s = lua_tolstring(L, index, &len);
				Quote(s, len, out);
				return;
			}
			case LUA_TTABLE:
				break;
			default:
			{
				size_t len = 0;
				const char* s = luaL_tolstring(L, index, &len);
				out.append(s, len);
				lua_pop(L, 1);
				return;
			}
			}
			// A table: its own __tostring, or its contents.
			std::string prefix;
			if (lua_getmetatable(L, index))
			{
				lua_rawgetfield(L, -1, "__tostring");
				const bool custom = !lua_isnil(L, -1);
				lua_pop(L, 1);
				if (custom)
				{
					lua_pop(L, 1);
					size_t len = 0;
					const char* s = luaL_tolstring(L, index, &len);
					out.append(s, len);
					lua_pop(L, 1);
					return;
				}
				lua_rawgetfield(L, -1, "__name");
				if (lua_isstring(L, -1))
					prefix = std::string(lua_tostring(L, -1)) + " ";
				lua_pop(L, 2);
			}
			const void* self = lua_topointer(L, index);
			if (std::find(seen.begin(), seen.end(), self) != seen.end())
			{
				out += prefix + "{<cycle>}";
				return;
			}
			if (depth >= 6)
			{
				out += prefix + "{...}";
				return;
			}
			seen.push_back(self);
			std::vector<std::string> items;
			const int n = lua_objlen(L, index);
			for (int i = 1; i <= n; i++)
			{
				lua_rawgeti(L, index, i);
				std::string item;
				ReprAt(L, -1, depth + 1, seen, item);
				lua_pop(L, 1);
				items.push_back(std::move(item));
			}
			std::vector<std::pair<std::string, std::string>> fields;
			lua_pushnil(L);
			while (lua_next(L, index))
			{
				if (lua_type(L, -2) == LUA_TNUMBER)
				{
					const double k = lua_tonumber(L, -2);
					if (k >= 1 && k <= n && k == (double)(int)k)
					{
						lua_pop(L, 1);
						continue;
					}
				}
				std::string key;
				if (lua_type(L, -2) == LUA_TSTRING)
				{
					size_t len = 0;
					const char* s = lua_tolstring(L, -2, &len);
					if (IsIdentifier(s, len))
						key.assign(s, len);
				}
				if (key.empty())
				{
					key = "[";
					ReprAt(L, -2, depth + 1, seen, key);
					key += "]";
				}
				std::string value;
				ReprAt(L, -1, depth + 1, seen, value);
				fields.emplace_back(std::move(key), std::move(value));
				lua_pop(L, 1);
			}
			seen.pop_back();
			std::sort(fields.begin(), fields.end());
			for (auto& [key, value] : fields)
				items.push_back(key + " = " + value);
			out += prefix + "{";
			for (size_t i = 0; i < items.size(); i++)
				out += (i ? ", " : "") + items[i];
			out += "}";
		}

		int ReprGlobal(lua_State* L)
		{
			luaL_checkany(L, 1);
			const std::string text = Repr(L, 1);
			lua_pushlstring(L, text.data(), text.size());
			return 1;
		}

		// ---- the REPL's line reader -----------------------------------------

		enum class Read { Line, End, Cancelled };

	#ifdef CLUAU_HAS_ISOCLINE
		// isocline gives no line for Ctrl-D on an empty line and for Ctrl-C
		// alike; its highlighter sees every edit, so a line that had text when
		// the reader gave up was Ctrl-C.
		size_t g_EditLength = 0;

		void TrackEdits(ic_highlight_env_t*, const char* input, void*) { g_EditLength = std::strlen(input); }

		bool IsCompletionChar(const char* s, long len)
		{
			const char c = *s;
			return len == 1 && (std::isalnum((unsigned char)c) || c == '.' || c == ':' || c == '_');
		}

		// The members a completion looks through for `table`: its own keys, then
		// an `__index` table's, then a script class's base's.
		void CompleteMembers(lua_State* L, const std::string& prefix, bool methodsOnly, std::set<std::string>& seen, ic_completion_env_t* cenv, const std::string& before)
		{
			for (int depth = 0; depth < 8 && lua_istable(L, -1); depth++)
			{
				lua_pushnil(L);
				while (lua_next(L, -2))
				{
					if (lua_type(L, -2) == LUA_TSTRING)
					{
						const std::string key = lua_tostring(L, -2);
						const bool isFunction = lua_isfunction(L, -1);
						if (key.compare(0, prefix.size(), prefix) == 0 && key.rfind("__", 0) != 0 && (!methodsOnly || isFunction) &&
							seen.insert(key).second)
						{
							const std::string completion = before + key + (isFunction ? "(" : "");
							ic_add_completion_ex(cenv, completion.c_str(), key.c_str(), nullptr);
						}
					}
					lua_pop(L, 1);
				}
				// Where missing keys come from.
				bool next = false;
				if (lua_getmetatable(L, -1))
				{
					lua_rawgetfield(L, -1, "__index");
					if (lua_istable(L, -1) && !lua_rawequal(L, -1, -3))
					{
						lua_remove(L, -2); // the metatable
						lua_remove(L, -2); // the table
						next = true;
					}
					else
						lua_pop(L, 2);
				}
				if (!next)
				{
					lua_rawgetfield(L, -1, "__base");
					if (!lua_istable(L, -1))
					{
						lua_pop(L, 1);
						break;
					}
					lua_remove(L, -2);
				}
			}
		}

		constexpr const char* kKeywords[] = { "let", "const", "class", "extends", "super", "function", "return", "if", "else", "while", "for",
			"do", "break", "continue", "static", "signal", "true", "false", "nil", "type", "export", "typeof" };

		void Completions(ic_completion_env_t* cenv, const char* word)
		{
			lua_State* L = static_cast<lua_State*>(ic_completion_arg(cenv));
			const std::string text = word;
			lua_checkstack(L, 16);
			const int top = lua_gettop(L);
			lua_pushvalue(L, LUA_GLOBALSINDEX);
			size_t start = 0;
			bool methodsOnly = false;
			for (;;)
			{
				const size_t sep = text.find_first_of(".:", start);
				if (sep == std::string::npos)
					break;
				// The next table along the path, without running any code.
				const std::string key = text.substr(start, sep - start);
				lua_pushlstring(L, key.data(), key.size());
				lua_rawget(L, -2);
				if (!lua_istable(L, -1))
				{
					// An instance's key may be its class's (through __index).
					lua_pop(L, 1);
					if (!lua_getmetatable(L, -1))
					{
						lua_settop(L, top);
						return;
					}
					lua_rawgetfield(L, -1, "__index");
					lua_pushlstring(L, key.data(), key.size());
					lua_rawget(L, -2);
					if (!lua_istable(L, -1))
					{
						lua_settop(L, top);
						return;
					}
					lua_remove(L, -2);
					lua_remove(L, -2);
				}
				lua_remove(L, -2);
				methodsOnly = text[sep] == ':';
				start = sep + 1;
			}
			const std::string prefix = text.substr(start);
			std::set<std::string> seen;
			CompleteMembers(L, prefix, methodsOnly, seen, cenv, text.substr(0, start));
			if (start == 0)
				for (const char* keyword : kKeywords)
					if (std::string(keyword).compare(0, prefix.size(), prefix) == 0 && seen.insert(keyword).second)
						ic_add_completion(cenv, keyword);
			lua_settop(L, top);
		}

		void Complete(ic_completion_env_t* cenv, const char* input) { ic_complete_word(cenv, input, Completions, IsCompletionChar); }
	#endif

		Read ReadLine(bool interactive, const char* prompt, std::string& line)
		{
	#ifdef CLUAU_HAS_ISOCLINE
			if (interactive)
			{
				g_EditLength = 0;
				char* text = ic_readline(prompt);
				if (!text)
					return g_EditLength > 0 ? Read::Cancelled : Read::End;
				line = text;
				free(text);
				if (!line.empty())
					ic_history_add(line.c_str());
				return Read::Line;
			}
	#else
			if (interactive)
			{
				std::fputs(prompt, stdout);
				std::fflush(stdout);
			}
#endif
			if (!std::getline(std::cin, line))
				return Read::End;
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			return Read::Line;
		}

	} // namespace

	void detail::RaiseIfInterrupted(lua_State* L)
	{
		if (!g_Interrupt)
			return;
		g_Interrupt = 0;
		g_Interrupted = true;
		std::cin.clear();
		luaL_error(L, "interrupted");
	}

	std::string Repr(lua_State* L, int index)
	{
		std::vector<const void*> seen;
		std::string out;
		ReprAt(L, index, 0, seen, out);
		return out;
	}

	Runtime::Runtime(Options options)
		: m_Impl(std::make_unique<Impl>())
	{
		Impl& rt = *m_Impl;
		rt.Opts = std::move(options);
		std::error_code ec;
		rt.StartDir = fs::current_path(ec);
		rt.MainDir = rt.StartDir;
		lua_State* L = rt.L = luaL_newstate();
		luaL_openlibs(L);

		lua_pushlightuserdata(L, &rt);
		lua_setfield(L, LUA_REGISTRYINDEX, kRuntimeRegistry);
		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, kModulesRegistry);

		lua_pushcfunction(L, Require, "require");
		lua_setglobal(L, "require");
		lua_pushcfunction(L, ReprGlobal, "repr");
		lua_setglobal(L, "repr");
		detail::OpenStd(L, rt.Opts.Args);

		Luau::CompileOptions copts;
		copts.debugLevel = 1;
		const std::string bytecode = Luau::compile(kPrelude, copts);
		if (luau_load(L, "=prelude", bytecode.data(), bytecode.size(), 0) != 0 || lua_pcall(L, 0, 0, 0) != 0)
		{
			std::fprintf(stderr, "cluau: the runtime's prelude failed: %s\n", lua_tostring(L, -1));
			lua_pop(L, 1);
		}

		if (rt.Opts.CatchInterrupt)
			CatchSigint();
	}

	Runtime::~Runtime()
	{
		if (m_Impl->L)
			lua_close(m_Impl->L);
	}

	lua_State* Runtime::State() const { return m_Impl->L; }

	int Runtime::RunFile(const std::string& path)
	{
		Impl& rt = *m_Impl;
		const fs::path file = fs::u8path(path);
		std::string text;
		std::error_code ec;
		if (fs::is_directory(file, ec) || !ReadFile(file, text))
		{
			std::fprintf(stderr, "cluau: cannot open '%s'\n", path.c_str());
			return 2;
		}
		rt.MainDir = (rt.StartDir / file).lexically_normal().parent_path();
		detail::SetScript(rt.L, path);
		return rt.RunChunk(text, path, detail::IsLuauFile(file));
	}

	int Runtime::RunSource(const std::string& source, const std::string& name)
	{
		Impl& rt = *m_Impl;
		rt.MainDir = rt.StartDir;
		detail::SetScript(rt.L, name);
		return rt.RunChunk(source, name);
	}

	int Runtime::Repl()
	{
		Impl& rt = *m_Impl;
		lua_State* L = rt.L;
		const bool interactive = CLUAU_ISATTY(0) && CLUAU_ISATTY(1);
		rt.MainDir = rt.StartDir;
		detail::SetScript(L, "");
#ifdef CLUAU_HAS_ISOCLINE
		if (interactive)
		{
			ic_set_prompt_marker("", "");
			ic_enable_brace_insertion(false);
			ic_set_default_highlighter(TrackEdits, nullptr);
			ic_set_default_completer(Complete, L);
			if (const char* home = std::getenv(
#ifdef _WIN32
					"USERPROFILE"
#else
					"HOME"
#endif
					))
				ic_set_history(ToUtf8(fs::u8path(home) / ".cluau_history").c_str(), -1);
		}
#endif
		if (interactive)
			std::printf("cluau %s (C-syntax-flavoured Luau). Tab completes; Ctrl-C abandons an input; Ctrl-D exits.\n", Version());

		std::string buffer;
		std::string line;
		for (;;)
		{
			const Read read = ReadLine(interactive, buffer.empty() ? "> " : ">> ", line);
			if (read == Read::Cancelled || (read == Read::End && !buffer.empty() && interactive))
			{
				// Ctrl-C (or Ctrl-D inside a statement): the input is abandoned.
				buffer.clear();
				continue;
			}
			if (read == Read::End)
				break;
			if (buffer.empty() && line.find_first_not_of(" \t") == std::string::npos)
				continue;
			const bool force = !buffer.empty() && line.empty(); // an empty line ends an input
			buffer += buffer.empty() ? line : "\n" + line;

			std::string error;
			// An expression prints its value; anything else runs as statements.
			bool expression = rt.Load(L, "return " + buffer, "stdin", true, true, error, false, &buffer) == LoadStatus::Ok;
			if (!expression)
			{
				const LoadStatus status = rt.Load(L, buffer, "stdin", true, false, error);
				if (status == LoadStatus::Incomplete && !force)
					continue;
				if (status != LoadStatus::Ok)
				{
					std::fprintf(stderr, "%s\n", error.c_str());
					buffer.clear();
					continue;
				}
			}
			buffer.clear();
			lua_State* T = rt.Execute();
			if (!T)
				continue;
			const int n = lua_gettop(T);
			bool any = false;
			for (int i = 1; i <= n; i++)
				any = any || !lua_isnil(T, i);
			if (expression && any)
			{
				std::string out;
				for (int i = 1; i <= n; i++)
					out += (i > 1 ? "\t" : "") + Repr(T, i);
				std::printf("%s\n", out.c_str());
				lua_pushvalue(T, 1);
				lua_setglobal(T, "_");
			}
			std::fflush(stdout);
			lua_pop(L, 1); // the thread
		}
		if (!buffer.empty())
		{
			// The input ended inside a statement.
			std::string error;
			if (rt.Load(L, buffer, "stdin", true, false, error) != LoadStatus::Ok)
			{
				std::fprintf(stderr, "%s\n", error.c_str());
				return 1;
			}
			lua_pop(L, 1);
		}
		if (interactive)
			std::printf("\n");
		return 0;
	}

} // namespace cluau::runtime
