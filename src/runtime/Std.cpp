// The standard modules: @std/fs, @std/path, @std/process, @std/io, @std/json,
// and the `input` global. Errors are Luau errors at the calling line
// (`fs.read: cannot read 'x': no such file`). Paths come back with `/` on
// every platform (Windows accepts it), so a script's output is the same
// everywhere.

#include "Std.h"
#include "Spawn.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace cluau::runtime::detail {

	namespace {

		fs::path P(lua_State* L, int arg) { return fs::u8path(luaL_checkstring(L, arg)); }

		std::string S(const fs::path& p)
		{
			const auto u = p.generic_u8string(); // `/` on every platform
			return std::string(u.begin(), u.end());
		}

		void PushPath(lua_State* L, const fs::path& p)
		{
			const std::string s = S(p);
			lua_pushlstring(L, s.data(), s.size());
		}

		void PushStrings(lua_State* L, const std::vector<std::string>& items)
		{
			lua_createtable(L, (int)items.size(), 0);
			for (size_t i = 0; i < items.size(); i++)
			{
				lua_pushlstring(L, items[i].data(), items[i].size());
				lua_rawseti(L, -2, (int)i + 1);
			}
		}

		[[noreturn]] void Fail(lua_State* L, const char* fn, const char* what, const fs::path& path, const std::string& why)
		{
			luaL_error(L, "%s: %s '%s': %s", fn, what, S(path).c_str(), why.c_str());
			std::abort(); // not reached
		}

		std::string Reason(const fs::path& path, const std::error_code& ec)
		{
			if (ec)
				return ec.message();
			std::error_code ignored;
			if (!fs::exists(path, ignored))
				return "no such file or directory";
			if (fs::is_directory(path, ignored))
				return "is a directory";
			return "permission denied or unreadable";
		}

		void SetFunctions(lua_State* L, const luaL_Reg* functions)
		{
			for (; functions->name; functions++)
			{
				lua_pushcfunction(L, functions->func, functions->name);
				lua_setfield(L, -2, functions->name);
			}
		}

		// ---- @std/fs --------------------------------------------------------

		int FsRead(lua_State* L)
		{
			const fs::path path = P(L, 1);
			std::ifstream in(path, std::ios::binary);
			std::error_code ec;
			if (!in || fs::is_directory(path, ec))
				Fail(L, "fs.read", "cannot read", path, Reason(path, {}));
			std::stringstream buffer;
			buffer << in.rdbuf();
			const std::string text = buffer.str();
			lua_pushlstring(L, text.data(), text.size());
			return 1;
		}

		int WriteMode(lua_State* L, const char* fn, std::ios::openmode mode)
		{
			const fs::path path = P(L, 1);
			size_t len = 0;
			const char* text = luaL_checklstring(L, 2, &len);
			std::ofstream out(path, std::ios::binary | mode);
			if (!out)
			{
				std::error_code ec;
				const fs::path dir = path.parent_path().empty() ? fs::path(".") : path.parent_path();
				Fail(L, fn, "cannot write", path,
					fs::is_directory(path, ec) ? "is a directory"
					: !fs::is_directory(dir, ec) ? "its directory does not exist"
												 : "permission denied");
			}
			out.write(text, (std::streamsize)len);
			if (!out)
				Fail(L, fn, "cannot write", path, "write failed");
			return 0;
		}

		int FsWrite(lua_State* L) { return WriteMode(L, "fs.write", std::ios::trunc); }
		int FsAppend(lua_State* L) { return WriteMode(L, "fs.append", std::ios::app); }

		int FsExists(lua_State* L)
		{
			std::error_code ec;
			lua_pushboolean(L, fs::exists(P(L, 1), ec));
			return 1;
		}

		int FsIsFile(lua_State* L)
		{
			std::error_code ec;
			lua_pushboolean(L, fs::is_regular_file(P(L, 1), ec));
			return 1;
		}

		int FsIsDir(lua_State* L)
		{
			std::error_code ec;
			lua_pushboolean(L, fs::is_directory(P(L, 1), ec));
			return 1;
		}

		int FsList(lua_State* L)
		{
			const fs::path dir = P(L, 1);
			std::error_code ec;
			std::vector<std::string> names;
			for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
				names.push_back(S(it->path().filename()));
			if (ec)
				Fail(L, "fs.list", "cannot list", dir, ec.message());
			std::sort(names.begin(), names.end());
			PushStrings(L, names);
			return 1;
		}

		int FsWalk(lua_State* L)
		{
			const fs::path dir = P(L, 1);
			std::error_code ec;
			std::vector<std::string> files;
			for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
			{
				std::error_code fileEc;
				if (it->is_regular_file(fileEc))
					files.push_back(S(it->path()));
			}
			if (ec)
				Fail(L, "fs.walk", "cannot walk", dir, ec.message());
			std::sort(files.begin(), files.end());
			PushStrings(L, files);
			return 1;
		}

		int FsMkdir(lua_State* L)
		{
			const fs::path dir = P(L, 1);
			std::error_code ec;
			fs::create_directories(dir, ec);
			if (ec)
				Fail(L, "fs.mkdir", "cannot create", dir, ec.message());
			return 0;
		}

		int FsRemove(lua_State* L)
		{
			const fs::path path = P(L, 1);
			const bool all = lua_toboolean(L, 2);
			std::error_code ec;
			if (!fs::exists(fs::symlink_status(path, ec)))
				return 0;
			if (all)
				fs::remove_all(path, ec);
			else
				fs::remove(path, ec);
			if (ec)
				Fail(L, "fs.remove", "cannot remove", path, ec.message() + (all ? "" : " (fs.remove(path, true) removes a directory's contents)"));
			return 0;
		}

		int FsRename(lua_State* L)
		{
			const fs::path from = P(L, 1), to = P(L, 2);
			std::error_code ec;
			fs::rename(from, to, ec);
			if (ec)
				Fail(L, "fs.rename", "cannot rename", from, ec.message());
			return 0;
		}

		int FsCopy(lua_State* L)
		{
			const fs::path from = P(L, 1), to = P(L, 2);
			std::error_code ec;
			fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
			if (ec)
				Fail(L, "fs.copy", "cannot copy", from, ec.message());
			return 0;
		}

		int FsSize(lua_State* L)
		{
			const fs::path path = P(L, 1);
			std::error_code ec;
			const auto size = fs::file_size(path, ec);
			if (ec)
				Fail(L, "fs.size", "cannot stat", path, ec.message());
			lua_pushnumber(L, (double)size);
			return 1;
		}

		int FsModified(lua_State* L)
		{
			const fs::path path = P(L, 1);
			std::error_code ec;
			const auto ft = fs::last_write_time(path, ec);
			if (ec)
				Fail(L, "fs.modified", "cannot stat", path, ec.message());
			const auto now = std::chrono::system_clock::now() +
				std::chrono::duration_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now());
			lua_pushnumber(L, std::chrono::duration<double>(now.time_since_epoch()).count());
			return 1;
		}

		int FsTempDir(lua_State* L)
		{
			std::error_code ec;
			PushPath(L, fs::temp_directory_path(ec));
			return 1;
		}

		const luaL_Reg kFs[] = {
			{ "read", FsRead }, { "write", FsWrite }, { "append", FsAppend }, { "exists", FsExists }, { "isFile", FsIsFile },
			{ "isDir", FsIsDir }, { "list", FsList }, { "walk", FsWalk }, { "mkdir", FsMkdir }, { "remove", FsRemove },
			{ "rename", FsRename }, { "copy", FsCopy }, { "size", FsSize }, { "modified", FsModified }, { "tempDir", FsTempDir },
			{ nullptr, nullptr },
		};

		// ---- @std/path ------------------------------------------------------

		int PathJoin(lua_State* L)
		{
			const int n = lua_gettop(L);
			fs::path out;
			for (int i = 1; i <= n; i++)
				out /= P(L, i);
			PushPath(L, out);
			return 1;
		}

		int PathDirname(lua_State* L)
		{
			PushPath(L, P(L, 1).parent_path());
			return 1;
		}

		int PathBasename(lua_State* L)
		{
			PushPath(L, P(L, 1).filename());
			return 1;
		}

		int PathStem(lua_State* L)
		{
			PushPath(L, P(L, 1).stem());
			return 1;
		}

		int PathExtension(lua_State* L)
		{
			PushPath(L, P(L, 1).extension());
			return 1;
		}

		int PathAbsolute(lua_State* L)
		{
			std::error_code ec;
			PushPath(L, fs::absolute(P(L, 1), ec).lexically_normal());
			return 1;
		}

		int PathNormalize(lua_State* L)
		{
			PushPath(L, P(L, 1).lexically_normal());
			return 1;
		}

		int PathRelative(lua_State* L)
		{
			std::error_code ec;
			const fs::path path = fs::absolute(P(L, 1), ec);
			const fs::path base = lua_isnoneornil(L, 2) ? fs::current_path(ec) : fs::absolute(P(L, 2), ec);
			PushPath(L, path.lexically_normal().lexically_relative(base.lexically_normal()));
			return 1;
		}

		int PathIsAbsolute(lua_State* L)
		{
			lua_pushboolean(L, P(L, 1).is_absolute());
			return 1;
		}

		const luaL_Reg kPath[] = {
			{ "join", PathJoin }, { "dirname", PathDirname }, { "basename", PathBasename }, { "stem", PathStem },
			{ "extension", PathExtension }, { "absolute", PathAbsolute }, { "normalize", PathNormalize }, { "relative", PathRelative },
			{ "isAbsolute", PathIsAbsolute }, { nullptr, nullptr },
		};

		// ---- @std/process ---------------------------------------------------

		int ProcessExit(lua_State* L)
		{
			const int code = (int)luaL_optinteger(L, 1, 0);
			std::fflush(stdout);
			std::fflush(stderr);
			std::exit(code);
		}

		int ProcessEnv(lua_State* L)
		{
			const char* value = std::getenv(luaL_checkstring(L, 1));
			if (value)
				lua_pushstring(L, value);
			else
				lua_pushnil(L);
			return 1;
		}

		int ProcessSetEnv(lua_State* L)
		{
			const char* name = luaL_checkstring(L, 1);
			const char* value = luaL_optstring(L, 2, nullptr);
#ifdef _WIN32
			_putenv_s(name, value ? value : "");
#else
			if (value)
				setenv(name, value, 1);
			else
				unsetenv(name);
#endif
			return 0;
		}

		int ProcessCwd(lua_State* L)
		{
			std::error_code ec;
			PushPath(L, fs::current_path(ec));
			return 1;
		}

		int ProcessChdir(lua_State* L)
		{
			const fs::path dir = P(L, 1);
			std::error_code ec;
			fs::current_path(dir, ec);
			if (ec)
				Fail(L, "process.chdir", "cannot enter", dir, ec.message());
			return 0;
		}

		// The command: a string (a command line for the system shell) or a
		// list ({ "git", "status" }: the program and its arguments, passed
		// as they are, no shell). Options: input (standard input text), cwd,
		// env ({ NAME = "value" } on top of the script's environment), and
		// for run, stderr = "inherit" | "capture" | "merge".
		SpawnRequest Command(lua_State* L, const char* fn, bool run)
		{
			SpawnRequest request;
			if (lua_type(L, 1) == LUA_TSTRING)
			{
				request.Shell = true;
				request.Args.push_back(lua_tostring(L, 1));
			}
			else if (lua_type(L, 1) == LUA_TTABLE)
			{
				const int n = lua_objlen(L, 1);
				for (int i = 1; i <= n; i++)
				{
					lua_rawgeti(L, 1, i);
					if (!lua_isstring(L, -1))
						luaL_error(L, "%s: argument %d of the command is a %s, not a string", fn, i, luaL_typename(L, -1));
					request.Args.push_back(lua_tostring(L, -1));
					lua_pop(L, 1);
				}
				if (request.Args.empty())
					luaL_error(L, "%s: the command list is empty (it starts with the program)", fn);
			}
			else
				luaL_error(L, "%s: the command is a string (a shell command line) or a list ({ program, args... })", fn);
			if (lua_isnoneornil(L, 2))
				return request;
			luaL_checktype(L, 2, LUA_TTABLE);
			lua_getfield(L, 2, "input");
			if (!lua_isnil(L, -1))
			{
				size_t len = 0;
				const char* text = lua_tolstring(L, -1, &len);
				if (!text)
					luaL_error(L, "%s: options.input is a string", fn);
				request.Input = std::string(text, len);
			}
			lua_pop(L, 1);
			lua_getfield(L, 2, "cwd");
			if (!lua_isnil(L, -1))
			{
				if (!lua_isstring(L, -1))
					luaL_error(L, "%s: options.cwd is a string", fn);
				request.Cwd = lua_tostring(L, -1);
			}
			lua_pop(L, 1);
			lua_getfield(L, 2, "env");
			if (lua_istable(L, -1))
			{
				lua_pushnil(L);
				while (lua_next(L, -2))
				{
					if (lua_type(L, -2) != LUA_TSTRING || !lua_isstring(L, -1))
						luaL_error(L, "%s: options.env maps names to strings", fn);
					request.Env.emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
					lua_pop(L, 1);
				}
				std::sort(request.Env.begin(), request.Env.end());
			}
			else if (!lua_isnil(L, -1))
				luaL_error(L, "%s: options.env is a table ({ NAME = \"value\" })", fn);
			lua_pop(L, 1);
			lua_getfield(L, 2, "stderr");
			if (!lua_isnil(L, -1))
			{
				const char* mode = lua_tostring(L, -1);
				if (!run)
					luaL_error(L, "%s: options.stderr is process.run's (exec's output goes where the script's goes)", fn);
				if (mode && std::strcmp(mode, "capture") == 0)
					request.Error = Stream::Capture;
				else if (mode && std::strcmp(mode, "merge") == 0)
					request.Error = Stream::Merge;
				else if (!mode || std::strcmp(mode, "inherit") != 0)
					luaL_error(L, "%s: options.stderr is \"inherit\", \"capture\" or \"merge\"", fn);
			}
			lua_pop(L, 1);
			return request;
		}

		void StartOrFail(lua_State* L, const char* fn, const SpawnRequest& request, SpawnResult& result)
		{
			std::string why;
			if (!Spawn(request, result, why))
			{
				const std::string what = request.Shell ? std::string("the shell") : "'" + request.Args[0] + "'";
				luaL_error(L, "%s: cannot start %s: %s", fn, what.c_str(), why.c_str());
			}
		}

		// process.run(command, options?): { code, output, error } - the
		// command's standard output captured (and its standard error, with
		// stderr = "capture").
		int ProcessRun(lua_State* L)
		{
			SpawnRequest request = Command(L, "process.run", true);
			request.CaptureOutput = true;
			SpawnResult result;
			StartOrFail(L, "process.run", request, result);
			lua_createtable(L, 0, 3);
			lua_pushinteger(L, result.Code);
			lua_setfield(L, -2, "code");
			lua_pushlstring(L, result.Output.data(), result.Output.size());
			lua_setfield(L, -2, "output");
			lua_pushlstring(L, result.Error.data(), result.Error.size());
			lua_setfield(L, -2, "error");
			return 1;
		}

		// process.exec(command, options?): the exit code, the command's
		// output going where the script's goes.
		int ProcessExec(lua_State* L)
		{
			const SpawnRequest request = Command(L, "process.exec", false);
			SpawnResult result;
			StartOrFail(L, "process.exec", request, result);
			lua_pushinteger(L, result.Code);
			return 1;
		}

		int ProcessSleep(lua_State* L)
		{
			const double seconds = luaL_checknumber(L, 1);
			if (seconds > 0)
				std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
			return 0;
		}

		const luaL_Reg kProcess[] = {
			{ "exit", ProcessExit }, { "env", ProcessEnv }, { "setEnv", ProcessSetEnv }, { "cwd", ProcessCwd },
			{ "chdir", ProcessChdir }, { "run", ProcessRun }, { "exec", ProcessExec }, { "sleep", ProcessSleep },
			{ nullptr, nullptr },
		};

		// The running program's path (process.executable).
		std::string Executable()
		{
#if defined(_WIN32)
			std::wstring buffer(32768, L'\0');
			const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), (DWORD)buffer.size());
			buffer.resize(n);
			return S(fs::path(buffer));
