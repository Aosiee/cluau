// cluau's runner: script-mode translation, Luau's compiler and VM, `require`
// for .cluau / .luau files, the REPL and `repr`.

#include "cluau/runtime.h"
#include "Debugger.h"
#include "Profile.h"
#include "Resolve.h"
#include "Spawn.h"
#include "Std.h"
#include "Worker.h"

#include "Luau/CodeGen.h"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include "lua.h"
#include "lualib.h"

#ifdef CLUAU_HAS_ISOCLINE
#include "isocline.h"
#endif

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
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

LUAU_DYNAMIC_FASTFLAG(LuauGcHeapShrinkFix)

namespace cluau::runtime {

	namespace {

		constexpr const char* kRuntimeRegistry = "_cluau_runtime";
		constexpr const char* kModulesRegistry = "_cluau_modules";

		// The metatable every script class's table has (the script profile's
		// class line): calling a class constructs an instance, whose
		// metatable is the class table; a class's missing members are its
		// base's (`__base`, from `extends`).
		constexpr const char* kPrelude = R"LUAU(
local kClassKeys = { __index = true, __name = true, __base = true, __abstract = true, __abstracts = true, Fields = true }

-- The body an abstract method has until a subclass gives it one.
function __cluau_abstract(class, method)
	error(`{class}:{method} is abstract: a subclass of {class} defines it`, 3)
end

-- `throw value` in a script: raises the value as it is (a `catch (error)`
-- may hide Luau's `error` where the throw is).
function __cluau_throw(value)
	error(value, 0)
end

__cluau_meta = {
	__index = function(class, key)
		local base = rawget(class, "__base")
		if base then
			return base[key]
		end
		return nil
	end,
	__call = function(class, ...)
		if rawget(class, "__abstract") then
			error(`{rawget(class, "__name")} is abstract: construct one of its subclasses`, 2)
		end
		if rawget(class, "__index") == nil then
			-- Every abstract method of the bases must be defined by now.
			local missing = {}
			local c = class
			while c do
				for _, name in rawget(c, "__abstracts") or {} do
					if class[name] == rawget(c, name) and c ~= class then
						table.insert(missing, `{name} (abstract in {rawget(c, "__name")})`)
					end
				end
				c = rawget(c, "__base")
			end
			if #missing > 0 then
				table.sort(missing)
				error(`{rawget(class, "__name")} does not define {table.concat(missing, ", ")}`, 2)
			end
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
	-- `for (k, v in Class)`: the class's own members (its statics and
	-- methods), without the runtime's bookkeeping. Without this Luau
	-- would iterate a callable table by calling it, constructing
	-- instances without end.
	__iter = function(class)
		local function step(t, key)
			local value
			repeat
				key, value = next(t, key)
			until key == nil or not kClassKeys[key]
			return key, value
		end
		return step, class
	end,
}
)LUAU";

		// A Ctrl-C waiting for the running script's next safepoint. The
		// signal is the process's, so this is too: whichever runtime that
		// catches interrupts reaches a safepoint first stops.
		std::atomic<int> g_Interrupt{ 0 };
		// Counts every Ctrl-C and is never cleared, so a job's thread sees one
		// even after the script's thread has raised it.
		std::atomic<unsigned> g_Interrupts{ 0 };
		static_assert(std::atomic<int>::is_always_lock_free && std::atomic<unsigned>::is_always_lock_free);

		// The message of the error a Ctrl-C raises: a run that ends in it
		// (however it was caught and raised again) exits with status 130.
		constexpr const char* kInterrupted = "interrupted";

		void Interrupt(lua_State* L, int gc);
		void InterruptQuick(lua_State* L, int gc);
		void StopIfCatching(lua_State* L);

		// The error is the bare message (no position), so the run's end can
		// tell it, wherever it was raised.
		[[noreturn]] void RaiseInterrupted(lua_State* L)
		{
			lua_rawcheckstack(L, 1);
			lua_pushstring(L, kInterrupted);
			lua_error(L);
		}

		void Interrupted()
		{
			// A second Ctrl-C while the first is pending ends the process.
			if (g_Interrupt)
				std::_Exit(130);
			g_Interrupt = 1;
			g_Interrupts++;
		}

	#ifdef _WIN32
		void OnSigint(int)
		{
			Interrupted();
			std::signal(SIGINT, OnSigint);
		}
	#else
		// With SA_SIGINFO's three arguments: the prompt's line editor calls
		// the handler it found before its own that way.
		void OnSigint(int, siginfo_t*, void*) { Interrupted(); }
	#endif

		void CatchSigint()
		{
	#ifdef _WIN32
			std::signal(SIGINT, OnSigint);
	#else
			// A SIGINT ignored by whoever started the script (a background
			// job of a shell) stays ignored.
			struct sigaction current = {};
			if (sigaction(SIGINT, nullptr, &current) == 0 && current.sa_handler == SIG_IGN)
				return;
			// Without SA_RESTART, so a script blocked reading standard input
			// gets its read back and stops at the first Ctrl-C.
			struct sigaction action = {};
			action.sa_sigaction = OnSigint;
			sigemptyset(&action.sa_mask);
			action.sa_flags = SA_SIGINFO;
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
			// A UTF-8 byte order mark (some Windows editors save one) is not code.
			if (text.rfind("\xEF\xBB\xBF", 0) == 0)
				text.erase(0, 3);
			return true;
		}

		bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

		enum class LoadStatus { Ok, Incomplete, Error };

