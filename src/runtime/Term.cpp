// @std/term: the terminal: whether a stream is one, its size, and text in
// colours and styles (ANSI escape sequences) that are left out when the
// output is not a terminal, or NO_COLOR is set (FORCE_COLOR keeps them).
// On Windows the console is switched to escape sequences on first use.

#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#define CLUAU_ISATTY(fd) _isatty(fd)
#else
#include <sys/ioctl.h>
#include <unistd.h>
#define CLUAU_ISATTY(fd) isatty(fd)
#endif

namespace cluau::runtime::detail {

	namespace {

		int StreamNumber(lua_State* L, int index)
		{
			const char* name = luaL_optstring(L, index, "stdout");
			if (std::strcmp(name, "stdout") == 0)
				return 1;
			if (std::strcmp(name, "stderr") == 0)
				return 2;
			if (std::strcmp(name, "stdin") == 0)
				return 0;
			luaL_error(L, "term: the stream is \"stdout\", \"stderr\" or \"stdin\", not '%s'", name);
			return 1;
		}

		bool Terminal(int fd)
		{
			return CLUAU_ISATTY(fd) != 0;
		}

		// Escape sequences on (Windows' console takes them once told).
		bool Colors(int fd)
		{
			if (const char* force = std::getenv("FORCE_COLOR"); force && *force && std::strcmp(force, "0") != 0)
				return true;
			if (const char* none = std::getenv("NO_COLOR"); none && *none)
				return false;
			if (!Terminal(fd))
				return false;
			if (const char* term = std::getenv("TERM"); term && std::strcmp(term, "dumb") == 0)
				return false;
#ifdef _WIN32
			HANDLE handle = GetStdHandle(fd == 2 ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
			DWORD mode = 0;
			if (!GetConsoleMode(handle, &mode))
				return false;
			if (!(mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) && !SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
				return false;
#endif
			return true;
		}

		int TermIsTerminal(lua_State* L)
		{
			lua_pushboolean(L, Terminal(StreamNumber(L, 1)));
			return 1;
		}

		int TermColors(lua_State* L)
		{
			lua_pushboolean(L, Colors(StreamNumber(L, 1)));
			return 1;
		}

		// { columns, rows } of the terminal standard output (or stderr) is; nil
		// when neither is a terminal.
		int TermSize(lua_State* L)
		{
			int columns = 0, rows = 0;
#ifdef _WIN32
			CONSOLE_SCREEN_BUFFER_INFO info;
			for (DWORD which : { STD_OUTPUT_HANDLE, STD_ERROR_HANDLE })
				if (GetConsoleScreenBufferInfo(GetStdHandle(which), &info))
				{
					columns = info.srWindow.Right - info.srWindow.Left + 1;
					rows = info.srWindow.Bottom - info.srWindow.Top + 1;
					break;
				}
#else
			winsize size{};
			for (int fd : { 1, 2, 0 })
				if (ioctl(fd, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
				{
					columns = size.ws_col;
					rows = size.ws_row;
					break;
				}
#endif
			if (columns <= 0)
				return 0;
			lua_createtable(L, 0, 2);
			lua_pushinteger(L, columns);
			lua_setfield(L, -2, "columns");
			lua_pushinteger(L, rows);
			lua_setfield(L, -2, "rows");
			return 1;
		}

		struct Style
		{
			const char* Name;
			int On;
			int Off;
		};

		const Style kStyles[] = {
			{ "bold", 1, 22 }, { "dim", 2, 22 }, { "italic", 3, 23 }, { "underline", 4, 24 }, { "inverse", 7, 27 }, { "strike", 9, 29 },
			{ "black", 30, 39 }, { "red", 31, 39 }, { "green", 32, 39 }, { "yellow", 33, 39 }, { "blue", 34, 39 }, { "magenta", 35, 39 },
			{ "cyan", 36, 39 }, { "white", 37, 39 }, { "gray", 90, 39 }, { "grey", 90, 39 },
			{ "brightRed", 91, 39 }, { "brightGreen", 92, 39 }, { "brightYellow", 93, 39 }, { "brightBlue", 94, 39 },
			{ "brightMagenta", 95, 39 }, { "brightCyan", 96, 39 }, { "brightWhite", 97, 39 },
			{ "bgBlack", 40, 49 }, { "bgRed", 41, 49 }, { "bgGreen", 42, 49 }, { "bgYellow", 43, 49 }, { "bgBlue", 44, 49 },
			{ "bgMagenta", 45, 49 }, { "bgCyan", 46, 49 }, { "bgWhite", 47, 49 }, { "bgGray", 100, 49 },
		};

		const Style* Find(const char* name)
		{
			for (const Style& s : kStyles)
				if (std::strcmp(s.Name, name) == 0)
					return &s;
			return nullptr;
		}

		// The text in the style. A style inside it that ends with the same
		// code (red in blue, dim in bold) would end this one too: this one
		// starts again after each.
		std::string Apply(const std::string& text, const Style& style)
		{
			const std::string on = "\x1b[" + std::to_string(style.On) + "m";
			const std::string off = "\x1b[" + std::to_string(style.Off) + "m";
			std::string inner;
			size_t from = 0;
			for (size_t at; (at = text.find(off, from)) != std::string::npos; from = at + off.size())
				inner.append(text, from, at + off.size() - from).append(on);
			inner.append(text, from, std::string::npos);
			return on + inner + off;
		}

		// style(text, ...names): the text in each style named (plain when
		// standard output takes no colours).
		int TermStyle(lua_State* L)
		{
			size_t len = 0;
			const char* text = luaL_checklstring(L, 1, &len);
			std::string out(text, len);
			const bool colors = Colors(1);
			const int n = lua_gettop(L);
			for (int i = 2; i <= n; i++)
			{
				const char* name = luaL_checkstring(L, i);
				const Style* style = Find(name);
				if (!style)
					luaL_error(L, "term.style: no style '%s' (bold, dim, italic, underline, inverse, strike, the colours, bg<Colour>)", name);
				if (colors)
					out = Apply(out, *style);
			}
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		// term.red(text) and the others: one style each.
		int TermNamed(lua_State* L)
		{
			const Style* style = static_cast<const Style*>(lua_touserdata(L, lua_upvalueindex(1)));
			size_t len = 0;
			const char* text = luaL_checklstring(L, 1, &len);
			const std::string out = Colors(1) ? Apply(std::string(text, len), *style) : std::string(text, len);
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		// The text without its escape sequences: CSI (ESC [ ... a final
		// byte) and OSC (ESC ] ... BEL or ESC and a backslash, such as a link).
		int TermStrip(lua_State* L)
		{
			size_t len = 0;
			const char* text = luaL_checklstring(L, 1, &len);
			std::string out;
			for (size_t i = 0; i < len; i++)
			{
				if (text[i] == '\x1b' && i + 1 < len && text[i + 1] == '[')
				{
					i += 2;
					while (i < len && !((unsigned char)text[i] >= 0x40 && (unsigned char)text[i] <= 0x7e))
						i++;
					continue;
				}
				if (text[i] == '\x1b' && i + 1 < len && text[i + 1] == ']')
				{
					i += 2;
					while (i < len && text[i] != '\a' && !(text[i] == '\x1b' && i + 1 < len && text[i + 1] == '\\'))
						i++;
					if (i < len && text[i] == '\x1b')
						i++; // past the ESC; the loop's i++ takes the backslash
					continue;
				}
				out += text[i];
			}
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

	} // namespace

	void OpenTerm(lua_State* L)
	{
		lua_newtable(L);
		const luaL_Reg functions[] = { { "isTerminal", TermIsTerminal }, { "colors", TermColors }, { "size", TermSize }, { "style", TermStyle },
			{ "strip", TermStrip }, { nullptr, nullptr } };
		for (const luaL_Reg* f = functions; f->name; f++)
		{
			lua_pushcfunction(L, f->func, f->name);
			lua_setfield(L, -2, f->name);
		}
		for (const Style& style : kStyles)
		{
			lua_pushlightuserdata(L, const_cast<Style*>(&style));
			lua_pushcclosure(L, TermNamed, style.Name, 1);
			lua_setfield(L, -2, style.Name);
		}
	}

} // namespace cluau::runtime::detail