#elif defined(__APPLE__)
			uint32_t size = 0;
			_NSGetExecutablePath(nullptr, &size);
			std::string buffer(size, '\0');
			if (_NSGetExecutablePath(buffer.data(), &size) != 0)
				return {};
			buffer.resize(std::strlen(buffer.c_str()));
			std::error_code ec;
			return S(fs::weakly_canonical(fs::u8path(buffer), ec));
#else
			std::error_code ec;
			const fs::path self = fs::read_symlink("/proc/self/exe", ec);
			return ec ? std::string() : S(self);
#endif
		}

		const char* Platform()
		{
#if defined(_WIN32)
			return "windows";
#elif defined(__APPLE__)
			return "macos";
#elif defined(__linux__)
			return "linux";
#else
			return "other";
#endif
		}

		// ---- @std/io and input ----------------------------------------------

		int WriteTo(lua_State* L, FILE* to)
		{
			const int n = lua_gettop(L);
			for (int i = 1; i <= n; i++)
			{
				size_t len = 0;
				const char* s = luaL_tolstring(L, i, &len);
				std::fwrite(s, 1, len, to);
				lua_pop(L, 1);
			}
			return 0;
		}

		int IoWrite(lua_State* L) { return WriteTo(L, stdout); }

		int IoWriteError(lua_State* L)
		{
			std::fflush(stdout);
			return WriteTo(L, stderr);
		}

		bool GetLine(lua_State* L, std::string& line)
		{
			if (!std::getline(std::cin, line))
			{
				RaiseIfInterrupted(L);
				return false;
			}
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			return true;
		}

		int IoReadLine(lua_State* L)
		{
			std::fflush(stdout);
			std::string line;
			if (!GetLine(L, line))
				lua_pushnil(L);
			else
				lua_pushlstring(L, line.data(), line.size());
			return 1;
		}

		int IoReadAll(lua_State* L)
		{
			std::fflush(stdout);
			std::stringstream buffer;
			buffer << std::cin.rdbuf();
			RaiseIfInterrupted(L);
			const std::string text = buffer.str();
			lua_pushlstring(L, text.data(), text.size());
			return 1;
		}

		int IoFlush(lua_State*)
		{
			std::fflush(stdout);
			std::fflush(stderr);
			return 0;
		}

		const luaL_Reg kIo[] = {
			{ "write", IoWrite }, { "writeError", IoWriteError }, { "readLine", IoReadLine }, { "readAll", IoReadAll },
			{ "flush", IoFlush }, { nullptr, nullptr },
		};

		// input(prompt?): a line from standard input (nil at its end).
		int Input(lua_State* L)
		{
			if (!lua_isnoneornil(L, 1))
			{
				size_t len = 0;
				const char* s = luaL_tolstring(L, 1, &len);
				std::fwrite(s, 1, len, stdout);
				lua_pop(L, 1);
			}
			return IoReadLine(L);
		}

		// ---- @std/json ------------------------------------------------------

		std::string FormatNumber(double d)
		{
			char buf[32];
			if (d == std::floor(d) && std::fabs(d) < 1e15)
			{
				std::snprintf(buf, sizeof(buf), "%.0f", d);
				return buf;
			}
			for (int precision = 15; precision <= 17; precision++)
			{
				std::snprintf(buf, sizeof(buf), "%.*g", precision, d);
				if (std::strtod(buf, nullptr) == d)
					break;
			}
			return buf;
		}

		void JsonString(const char* s, size_t len, std::string& out)
		{
			out += '"';
			for (size_t i = 0; i < len; i++)
			{
				const unsigned char c = (unsigned char)s[i];
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\b': out += "\\b"; break;
				case '\f': out += "\\f"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default:
					if (c < 0x20)
					{
						char buf[8];
						std::snprintf(buf, sizeof(buf), "\\u%04x", c);
						out += buf;
					}
					else
						out += (char)c;
				}
			}
			out += '"';
		}

		void Newline(std::string& out, const std::string& indent, int depth)
		{
			if (indent.empty())
				return;
			out += '\n';
			for (int i = 0; i < depth; i++)
				out += indent;
		}

		void JsonEncode(lua_State* L, int index, const std::string& indent, int depth, std::vector<const void*>& seen, std::string& out)
		{
			index = lua_absindex(L, index);
			lua_checkstack(L, 6);
			switch (lua_type(L, index))
			{
			case LUA_TNIL: out += "null"; return;
			case LUA_TBOOLEAN: out += lua_toboolean(L, index) ? "true" : "false"; return;
			case LUA_TNUMBER:
			{
				const double d = lua_tonumber(L, index);
				if (!std::isfinite(d))
					luaL_error(L, "json.encode: %s has no JSON form", d != d ? "nan" : "an infinite number");
				out += FormatNumber(d);
				return;
			}
			case LUA_TSTRING:
			{
				size_t len = 0;
				const char* s = lua_tolstring(L, index, &len);
				JsonString(s, len, out);
				return;
			}
			case LUA_TTABLE: break;
			default: luaL_error(L, "json.encode: a %s value has no JSON form", luaL_typename(L, index));
			}
			const void* self = lua_topointer(L, index);
			if (std::find(seen.begin(), seen.end(), self) != seen.end())
				luaL_error(L, "json.encode: a table contains itself");
			seen.push_back(self);
			const int n = lua_objlen(L, index);
			int count = 0;
			lua_pushnil(L);
			while (lua_next(L, index))
			{
				count++;
				lua_pop(L, 1);
			}
			const std::string colon = indent.empty() ? ":" : ": ";
			if (count == n) // an array (an empty table is `[]`)
			{
				out += '[';
				for (int i = 1; i <= n; i++)
				{
					if (i > 1)
						out += ',';
					Newline(out, indent, depth + 1);
					lua_rawgeti(L, index, i);
					JsonEncode(L, -1, indent, depth + 1, seen, out);
					lua_pop(L, 1);
				}
				if (n > 0)
					Newline(out, indent, depth);
				out += ']';
			}
			else
			{
				// Keys in sorted order: the output is the same every run.
				std::vector<std::pair<std::string, int>> keys; // key text, its type
				lua_pushnil(L);
				while (lua_next(L, index))
				{
					lua_pop(L, 1);
					const int type = lua_type(L, -1);
					if (type == LUA_TSTRING)
					{
						size_t len = 0;
						const char* s = lua_tolstring(L, -1, &len);
						keys.emplace_back(std::string(s, len), type);
					}
					else if (type == LUA_TNUMBER)
						keys.emplace_back(FormatNumber(lua_tonumber(L, -1)), type);
					else
						luaL_error(L, "json.encode: an object key is a %s (keys are strings or numbers)", luaL_typename(L, -1));
				}
				std::sort(keys.begin(), keys.end());
				out += '{';
				for (size_t k = 0; k < keys.size(); k++)
				{
					if (k > 0)
						out += ',';
					Newline(out, indent, depth + 1);
					JsonString(keys[k].first.data(), keys[k].first.size(), out);
					out += colon;
					if (keys[k].second == LUA_TSTRING)
						lua_pushlstring(L, keys[k].first.data(), keys[k].first.size());
					else
						lua_pushnumber(L, std::strtod(keys[k].first.c_str(), nullptr));
					lua_rawget(L, index);
					JsonEncode(L, -1, indent, depth + 1, seen, out);
					lua_pop(L, 1);
				}
				if (!keys.empty())
					Newline(out, indent, depth);
				out += '}';
			}
			seen.pop_back();
		}

		// json.encode(value, pretty?): `pretty` is true (two spaces) or an
		// indent string.
		int JsonEncodeFn(lua_State* L)
		{
			luaL_checkany(L, 1);
			std::string indent;
			if (lua_isstring(L, 2))
				indent = lua_tostring(L, 2);
			else if (lua_toboolean(L, 2))
				indent = "  ";
			std::vector<const void*> seen;
			std::string out;
			JsonEncode(L, 1, indent, 0, seen, out);
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		class JsonParser
		{
		public:
			JsonParser(lua_State* L, const char* text, size_t len)
				: m_L(L), m_Text(text), m_Len(len)
			{
			}

			void Parse()
			{
				Value(0);
				Space();
				if (m_Pos < m_Len)
					Error("unexpected text after the value");
			}

		private:
			[[noreturn]] void Error(const char* what)
			{
				int line = 1, col = 1;
				for (size_t i = 0; i < m_Pos && i < m_Len; i++)
				{
					if (m_Text[i] == '\n')
					{
						line++;
						col = 1;
					}
					else
						col++;
				}
				luaL_error(m_L, "json.decode: %s at line %d column %d", what, line, col);
				std::abort();
			}

			void Space()
			{
				while (m_Pos < m_Len && (m_Text[m_Pos] == ' ' || m_Text[m_Pos] == '\t' || m_Text[m_Pos] == '\n' || m_Text[m_Pos] == '\r'))
					m_Pos++;
			}

			bool Literal(const char* word)
			{
				const size_t n = std::strlen(word);
				if (m_Len - m_Pos >= n && std::memcmp(m_Text + m_Pos, word, n) == 0)
				{
					m_Pos += n;
					return true;
				}
				return false;
			}

			void Value(int depth)
			{
				if (depth > 200)
					Error("nesting too deep");
				lua_checkstack(m_L, 4);
				Space();
				if (m_Pos >= m_Len)
					Error("unexpected end of input");
				const char c = m_Text[m_Pos];
				if (c == '{')
					Object(depth);
				else if (c == '[')
					Array(depth);
				else if (c == '"')
				{
					const std::string s = String();
					lua_pushlstring(m_L, s.data(), s.size());
				}
				else if (Literal("true"))
					lua_pushboolean(m_L, 1);
				else if (Literal("false"))
					lua_pushboolean(m_L, 0);
				else if (Literal("null"))
					lua_pushnil(m_L);
				else if (c == '-' || (c >= '0' && c <= '9'))
					Number();
				else
					Error("unexpected character");
			}

			void Number()
			{
				const size_t start = m_Pos;
				if (m_Text[m_Pos] == '-')
					m_Pos++;
				while (m_Pos < m_Len && (std::isdigit((unsigned char)m_Text[m_Pos]) || m_Text[m_Pos] == '.' || m_Text[m_Pos] == 'e' ||
										   m_Text[m_Pos] == 'E' || m_Text[m_Pos] == '+' || m_Text[m_Pos] == '-'))
					m_Pos++;
				const std::string text(m_Text + start, m_Pos - start);
				char* end = nullptr;
				const double d = std::strtod(text.c_str(), &end);
				if (end != text.c_str() + text.size())
				{
					m_Pos = start;
					Error("malformed number");
				}
				lua_pushnumber(m_L, d);
			}

			unsigned Hex4()
			{
				if (m_Len - m_Pos < 4)
					Error("short \\u escape");
				unsigned v = 0;
				for (int i = 0; i < 4; i++)
				{
					const char h = m_Text[m_Pos++];
					v <<= 4;
					if (h >= '0' && h <= '9')
						v |= h - '0';
					else if (h >= 'a' && h <= 'f')
						v |= h - 'a' + 10;
					else if (h >= 'A' && h <= 'F')
						v |= h - 'A' + 10;
					else
						Error("bad \\u escape");
				}
				return v;
			}

			static void Utf8(unsigned cp, std::string& out)
			{
				if (cp < 0x80)
					out += (char)cp;
				else if (cp < 0x800)
				{
					out += (char)(0xC0 | (cp >> 6));
					out += (char)(0x80 | (cp & 0x3F));
				}
				else if (cp < 0x10000)
				{
					out += (char)(0xE0 | (cp >> 12));
					out += (char)(0x80 | ((cp >> 6) & 0x3F));
					out += (char)(0x80 | (cp & 0x3F));
				}
				else
				{
					out += (char)(0xF0 | (cp >> 18));
					out += (char)(0x80 | ((cp >> 12) & 0x3F));
					out += (char)(0x80 | ((cp >> 6) & 0x3F));
					out += (char)(0x80 | (cp & 0x3F));
				}
			}

			std::string String()
			{
				m_Pos++; // the opening quote
				std::string out;
				for (;;)
				{
					if (m_Pos >= m_Len)
						Error("unterminated string");
					const char c = m_Text[m_Pos++];
					if (c == '"')
						return out;
					if ((unsigned char)c < 0x20)
						Error("a control character in a string");
					if (c != '\\')
					{
						out += c;
						continue;
					}
					if (m_Pos >= m_Len)
						Error("unterminated string");
					const char e = m_Text[m_Pos++];
					switch (e)
					{
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					case '/': out += '/'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'u':
					{
						unsigned cp = Hex4();
						if (cp >= 0xD800 && cp <= 0xDBFF && m_Len - m_Pos >= 6 && m_Text[m_Pos] == '\\' && m_Text[m_Pos + 1] == 'u')
						{
							m_Pos += 2;
							const unsigned low = Hex4();
							if (low >= 0xDC00 && low <= 0xDFFF)
								cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
						}
						Utf8(cp, out);
						break;
					}
					default: Error("bad escape");
					}
				}
			}

			void Array(int depth)
			{
				m_Pos++;
				lua_newtable(m_L);
				Space();
				if (m_Pos < m_Len && m_Text[m_Pos] == ']')
				{
					m_Pos++;
					return;
				}
				for (int i = 1;; i++)
				{
					Value(depth + 1);
					lua_rawseti(m_L, -2, i);
					Space();
					if (m_Pos < m_Len && m_Text[m_Pos] == ',')
					{
						m_Pos++;
						continue;
					}
					if (m_Pos < m_Len && m_Text[m_Pos] == ']')
					{
						m_Pos++;
						return;
					}
					Error("expected ',' or ']'");
				}
			}

			void Object(int depth)
			{
				m_Pos++;
				lua_newtable(m_L);
				Space();
				if (m_Pos < m_Len && m_Text[m_Pos] == '}')
				{
					m_Pos++;
					return;
				}
				for (;;)
				{
					Space();
					if (m_Pos >= m_Len || m_Text[m_Pos] != '"')
						Error("expected a string key");
					const std::string key = String();
					Space();
					if (m_Pos >= m_Len || m_Text[m_Pos] != ':')
						Error("expected ':'");
					m_Pos++;
					lua_pushlstring(m_L, key.data(), key.size());
					Value(depth + 1);
					lua_rawset(m_L, -3);
					Space();
					if (m_Pos < m_Len && m_Text[m_Pos] == ',')
					{
						m_Pos++;
						continue;
					}
					if (m_Pos < m_Len && m_Text[m_Pos] == '}')
					{
						m_Pos++;
						return;
					}
					Error("expected ',' or '}'");
				}
			}

			lua_State* m_L;
			const char* m_Text;
			size_t m_Len;
			size_t m_Pos = 0;
		};

		int JsonDecodeFn(lua_State* L)
		{
			size_t len = 0;
			const char* text = luaL_checklstring(L, 1, &len);
			JsonParser(L, text, len).Parse();
			return 1;
		}

		const luaL_Reg kJson[] = {
			{ "encode", JsonEncodeFn }, { "decode", JsonDecodeFn }, { nullptr, nullptr },
		};

	} // namespace

	void OpenStd(lua_State* L, const std::vector<std::string>& args)
	{
		lua_newtable(L); // the registry's module table

		lua_newtable(L);
		SetFunctions(L, kFs);
		lua_setfield(L, -2, "fs");

		lua_newtable(L);
		SetFunctions(L, kPath);
		lua_setfield(L, -2, "path");

		lua_newtable(L);
		SetFunctions(L, kProcess);
		PushStrings(L, args);
		lua_setfield(L, -2, "args");
		lua_pushstring(L, Platform());
		lua_setfield(L, -2, "platform");
		lua_pushstring(L, Executable().c_str());
		lua_setfield(L, -2, "executable");
		lua_pushstring(L, "");
		lua_setfield(L, -2, "script");
		lua_setfield(L, -2, "process");

		lua_newtable(L);
		SetFunctions(L, kIo);
		lua_setfield(L, -2, "io");

		lua_newtable(L);
		SetFunctions(L, kJson);
		lua_setfield(L, -2, "json");

		lua_setfield(L, LUA_REGISTRYINDEX, kStdRegistry);

		lua_pushcfunction(L, Input, "input");
		lua_setglobal(L, "input");
	}

	void SetScript(lua_State* L, const std::string& script)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kStdRegistry);
		lua_getfield(L, -1, "process");
		lua_pushlstring(L, script.data(), script.size());
		lua_setfield(L, -2, "script");
		lua_pop(L, 2);
	}

	const char* StdNames() { return "@std/fs, @std/path, @std/process, @std/io, @std/json"; }

} // namespace cluau::runtime::detail