		// The runtime running a script on this thread (the innermost, when
		// one runs another), which Luau's interrupt callback serves.
		thread_local Runtime::Impl* t_Running = nullptr;
		thread_local lua_State* t_RunningThread = nullptr; // the thread its innermost Execute resumed

	} // namespace

	struct Runtime::Impl
	{
		Options Opts;
		lua_State* L = nullptr;
		fs::path StartDir; // the working directory at start (relative chunk names resolve from it)
		fs::path MainDir;  // the main script's directory (bare requires search it first)
		std::set<std::string> Loading; // modules whose chunk is running (a require cycle)
		std::map<std::string, std::string> Sources; // each chunk's source, by the name errors give it
		// A file's chunk name -> the directory its relative requires start
		// from: the real file's (through any symbolic link), as the
		// checker's.
		std::map<std::string, fs::path> ChunkDirs;
		bool Native = false; // chunks compile to native code (where Luau's code generator supports the machine)
		// The host's modules (AddModule), by the name `require` gives them.
		struct HostModule
		{
			lua_CFunction Open = nullptr;
			std::string Source; // written in cluau, when Open is null
		};
		std::map<std::string, HostModule> HostModules;
		// A debugger (`cluau debug`): told of each chunk loaded and of an
		// error that ends the script; null otherwise.
		detail::DebugHooks* Debug = nullptr;
		// The chunks loaded from files, kept for their coverage (Options::Coverage).
		std::vector<std::pair<std::string, int>> CoverageChunks;
		std::unique_ptr<detail::Profiler> Profile;
		bool Reported = false; // the profile's report is written
		// The interrupt callback the host had set on the state before a
		// run: the runtime's own calls it first.
		void (*HostInterrupt)(lua_State* L, int gc) = nullptr;
		std::vector<lua_State*> Threads; // the threads Execute is running (innermost last)
		bool Interrupted = false;        // the last run ended in a Ctrl-C's `interrupted`
		std::string Script;              // process.script
		// A worker's runtime (@std/worker): the error that ends its script
		// goes here instead of to stderr (the message, the stack's frames,
		// whether they are a task's), and what the script returned is given
		// to OnResults (which returns "" or why it cannot be kept).
		std::function<void(const std::string& message, const std::string& trace, bool task)> OnError;
		std::function<std::string(lua_State* T)> OnResults;
		// While a run is in progress, another (a host's callback running a
		// file) keeps the main directory and the script's name for it.
		struct NestedRun
		{
			Impl& Rt;
			bool Nested;
			fs::path MainDir;
			std::string Script;
			explicit NestedRun(Impl& rt)
				: Rt(rt)
				, Nested(!rt.Threads.empty())
				, MainDir(rt.MainDir)
				, Script(rt.Script)
			{
			}
			~NestedRun()
			{
				if (!Nested)
					return;
				Rt.MainDir = MainDir;
				Rt.SetScript(Script);
			}
		};

		void SetScript(const std::string& script)
		{
			Script = script;
			detail::SetScript(L, script);
		}

		// A sample for the profiler, when one is due.
		void Sample(lua_State* T)
		{
			if (Profile && Profile->Due())
				Profile->Sample(T, std::find(Threads.begin(), Threads.end(), T) != Threads.end());
		}

		void ReportProfile()
		{
			if (!Profile || Reported)
				return;
			Reported = true;
			Profile->Stop();
			std::fflush(stdout);
			std::fputs(("\n" + Profile->Report((size_t)std::max(1, Opts.ProfileTop))).c_str(), stderr);
			if (!Opts.ProfileFolded.empty())
			{
				std::ofstream out(fs::u8path(Opts.ProfileFolded), std::ios::binary);
				out << Profile->Folded();
				if (!out)
					std::fprintf(stderr, "cluau: cannot write '%s'\n", Opts.ProfileFolded.c_str());
			}
		}

		// The sandbox's files: a module required in it must be under the
		// main script's directory, a search directory, or a cluau_modules
		// directory up from the main script's.
		bool SandboxAllows(const fs::path& file, const std::vector<fs::path>& searchDirs) const
		{
			auto under = [](const fs::path& path, const fs::path& root) {
				std::error_code ec;
				const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
				if (ec)
					return false;
				const fs::path rel = path.lexically_relative(canonicalRoot);
				return !rel.empty() && *rel.begin() != ".." && !rel.is_absolute();
			};
			std::error_code ec;
			const fs::path path = fs::weakly_canonical(file, ec);
			if (ec)
				return false;
			if (under(path, MainDir))
				return true;
			for (const fs::path& dir : searchDirs)
				if (under(path, dir))
					return true;
			for (fs::path dir = MainDir; !dir.empty(); dir = dir.parent_path())
			{
				if (under(path, dir / "cluau_modules"))
					return true;
				if (dir == dir.parent_path())
					break;
			}
			return false;
		}

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
				// The runtime's own code (a class's constructor) is not the script's.
				if (frame.empty() || StartsWith(frame, "prelude:"))
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
			// Luau's lexer reads a NUL byte as the end of the text: the
			// code after it would silently not run.
			if (const size_t nul = source.find('\0'); nul != std::string::npos)
			{
				const long line = 1 + std::count(source.begin(), source.begin() + (std::ptrdiff_t)nul, '\n');
				error = name + ":" + std::to_string(line) + ": the source contains a NUL byte";
				return LoadStatus::Error;
			}
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
			// Level 1: level 2's inlining measured no faster on scripts, and
			// it takes inlined calls' frames out of tracebacks. A debugger
			// gets unoptimised code (each line's own instructions) and the
			// locals' names.
			copts.optimizationLevel = Debug ? 0 : 1;
			copts.debugLevel = Debug ? 2 : 1;
			copts.coverageLevel = Opts.Coverage ? 1 : 0;
			const std::string bytecode = Luau::compile(result.Luau, copts);
			const std::string chunkname = "@" + name;
			const bool loadFailed = luau_load(L, chunkname.c_str(), bytecode.data(), bytecode.size(), 0) != 0;
			if (!loadFailed && Debug)
				Debug->OnLoaded(L, name, FileOf(name));
			if (!loadFailed && Opts.Coverage)
				if (const std::string file = FileOf(name); !file.empty())
				{
					lua_pushvalue(L, -1);
					CoverageChunks.push_back({ file, lua_ref(L, -1) });
					lua_pop(L, 1);
				}
			// Native code costs about 5 microseconds a line to generate: the
			// bundled modules that only parse arguments, run tests or manage
			// packages stay interpreted; @std/csv's parsing and @std/collections'
			// sorting gain from it.
			const bool tooling = StartsWith(name, "@std/") && name != "@std/csv" && name != "@std/collections";
			if (!loadFailed && Native && !tooling)
				Luau::CodeGen::compile(L, -1, 0);
			if (loadFailed)
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

		// The @std/task task whose error is the one on T's stack, if it is
		// one: its own stack shows where the error was raised.
		lua_State* FailedTask(lua_State* T)
		{
			lua_checkstack(L, 4);
			const int top = lua_gettop(L);
			lua_State* task = nullptr;
			lua_getfield(L, LUA_REGISTRYINDEX, detail::kStdRegistry);
			lua_getfield(L, -1, "task");
			if (lua_istable(L, -1))
			{
				lua_getfield(L, -1, "_failure");
				if (lua_istable(L, -1))
				{
					lua_getfield(L, -1, "message");
					lua_pushvalue(T, -1);
					lua_xmove(T, L, 1);
					if (!lua_isnil(L, -1) && lua_rawequal(L, -1, -2))
					{
						lua_getfield(L, -3, "thread");
						task = lua_tothread(L, -1);
					}
				}
			}
			lua_settop(L, top);
			return task;
		}

		// The file a chunk was loaded from, as an absolute path ("" for one
		// with no file: a standard or host module, `-e`, the prompt).
		std::string FileOf(const std::string& name) const
		{
			if (name.empty() || name[0] == '@' || name[0] == '=' || name == "-e" || name == "stdin" || StartsWith(name, "stdin["))
				return {};
			std::error_code ec;
			const fs::path path = (StartDir / fs::u8path(name)).lexically_normal();
			return fs::is_regular_file(path, ec) ? ToUtf8(path) : std::string();
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
			// A Ctrl-C from before the run is not for it.
			if (Opts.CatchInterrupt && Threads.empty())
				g_Interrupt = 0;
			Interrupted = false;
			// A debugger keeps the interrupt callback, and a thread it breaks
			// out of the VM (to step) is resumed where it stopped.
			if (!Debug)
			{
				lua_Callbacks* callbacks = lua_callbacks(L);
				if (callbacks->interrupt != Interrupt && callbacks->interrupt != InterruptQuick)
					HostInterrupt = callbacks->interrupt;
				// With no profile and no callback of the host's, a run
				// only looks for a Ctrl-C (at every call and loop: cheaply).
				callbacks->interrupt = Profile || HostInterrupt ? Interrupt : InterruptQuick;
			}
			struct Running
			{
				Impl& Rt;
				Impl* Outer;
				lua_State* OuterThread;
				~Running()
				{
					Rt.Threads.pop_back();
					t_Running = Outer;
					t_RunningThread = OuterThread;
				}
			} running{ *this, t_Running, t_RunningThread };
			Threads.push_back(T);
			t_Running = this;
			t_RunningThread = T;
			int status = lua_resume(T, nullptr, 0);
			while (Debug && status == LUA_BREAK)
				status = Debug->ResumeBroken(T);
			if (status == LUA_OK)
				return T;
			std::string message;
			if (status == LUA_YIELD)
				message = "the script yielded outside a coroutine";
			else if (lua_isstring(T, -1))
				message = lua_tostring(T, -1);
			else
			{
				message = std::string("(an error value of type ") + luaL_typename(T, -1) + ")";
				// An error object that says how it is shown (its __tostring
				// run protected, on the main thread: this one has failed).
				if (luaL_getmetafield(T, -1, "__tostring"))
				{
					lua_pop(T, 1);
					lua_pushcfunction(
						L,
						[](lua_State* S) {
							luaL_tolstring(S, 1, nullptr);
							return 1;
						},
						"tostring");
					lua_pushvalue(T, -1);
					lua_xmove(T, L, 1);
					if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_isstring(L, -1))
						message = lua_tostring(L, -1);
					lua_pop(L, 1);
				}
			}
			Interrupted = status != LUA_YIELD && message == kInterrupted;
			if (Debug)
				Debug->OnUncaught(T, message);
			std::fflush(stdout);
			lua_State* traced = FailedTask(T);
			std::string trace;
			{
				// Luau writes the trace into a buffer of its own, one for the
				// process: workers failing at once take turns.
				static std::mutex tracing;
				std::lock_guard<std::mutex> lock(tracing);
				trace = lua_debugtrace(traced ? traced : T);
			}
			while (!trace.empty() && trace.back() == '\n')
				trace.pop_back();
			if (OnError)
				OnError(message, trace.empty() ? std::string() : Backtrace(trace), traced != nullptr);
			else if (trace.empty())
				std::fprintf(stderr, "%s\n", message.c_str());
			else
				std::fprintf(stderr, "%s\nstack backtrace%s:\n%s\n", message.c_str(), traced ? " (in the task)" : "", Backtrace(trace).c_str());
			lua_pop(L, 1); // the thread
			return nullptr;
		}

		// The status of a run that ended in an error: 130 for a Ctrl-C, which
		// also ends the programs the run started.
		int Ended()
		{
			if (!Interrupted)
				return 1;
			detail::KillChildren();
			return 130;
		}

		// An error that ends the script without a stack (one in loading it).
		void Report(const std::string& message)
		{
			if (OnError)
				OnError(message, {}, false);
			else
				std::fprintf(stderr, "%s\n", message.c_str());
		}

		int RunChunk(const std::string& source, const std::string& name, bool luau = false)
		{
			std::string error;
			if (Load(L, source, name, false, false, error, luau) != LoadStatus::Ok)
			{
				Report(error);
				return Finished(1);
			}
			lua_State* T = Execute();
			std::fflush(stdout);
			if (!T)
				return Finished(Ended());
			if (OnResults)
				if (const std::string why = OnResults(T); !why.empty())
				{
					lua_pop(L, 1);
					Report(why);
					return Finished(1);
				}
			lua_pop(L, 1);
			int status = RunTasks();
			if (status == 0)
				status = FinishWorkers();
			return Finished(status);
		}

		// The end of a run: after a failure, the workers it started are
		// terminated (a run inside another leaves them to the outer one).
		int Finished(int status)
		{
			if (status != 0 && Threads.empty())
				detail::StopWorkers(L);
			return status;
		}

		// After the script's code and its tasks: the @std/worker workers
		// still running are waited for, and the error of one that nothing
		// joined is reported as the script's.
		int FinishWorkers()
		{
			if (!Threads.empty() || !detail::HasWorkers(L))
				return 0;
			lua_pushcfunction(L, detail::FinishWorkers, "worker");
			lua_State* T = Execute();
			std::fflush(stdout);
			if (!T)
				return Ended();
			const bool failed = lua_gettop(T) > 0 && lua_isstring(T, -1);
			if (failed)
				Report(lua_tostring(T, -1));
			lua_pop(L, 1);
			return failed ? 1 : 0;
		}

		// After the script's code: the @std/task tasks still waiting run to
		// their end (when the script used @std/task).
		int RunTasks()
		{
			lua_getfield(L, LUA_REGISTRYINDEX, detail::kStdRegistry);
			lua_getfield(L, -1, "task");
			if (!lua_istable(L, -1))
			{
				lua_pop(L, 2);
				return 0;
			}
			lua_getfield(L, -1, "_run");
			lua_remove(L, -2);
			lua_remove(L, -2);
			lua_State* T = Execute();
			std::fflush(stdout);
			if (!T)
				return Ended();
			lua_pop(L, 1);
			return 0;
		}
	};

	namespace {

		// Every runtime, by its state's main thread: Luau's interrupt
		// callback finds its runtime here when a host calls into a state
		// outside a run (t_Running is the quick way).
		std::mutex g_RuntimesMutex;
		std::map<lua_State*, Runtime::Impl*> g_Runtimes;
		int g_Profiling = 0; // the runtimes profiling (the exit hook reports theirs)

		Runtime::Impl* RuntimeOf(lua_State* L)
		{
			// The thread a run resumed is found without a call.
			if (L == t_RunningThread)
				return t_Running;
			lua_State* main = lua_mainthread(L);
			if (t_Running && t_Running->L == main)
				return t_Running;
			std::lock_guard<std::mutex> lock(g_RuntimesMutex);
			auto it = g_Runtimes.find(main);
			return it == g_Runtimes.end() ? nullptr : it->second;
		}

		// Luau's interrupt callback: the host's own first (one it had set),
		// then a sample for the profiler when one is due; a pending Ctrl-C
		// becomes an error in the running script.
		void Interrupt(lua_State* L, int gc)
		{
			Runtime::Impl* rt = RuntimeOf(L);
			if (!rt)
				return;
			if (rt->HostInterrupt)
				rt->HostInterrupt(L, gc);
			if (gc >= 0)
				return;
			rt->Sample(L);
			if (g_Interrupt.load(std::memory_order_relaxed))
				StopIfCatching(L);
		}

		// A Ctrl-C is pending: the running script stops, if its runtime
		// catches them. Kept out of line, so the callback's usual path (no
		// Ctrl-C) is a test and a return.
#ifdef _MSC_VER
		__declspec(noinline)
#else
		__attribute__((noinline))
#endif
		void StopIfCatching(lua_State* L)
		{
			Runtime::Impl* rt = RuntimeOf(L);
			if (rt && rt->Opts.CatchInterrupt && g_Interrupt.exchange(0))
				RaiseInterrupted(L);
		}

		// The callback when only a Ctrl-C is looked for.
		void InterruptQuick(lua_State* L, int gc)
		{
			if (gc < 0 && g_Interrupt.load(std::memory_order_relaxed))
				StopIfCatching(L);
		}

		// Runs the module chunk on top of L's stack and keeps its value (true
		// for none) under `key`, the module's identity; returns the value.
		int CallModule(lua_State* L, Runtime::Impl& rt, const std::string& key)
		{
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

		// Under the debugger's require: a frame of it further up the stack
		// is running the module's chunk (its local `key` names the module).
		bool LoadingOnStack(lua_State* L, const std::string& key)
		{
			lua_Debug ar;
			lua_checkstack(L, 2);
			for (int level = 1; lua_getinfo(L, level, "s", &ar); level++)
			{
				if (!ar.source || std::strcmp(ar.source, "=require") != 0)
					continue;
				for (int n = 1;; n++)
				{
					const char* local = lua_getlocal(L, level, n);
					if (!local)
						break;
					const bool same = std::strcmp(local, "key") == 0 && lua_isstring(L, -1) && key == lua_tostring(L, -1);
					lua_pop(L, 1);
					if (same)
						return true;
				}
			}
			return false;
		}

		// A module's chunk, to run: by CallModule, or (under a debugger, so
		// that stepping can enter it) by the require written in Luau, which
		// gets nil, the chunk and its key, runs the chunk and gives the
		// value to RequireFinish. A cycle there is a frame of that require
		// further up the stack loading the same module.
		int RunModule(lua_State* L, Runtime::Impl& rt, const std::string& key, bool deferred)
		{
			if (!deferred)
				return CallModule(L, rt, key);
			lua_pushnil(L);
			lua_insert(L, -2);
			lua_pushlstring(L, key.data(), key.size());
			return 3;
		}

		// The calling chunk's directory (past C functions such as pcall:
		// `pcall(require, "./x")` is the caller's), where relative requires
		// start.
		fs::path CallerDir(lua_State* L, const Runtime::Impl& rt)
		{
			lua_Debug ar;
			for (int level = 1; lua_getinfo(L, level, "s", &ar); level++)
			{
				if (!ar.source || std::strcmp(ar.source, "=[C]") == 0 || std::strcmp(ar.source, "=require") == 0)
					continue;
				if (ar.source[0] == '@')
				{
					auto dir = rt.ChunkDirs.find(ar.source + 1);
					return dir != rt.ChunkDirs.end() ? dir->second : (rt.StartDir / fs::u8path(ar.source + 1)).parent_path();
				}
				break;
			}
			return rt.StartDir;
		}

		int RequireWith(lua_State* L, bool deferred)
		{
			Runtime::Impl& rt = Runtime::Impl::Get(L);
			const std::string name = luaL_checkstring(L, 1);
			if (StartsWith(name, "@std/"))
			{
				if (rt.Opts.Sandbox && detail::SandboxLeavesOut(name.substr(5)))
					luaL_error(L, "%s is not available in the sandbox", name.c_str());
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
			if (auto host = rt.HostModules.find(name); host != rt.HostModules.end())
			{
				lua_getfield(L, LUA_REGISTRYINDEX, kModulesRegistry);
				lua_getfield(L, -1, name.c_str());
				if (!lua_isnil(L, -1))
					return 1;
				lua_pop(L, 2);
				if (rt.Loading.count(name) || (deferred && LoadingOnStack(L, name)))
					luaL_error(L, "require cycle: '%s' is still loading (it requires itself through the modules it requires)", name.c_str());
				if (host->second.Open)
					lua_pushcfunction(L, host->second.Open, name.c_str());
				else
				{
					std::string error;
					if (rt.Load(L, host->second.Source, name, false, false, error) != LoadStatus::Ok)
					{
						lua_pushlstring(L, error.data(), error.size());
						lua_error(L);
					}
				}
				return RunModule(L, rt, name, deferred);
			}
			if (StartsWith(name, "@"))
				luaL_error(L, "unknown alias in '%s' (the standard modules are @std/<name>)", name.c_str());

			const fs::path from = CallerDir(L, rt);
			std::vector<fs::path> searchDirs;
			for (const std::string& dir : rt.Opts.SearchPath)
				searchDirs.push_back(rt.StartDir / fs::u8path(dir));
			std::vector<fs::path> looked;
			const fs::path found = detail::ResolveRequire(name, from, rt.MainDir, searchDirs, &looked);
			// In the sandbox a module outside it reads as one that isn't
			// there, so a script cannot learn which files exist outside.
			if (found.empty() || (rt.Opts.Sandbox && !rt.SandboxAllows(found, searchDirs)))
			{
				if (rt.Opts.Sandbox)
					luaL_error(L, "module '%s' not found in the sandbox (it loads modules from the script's directory, cluau_modules and the search path)",
						name.c_str());
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
			if (rt.Loading.count(key) || (deferred && LoadingOnStack(L, key)))
				luaL_error(L, "require cycle: '%s' is still loading (it requires itself through the modules it requires)", rt.Display(found).c_str());

			std::string text;
			if (!ReadFile(found, text))
				luaL_error(L, "cannot read module '%s'", rt.Display(found).c_str());
			std::string error;
			const std::string chunk = rt.Display(found);
			rt.ChunkDirs[chunk] = fs::u8path(key).parent_path();
			if (rt.Load(L, text, chunk, false, false, error, detail::IsLuauFile(found)) != LoadStatus::Ok)
			{
				lua_pushlstring(L, error.data(), error.size());
				lua_error(L);
			}
			return RunModule(L, rt, key, deferred);
		}

		int Require(lua_State* L) { return RequireWith(L, false); }
		int RequireDeferred(lua_State* L) { return RequireWith(L, true); }

		// The deferred require's second half: the module ran; its value (true
		// for none) is kept under its key and returned.
		int RequireFinish(lua_State* L)
		{
			Runtime::Impl& rt = Runtime::Impl::Get(L);
			const std::string key = luaL_checkstring(L, 1);
			lua_settop(L, 2);
			if (lua_isnil(L, 2))
			{
				lua_pop(L, 1);
				lua_pushboolean(L, 1);
			}
			lua_getfield(L, LUA_REGISTRYINDEX, kModulesRegistry);
			lua_pushvalue(L, 2);
			lua_setfield(L, -2, key.c_str());
			lua_pop(L, 1);
			return 1;
		}

		constexpr const char* kDeferredRequire = R"LUAU(
local load, finish = ...
return function(name)
	local value, chunk, key = load(name)
	if chunk == nil then
		return value
	end
	return finish(key, chunk())
end
)LUAU";

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
						// Three digits always: `\1` before a digit would read as another byte.
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\%03d", c);
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
			// The other keys: numbers first, in numeric order, then the rest by their text.
			struct Field
			{
				bool Number;
				double Value;
				std::string Key, Text;
				bool operator<(const Field& other) const
				{
					if (Number != other.Number)
						return Number;
					if (Number && Value != other.Value)
						return Value < other.Value;
					return Key != other.Key ? Key < other.Key : Text < other.Text;
				}
			};
			std::vector<Field> fields;
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
				const bool number = lua_type(L, -2) == LUA_TNUMBER;
				fields.push_back({ number, number ? lua_tonumber(L, -2) : 0.0, std::move(key), std::move(value) });
				lua_pop(L, 1);
			}
			seen.pop_back();
			std::sort(fields.begin(), fields.end());
			for (const Field& field : fields)
				items.push_back(field.Key + " = " + field.Text);
			out += prefix + "{";
			for (size_t i = 0; i < items.size(); i++)
				out += (i ? ", " : "") + items[i];
			out += "}";
		}

		// The prompt's line for an expression's values: each one's repr,
		// tab-separated.
		int ReprValues(lua_State* L)
		{
			std::string out;
			for (int i = 1, n = lua_gettop(L); i <= n; i++)
				out += (i > 1 ? "\t" : "") + Repr(L, i);
			lua_pushlstring(L, out.data(), out.size());
			return 1;
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
				// A key the table does not have itself: an instance's class's
				// (an __index table), or a script class's base's (__base). An
				// __index function would be code to run, and is not called.
				for (int depth = 0; depth < 8 && !lua_istable(L, -1); depth++)
				{
					lua_pop(L, 1);
					if (lua_getmetatable(L, -1))
					{
						lua_rawgetfield(L, -1, "__index");
						lua_remove(L, -2);
					}
					else
						lua_pushnil(L);
					if (!lua_istable(L, -1))
					{
						lua_pop(L, 1);
						lua_rawgetfield(L, -1, "__base");
					}
					if (!lua_istable(L, -1))
					{
						lua_settop(L, top);
						return;
					}
					lua_remove(L, -2);
					lua_pushlstring(L, key.data(), key.size());
					lua_rawget(L, -2);
				}
				if (!lua_istable(L, -1))
				{
					lua_settop(L, top);
					return;
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

	#if defined(CLUAU_HAS_ISOCLINE) && !defined(_WIN32)
		// While it reads a line, isocline handles the signals that end a
		// process: it gives the terminal its mode back, then calls the handler
		// that was there before, but only a handler of the program's own, not
		// the default action. With none of ours under it the signal would be
		// lost: a closed terminal's SIGHUP left the prompt reading a terminal
		// that is gone, at full speed, for good. Ours takes the default action.
		void TakeDefault(int signum, siginfo_t*, void*)
		{
			std::signal(signum, SIG_DFL);
			std::raise(signum); // delivered once isocline's handler returns
		}

		void PassOnEndingSignals()
		{
			for (int signum : { SIGTERM, SIGHUP, SIGQUIT, SIGSEGV, SIGBUS, SIGTRAP })
			{
				struct sigaction current = {};
				if (sigaction(signum, nullptr, &current) != 0 || (current.sa_flags & SA_SIGINFO) || current.sa_handler != SIG_DFL)
					continue;
				struct sigaction action = {};
				action.sa_sigaction = TakeDefault; // isocline calls it with three arguments
				sigemptyset(&action.sa_mask);
				action.sa_flags = SA_SIGINFO;
				sigaction(signum, &action, nullptr);
			}
		}
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
			// The script's io.readLine reads from the same buffer.
			if (!detail::ReadStandardInputLine(line))
				return Read::End;
			return Read::Line;
		}

	} // namespace

	// A worker's thread sees its own stop instead of the pending Ctrl-C,
	// which is the main script's to take (a Ctrl-C stops a worker too: it
	// counts the Ctrl-Cs since it started).
	bool detail::InterruptPending()
	{
		if (detail::InWorker())
			return detail::WorkerStopping() != nullptr;
		return g_Interrupt.load() != 0;
	}

	unsigned detail::InterruptCount() { return g_Interrupts; }

	void detail::RaiseIfInterrupted(lua_State* L)
	{
		if (detail::InWorker())
		{
			if (const char* why = detail::WorkerStopping())
			{
				lua_rawcheckstack(L, 1);
				lua_pushstring(L, why);
				lua_error(L);
			}
			return;
		}
		if (!g_Interrupt.exchange(0))
			return;
		std::cin.clear();
		RaiseInterrupted(L);
	}

	void detail::SampleBlocking(lua_State* L)
	{
		if (Runtime::Impl* rt = RuntimeOf(L))
			rt->Sample(L);
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
		// Luau's collector takes the heap's growth between collections as
		// an unsigned difference, which a heap that shrank meanwhile turns
		// into a huge number (and an out-of-range conversion); the fix Luau
		// keeps behind this flag measures it properly. The flag is the
		// process's: set once, before any runtime (on any thread) runs.
		static std::once_flag shrinkFix;
		std::call_once(shrinkFix, [] { DFFlag::LuauGcHeapShrinkFix.value = true; });
		lua_State* L = rt.L = luaL_newstate();
		rt.Native = rt.Opts.Native && !rt.Opts.Coverage && Luau::CodeGen::isSupported();
		if (rt.Native)
			Luau::CodeGen::create(L);
		luaL_openlibs(L);

		lua_pushlightuserdata(L, &rt);
		lua_setfield(L, LUA_REGISTRYINDEX, kRuntimeRegistry);
		{
			std::lock_guard<std::mutex> lock(g_RuntimesMutex);
			g_Runtimes[L] = &rt;
		}
		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, kModulesRegistry);

		lua_pushcfunction(L, Require, "require");
		lua_setglobal(L, "require");
		lua_pushcfunction(L, ReprGlobal, "repr");
		lua_setglobal(L, "repr");
		detail::OpenStd(L, rt.Opts.Args, rt.Opts.Sandbox);

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
		if (rt.Opts.Profile)
		{
			rt.Profile = std::make_unique<detail::Profiler>();
			// process.exit ends the process: the report of the runtime
			// that calls it goes first.
			std::lock_guard<std::mutex> lock(g_RuntimesMutex);
			if (g_Profiling++ == 0)
				detail::SetExitHook([](int) {
					if (t_Running)
						t_Running->ReportProfile();
				});
		}
	}

	Runtime::~Runtime()
	{
		{
			std::lock_guard<std::mutex> lock(g_RuntimesMutex);
			g_Runtimes.erase(m_Impl->L);
			if (m_Impl->Profile && --g_Profiling == 0)
				detail::SetExitHook(nullptr);
		}
		if (m_Impl->Profile)
			m_Impl->ReportProfile();
		if (m_Impl->L)
			lua_close(m_Impl->L);
	}

	lua_State* Runtime::State() const { return m_Impl->L; }

	std::map<std::string, std::map<int, int>> Runtime::Coverage() const
	{
		std::map<std::string, std::map<int, int>> out;
		lua_State* L = m_Impl->L;
		for (const auto& [file, ref] : m_Impl->CoverageChunks)
		{
			std::map<int, int>& lines = out[file];
			lua_getref(L, ref);
			// Each function of the chunk (and the functions inside it).
			lua_getcoverage(L, -1, &lines, [](void* context, const char*, int, int, const int* hits, size_t size) {
				auto& lines = *static_cast<std::map<int, int>*>(context);
				for (size_t line = 1; line < size; line++)
					if (hits[line] >= 0)
					{
						int& count = lines[(int)line];
						count = std::max(count, hits[line]);
					}
			});
			lua_pop(L, 1);
		}
		return out;
	}

	namespace {

		bool HostModuleName(const std::string& name)
		{
			const size_t slash = name.find('/');
			return name.size() > 1 && name[0] == '@' && slash != std::string::npos && slash > 1 && slash + 1 < name.size() &&
				!StartsWith(name, "@std/");
		}

	} // namespace

	bool Runtime::AddModule(const std::string& name, int (*open)(lua_State*))
	{
		if (!HostModuleName(name) || !open)
			return false;
		m_Impl->HostModules[name] = { open, {} };
		return true;
	}

	bool Runtime::AddModuleSource(const std::string& name, const std::string& source)
	{
		if (!HostModuleName(name))
			return false;
		m_Impl->HostModules[name] = { nullptr, source };
		return true;
	}

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
		Impl::NestedRun nested(rt);
		// The real file's directory (a script linked into a bin directory
		// finds the modules beside it, as the checker does).
		const fs::path real = fs::weakly_canonical(rt.StartDir / file, ec);
		rt.MainDir = (ec ? (rt.StartDir / file).lexically_normal() : real).parent_path();
		rt.ChunkDirs[path] = rt.MainDir;
		rt.SetScript(path);
		return rt.RunChunk(text, path, detail::IsLuauFile(file));
	}

	bool Runtime::RunTest(const std::string& path, const std::string& filter, TestCounts& counts)
	{
		Impl& rt = *m_Impl;
		lua_State* L = rt.L;
		// @std/test, told the file and the filter.
		lua_getglobal(L, "require");
		lua_pushstring(L, "@std/test");
		if (lua_pcall(L, 1, 1, 0) != 0)
		{
			std::fprintf(stderr, "cluau: %s\n", lua_tostring(L, -1));
			lua_pop(L, 1);
			return false;
		}
		const int module = lua_gettop(L);
		lua_getfield(L, module, "_begin");
		lua_pushstring(L, path.c_str());
		lua_pushstring(L, filter.c_str());
		lua_call(L, 2, 0);

		const bool ran = RunFile(path) == 0;

		lua_getfield(L, module, "_finish");
		lua_call(L, 0, 3);
		counts.Passed = (int)lua_tonumber(L, -3);
		counts.Failed = (int)lua_tonumber(L, -2);
		counts.Skipped = (int)lua_tonumber(L, -1);
		lua_settop(L, module - 1);
		return ran;
	}

	int Runtime::RunSource(const std::string& source, const std::string& name)
	{
		Impl& rt = *m_Impl;
		Impl::NestedRun nested(rt);
		rt.MainDir = rt.StartDir;
		rt.SetScript(name);
		// A byte order mark is not code (as in a file).
		if (source.rfind("\xEF\xBB\xBF", 0) == 0)
			return rt.RunChunk(source.substr(3), name);
		return rt.RunChunk(source, name);
	}

	int Runtime::Repl()
	{
		Impl& rt = *m_Impl;
		lua_State* L = rt.L;
		const bool interactive = CLUAU_ISATTY(0) && CLUAU_ISATTY(1);
		rt.MainDir = rt.StartDir;
		rt.SetScript("");
#ifdef CLUAU_HAS_ISOCLINE
		if (interactive)
		{
#ifndef _WIN32
			PassOnEndingSignals();
#endif
			ic_set_prompt_marker("", "");
			ic_enable_brace_insertion(false);
			ic_set_default_highlighter(TrackEdits, nullptr);
			ic_set_default_completer(Complete, L);
#ifdef _WIN32
			// The variable and the history file's name in the ANSI code page,
			// as isocline opens it.
			if (const char* home = std::getenv("USERPROFILE"))
				ic_set_history((std::string(home) + "\\.cluau_history").c_str(), -1);
#else
			if (const char* home = std::getenv("HOME"))
				ic_set_history(ToUtf8(fs::u8path(home) / ".cluau_history").c_str(), -1);
#endif
		}
#endif
		if (interactive)
			std::printf("cluau %s (C-syntax-flavoured Luau). Tab completes; Ctrl-C abandons an input; Ctrl-D exits.\n", Version());

		std::string buffer;
		std::string line;
		// Each input is a chunk of its own, `stdin[n]`: a function defined
		// in an earlier one quotes its own lines in a backtrace.
		int inputs = 0;
		std::string name;
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
			if (buffer.empty())
				name = "stdin[" + std::to_string(++inputs) + "]";
			buffer += buffer.empty() ? line : "\n" + line;

			std::string error;
			// An expression prints its value; anything else runs as statements.
			bool expression = rt.Load(L, "return " + buffer, name, true, true, error, false, &buffer) == LoadStatus::Ok;
			if (!expression)
			{
				const LoadStatus status = rt.Load(L, buffer, name, true, false, error);
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
				// The values' text, in a protected call: a __tostring
				// can fail (or yield), which is reported as any error is.
				lua_checkstack(L, n + 2);
				lua_pushcfunction(L, ReprValues, "repr");
				for (int i = 1; i <= n; i++)
					lua_xpush(T, L, i);
				if (lua_pcall(L, n, 1, 0) == LUA_OK)
					std::printf("%s\n", lua_tostring(L, -1));
				else
					std::fprintf(stderr, "%s\n", lua_isstring(L, -1) ? lua_tostring(L, -1) : "(the value's __tostring failed)");
				lua_pop(L, 1);
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
			if (rt.Load(L, buffer, name, true, false, error) != LoadStatus::Ok)
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

	void detail::SetDebugHooks(lua_State* L, DebugHooks* hooks)
	{
		Runtime::Impl& rt = Runtime::Impl::Get(L);
		rt.Debug = hooks;
		if (!hooks)
		{
			lua_pushcfunction(L, Require, "require");
			lua_setglobal(L, "require");
			return;
		}
		Luau::CompileOptions copts;
		copts.debugLevel = 2; // its locals' names (LoadingOnStack)
		const std::string bytecode = Luau::compile(kDeferredRequire, copts);
		if (luau_load(L, "=require", bytecode.data(), bytecode.size(), 0) != 0)
		{
			lua_pop(L, 1);
			return;
		}
		lua_pushcfunction(L, RequireDeferred, "require");
		lua_pushcfunction(L, RequireFinish, "require");
		lua_call(L, 2, 1);
		lua_setglobal(L, "require");
	}

	std::string detail::CompileExpression(const std::string& expression, bool luau, std::string& error)
	{
		std::string source = "return (" + expression + ")";
		if (!luau)
		{
			TranslateOptions options;
			options.Script = true;
			options.Profile = &ScriptProfile();
			const TranslateResult result = Translate(source, options);
			for (const Diagnostic& d : result.Diagnostics)
				if (!d.Warning)
				{
					error = d.Message;
					return {};
				}
			source = result.Luau;
		}
		Luau::CompileOptions copts;
		copts.debugLevel = 1;
		std::string bytecode = Luau::compile(source, copts);
		if (bytecode.empty() || bytecode[0] == 0)
		{
			error = bytecode.size() > 1 ? bytecode.substr(1) : "the expression does not compile";
			// Luau's message starts with the chunk's position (":1: ").
			if (error.size() > 4 && error.compare(0, 4, ":1: ") == 0)
				error = error.substr(4);
			return {};
		}
		return bytecode;
	}

	std::string detail::ChunkSource(lua_State* L, const std::string& name)
	{
		const Runtime::Impl& rt = Runtime::Impl::Get(L);
		auto it = rt.Sources.find(name);
		return it == rt.Sources.end() ? std::string() : it->second;
	}

	detail::WorkerLaunch detail::PrepareWorker(lua_State* L, const std::string& path, const char* fn)
	{
		const Runtime::Impl& rt = Runtime::Impl::Get(L);
		// A path that is not absolute is the calling script's directory's,
		// as `./path` is to require (never a package or the search path's).
		const bool relative = !fs::u8path(path).is_absolute() && !StartsWith(path, "./") && !StartsWith(path, "../");
		std::vector<fs::path> looked;
		const fs::path found = detail::ResolveRequire(relative ? "./" + path : path, CallerDir(L, rt), rt.MainDir, {}, &looked);
		if (found.empty())
		{
			std::string tried;
			for (const fs::path& base : looked)
				tried += (tried.empty() ? "" : ", ") + rt.Display(base) + "[.cluau|.luau]";
			luaL_error(L, "%s: no script '%s' (looked for %s)", fn, path.c_str(), tried.c_str());
		}
		WorkerLaunch launch;
		launch.File = ToUtf8(found);
		launch.Name = rt.Display(found);
		// The worker's runtime is the script's, but for what belongs to the
		// main script: Ctrl-C (it stops the workers through their own
		// check), the debugger, the profiler and coverage.
		launch.Opts.Args = rt.Opts.Args;
		for (const std::string& dir : rt.Opts.SearchPath)
			launch.Opts.SearchPath.push_back(ToUtf8(rt.StartDir / fs::u8path(dir)));
		launch.Opts.Native = rt.Native;
		return launch;
	}

	void detail::RunWorker(const WorkerLaunch& launch, const std::function<void(lua_State*)>& prepare,
		const std::function<std::string(lua_State*)>& results, WorkerFailure& failure)
	{
		Runtime runtime(launch.Opts);
		Runtime::Impl& rt = Runtime::Impl::Get(runtime.State());
		rt.OnError = [&failure](const std::string& message, const std::string& trace, bool task) {
			failure.Failed = true;
			failure.Message = message;
			failure.Trace = trace;
			failure.InTask = task;
		};
		rt.OnResults = results;
		prepare(rt.L);
		// As RunFile: the modules beside the real file are the script's.
		const fs::path file = fs::u8path(launch.File);
		const std::string name = rt.Display(file);
		std::string text;
		if (!ReadFile(file, text))
		{
			failure.Failed = true;
			failure.Message = "cannot read '" + name + "'";
			return;
		}
		std::error_code ec;
		const fs::path real = fs::weakly_canonical(file, ec);
		rt.MainDir = (ec ? file.lexically_normal() : real).parent_path();
		rt.ChunkDirs[name] = rt.MainDir;
		rt.SetScript(name);
		if (rt.RunChunk(text, name, detail::IsLuauFile(file)) != 0 && !failure.Failed)
		{
			failure.Failed = true;
			failure.Message = "the script failed";
		}
	}

} // namespace cluau::runtime
