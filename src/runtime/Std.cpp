// The standard modules: @std/fs, @std/path, @std/process, @std/io, @std/json,
// and the `input` global. Errors are Luau errors at the calling line
// (`fs.read: cannot read 'x': no such file`). Paths come back with `/` on
// every platform (Windows accepts it), so a script's output is the same
// everywhere.

#include "Std.h"
#include "Jobs.h"
#ifdef CLUAU_HAS_MBEDTLS
#include "Tls.h"
#endif
#include "Spawn.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <unistd.h>
extern char** environ;
#endif

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

		bool Awaits(lua_CFunction f);

		// The functions that may wait on a job (Await) get its continuation.
		void SetFunctions(lua_State* L, const luaL_Reg* functions)
		{
			for (; functions->name; functions++)
			{
				if (Awaits(functions->func))
					lua_pushcclosurek(L, functions->func, functions->name, 0, AwaitContinue);
				else
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
			// A directory under it that cannot be read is passed over (as
			// fs.glob does); only the starting one failing is an error.
			fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
			if (ec)
				Fail(L, "fs.walk", "cannot walk", dir, ec.message());
			for (fs::recursive_directory_iterator end; it != end;)
			{
				std::error_code fileEc;
				if (it->is_regular_file(fileEc))
					files.push_back(S(it->path()));
				it.increment(ec);
				if (ec)
				{
					ec.clear();
					if (it == end)
						break;
					it.pop(ec);
					if (ec)
						break;
				}
			}
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
			{
				fs::remove_all(path, ec);
				if (ec)
				{
					// Read-only files (Git's objects on Windows) refuse to go:
					// made writable, then removed again.
					std::error_code walk;
					for (fs::recursive_directory_iterator it(path, walk), end; !walk && it != end; it.increment(walk))
					{
						std::error_code ignored;
						fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ignored);
					}
					ec.clear();
					fs::remove_all(path, ec);
				}
			}
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

		// fs.realpath(path): the absolute path with every symbolic link and
		// `.`/`..` resolved; the path must exist.
		int FsRealpath(lua_State* L)
		{
			const fs::path path = P(L, 1);
			std::error_code ec;
			const fs::path real = fs::canonical(path, ec);
			if (ec)
				Fail(L, "fs.realpath", "cannot resolve", path, Reason(path, ec));
			PushPath(L, real);
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

		// ---- @std/fs: glob -----------------------------------------------

		// One path component against one pattern component: `*` (any run of
		// characters), `?` (one), `[abc]`, `[a-z]`, `[!abc]`. A name starting
		// with `.` matches only a pattern starting with `.`.
		bool MatchName(const std::string& pattern, const std::string& name)
		{
			if (!name.empty() && name[0] == '.' && (pattern.empty() || pattern[0] != '.'))
				return false;
			size_t p = 0, n = 0, starP = std::string::npos, starN = 0;
			while (n < name.size())
			{
				if (p < pattern.size() && pattern[p] == '*')
				{
					starP = p++;
					starN = n;
					continue;
				}
				if (p < pattern.size() && pattern[p] == '[')
				{
					size_t q = p + 1;
					const bool negate = q < pattern.size() && (pattern[q] == '!' || pattern[q] == '^');
					if (negate)
						q++;
					bool matched = false;
					size_t close = q;
					for (; close < pattern.size() && (close == q || pattern[close] != ']'); close++)
					{
						if (close + 2 < pattern.size() && pattern[close + 1] == '-' && pattern[close + 2] != ']')
						{
							matched = matched || (name[n] >= pattern[close] && name[n] <= pattern[close + 2]);
							close += 2;
						}
						else
							matched = matched || name[n] == pattern[close];
					}
					if (close >= pattern.size())
					{
						// No closing `]`: the `[` is an ordinary character.
						if (name[n] == '[')
						{
							p++;
							n++;
							continue;
						}
					}
					else if (matched != negate)
					{
						p = close + 1;
						n++;
						continue;
					}
				}
				else if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n]))
				{
					p++;
					n++;
					continue;
				}
				if (starP == std::string::npos)
					return false;
				p = starP + 1;
				n = ++starN;
			}
			while (p < pattern.size() && pattern[p] == '*')
				p++;
			return p == pattern.size();
		}

		bool HasWildcard(const std::string& part) { return part.find_first_of("*?[") != std::string::npos; }

		void GlobFrom(const fs::path& dir, const std::string& shown, const std::vector<std::string>& parts, size_t at, std::vector<std::string>& out)
		{
			auto join = [&](const std::string& name) { return shown.empty() ? name : shown == "/" ? "/" + name : shown + "/" + name; };
			std::error_code ec;
			if (at == parts.size())
			{
				if (fs::exists(dir, ec))
					out.push_back(shown.empty() ? "." : shown);
				return;
			}
			const std::string& part = parts[at];
			if (part == "**")
			{
				// Zero directories, then each directory below, at any depth.
				GlobFrom(dir, shown, parts, at + 1, out);
				for (fs::directory_iterator it(dir.empty() ? fs::path(".") : dir, ec), end; !ec && it != end; it.increment(ec))
				{
					const std::string name = S(it->path().filename());
					std::error_code dirEc;
					if (name[0] != '.' && it->is_directory(dirEc) && !it->is_symlink(dirEc))
						GlobFrom(it->path(), join(name), parts, at, out);
				}
				return;
			}
			if (!HasWildcard(part))
			{
				const fs::path next = dir.empty() ? fs::u8path(part) : dir / fs::u8path(part);
				if (at + 1 == parts.size() ? fs::exists(next, ec) : fs::is_directory(next, ec))
					GlobFrom(next, join(part), parts, at + 1, out);
				return;
			}
			for (fs::directory_iterator it(dir.empty() ? fs::path(".") : dir, ec), end; !ec && it != end; it.increment(ec))
			{
				const std::string name = S(it->path().filename());
				if (!MatchName(part, name))
					continue;
				std::error_code dirEc;
				if (at + 1 < parts.size() && !it->is_directory(dirEc))
					continue;
				GlobFrom(it->path(), join(name), parts, at + 1, out);
			}
		}

		// fs.glob(pattern): the paths that match, sorted. `**` is any number
		// of directories; `*`, `?` and `[...]` match within a name.
		int FsGlob(lua_State* L)
		{
			std::string pattern = luaL_checkstring(L, 1);
			std::replace(pattern.begin(), pattern.end(), '\\', '/');
			std::vector<std::string> parts;
			std::string shown;
			fs::path dir;
			size_t start = 0;
			if (!pattern.empty() && pattern[0] == '/')
			{
				dir = "/";
				shown = "/";
				start = 1;
			}
			else if (pattern.size() >= 3 && pattern[1] == ':' && pattern[2] == '/')
			{
				dir = fs::u8path(pattern.substr(0, 3));
				shown = pattern.substr(0, 2);
				start = 3;
			}
			for (size_t i = start; i <= pattern.size(); i++)
				if (i == pattern.size() || pattern[i] == '/')
				{
					if (i > start)
						parts.push_back(pattern.substr(start, i - start));
					start = i + 1;
				}
			std::vector<std::string> out;
			if (!parts.empty())
				GlobFrom(dir, shown == "/" ? "/" : shown, parts, 0, out);
			// A pattern ending in `/` matches only directories, shown with
			// the `/`.
			if (pattern.size() > 1 && pattern.back() == '/')
			{
				std::vector<std::string> dirs;
				for (const std::string& found : out)
				{
					std::error_code ec;
					if (fs::is_directory(fs::u8path(found), ec))
						dirs.push_back(found == "/" ? found : found + "/");
				}
				out = std::move(dirs);
			}
			std::sort(out.begin(), out.end());
			out.erase(std::unique(out.begin(), out.end()), out.end());
			PushStrings(L, out);
			return 1;
		}

		// ---- @std/fs: files ----------------------------------------------

		constexpr const char* kFileMeta = "cluau.File";

		struct FileBox
		{
			FILE* File = nullptr;
		};

		FILE* OpenFile(const fs::path& path, const char* mode)
		{
#ifdef _WIN32
			std::wstring wmode;
			for (const char* c = mode; *c; c++)
				wmode += (wchar_t)*c;
			return _wfopen(path.c_str(), wmode.c_str());
#else
			return std::fopen(path.c_str(), mode);
#endif
		}

		FILE* CheckFile(lua_State* L, int arg = 1)
		{
			FileBox* box = static_cast<FileBox*>(luaL_checkudata(L, arg, kFileMeta));
			if (!box->File)
				luaL_error(L, "the file is closed");
			return box->File;
		}

		// A line without its newline (nil at the end of the file).
		bool ReadFileLine(FILE* f, std::string& line)
		{
			line.clear();
			int c = 0;
			bool any = false;
			while ((c = std::fgetc(f)) != EOF)
			{
				any = true;
				if (c == '\n')
					break;
				line += (char)c;
			}
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			return any;
		}

		int FileRead(lua_State* L)
		{
			FILE* f = CheckFile(L);
			std::string out;
			if (lua_isnoneornil(L, 2))
			{
				char buffer[8192];
				size_t got = 0;
				while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
					out.append(buffer, got);
				lua_pushlstring(L, out.data(), out.size());
				return 1;
			}
			const double n = luaL_checknumber(L, 2);
			if (!(n >= 0) || n != std::floor(n))
				luaL_argerror(L, 2, "a whole number of bytes");
			// Read in pieces: a large count costs only what the file holds.
			char buffer[8192];
			const size_t want = n > 9e15 ? (size_t)9e15 : (size_t)n;
			while (out.size() < want)
			{
				const size_t piece = std::min<size_t>(want - out.size(), sizeof(buffer));
				const size_t got = std::fread(buffer, 1, piece, f);
				out.append(buffer, got);
				if (got < piece)
					break;
			}
			if (out.empty() && want > 0)
			{
				lua_pushnil(L);
				return 1;
			}
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		int FileReadLine(lua_State* L)
		{
			FILE* f = CheckFile(L);
			std::string line;
			if (!ReadFileLine(f, line))
				lua_pushnil(L);
			else
				lua_pushlstring(L, line.data(), line.size());
			return 1;
		}

		int FileLinesNext(lua_State* L)
		{
			FileBox* box = static_cast<FileBox*>(lua_touserdata(L, lua_upvalueindex(1)));
			const bool owned = lua_toboolean(L, lua_upvalueindex(2));
			std::string line;
			if (!box->File || !ReadFileLine(box->File, line))
			{
				if (owned && box->File)
				{
					std::fclose(box->File);
					box->File = nullptr;
				}
				lua_pushnil(L);
				return 1;
			}
			lua_pushlstring(L, line.data(), line.size());
			return 1;
		}

		int FileLines(lua_State* L)
		{
			CheckFile(L);
			lua_pushvalue(L, 1);
			lua_pushboolean(L, 0);
			lua_pushcclosure(L, FileLinesNext, "lines", 2);
			return 1;
		}

		int FileWrite(lua_State* L)
		{
			FILE* f = CheckFile(L);
			const int n = lua_gettop(L);
			for (int i = 2; i <= n; i++)
			{
				size_t len = 0;
				const char* text = luaL_checklstring(L, i, &len);
				if (std::fwrite(text, 1, len, f) != len)
					luaL_error(L, "file:write: the write failed");
			}
			return 0;
		}

		int FileSeek(lua_State* L)
		{
			FILE* f = CheckFile(L);
			const char* from = luaL_optstring(L, 3, "set");
			const int whence = std::strcmp(from, "set") == 0 ? SEEK_SET : std::strcmp(from, "current") == 0 ? SEEK_CUR
				: std::strcmp(from, "end") == 0 ? SEEK_END : -1;
			if (whence < 0)
				luaL_error(L, "file:seek: from is \"set\", \"current\" or \"end\"");
			if (!lua_isnoneornil(L, 2) && std::fseek(f, (long)luaL_checknumber(L, 2), whence) != 0)
				luaL_error(L, "file:seek: cannot seek there");
			lua_pushnumber(L, (double)std::ftell(f));
			return 1;
		}

		int FileFlush(lua_State* L)
		{
			std::fflush(CheckFile(L));
			return 0;
		}

		int FileClose(lua_State* L)
		{
			FileBox* box = static_cast<FileBox*>(luaL_checkudata(L, 1, kFileMeta));
			if (box->File)
			{
				std::fclose(box->File);
				box->File = nullptr;
			}
			return 0;
		}

		int FileToString(lua_State* L)
		{
			FileBox* box = static_cast<FileBox*>(luaL_checkudata(L, 1, kFileMeta));
			lua_pushstring(L, box->File ? "File" : "File (closed)");
			return 1;
		}

		void PushFile(lua_State* L, FILE* f)
		{
			FileBox* box = static_cast<FileBox*>(lua_newuserdatadtor(L, sizeof(FileBox), [](lua_State*, void* p) {
				FileBox* b = static_cast<FileBox*>(p);
				if (b->File)
					std::fclose(b->File);
			}));
			box->File = f;
			luaL_getmetatable(L, kFileMeta);
			lua_setmetatable(L, -2);
		}

		const luaL_Reg kFileMethods[] = {
			{ "read", FileRead }, { "readLine", FileReadLine }, { "lines", FileLines }, { "write", FileWrite },
			{ "seek", FileSeek }, { "flush", FileFlush }, { "close", FileClose }, { nullptr, nullptr },
		};

		void OpenFileType(lua_State* L)
		{
			luaL_newmetatable(L, kFileMeta);
			lua_newtable(L);
			SetFunctions(L, kFileMethods);
			lua_setfield(L, -2, "__index");
			lua_pushcfunction(L, FileToString, "tostring");
			lua_setfield(L, -2, "__tostring");
			lua_pushstring(L, "File");
			lua_setfield(L, -2, "__type");
			lua_pop(L, 1);
		}

		// fs.open(path, mode?): a file: "r" (the default) reads, "w" writes
		// from empty, "a" adds to the end. Bytes go through unchanged.
		int FsOpen(lua_State* L)
		{
			const fs::path path = P(L, 1);
			const std::string mode = luaL_optstring(L, 2, "r");
			const char* cmode = mode == "r" ? "rb" : mode == "w" ? "wb" : mode == "a" ? "ab" : nullptr;
			if (!cmode)
				luaL_error(L, "fs.open: the mode is \"r\", \"w\" or \"a\"");
			FILE* f = OpenFile(path, cmode);
			if (!f)
				Fail(L, "fs.open", "cannot open", path, mode == "r" ? Reason(path, {}) : "cannot create or write it");
			PushFile(L, f);
			return 1;
		}

		// fs.lines(path): the file's lines, one by one (the file closes at the end).
		int FsLines(lua_State* L)
		{
			const fs::path path = P(L, 1);
			FILE* f = OpenFile(path, "rb");
			if (!f)
				Fail(L, "fs.lines", "cannot read", path, Reason(path, {}));
			PushFile(L, f);
			lua_pushboolean(L, 1);
			lua_pushcclosure(L, FileLinesNext, "lines", 2);
			return 1;
		}

		int FsReadBuffer(lua_State* L)
		{
			const fs::path path = P(L, 1);
			std::ifstream in(path, std::ios::binary);
			std::error_code ec;
			if (!in || fs::is_directory(path, ec))
				Fail(L, "fs.readBuffer", "cannot read", path, Reason(path, {}));
			std::stringstream buffer;
			buffer << in.rdbuf();
			const std::string bytes = buffer.str();
			void* data = lua_newbuffer(L, bytes.size());
			if (!bytes.empty())
				std::memcpy(data, bytes.data(), bytes.size());
			return 1;
		}

		int FsWriteBuffer(lua_State* L)
		{
			const fs::path path = P(L, 1);
			size_t len = 0;
			void* data = lua_tobuffer(L, 2, &len);
			if (!data && len == 0 && lua_type(L, 2) != LUA_TBUFFER)
				luaL_typeerror(L, 2, "buffer");
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			if (!out)
				Fail(L, "fs.writeBuffer", "cannot write", path, "cannot create or write it");
			out.write(static_cast<const char*>(data), (std::streamsize)len);
			return 0;
		}

		// fs.tempFile(prefix?): a new, empty file of its own in the
		// temporary directory (its path).
		int FsTempFile(lua_State* L)
		{
			const std::string prefix = luaL_optstring(L, 1, "cluau");
			std::error_code ec;
			const fs::path dir = fs::temp_directory_path(ec);
#ifdef _WIN32
			for (int attempt = 0; attempt < 100; attempt++)
			{
				const fs::path path = dir / fs::u8path(prefix + "-" + std::to_string(GetCurrentProcessId()) + "-" +
					std::to_string(GetTickCount64()) + "-" + std::to_string(attempt));
				HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (h != INVALID_HANDLE_VALUE)
				{
					CloseHandle(h);
					PushPath(L, path);
					return 1;
				}
			}
			luaL_error(L, "fs.tempFile: cannot create a file in %s", S(dir).c_str());
#else
			std::string templ = S(dir / fs::u8path(prefix + "-XXXXXX"));
			const int fd = mkstemp(templ.data());
			if (fd < 0)
				luaL_error(L, "fs.tempFile: cannot create a file in %s: %s", S(dir).c_str(), std::strerror(errno));
			close(fd);
			lua_pushstring(L, templ.c_str());
#endif
			return 1;
		}

		const luaL_Reg kFs[] = {
			{ "read", FsRead }, { "write", FsWrite }, { "append", FsAppend }, { "exists", FsExists }, { "isFile", FsIsFile },
			{ "isDir", FsIsDir }, { "list", FsList }, { "walk", FsWalk }, { "mkdir", FsMkdir }, { "remove", FsRemove },
			{ "rename", FsRename }, { "copy", FsCopy }, { "size", FsSize }, { "modified", FsModified }, { "tempDir", FsTempDir },
			{ "glob", FsGlob }, { "open", FsOpen }, { "lines", FsLines }, { "readBuffer", FsReadBuffer }, { "writeBuffer", FsWriteBuffer },
			{ "tempFile", FsTempFile }, { "realpath", FsRealpath }, { nullptr, nullptr },
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

		void (*g_ExitHook)(int) = nullptr;

		int ProcessExit(lua_State* L)
		{
			const int code = (int)luaL_optinteger(L, 1, 0);
			std::fflush(stdout);
			std::fflush(stderr);
			if (g_ExitHook)
				g_ExitHook(code);
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

		// process.environment(): every environment variable ({ NAME = value }).
		int ProcessEnvironment(lua_State* L)
		{
			lua_newtable(L);
#ifdef _WIN32
			if (wchar_t* block = GetEnvironmentStringsW())
			{
				for (const wchar_t* p = block; *p; p += wcslen(p) + 1)
				{
					const std::wstring entry = p;
					const size_t eq = entry.find(L'=', 1);
					if (eq == std::wstring::npos)
						continue;
					const std::string name = S(fs::path(entry.substr(0, eq))), value = S(fs::path(entry.substr(eq + 1)));
					lua_pushlstring(L, value.data(), value.size());
					lua_setfield(L, -2, name.c_str());
				}
				FreeEnvironmentStringsW(block);
			}
#else
			for (char** e = environ; e && *e; e++)
			{
				const char* eq = std::strchr(*e, '=');
				if (!eq)
					continue;
				lua_pushlstring(L, *e, (size_t)(eq - *e));
				lua_pushstring(L, eq + 1);
				lua_rawset(L, -3);
			}
#endif
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
		// `textInput`: options.input is the child's standard input as text
		// (process.spawn's is a flag of its own).
		SpawnRequest Command(lua_State* L, const char* fn, bool run, bool textInput = true)
		{
			SpawnRequest request;
			if (lua_type(L, 1) == LUA_TSTRING)
			{
				size_t len = 0;
				const char* line = lua_tolstring(L, 1, &len);
				if (std::memchr(line, 0, len))
					luaL_error(L, "%s: the command line holds a NUL byte", fn);
				request.Shell = true;
				request.Args.push_back(std::string(line, len));
			}
			else if (lua_type(L, 1) == LUA_TTABLE)
			{
				const int n = lua_objlen(L, 1);
				for (int i = 1; i <= n; i++)
				{
					lua_rawgeti(L, 1, i);
					if (!lua_isstring(L, -1))
						luaL_error(L, "%s: argument %d of the command is a %s, not a string", fn, i, luaL_typename(L, -1));
					size_t len = 0;
					const char* arg = lua_tolstring(L, -1, &len);
					if (std::memchr(arg, 0, len))
						luaL_error(L, "%s: argument %d of the command holds a NUL byte (a program cannot be given one)", fn, i);
					request.Args.push_back(std::string(arg, len));
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
			if (textInput && !lua_isnil(L, -1))
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
			lua_getfield(L, 2, "timeout");
			if (!lua_isnil(L, -1))
			{
				if (!lua_isnumber(L, -1) || lua_tonumber(L, -1) <= 0)
					luaL_error(L, "%s: options.timeout is a number of seconds", fn);
				request.Timeout = lua_tonumber(L, -1);
			}
			lua_pop(L, 1);
			lua_getfield(L, 2, "stderr");
			if (!lua_isnil(L, -1))
			{
				const char* mode = lua_tostring(L, -1);
				if (!run)
					luaL_error(L, "%s: options.stderr is process.run's and process.spawn's (exec's output goes where the script's goes)", fn);
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

		// Runs the command (on a job's thread); the result pushes `push`'s
		// values, or raises that it could not start.
		JobWork RunCommand(const char* fn, SpawnRequest request, std::function<int(lua_State*, const SpawnResult&)> push)
		{
			return [fn, request = std::move(request), push = std::move(push)]() -> JobResult {
				auto result = std::make_shared<SpawnResult>();
				std::string why;
				const bool started = Spawn(request, *result, why);
				const std::string what = request.Shell ? std::string("the shell") : "'" + request.Args[0] + "'";
				return [fn, started, why, what, result, push](lua_State* L) -> int {
					if (!started)
						luaL_error(L, "%s: cannot start %s: %s", fn, what.c_str(), why.c_str());
					return push(L, *result);
				};
			};
		}

		// process.run(command, options?): { code, output, error } - the
		// command's standard output captured (and its standard error, with
		// stderr = "capture").
		int ProcessRun(lua_State* L)
		{
			SpawnRequest request = Command(L, "process.run", true);
			request.CaptureOutput = true;
			return Await(L, RunCommand("process.run", std::move(request), [](lua_State* L, const SpawnResult& result) {
				lua_createtable(L, 0, 3);
				lua_pushinteger(L, result.Code);
				lua_setfield(L, -2, "code");
				lua_pushlstring(L, result.Output.data(), result.Output.size());
				lua_setfield(L, -2, "output");
				lua_pushlstring(L, result.Error.data(), result.Error.size());
				lua_setfield(L, -2, "error");
				lua_pushboolean(L, result.TimedOut);
				lua_setfield(L, -2, "timedOut");
				return 1;
			}));
		}

		// process.exec(command, options?): the exit code, the command's
		// output going where the script's goes.
		int ProcessExec(lua_State* L)
		{
			SpawnRequest request = Command(L, "process.exec", false);
			return Await(L, RunCommand("process.exec", std::move(request), [](lua_State* L, const SpawnResult& result) {
				lua_pushinteger(L, result.Code);
				lua_pushboolean(L, result.TimedOut);
				return 2;
			}));
		}

		// ---- process.spawn -------------------------------------------------

		constexpr const char* kChildMeta = "cluau.Child";

		// A child is shared with the jobs reading it or waiting for it (a
		// task's read outlives nothing it needs).
		struct ChildBox
		{
			std::shared_ptr<Child>* Process = nullptr;
		};

		std::shared_ptr<Child> SharedChild(lua_State* L, int index)
		{
			ChildBox* box = static_cast<ChildBox*>(luaL_checkudata(L, index, kChildMeta));
			return *box->Process;
		}

		Child& CheckChild(lua_State* L) { return *SharedChild(L, 1); }

		// A read (a chunk, or a line) on a job's thread: the text, or nil at the end.
		int AwaitRead(lua_State* L, std::shared_ptr<Child> child, bool line)
		{
			return Await(L, [child, line]() -> JobResult {
				auto text = std::make_shared<std::string>();
				const bool got = line ? child->ReadLine(*text) : child->Read(*text);
				return [text, got](lua_State* L) {
					if (!got)
						lua_pushnil(L);
					else
						lua_pushlstring(L, text->data(), text->size());
					return 1;
				};
			});
		}

		int ChildRead(lua_State* L) { return AwaitRead(L, SharedChild(L, 1), false); }

		int ChildReadLine(lua_State* L) { return AwaitRead(L, SharedChild(L, 1), true); }

		int ChildLinesNext(lua_State* L) { return AwaitRead(L, SharedChild(L, lua_upvalueindex(1)), true); }

		int ChildLines(lua_State* L)
		{
			CheckChild(L);
			lua_pushvalue(L, 1);
			lua_pushcclosurek(L, ChildLinesNext, "lines", 1, AwaitContinue);
			return 1;
		}

		// A job: a child that is slow to read (or a task reading its output
		// meanwhile) must not stop the script.
		int ChildWrite(lua_State* L)
		{
			std::shared_ptr<Child> child = SharedChild(L, 1);
			std::string text;
			const int n = lua_gettop(L);
			for (int i = 2; i <= n; i++)
			{
				size_t len = 0;
				const char* part = luaL_checklstring(L, i, &len);
				text.append(part, len);
			}
			return Await(L, [child, text = std::move(text)]() -> JobResult {
				std::string why;
				const bool ok = child->Write(text, why);
				return [ok, why](lua_State* L) {
					if (!ok)
						luaL_error(L, "child:write: %s", why.c_str());
					return 0;
				};
			});
		}

		int ChildCloseInput(lua_State* L)
		{
			CheckChild(L).CloseInput();
			return 0;
		}

		int ChildWait(lua_State* L)
		{
			std::shared_ptr<Child> child = SharedChild(L, 1);
			return Await(L, [child]() -> JobResult {
				const int code = child->Wait();
				return [code](lua_State* L) {
					lua_pushinteger(L, code);
					return 1;
				};
			});
		}

		int ChildKill(lua_State* L)
		{
			CheckChild(L).Kill();
			return 0;
		}

		int ChildId(lua_State* L)
		{
			lua_pushnumber(L, (double)CheckChild(L).Id());
			return 1;
		}

		const luaL_Reg kChildMethods[] = {
			{ "read", ChildRead }, { "readLine", ChildReadLine }, { "lines", ChildLines }, { "write", ChildWrite },
			{ "closeInput", ChildCloseInput }, { "wait", ChildWait }, { "kill", ChildKill }, { "id", ChildId }, { nullptr, nullptr },
		};

		void OpenChildType(lua_State* L)
		{
			luaL_newmetatable(L, kChildMeta);
			lua_newtable(L);
			SetFunctions(L, kChildMethods);
			lua_setfield(L, -2, "__index");
			lua_pushstring(L, "Child");
			lua_setfield(L, -2, "__type");
			lua_pop(L, 1);
		}

		// process.spawn(command, options?): a child running alongside the
		// script. Options: cwd, env, stderr = "inherit" | "merge", and
		// input = true to write its standard input with child:write.
		int ProcessSpawn(lua_State* L)
		{
			bool pipeInput = false;
			if (lua_istable(L, 2))
			{
				lua_getfield(L, 2, "input");
				pipeInput = lua_toboolean(L, -1);
				const bool text = lua_isstring(L, -1);
				lua_pop(L, 1);
				if (text)
					luaL_error(L, "process.spawn: options.input is true (then child:write sends it text), not the text itself");
			}
			SpawnRequest request = Command(L, "process.spawn", true, false);
			if (request.Error == Stream::Capture)
				luaL_error(L, "process.spawn: options.stderr is \"inherit\" or \"merge\" (capturing it apart could stall the child)");
			if (request.Timeout > 0)
				luaL_error(L, "process.spawn: no timeout (child:kill() ends it)");
			std::string why;
			std::unique_ptr<Child> child = Child::Start(request, pipeInput, why);
			if (!child)
			{
				const std::string what = request.Shell ? std::string("the shell") : "'" + request.Args[0] + "'";
				luaL_error(L, "process.spawn: cannot start %s: %s", what.c_str(), why.c_str());
			}
			ChildBox* box = static_cast<ChildBox*>(lua_newuserdatadtor(L, sizeof(ChildBox), [](lua_State*, void* p) {
				delete static_cast<ChildBox*>(p)->Process;
			}));
			box->Process = new std::shared_ptr<Child>(std::move(child));
			luaL_getmetatable(L, kChildMeta);
			lua_setmetatable(L, -2);
			return 1;
		}

		int ProcessSleep(lua_State* L)
		{
			const double seconds = luaL_checknumber(L, 1);
			if (seconds <= 0)
				return 0;
			return Await(L, [seconds]() -> JobResult {
				std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
				return [](lua_State*) { return 0; };
			});
		}

		const luaL_Reg kProcess[] = {
			{ "exit", ProcessExit }, { "env", ProcessEnv }, { "setEnv", ProcessSetEnv }, { "environment", ProcessEnvironment }, { "cwd", ProcessCwd },
			{ "chdir", ProcessChdir }, { "run", ProcessRun }, { "exec", ProcessExec }, { "spawn", ProcessSpawn }, { "sleep", ProcessSleep },
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

		// Standard input is read on a job's thread, one read at a time.
		std::mutex g_Reading;

		int IoReadLine(lua_State* L)
		{
			std::fflush(stdout);
			return Await(L, []() -> JobResult {
				std::lock_guard<std::mutex> reading(g_Reading);
				auto line = std::make_shared<std::string>();
				const bool got = (bool)std::getline(std::cin, *line);
				if (got && !line->empty() && line->back() == '\r')
					line->pop_back();
				return [line, got](lua_State* L) {
					if (!got)
					{
						RaiseIfInterrupted(L);
						lua_pushnil(L);
					}
					else
						lua_pushlstring(L, line->data(), line->size());
					return 1;
				};
			});
		}

		int IoReadAll(lua_State* L)
		{
			std::fflush(stdout);
			return Await(L, []() -> JobResult {
				std::lock_guard<std::mutex> reading(g_Reading);
				std::stringstream buffer;
				buffer << std::cin.rdbuf();
				auto text = std::make_shared<std::string>(buffer.str());
				return [text](lua_State* L) {
					RaiseIfInterrupted(L);
					lua_pushlstring(L, text->data(), text->size());
					return 1;
				};
			});
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

		// ---- @std/time -----------------------------------------------------
		//
		// Times are numbers: seconds since 1970-01-01T00:00:00Z, with a
		// fraction. Options tables take `utc = true` (the default is the
		// system's local time zone).

		bool UtcOption(lua_State* L, int arg)
		{
			if (lua_isnoneornil(L, arg))
				return false;
			luaL_checktype(L, arg, LUA_TTABLE);
			lua_getfield(L, arg, "utc");
			const bool utc = lua_toboolean(L, -1);
			lua_pop(L, 1);
			return utc;
		}

		double Now()
		{
			return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
		}

		std::tm Breakdown(double t, bool utc)
		{
			const std::time_t whole = (std::time_t)std::floor(t);
			std::tm out{};
#ifdef _WIN32
			utc ? gmtime_s(&out, &whole) : localtime_s(&out, &whole);
#else
			utc ? gmtime_r(&whole, &out) : localtime_r(&whole, &out);
#endif
			return out;
		}

		// Seconds since 1970 for calendar fields, read as UTC or local time.
		double Compose(std::tm fields, bool utc)
		{
			fields.tm_isdst = -1;
#ifdef _WIN32
			return (double)(utc ? _mkgmtime(&fields) : std::mktime(&fields));
#else
			return (double)(utc ? timegm(&fields) : std::mktime(&fields));
#endif
		}

		// The local time zone's offset from UTC at `t`, in seconds east.
		int LocalOffset(double t)
		{
			std::tm local = Breakdown(t, false);
			return (int)(Compose(local, true) - std::floor(t));
		}

		int TimeNow(lua_State* L)
		{
			lua_pushnumber(L, Now());
			return 1;
		}

		int TimeMonotonic(lua_State* L)
		{
			lua_pushnumber(L, std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
			return 1;
		}

		// time.iso(t?, options?): "2026-11-24T21:05:09.250+01:00" (UTC: "...Z").
		int TimeIso(lua_State* L)
		{
			const double t = luaL_optnumber(L, 1, Now());
			const bool utc = UtcOption(L, 2);
			const std::tm tm = Breakdown(t, utc);
			char buf[64];
			int n = std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
				tm.tm_min, tm.tm_sec);
			const int ms = (int)std::floor((t - std::floor(t)) * 1000.0 + 1e-6);
			if (ms > 0)
				n += std::snprintf(buf + n, sizeof(buf) - n, ".%03d", ms);
			if (utc)
				std::snprintf(buf + n, sizeof(buf) - n, "Z");
			else
			{
				const int offset = LocalOffset(t);
				const int a = std::abs(offset) / 60;
				std::snprintf(buf + n, sizeof(buf) - n, "%c%02d:%02d", offset < 0 ? '-' : '+', a / 60, a % 60);
			}
			lua_pushstring(L, buf);
			return 1;
		}

		// time.parse(text): an ISO 8601 date or date and time. A date alone is
		// UTC midnight; a time without `Z` or an offset is local time.
		int TimeParse(lua_State* L)
		{
			const std::string text = luaL_checkstring(L, 1);
			size_t at = 0;
			auto fail = [&](const char* what) { luaL_error(L, "time.parse: %s in '%s'", what, text.c_str()); };
			auto digits = [&](int count) {
				int value = 0;
				for (int i = 0; i < count; i++, at++)
				{
					if (at >= text.size() || !std::isdigit((unsigned char)text[at]))
						fail("expected a digit");
					value = value * 10 + (text[at] - '0');
				}
				return value;
			};
			auto expect = [&](char c) {
				if (at >= text.size() || text[at] != c)
				{
					const char what[] = { 'e', 'x', 'p', 'e', 'c', 't', 'e', 'd', ' ', '\'', c, '\'', 0 };
					fail(what);
				}
				at++;
			};
			std::tm tm{};
			tm.tm_year = digits(4) - 1900;
			expect('-');
			tm.tm_mon = digits(2) - 1;
			expect('-');
			tm.tm_mday = digits(2);
			if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31)
				fail("the month or the day is out of range");
			{
				// The day exists in that month (February 29th in leap years only).
				const int year = tm.tm_year + 1900;
				const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
				const int days[] = { 31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
				if (tm.tm_mday > days[tm.tm_mon])
					fail("the day is not in that month");
			}
			double fraction = 0;
			bool hasTime = false, hasOffset = false;
			int offset = 0;
			if (at < text.size() && (text[at] == 'T' || text[at] == 't' || text[at] == ' '))
			{
				at++;
				hasTime = true;
				tm.tm_hour = digits(2);
				expect(':');
				tm.tm_min = digits(2);
				if (at < text.size() && text[at] == ':')
				{
					at++;
					tm.tm_sec = digits(2);
					if (at < text.size() && (text[at] == '.' || text[at] == ','))
					{
						at++;
						double scale = 0.1;
						if (at >= text.size() || !std::isdigit((unsigned char)text[at]))
							fail("expected the fraction's digits");
						while (at < text.size() && std::isdigit((unsigned char)text[at]))
						{
							fraction += (text[at++] - '0') * scale;
							scale /= 10;
						}
					}
				}
				if (tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 60)
					fail("the time is out of range");
				if (at < text.size() && (text[at] == 'Z' || text[at] == 'z'))
				{
					at++;
					hasOffset = true;
				}
				else if (at < text.size() && (text[at] == '+' || text[at] == '-'))
				{
					const int sign = text[at++] == '-' ? -1 : 1;
					const int hours = digits(2);
					int minutes = 0;
					if (at < text.size() && text[at] == ':')
						at++;
					if (at < text.size() && std::isdigit((unsigned char)text[at]))
						minutes = digits(2);
					offset = sign * (hours * 3600 + minutes * 60);
					hasOffset = true;
				}
			}
			if (at != text.size())
				fail("unexpected text after the time");
			const bool utc = !hasTime || hasOffset;
			lua_pushnumber(L, Compose(tm, utc) - offset + fraction);
			return 1;
		}

		// time.format(t, pattern, options?): strftime's %Y-%m-%d %H:%M:%S ...
		int TimeFormat(lua_State* L)
		{
			const double t = luaL_checknumber(L, 1);
			const char* pattern = luaL_checkstring(L, 2);
			const std::tm tm = Breakdown(t, UtcOption(L, 3));
			std::vector<char> buf(256);
			for (;;)
			{
				const size_t n = std::strftime(buf.data(), buf.size(), pattern, &tm);
				if (n > 0 || *pattern == '\0' || buf.size() > 65536)
				{
					lua_pushlstring(L, buf.data(), n);
					return 1;
				}
				buf.resize(buf.size() * 4);
			}
		}

		void SetNumber(lua_State* L, const char* name, double value)
		{
			lua_pushnumber(L, value);
			lua_setfield(L, -2, name);
		}

		// time.parts(t?, options?): { year, month, day, hour, minute, second,
		// millisecond, weekday (1 = Monday), yearday, offset (seconds east of UTC) }.
		int TimeParts(lua_State* L)
		{
			const double t = luaL_optnumber(L, 1, Now());
			const bool utc = UtcOption(L, 2);
			const std::tm tm = Breakdown(t, utc);
			lua_createtable(L, 0, 10);
			SetNumber(L, "year", tm.tm_year + 1900);
			SetNumber(L, "month", tm.tm_mon + 1);
			SetNumber(L, "day", tm.tm_mday);
			SetNumber(L, "hour", tm.tm_hour);
			SetNumber(L, "minute", tm.tm_min);
			SetNumber(L, "second", tm.tm_sec);
			SetNumber(L, "millisecond", std::floor((t - std::floor(t)) * 1000.0 + 1e-6));
			SetNumber(L, "weekday", tm.tm_wday == 0 ? 7 : tm.tm_wday);
			SetNumber(L, "yearday", tm.tm_yday + 1);
			SetNumber(L, "offset", utc ? 0 : LocalOffset(t));
			return 1;
		}

		int Field(lua_State* L, int table, const char* name, int fallback)
		{
			lua_getfield(L, table, name);
			if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TNUMBER)
				luaL_error(L, "time.fromParts: parts.%s is a number (got %s)", name, luaL_typename(L, -1));
			const int value = lua_isnil(L, -1) ? fallback : (int)lua_tonumber(L, -1);
			lua_pop(L, 1);
			return value;
		}

		// time.fromParts(parts, options?): the time those calendar fields name
		// (missing ones: the first month, day, hour ...). Out-of-range fields
		// carry over: day 32 is the next month's first.
		int TimeFromParts(lua_State* L)
		{
			luaL_checktype(L, 1, LUA_TTABLE);
			std::tm tm{};
			lua_getfield(L, 1, "year");
			if (lua_isnil(L, -1))
				luaL_error(L, "time.fromParts: the parts need a year");
			lua_pop(L, 1);
			tm.tm_year = Field(L, 1, "year", 1970) - 1900;
			tm.tm_mon = Field(L, 1, "month", 1) - 1;
			tm.tm_mday = Field(L, 1, "day", 1);
			tm.tm_hour = Field(L, 1, "hour", 0);
			tm.tm_min = Field(L, 1, "minute", 0);
			tm.tm_sec = Field(L, 1, "second", 0);
			const int ms = Field(L, 1, "millisecond", 0);
			lua_pushnumber(L, Compose(tm, UtcOption(L, 2)) + ms / 1000.0);
			return 1;
		}

		const luaL_Reg kTime[] = {
			{ "now", TimeNow }, { "monotonic", TimeMonotonic }, { "iso", TimeIso }, { "parse", TimeParse }, { "format", TimeFormat },
			{ "parts", TimeParts }, { "fromParts", TimeFromParts }, { nullptr, nullptr },
		};

		// ---- @std/json ------------------------------------------------------

		// json.null (a value of its own, for JSON's null where nil would drop
		// the key), and the markers json.object / json.array put on a table.
		char g_JsonNull = 0;
		constexpr const char* kJsonObject = "cluau.json.object";
		constexpr const char* kJsonArray = "cluau.json.array";

		bool IsJsonNull(lua_State* L, int index) { return lua_islightuserdata(L, index) && lua_tolightuserdata(L, index) == &g_JsonNull; }

		// The marker on the table at `index`: 'o', 'a' or 0.
		char JsonMark(lua_State* L, int index)
		{
			if (!lua_getmetatable(L, index))
				return 0;
			luaL_getmetatable(L, kJsonObject);
			const bool object = lua_rawequal(L, -1, -2);
			lua_pop(L, 1);
			luaL_getmetatable(L, kJsonArray);
			const bool array = lua_rawequal(L, -1, -2);
			lua_pop(L, 2);
			return object ? 'o' : array ? 'a' : 0;
		}

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
			case LUA_TLIGHTUSERDATA:
				if (IsJsonNull(L, index))
				{
					out += "null";
					return;
				}
				[[fallthrough]];
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
			// An array: a table whose keys are 1..n (an empty one is `[]`),
			// unless json.object marks it; json.array marks one as an array.
			const char mark = JsonMark(L, index);
			if (mark == 'a' && count != n)
				luaL_error(L, "json.encode: a table json.array marks has keys other than 1..n");
			if (mark != 'o' && count == n)
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
				for (size_t k = 1; k < keys.size(); k++)
					if (keys[k].first == keys[k - 1].first)
						luaL_error(L, "json.encode: the keys %s and \"%s\" are the same JSON key", keys[k].first.c_str(), keys[k].first.c_str());
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

	} // namespace

	std::string JsonEncodeValue(lua_State* L, int index)
	{
		std::vector<const void*> seen;
		std::string out;
		JsonEncode(L, lua_absindex(L, index), "", 0, seen, out);
		return out;
	}

	namespace {

		// json.encode(value, pretty?): `pretty` is true (two spaces) or an
		// indent string.
		int JsonEncodeFn(lua_State* L)
		{
			luaL_checkany(L, 1);
			// `pretty`: true (two spaces), a number of spaces, or the indent itself.
			std::string indent;
			if (lua_type(L, 2) == LUA_TNUMBER)
			{
				const double spaces = lua_tonumber(L, 2);
				if (spaces < 0 || spaces > 16 || spaces != std::floor(spaces))
					luaL_argerror(L, 2, "a number of spaces from 0 to 16");
				indent.assign((size_t)spaces, ' ');
			}
			else if (lua_type(L, 2) == LUA_TSTRING)
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
			// `null`: the stack index of the value JSON's null decodes to (0: nil).
			JsonParser(lua_State* L, const char* text, size_t len, int null)
				: m_L(L), m_Text(text), m_Len(len), m_Null(null)
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
				{
					if (m_Null)
						lua_pushvalue(m_L, m_Null);
					else
						lua_pushnil(m_L);
				}
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
				// JSON's own form: no leading zero (01), and digits on both sides
				// of a point (1., .5); strtod alone takes more.
				const size_t digits = text[0] == '-' ? 1 : 0;
				const size_t point = text.find_first_of(".eE");
				const bool leadingZero = text.size() > digits + 1 && text[digits] == '0' && std::isdigit((unsigned char)text[digits + 1]);
				const bool badPoint = point != std::string::npos && text[point] == '.' &&
					(point == digits || point + 1 >= text.size() || !std::isdigit((unsigned char)text[point + 1]));
				char* end = nullptr;
				const double d = std::strtod(text.c_str(), &end);
				if (end != text.c_str() + text.size() || leadingZero || badPoint || text.size() == digits || !std::isdigit((unsigned char)text[digits]))
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
							const size_t at = m_Pos;
							m_Pos += 2;
							const unsigned low = Hex4();
							if (low >= 0xDC00 && low <= 0xDFFF)
								cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
							else
								m_Pos = at; // not its pair: the next escape is read on its own
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
					// An empty object stays an object when encoded again.
					luaL_getmetatable(m_L, kJsonObject);
					lua_setmetatable(m_L, -2);
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
			int m_Null = 0;
		};

		// json.decode(text, options?): `options.null` is what JSON's null
		// becomes (nil by default; json.null keeps the key).
		int JsonDecodeFn(lua_State* L)
		{
			size_t len = 0;
			const char* text = luaL_checklstring(L, 1, &len);
			int null = 0;
			if (!lua_isnoneornil(L, 2))
			{
				luaL_checktype(L, 2, LUA_TTABLE);
				lua_getfield(L, 2, "null");
				null = lua_isnil(L, -1) ? 0 : lua_gettop(L);
				if (!null)
					lua_pop(L, 1);
			}
			JsonParser(L, text, len, null).Parse();
			return 1;
		}

		int JsonMarkFn(lua_State* L, const char* marker)
		{
			if (lua_isnoneornil(L, 1))
			{
				lua_settop(L, 0);
				lua_newtable(L);
			}
			luaL_checktype(L, 1, LUA_TTABLE);
			lua_settop(L, 1);
			luaL_getmetatable(L, marker);
			lua_setmetatable(L, 1);
			return 1;
		}

		// json.object(t?) / json.array(t?): `t` (or a new table) marked to
		// encode as an object / an array, an empty one included.
		int JsonObjectFn(lua_State* L) { return JsonMarkFn(L, kJsonObject); }
		int JsonArrayFn(lua_State* L) { return JsonMarkFn(L, kJsonArray); }

		const luaL_Reg kJson[] = {
			{ "encode", JsonEncodeFn }, { "decode", JsonDecodeFn }, { "object", JsonObjectFn }, { "array", JsonArrayFn }, { nullptr, nullptr },
		};

		bool Awaits(lua_CFunction f)
		{
			for (lua_CFunction awaiting : { ProcessRun, ProcessExec, ProcessSleep, ChildRead, ChildReadLine, ChildWait, IoReadLine, IoReadAll })
				if (f == awaiting)
					return true;
			return false;
		}

	} // namespace

	bool SandboxLeavesOut(const std::string& name)
	{
		return name == "fs" || name == "http" || name == "net" || name == "sqlite" || name == "zip" || name == "tar" || name == "_packages";
	}

	void OpenStd(lua_State* L, const std::vector<std::string>& args, bool sandbox)
	{
		OpenFileType(L);
		OpenChildType(L);
		lua_newtable(L); // the registry's module table

		if (!sandbox)
		{
			lua_newtable(L);
			SetFunctions(L, kFs);
			lua_setfield(L, -2, "fs");
		}

		lua_newtable(L);
		SetFunctions(L, kPath);
		lua_setfield(L, -2, "path");

		lua_newtable(L);
		if (sandbox)
		{
			const luaL_Reg kSandboxProcess[] = { { "exit", ProcessExit }, { "sleep", ProcessSleep }, { nullptr, nullptr } };
			SetFunctions(L, kSandboxProcess);
		}
		else
		{
			SetFunctions(L, kProcess);
			lua_pushstring(L, Executable().c_str());
			lua_setfield(L, -2, "executable");
		}
		PushStrings(L, args);
		lua_setfield(L, -2, "args");
		lua_pushstring(L, Platform());
		lua_setfield(L, -2, "platform");
		lua_pushstring(L, "");
		lua_setfield(L, -2, "script");
		lua_setfield(L, -2, "process");

		lua_newtable(L);
		SetFunctions(L, kIo);
		lua_setfield(L, -2, "io");

		luaL_newmetatable(L, kJsonObject);
		lua_pop(L, 1);
		luaL_newmetatable(L, kJsonArray);
		lua_pop(L, 1);
		lua_newtable(L);
		SetFunctions(L, kJson);
		lua_pushlightuserdata(L, &g_JsonNull);
		lua_setfield(L, -2, "null");
		lua_setfield(L, -2, "json");

		lua_newtable(L);
		SetFunctions(L, kTime);
		lua_setfield(L, -2, "time");

#ifdef CLUAU_HAS_HTTP
		if (!sandbox)
		{
			OpenHttp(L);
			lua_setfield(L, -2, "http");
		}
#endif
#ifdef CLUAU_HAS_REGEX
		OpenRegex(L);
		lua_setfield(L, -2, "regex");
#endif
		if (!sandbox)
		{
			OpenNet(L);
			lua_setfield(L, -2, "net");
		}
#ifdef CLUAU_HAS_SQLITE
		if (!sandbox)
		{
			OpenSqlite(L);
			lua_setfield(L, -2, "sqlite");
		}
#endif
#ifdef CLUAU_HAS_CRYPTO
		OpenCrypto(L);
		lua_setfield(L, -2, "crypto");
#endif
		OpenEncoding(L);
		lua_setfield(L, -2, "encoding");
		OpenTerm(L);
		lua_setfield(L, -2, "term");
#ifdef CLUAU_HAS_COMPRESS
		OpenCompress(L);
		lua_setfield(L, -2, "compress");
		if (!sandbox)
		{
			OpenZip(L);
			lua_setfield(L, -2, "zip");
		}
#endif

		OpenJobs(L);
		lua_setfield(L, -2, "_jobs");

		lua_setfield(L, LUA_REGISTRYINDEX, kStdRegistry);

		lua_pushcclosurek(L, Input, "input", 0, AwaitContinue);
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

	const char* StdNames()
	{
		static const std::string names = std::string("@std/fs, @std/path, @std/process, @std/io, @std/json, @std/time, @std/encoding, @std/term, @std/url")
#ifdef CLUAU_HAS_CRYPTO
										 + ", @std/crypto"
#endif
#ifdef CLUAU_HAS_COMPRESS
										 + ", @std/compress, @std/zip"
#endif
#ifdef CLUAU_HAS_HTTP
										 + ", @std/http"
#endif
#ifdef CLUAU_HAS_REGEX
										 + ", @std/regex"
#endif
										 + ", @std/net"
#ifdef CLUAU_HAS_SQLITE
										 + ", @std/sqlite"
#endif
										 + ", @std/server, @std/websocket, @std/tar, @std/task, @std/args, @std/csv, @std/test";
		return names.c_str();
	}

	void SetExitHook(void (*hook)(int code)) { g_ExitHook = hook; }

} // namespace cluau::runtime::detail

namespace cluau::runtime {

	const char* StandardModules() { return detail::StdNames(); }

} // namespace cluau::runtime
