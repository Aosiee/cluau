// @std/regex: Perl-compatible regular expressions (PCRE2). A pattern is
// compiled once (`regex.compile`) and matched as UTF-8 text, Unicode-aware
// (\w, \d and case-insensitive matching know letters beyond ASCII). Text
// that is not valid UTF-8 is matched where it is valid: no pattern matches
// an invalid byte. Positions are 1-based byte offsets, as string.find
// gives them.

#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <pcre2.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace cluau::runtime::detail {

	namespace {

		constexpr const char* kRegexType = "Regex";

		// The steps one search may take (PCRE2's own default), and the memory
		// its backtracking may use.
		constexpr uint32_t kMatchLimit = 10000000;
		constexpr uint32_t kHeapLimitKiB = 64 * 1024;
		// What a call that searches again and again (replace, split, matches)
		// may take in all: one search's steps, and so many more for each byte
		// of the text.
		constexpr uint64_t kStepsPerByte = 1000;
		// Such a call's searches are first tried with this many steps, then
		// four times more until one is enough. A search that runs out is
		// charged the steps it took; one that ends is charged the steps
		// known not to have been enough (none, when the first try was), so
		// no call is stopped before it has used what it may, and each search
		// does at most about twice its work.
		constexpr uint32_t kFirstSteps = 1000;
		// A match data's backtracking memory grows to what its largest match
		// needed; past this it is freed once the call is done.
		constexpr size_t kKeepFrames = 64 * 1024;

		struct RegexBox
		{
			pcre2_code* Code = nullptr;
			// The pattern again, compiled for text known to be valid UTF-8.
			// Code takes text that may not be, and so checks it from the start
			// of each search to its end: searching again and again through a
			// long text with it would take time growing with the square of
			// its length. Null if it could not be compiled.
			pcre2_code* Valid = nullptr;
			pcre2_match_data* Data = nullptr; // made when first needed
			pcre2_match_context* Context = nullptr;
			std::string* Pattern = nullptr;
		};

		// The steps the searches of one call may still take.
		struct Budget
		{
			uint64_t Left = 0;
		};

		Budget BudgetFor(std::string_view text) { return Budget{ kMatchLimit + kStepsPerByte * (uint64_t)text.size() }; }

		// Frees a match data whose backtracking memory grew large, at the end
		// of each call (Lua's collector does not see it, so a Regex kept or
		// waiting to be collected would hold it).
		struct Release
		{
			RegexBox& Re;
			~Release()
			{
				if (Re.Data && pcre2_get_match_data_heapframes_size(Re.Data) > kKeepFrames)
				{
					pcre2_match_data_free(Re.Data);
					Re.Data = nullptr;
				}
			}
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
		// after an empty match. With a budget, the search's steps come out of
		// it (see kFirstSteps).
		// `valid`: the text is known to be valid UTF-8, and `from` is at the
		// start of a character.
		bool Find(lua_State* L, RegexBox& re, std::string_view text, size_t from, uint32_t options = 0, Budget* budget = nullptr, bool valid = false)
		{
			const pcre2_code* code = valid && re.Valid ? re.Valid : re.Code;
			if (code == re.Valid)
				options |= PCRE2_NO_UTF_CHECK;
			if (!re.Data)
			{
				re.Data = pcre2_match_data_create_from_pattern(re.Code, nullptr);
				if (!re.Data)
					luaL_error(L, "the regex failed: out of memory");
			}
			uint32_t limit = kMatchLimit;
			if (budget)
				limit = (uint32_t)std::min<uint64_t>(kFirstSteps, budget->Left);
			uint32_t tooFew = 0; // the last limit that was not enough
			int rc = 0;
			for (;;)
			{
				pcre2_set_match_limit(re.Context, limit);
				rc = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(), from, options, re.Data, re.Context);
				if (!budget)
					break;
				if (rc != PCRE2_ERROR_MATCHLIMIT)
				{
					budget->Left -= std::min<uint64_t>(tooFew, budget->Left);
					break;
				}
				budget->Left -= std::min<uint64_t>(limit, budget->Left);
				if (limit >= kMatchLimit || budget->Left == 0)
					break;
				tooFew = limit;
				limit = (uint32_t)std::min<uint64_t>({ (uint64_t)limit * 4, kMatchLimit, budget->Left });
			}
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

		// Whether the text is valid UTF-8 (as PCRE2 has it: shortest forms,
		// no surrogates, nothing past U+10FFFF).
		bool ValidUtf8(std::string_view text)
		{
			const unsigned char* p = reinterpret_cast<const unsigned char*>(text.data());
			const unsigned char* end = p + text.size();
			while (p < end)
			{
				const unsigned char c = *p;
				if (c < 0x80)
				{
					p++;
					continue;
				}
				size_t n = 0;
				uint32_t cp = 0;
				if (c >= 0xC2 && c <= 0xDF)
					n = 1, cp = c & 0x1F;
				else if (c >= 0xE0 && c <= 0xEF)
					n = 2, cp = c & 0x0F;
				else if (c >= 0xF0 && c <= 0xF4)
					n = 3, cp = c & 0x07;
				else
					return false;
				if ((size_t)(end - p) <= n)
					return false;
				for (size_t i = 1; i <= n; i++)
				{
					if ((p[i] & 0xC0) != 0x80)
						return false;
					cp = (cp << 6) | (p[i] & 0x3F);
				}
				if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
					return false;
				p += n + 1;
			}
			return true;
		}

		// One character on from `at` (a whole UTF-8 sequence).
		size_t NextCharacter(std::string_view text, size_t at)
		{
			size_t next = at + 1;
			while (next < text.size() && ((unsigned char)text[next] & 0xC0) == 0x80)
				next++;
			return next;
		}

		// A walk over every match in a text, as Perl and Python make it: after
		// an empty match, a non-empty one at the same place is tried first,
		// and only when there is none does the search move on a character.
		struct Scan
		{
			size_t At = 0;           // where the next search starts
			bool AfterEmpty = false; // the last match was empty, ending at At
			bool Done = false;
			bool Valid = false; // the text is valid UTF-8
		};

		// The next match of the walk (in re's match data); false at the end.
		bool Next(lua_State* L, RegexBox& re, std::string_view text, Scan& scan, Budget& budget)
		{
			while (!scan.Done)
			{
				if (InterruptPending())
					RaiseIfInterrupted(L);
				if (scan.At > text.size())
					break;
				// A match can end inside a character (\C matches one byte).
				const bool valid = scan.Valid && (scan.At == text.size() || ((unsigned char)text[scan.At] & 0xC0) != 0x80);
				if (!scan.AfterEmpty)
				{
					if (!Find(L, re, text, scan.At, 0, &budget, valid))
						break;
				}
				else if (!Find(L, re, text, scan.At, PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED, &budget, valid))
				{
					scan.AfterEmpty = false;
					if (scan.At >= text.size())
						break;
					scan.At = NextCharacter(text, scan.At);
					continue;
				}
				const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(re.Data);
				if (ovector[0] > ovector[1])
					luaL_error(L, "the regex failed: \\K made a match that ends before it starts");
				// An empty match (\K can make one that took text) is followed
				// by the retry.
				scan.AfterEmpty = ovector[0] == ovector[1];
				scan.At = ovector[1];
				return true;
			}
			scan.Done = true;
			return false;
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

		// The start position at `index` (1 by default), 0-based in `from`;
		// false when it is past the end of the text plus one, where nothing
		// matches (as with string.find).
		bool StartArg(lua_State* L, int index, std::string_view text, size_t& from)
		{
			from = 0;
			if (lua_isnoneornil(L, index))
				return true;
			const double start = std::floor(luaL_checknumber(L, index));
			if (!(start >= 1))
				luaL_argerror(L, index, "a position from 1");
			if (start - 1 > (double)text.size())
				return false;
			from = (size_t)start - 1;
			return true;
		}

		// A count argument (nil: no limit): a number, negative ones and those
		// past what a call could reach meaning no limit.
		int64_t CountArg(lua_State* L, int index)
		{
			if (lua_isnoneornil(L, index))
				return -1;
			const double n = luaL_checknumber(L, index);
			if (std::isnan(n))
				luaL_argerror(L, index, "a number, not NaN");
			if (n < 0 || n >= 9007199254740992.0)
				return -1;
			return (int64_t)n;
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
			Release release{ re };
			size_t from = 0;
			lua_pushboolean(L, StartArg(L, 3, text, from) && Find(L, re, text, from));
			return 1;
		}

		int RegexMatch(lua_State* L)
		{
			RegexBox& re = Check(L, 1);
			const std::string_view text = Text(L, 2);
			Release release{ re };
			size_t from = 0;
			if (!StartArg(L, 3, text, from) || !Find(L, re, text, from))
				return 0;
			PushMatch(L, re, text);
			return 1;
		}

		// The iterator `matches` returns: upvalues are the regex, the text,
		// where the next search starts (nil once done), whether the last
		// match was empty, the steps its searches may still take, and
		// whether the text is valid UTF-8.
		int MatchesNext(lua_State* L)
		{
			RegexBox& re = Check(L, lua_upvalueindex(1));
			const std::string_view text = Text(L, lua_upvalueindex(2));
			if (lua_isnil(L, lua_upvalueindex(3)))
				return 0;
			Release release{ re };
			Scan scan;
			scan.At = (size_t)lua_tonumber(L, lua_upvalueindex(3));
			scan.AfterEmpty = lua_toboolean(L, lua_upvalueindex(4));
			scan.Valid = lua_toboolean(L, lua_upvalueindex(6));
			Budget budget{ (uint64_t)lua_tonumber(L, lua_upvalueindex(5)) };
			// Spent whether or not the search raises.
			lua_pushnumber(L, 0);
			lua_replace(L, lua_upvalueindex(5));
			const bool found = Next(L, re, text, scan, budget);
			lua_pushnumber(L, (double)budget.Left);
			lua_replace(L, lua_upvalueindex(5));
			if (!found)
			{
				lua_pushnil(L);
				lua_replace(L, lua_upvalueindex(3));
				return 0;
			}
			lua_pushnumber(L, (double)scan.At);
			lua_replace(L, lua_upvalueindex(3));
			lua_pushboolean(L, scan.AfterEmpty);
			lua_replace(L, lua_upvalueindex(4));
			PushMatch(L, re, text);
			return 1;
		}

		int RegexMatches(lua_State* L)
		{
			Check(L, 1);
			const std::string_view text = Text(L, 2);
			lua_pushvalue(L, 1);
			lua_pushvalue(L, 2);
			lua_pushnumber(L, 0);
			lua_pushboolean(L, false);
			lua_pushnumber(L, (double)BudgetFor(text).Left);
			lua_pushboolean(L, ValidUtf8(text));
			lua_pushcclosure(L, MatchesNext, "matches", 6);
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
					// With (?J) a name can be several groups': the one that
					// took part, if any.
					PCRE2_SPTR first = nullptr, last = nullptr;
					const int found = pcre2_substring_nametable_scan(re.Code, reinterpret_cast<PCRE2_SPTR>(name.c_str()), &first, &last);
					if (found < 0 && found != PCRE2_ERROR_NOUNIQUESUBSTRING)
						luaL_error(L, "the replacement names group '%s', which the pattern does not have", name.c_str());
					uint32_t entry = 0;
					pcre2_pattern_info(re.Code, PCRE2_INFO_NAMEENTRYSIZE, &entry);
					int n = (first[0] << 8) | first[1];
					for (PCRE2_SPTR at = first; at <= last; at += entry)
					{
						const int candidate = (at[0] << 8) | at[1];
						if (ovector[2 * candidate] != PCRE2_UNSET)
						{
							n = candidate;
							break;
						}
					}
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
			const int64_t limit = CountArg(L, 4);
			const std::string replacement = function ? std::string() : lua_tostring(L, 3);
			Release release{ re };
			std::string out;
			size_t copied = 0;
			int64_t count = 0;
			Scan scan;
			scan.Valid = ValidUtf8(text);
			Budget budget = BudgetFor(text);
			while ((limit < 0 || count < limit) && Next(L, re, text, scan, budget))
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
			}
			out.append(text.substr(copied));
			lua_pushlstring(L, out.data(), out.size());
			lua_pushnumber(L, (double)count);
			return 2;
		}

		int RegexSplit(lua_State* L)
		{
			RegexBox& re = Check(L, 1);
			const std::string_view text = Text(L, 2);
			if (!lua_isnoneornil(L, 3))
			{
				const double given = luaL_checknumber(L, 3);
				if (!(given >= 1) && given != -1)
					luaL_argerror(L, 3, "the most pieces, from 1");
			}
			const int64_t limit = CountArg(L, 3);
			Release release{ re };
			lua_newtable(L);
			int64_t n = 0;
			size_t piece = 0;
			Scan scan;
			scan.Valid = ValidUtf8(text);
			Budget budget = BudgetFor(text);
			while ((limit < 0 || n + 1 < limit) && Next(L, re, text, scan, budget))
			{
				const size_t start = Start(re), end = End(re);
				// An empty match at the start or the end splits nothing off.
				if (end == start && (start == 0 || start >= text.size()))
					continue;
				lua_pushlstring(L, text.data() + piece, start - piece);
				lua_rawseti(L, -2, (int)++n);
				piece = end;
			}
			lua_pushlstring(L, text.data() + piece, text.size() - piece);
			lua_rawseti(L, -2, (int)++n);
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
				pcre2_match_context_free(box->Context);
				pcre2_code_free(box->Valid);
				pcre2_code_free(box->Code);
				delete box->Pattern;
			}));
			box->Code = code;
			box->Valid = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern), len, options & ~PCRE2_MATCH_INVALID_UTF, &error, &offset, nullptr);
			box->Data = nullptr;
			box->Context = pcre2_match_context_create(nullptr);
			box->Pattern = new std::string(pattern, len);
			if (!box->Context)
				luaL_error(L, "regex.compile: out of memory");
			pcre2_set_heap_limit(box->Context, kHeapLimitKiB);
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
				// And the white space beyond ASCII that extended mode skips
				// too (in UTF-8, then escaped).
				static const struct
				{
					std::string_view Text, Escaped;
				} kWide[] = { { "\xC2\x85", "\\x{85}" }, { "\xE2\x80\x8E", "\\x{200E}" }, { "\xE2\x80\x8F", "\\x{200F}" },
					{ "\xE2\x80\xA8", "\\x{2028}" }, { "\xE2\x80\xA9", "\\x{2029}" } };
				const std::string_view rest(text + i, len - i);
				bool wide = false;
				for (const auto& w : kWide)
					if (rest.substr(0, w.Text.size()) == w.Text)
					{
						out += w.Escaped;
						i += w.Text.size() - 1;
						wide = true;
						break;
					}
				if (wide)
					continue;
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
