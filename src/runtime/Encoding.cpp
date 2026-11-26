// @std/encoding: bytes as text and back: base64 (standard or URL-safe,
// padded or not) and hex. Text and buffers are both accepted; what is
// decoded is a string of the bytes.

#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace cluau::runtime::detail {

	namespace {

		// A byte for an error message: 'x', or 0xNN when it is not printable.
		std::string Shown(char c)
		{
			char buf[16];
			const unsigned char b = (unsigned char)c;
			if (b >= 0x20 && b < 0x7f)
				std::snprintf(buf, sizeof(buf), "'%c'", c);
			else
				std::snprintf(buf, sizeof(buf), "0x%02X", b);
			return buf;
		}

		std::string_view Bytes(lua_State* L, int index)
		{
			if (lua_type(L, index) == LUA_TBUFFER)
			{
				size_t len = 0;
				void* data = lua_tobuffer(L, index, &len);
				return std::string_view(static_cast<const char*>(data), len);
			}
			size_t len = 0;
			const char* s = luaL_checklstring(L, index, &len);
			return std::string_view(s, len);
		}

		bool Option(lua_State* L, int index, const char* name, bool otherwise)
		{
			if (lua_isnoneornil(L, index))
				return otherwise;
			luaL_checktype(L, index, LUA_TTABLE);
			lua_getfield(L, index, name);
			const bool value = lua_isnil(L, -1) ? otherwise : lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);
			return value;
		}

		// toBase64(data, options?): options.url for the URL-safe alphabet
		// (- and _), options.padding = false for no trailing =.
		int ToBase64(lua_State* L)
		{
			const std::string_view data = Bytes(L, 1);
			const bool url = Option(L, 2, "url", false);
			const bool padding = Option(L, 2, "padding", !url);
			const char* alphabet = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
									   : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string out;
			out.reserve((data.size() + 2) / 3 * 4);
			size_t i = 0;
			for (; i + 2 < data.size(); i += 3)
			{
				const unsigned v = ((unsigned char)data[i] << 16) | ((unsigned char)data[i + 1] << 8) | (unsigned char)data[i + 2];
				out += alphabet[(v >> 18) & 63];
				out += alphabet[(v >> 12) & 63];
				out += alphabet[(v >> 6) & 63];
				out += alphabet[v & 63];
			}
			if (i < data.size())
			{
				const bool two = i + 1 < data.size();
				const unsigned v = ((unsigned char)data[i] << 16) | (two ? (unsigned char)data[i + 1] << 8 : 0);
				out += alphabet[(v >> 18) & 63];
				out += alphabet[(v >> 12) & 63];
				if (two)
					out += alphabet[(v >> 6) & 63];
				if (padding)
					out += two ? "=" : "==";
			}
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		// fromBase64(text): either alphabet, with or without padding;
		// spaces and line breaks are skipped.
		int FromBase64(lua_State* L)
		{
			const std::string_view text = Bytes(L, 1);
			std::string out;
			unsigned v = 0;
			int bits = 0;
			bool ended = false;
			size_t digits = 0, padding = 0;
			for (size_t i = 0; i < text.size(); i++)
			{
				const char c = text[i];
				int n;
				if (c >= 'A' && c <= 'Z')
					n = c - 'A';
				else if (c >= 'a' && c <= 'z')
					n = c - 'a' + 26;
				else if (c >= '0' && c <= '9')
					n = c - '0' + 52;
				else if (c == '+' || c == '-')
					n = 62;
				else if (c == '/' || c == '_')
					n = 63;
				else if (c == '=')
				{
					ended = true;
					padding++;
					continue;
				}
				else if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
					continue;
				else
					luaL_error(L, "encoding.fromBase64: %s (at %d) is not base64", Shown(c).c_str(), (int)i + 1);
				if (ended)
					luaL_error(L, "encoding.fromBase64: text after the padding (at %d)", (int)i + 1);
				digits++;
				v = (v << 6) | (unsigned)n;
				bits += 6;
				if (bits >= 8)
				{
					bits -= 8;
					out += (char)((v >> bits) & 0xFF);
				}
			}
			if (bits >= 6)
				luaL_error(L, "encoding.fromBase64: the text ends in the middle of a byte");
			// Only one text decodes to each value: the bits left over in the
			// last character are zero, and padding, if any, fills the last
			// group of four exactly.
			if ((v & ((1u << bits) - 1)) != 0)
				luaL_error(L, "encoding.fromBase64: the last character has bits that belong to no byte");
			if (padding > 0 && (digits + padding) % 4 != 0)
				luaL_error(L, "encoding.fromBase64: the padding doesn't fill the last group of four");
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		int ToHex(lua_State* L)
		{
			const std::string_view data = Bytes(L, 1);
			static const char kHex[] = "0123456789abcdef";
			std::string out(data.size() * 2, '0');
			for (size_t i = 0; i < data.size(); i++)
			{
				out[2 * i] = kHex[(unsigned char)data[i] >> 4];
				out[2 * i + 1] = kHex[(unsigned char)data[i] & 15];
			}
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

		int FromHex(lua_State* L)
		{
			const std::string_view text = Bytes(L, 1);
			auto digit = [](char c) {
				return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			};
			if (text.size() % 2)
				luaL_error(L, "encoding.fromHex: an odd number of digits (%d)", (int)text.size());
			std::string out(text.size() / 2, '\0');
			for (size_t i = 0; i < out.size(); i++)
			{
				const int hi = digit(text[2 * i]), lo = digit(text[2 * i + 1]);
				if (hi < 0 || lo < 0)
					luaL_error(L, "encoding.fromHex: %s (at %d) is not a hex digit", Shown(hi < 0 ? text[2 * i] : text[2 * i + 1]).c_str(),
						(int)(hi < 0 ? 2 * i : 2 * i + 1) + 1);
				out[i] = (char)(hi * 16 + lo);
			}
			lua_pushlstring(L, out.data(), out.size());
			return 1;
		}

	} // namespace

	void OpenEncoding(lua_State* L)
	{
		lua_newtable(L);
		const luaL_Reg functions[] = { { "toBase64", ToBase64 }, { "fromBase64", FromBase64 }, { "toHex", ToHex }, { "fromHex", FromHex },
			{ nullptr, nullptr } };
		for (const luaL_Reg* f = functions; f->name; f++)
		{
			lua_pushcfunction(L, f->func, f->name);
			lua_setfield(L, -2, f->name);
		}
	}

} // namespace cluau::runtime::detail
