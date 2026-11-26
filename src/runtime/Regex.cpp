// @std/regex: Perl-compatible regular expressions (PCRE2). A pattern is
// compiled once (`regex.compile`) and matched as UTF-8 text, Unicode-aware
// (\w, \d and case-insensitive matching know letters beyond ASCII); text
// that is not valid UTF-8 still matches, byte by byte where it is invalid.
// Positions are 1-based byte offsets, as string.find gives them.

#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <pcre2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace cluau::runtime::detail {

	namespace {

		constexpr const char* kRegexType = "Regex";

		struct RegexBox
		{
			pcre2_code* Code = nullptr;
			pcre2_match_data* Data = nullptr;
			std::string* Pattern = nullptr;
		};

		RegexBox& Check(lua_State* L, int index)
		{
			RegexBox* box = static_cast<RegexBox*>(luaL_checkudata(L, index, kRegexType));
			if (!box->Code)
				luaL_error(L, "the regex is gone");
			return *box;
		}

		std::string Message(int code)
		{
			PCRE2_UCHAR buffer[256];
			pcre2_get_error_message(code, buffer, sizeof(buffer));
			return reinterpret_cast<const char*>(buffer);
		}

		// One match at or after `from` (0-based); false when there is none.
		// `options`: PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED, for the retry
		// after an empty match.
		bool Find(lua_State* L, RegexBox& re, std::string_view text, size_t from, uint32_t options = 0)
		{
			const int rc = pcre2_match(re.Code, reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(), from, options, re.Data, nullptr);
			if (rc >= 0)
				return true;
			if (rc == PCRE2_ERROR_NOMATCH)
				return false;
			if (rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT || rc == PCRE2_ERROR_HEAPLIMIT)
				luaL_error(L, "the regex gave up: the pattern backtracks too much on this text (%s)", re.Pattern->c_str());
			luaL_error(L, "the regex failed: %s", Message(rc).c_str());
			return false;
		}

		size_t Start(RegexBox& re) { return pcre2_get_ovector_pointer(re.Data)[0]; }
		size_t End(RegexBox& re) { return pcre2_get_ovector_pointer(re.Data)[1]; }

		// After a match ending at `end`, where the next search starts: past
		// an empty match by one character (a whole UTF-8 sequence).
		size_t Next(std::string_view text, size_t start, size_t end)
		{
			if (end > start)
				return end;
			size_t next = end + 1;
			while (next < text.size() && ((unsigned char)text[next] & 0xC0) == 0x80)
				next++;
			return next;
		}

		// The last match as { text, start, finish, groups, named }.
		void PushMatch(lua_State* L, RegexBox& re, std::string_view text)
		{
			const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(re.Data);
			uint32_t groups = 0;
			pcre2_pattern_info(re.Code, PCRE2_INFO_CAPTURECOUNT, &groups);
			lua_createtable(L, 0, 5);
			lua_pushlstring(L, text.data() + ovector[0], ovector[1] - ovector[0]);
			lua_setfield(L, -2, "text");
			lua_pushnumber(L, (double)ovector[0] + 1);
			lua_setfield(L, -2, "start");
			lua_pushnumber(L, (double)ovector[1]);
			lua_setfield(L, -2, "finish");
			lua_createtable(L, (int)groups, 0);
			for (uint32_t i = 1; i <= groups; i++)
				if (ovector[2 * i] != PCRE2_UNSET)
				{
					lua_pushlstring(L, text.data() + ovector[2 * i], ovector[2 * i + 1] - ovector[2 * i]);
					lua_rawseti(L, -2, (int)i);
				}
			lua_setfield(L, -2, "groups");
			lua_newtable(L);
			uint32_t names = 0, entry = 0;
			PCRE2_SPTR table = nullptr;
			pcre2_pattern_info(re.Code, PCRE2_INFO_NAMECOUNT, &names);
			pcre2_pattern_info(re.Code, PCRE2_INFO_NAMEENTRYSIZE, &entry);
			pcre2_pattern_info(re.Code, PCRE2_INFO_NAMETABLE, &table);
			for (uint32_t n = 0; n < names; n++)
			{
				const PCRE2_SPTR at = table + n * entry;
				const int group = (at[0] << 8) | at[1];
				if (ovector[2 * group] == PCRE2_UNSET)
					continue;
				lua_pushlstring(L, text.data() + ovector[2 * group], ovector[2 * group + 1] - ovector[2 * group]);
				lua_setfield(L, -2, reinterpret_cast<const char*>(at + 2));
			}
			lua_setfield(L, -2, "named");
		}

		size_t StartArg(lua_State* L, int index, std::string_view text)
		{
			const int start = (int)luaL_optinteger(L, index, 1);
			if (start < 1)
				luaL_argerror(L, index, "a position from 1");
			return std::min((size_t)start - 1, text.size());
		}

		// The string at `index`, in place (it stays on the stack while used).
		std::string_view Text(lua_State* L, int index)
		{
			size_t len = 0;
			const char* s = luaL_checklstring(L, index, &len);
			return std::string_view(s, len);
		}

		int RegexTest(lua_State* L)
		{
			RegexBox& re = Check(L, 1);
			const std::string_view text = Text(L, 2);
			lua_pushboolean(L, Find(L, re, text, StartArg(L, 3, text)));
			return 1;
		}

		int RegexMatch(lua_State* L)
		{
			RegexBox& re = Check(L, 1);
			const std::string_view text = Text(L, 2);
			if (!Find(L, re, text, StartArg(L, 3, text)))
				return 0;
			PushMatch(L, re, text);
			return 1;
		}

		// The iterator `matches` returns: upvalues are the regex, the text,
		// and where the next search starts (nil once done).
		int MatchesNext(lua_State* L)
		{
			RegexBox& re = Check(L, lua_upvalueindex(1));
			const std::string_view text = Text(L, lua_upvalueindex(2));
			if (lua_isnil(L, lua_upvalueindex(3)))
				return 0;
			const size_t from = (size_t)lua_tonumber(L, lua_upvalueindex(3));
			if (from > text.size() || !Find(L, re, text, from))
			{
				lua_pushnil(L);
				lua_replace(L, lua_upvalueindex(3));
				return 0;
			}
			const size_t start = Start(re), end = End(re);
			lua_pushnumber(L, (double)Next(text, start, end));
			lua_replace(L, lua_upvalueindex(3));
			PushMatch(L, re, text);
			return 1;
		}

		int RegexMatches(lua_State* L)
		{
			Check(L, 1);
			luaL_checkstring(L, 2);
			lua_pushvalue(L, 1);
			lua_pushvalue(L, 2);
			lua_pushnumber(L, 0);
			lua_pushcclosure(L, MatchesNext, "matches", 3);
			return 1;
		}

		// A replacement's template: $0 or $& the match, $1..$99 a group,
		// ${name} a named group, $$ a dollar sign.
		void Expand(lua_State* L, RegexBox& re, std::string_view text, const std::string& replacement, std::string& out)
		{
			const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(re.Data);
			uint32_t groups = 0;
			pcre2_pattern_info(re.Code, PCRE2_INFO_CAPTURECOUNT, &groups);
			auto group = [&](int n) {
				if (n < 0 || (uint32_t)n > groups)
					luaL_error(L, "the replacement names group %d; the pattern has %d", n, (int)groups);
				if (ovector[2 * n] != PCRE2_UNSET)
					out.append(text.substr(ovector[2 * n], ovector[2 * n + 1] - ovector[2 * n]));
			};
			for (size_t i = 0; i < replacement.size(); i++)
			{
				const char c = replacement[i];
				if (c != '$' || i + 1 == replacement.size())
				{
					out += c;
					continue;
				}
				const char next = replacement[i + 1];
				if (next == '$')
				{
					out += '$';
					i++;
				}
				else if (next == '&')
				{
					group(0);
					i++;
				}
				else if (next >= '0' && next <= '9')
				{
					int n = next - '0';
					i++;
					if (i + 1 < replacement.size() && replacement[i + 1] >= '0' && replacement[i + 1] <= '9' && n * 10 + (replacement[i + 1] - '0') <= (int)groups)
						n = n * 10 + (replacement[++i] - '0');
					group(n);
				}
				else if (next == '{')
				{
					const size_t close = replacement.find('}', i + 2);
					if (close == std::string::npos)
						luaL_error(L, "the replacement has '${' with no '}'");
					const std::string name = replacement.substr(i + 2, close - i - 2);
					const int n = pcre2_substring_number_from_name(re.Code, reinterpret_cast<PCRE2_SPTR>(name.c_str()));
					if (n < 0)
						luaL_error(L, "the replacement names group '%s', which the pattern does not have", name.c_str());
					group(n);
					i = close;
				}
				else
					out += c;
			}
		}

		int RegexReplace(lua_State* L)
		{
			RegexBox& re = Check(L, 1);
			const std::string_view text = Text(L, 2);
			const bool function = lua_isfunction(L, 3);
			if (!function)
				luaL_checkstring(L, 3);
			const int limit = lua_isnoneornil(L, 4) ? -1 : (int)luaL_checkinteger(L, 4);
			const std::string replacement = function ? std::string() : lua_tostring(L, 3);
			std::string out;
			size_t at = 0, copied = 0;
			int count = 0;
			while ((limit < 0 || count < limit) && at <= text.size() && Find(L, re, text, at))
			{
				const size_t start = Start(re), end = End(re);
				out.append(text.substr(copied, start - copied));
				if (function)
				{
					lua_pushvalue(L, 3);
					PushMatch(L, re, text);
					lua_call(L, 1, 1);
					if (!lua_isstring(L, -1))
						luaL_error(L, "the replacement function returned a %s, not a string", luaL_typename(L, -1));
					size_t len = 0;
					const char* s = lua_tolstring(L, -1, &len);
					out.append(s, len);
					lua_pop(L, 1);
				}
				else
					Expand(L, re, text, replacement, out);
				copied = end;
				count++;
				at = Next(text, start, end);
				if (end == start && start < text.size())
				{
					// An empty match: the character after it stays as it is.
					out.append(text.substr(start, at - start));
					copied = at;
				}
			}
			out.append(text.substr(std::min(copied, text.size())));
			lua_pushlstring(L, out.data(), out.size());
			lua_pushnumber(L, count);
			return 2;
		}

		int RegexSplit(lua_State* L)
		{
			RegexBox& re = Check(L, 1);
			const std::string_view text = Text(L, 2);
			const int limit = lua_isnoneornil(L, 3) ? -1 : (int)luaL_checkinteger(L, 3);
			if (limit == 0 || limit < -1)
				luaL_argerror(L, 3, "the most pieces, from 1");
			lua_newtable(L);
			int n = 0;
			size_t at = 0, piece = 0;
			while ((limit < 0 || n + 1 < limit) && at <= text.size() && Find(L, re, text, at))
			{
				const size_t start = Start(re), end = End(re);
				at = Next(text, start, end);
				// An empty match at the start or the end splits nothing off.
				if (end == start && (start == 0 || start >= text.size()))
					continue;
				lua_pushlstring(L, text.data() + piece, start - piece);
				lua_rawseti(L, -2, ++n);
				piece = end;
			}
			lua_pushlstring(L, text.data() + piece, text.size() - piece);
			lua_rawseti(L, -2, ++n);
			return 1;
		}

		int RegexIndex(lua_State* L)
		{
			RegexBox& re = Check(L, 1);
			const char* key = luaL_checkstring(L, 2);
			if (std::strcmp(key, "pattern") == 0)
			{
				lua_pushlstring(L, re.Pattern->data(), re.Pattern->size());
				return 1;
			}
			lua_getmetatable(L, 1);
			lua_getfield(L, -1, "methods");
			lua_getfield(L, -1, key);
			return 1;
		}

		int RegexToString(lua_State* L)
		{
			RegexBox& re = Check(L, 1);
			lua_pushstring(L, ("Regex(" + *re.Pattern + ")").c_str());
			return 1;
		}

		int RegexCompile(lua_State* L)
		{
			size_t len = 0;
			const char* pattern = luaL_checklstring(L, 1, &len);
			const char* flags = luaL_optstring(L, 2, "");
			uint32_t options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF;
			for (const char* f = flags; *f; f++)
				switch (*f)
				{
				case 'i': options |= PCRE2_CASELESS; break;
				case 'm': options |= PCRE2_MULTILINE; break;
				case 's': options |= PCRE2_DOTALL; break;
				case 'x': options |= PCRE2_EXTENDED; break;
				default: luaL_error(L, "unknown regex flag '%c' (the flags: i, m, s, x)", *f);
				}
			int error = 0;
			PCRE2_SIZE offset = 0;
			pcre2_code* code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern), len, options, &error, &offset, nullptr);
			if (!code)
				luaL_error(L, "bad regex at position %d: %s", (int)offset + 1, Message(error).c_str());
			RegexBox* box = static_cast<RegexBox*>(lua_newuserdatadtor(L, sizeof(RegexBox), [](lua_State*, void* p) {
				RegexBox* box = static_cast<RegexBox*>(p);
				pcre2_match_data_free(box->Data);
				pcre2_code_free(box->Code);
				delete box->Pattern;
			}));
			box->Code = code;
			box->Data = pcre2_match_data_create_from_pattern(code, nullptr);
			box->Pattern = new std::string(pattern, len);
			luaL_getmetatable(L, kRegexType);
			lua_setmetatable(L, -2);
			return 1;
		}

		int RegexEscape(lua_State* L)
		{
			size_t len = 0;
			const char* text = luaL_checklstring(L, 1, &len);
			std::string out;
			for (size_t i = 0; i < len; i++)
			{
				// White space too, as \xNN: in extended mode (the x flag) a
				// literal space would be ignored.
				if (std::strchr(" \t\n\r\f\v", text[i]) && text[i] != '\0')
				{
					char hex[8];
					std::snprintf(hex, sizeof(hex), "\\x%02X", (unsigned char)text[i]);
					out += hex;
					continue;
				}
				if (std::strchr("\\^$.|?*+()[]{}-/#", text[i]) && text[i] != '\0')
					out += '\\';
				out += text[i];
			}
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

	} // namespace

	void OpenRegex(lua_State* L)
	{
		luaL_newmetatable(L, kRegexType);
		lua_pushstring(L, kRegexType);
		lua_setfield(L, -2, "__type");
		lua_pushcfunction(L, RegexIndex, "index");
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, RegexToString, "tostring");
		lua_setfield(L, -2, "__tostring");
		lua_newtable(L);
		const luaL_Reg methods[] = { { "test", RegexTest }, { "match", RegexMatch }, { "matches", RegexMatches }, { "replace", RegexReplace },
			{ "split", RegexSplit }, { nullptr, nullptr } };
		for (const luaL_Reg* m = methods; m->name; m++)
		{
			lua_pushcfunction(L, m->func, m->name);
			lua_setfield(L, -2, m->name);
		}
		lua_setfield(L, -2, "methods");
		lua_pop(L, 1);

		lua_newtable(L);
		lua_pushcfunction(L, RegexCompile, "compile");
		lua_setfield(L, -2, "compile");
		lua_pushcfunction(L, RegexEscape, "escape");
		lua_setfield(L, -2, "escape");
	}

} // namespace cluau::runtime::detail
