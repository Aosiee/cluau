// @std/_release: what `cluau upgrade` (src/runtime/std/_upgrade.cluau)
// needs from C++. Releases are signed with minisign
// (https://jedisct1.github.io/minisign/): the release's SHA256SUMS has a
// SHA256SUMS.minisig beside it, an Ed25519 signature made with a key whose
// public half this build carries (installer/release-keys.pub). Monocypher
// does the Ed25519 and BLAKE2b; mbedTLS has neither.

#include "Std.h"

#include "cluau/cluau.h"

#include "lua.h"
#include "lualib.h"

#include "monocypher-ed25519.h"
#include "monocypher.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <sys/stat.h>
#endif

namespace cluau::runtime::detail {

	namespace {

		// The release keys, one minisign public key (its base64 line) each.
		const char* const kReleaseKeys[] = {
#include "cluau_release_keys.inc"
			nullptr,
		};

		// The release build this one is, as the downloads name it.
		const char* Target()
		{
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
			return "windows-x86_64";
#elif defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
			return "macos-arm64";
#elif defined(__APPLE__) && defined(__x86_64__)
			return "macos-x86_64";
#elif defined(__linux__) && defined(__x86_64__)
			return "linux-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
			return "linux-arm64";
#else
			return "";
#endif
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

		// Standard base64, padded (minisign's): the bytes, or false.
		bool FromBase64(std::string_view text, std::string& out)
		{
			out.clear();
			if (text.empty() || text.size() % 4 != 0)
				return false;
			auto value = [](char c) -> int {
				if (c >= 'A' && c <= 'Z')
					return c - 'A';
				if (c >= 'a' && c <= 'z')
					return c - 'a' + 26;
				if (c >= '0' && c <= '9')
					return c - '0' + 52;
				if (c == '+')
					return 62;
				if (c == '/')
					return 63;
				return -1;
			};
			for (size_t i = 0; i < text.size(); i += 4)
			{
				const bool last = i + 4 == text.size();
				const int a = value(text[i]), b = value(text[i + 1]);
				const int c = text[i + 2] == '=' && last ? -2 : value(text[i + 2]);
				const int d = text[i + 3] == '=' && last ? -2 : value(text[i + 3]);
				if (a < 0 || b < 0 || c == -1 || d == -1 || (c == -2 && d != -2))
					return false;
				const uint32_t n = (uint32_t)a << 18 | (uint32_t)b << 12 | (uint32_t)(c < 0 ? 0 : c) << 6 | (uint32_t)(d < 0 ? 0 : d);
				out += (char)(n >> 16);
				if (c >= 0)
					out += (char)(n >> 8 & 0xff);
				if (d >= 0)
					out += (char)(n & 0xff);
			}
			return true;
		}

		// The lines of a minisign file, without line ends (LF or CRLF) and
		// without empty lines.
		std::vector<std::string_view> Lines(std::string_view text)
		{
			std::vector<std::string_view> lines;
			size_t at = 0;
			while (at <= text.size())
			{
				size_t end = text.find('\n', at);
				if (end == std::string_view::npos)
					end = text.size();
				std::string_view line = text.substr(at, end - at);
				if (!line.empty() && line.back() == '\r')
					line.remove_suffix(1);
				if (!line.empty())
					lines.push_back(line);
				at = end + 1;
			}
			return lines;
		}

		bool StartsWith(std::string_view text, std::string_view prefix) { return text.substr(0, prefix.size()) == prefix; }

		// A key id as minisign prints it: the eight bytes as a little-endian
		// number, in uppercase hex.
		std::string KeyId(const unsigned char* id)
		{
			std::string out;
			char digits[3];
			for (int i = 7; i >= 0; i--)
			{
				std::snprintf(digits, sizeof(digits), "%02X", id[i]);
				out += digits;
			}
			return out;
		}

		struct PublicKey
		{
			unsigned char Id[8];
			unsigned char Key[32];
		};

		// A minisign public key: its file's text, or the base64 line alone.
		bool ParsePublicKey(std::string_view text, PublicKey& key)
		{
			std::string_view line;
			for (std::string_view l : Lines(text))
				if (!StartsWith(l, "untrusted comment:"))
				{
					line = l;
					break;
				}
			std::string bytes;
			if (!FromBase64(line, bytes) || bytes.size() != 42 || bytes[0] != 'E' || bytes[1] != 'd')
				return false;
			std::memcpy(key.Id, bytes.data() + 2, 8);
			std::memcpy(key.Key, bytes.data() + 10, 32);
			return true;
		}

		// _release.verify(signature, data, keys): the trusted comment of a
		// minisign signature of `data` made with one of `keys` (minisign
		// public keys); an error saying why when there is none.
		int ReleaseVerify(lua_State* L)
		{
			const std::string_view signature = Bytes(L, 1);
			const std::string_view data = Bytes(L, 2);
			luaL_checktype(L, 3, LUA_TTABLE);
			std::vector<PublicKey> keys;
			for (int i = 1;; i++)
			{
				lua_rawgeti(L, 3, i);
				if (lua_isnil(L, -1))
				{
					lua_pop(L, 1);
					break;
				}
				size_t len = 0;
				const char* text = lua_tolstring(L, -1, &len);
				PublicKey key{};
				if (!text || !ParsePublicKey(std::string_view(text, len), key))
					luaL_error(L, "key %d is not a minisign public key", i);
				keys.push_back(key);
				lua_pop(L, 1);
			}
			if (keys.empty())
				luaL_error(L, "there is no key to check the signature with");

			// untrusted comment, signature, trusted comment, global signature.
			const std::vector<std::string_view> lines = Lines(signature);
			if (lines.size() != 4 || !StartsWith(lines[0], "untrusted comment:") || !StartsWith(lines[2], "trusted comment: "))
				luaL_error(L, "the signature is not a minisign signature");
			std::string sig, global;
			if (!FromBase64(lines[1], sig) || sig.size() != 74 || !FromBase64(lines[3], global) || global.size() != 64)
				luaL_error(L, "the signature is not a minisign signature");
			const std::string_view algorithm(sig.data(), 2);
			if (algorithm != "Ed" && algorithm != "ED")
				luaL_error(L, "the signature is not a minisign signature");
			const unsigned char* id = reinterpret_cast<const unsigned char*>(sig.data() + 2);
			const PublicKey* key = nullptr;
			for (const PublicKey& k : keys)
				if (std::memcmp(k.Id, id, 8) == 0)
					key = &k;
			if (!key)
				luaL_error(L, "the signature is by key %s, which is not one this cluau trusts", KeyId(id).c_str());

			// "ED" signs the file's BLAKE2b-512 hash (minisign's default since
			// 0.11); "Ed", the file itself.
			const unsigned char* sigBytes = reinterpret_cast<const unsigned char*>(sig.data() + 10);
			int bad = 0;
			if (algorithm == "ED")
			{
				uint8_t hash[64];
				crypto_blake2b(hash, sizeof(hash), reinterpret_cast<const uint8_t*>(data.data()), data.size());
				bad = crypto_ed25519_check(sigBytes, key->Key, hash, sizeof(hash));
			}
			else
				bad = crypto_ed25519_check(sigBytes, key->Key, reinterpret_cast<const uint8_t*>(data.data()), data.size());
			if (bad)
				luaL_error(L, "the signature does not match: the file is not the one key %s signed", KeyId(id).c_str());

			// The trusted comment is signed too, with the signature.
			const std::string_view comment = lines[2].substr(std::string_view("trusted comment: ").size());
			std::string signedComment(sig.data() + 10, 64);
			signedComment.append(comment.data(), comment.size());
			if (crypto_ed25519_check(reinterpret_cast<const uint8_t*>(global.data()), key->Key,
					reinterpret_cast<const uint8_t*>(signedComment.data()), signedComment.size()))
				luaL_error(L, "the signature's trusted comment does not match: it was changed after signing");
			lua_pushlstring(L, comment.data(), comment.size());
			return 1;
		}

		// _release.makeExecutable(path): a downloaded program may run (no
		// change on Windows, where every file may).
		int ReleaseMakeExecutable(lua_State* L)
		{
			const char* path = luaL_checkstring(L, 1);
#ifdef _WIN32
			(void)path;
#else
			if (chmod(path, 0755) != 0)
				luaL_error(L, "cannot make '%s' executable: %s", path, std::strerror(errno));
#endif
			return 0;
		}

	} // namespace

	void OpenRelease(lua_State* L)
	{
		lua_newtable(L);
		lua_pushcfunction(L, ReleaseVerify, "verify");
		lua_setfield(L, -2, "verify");
		lua_pushcfunction(L, ReleaseMakeExecutable, "makeExecutable");
		lua_setfield(L, -2, "makeExecutable");
		lua_newtable(L);
		for (int i = 0; kReleaseKeys[i]; i++)
		{
			lua_pushstring(L, kReleaseKeys[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "keys");
		lua_pushstring(L, Target());
		lua_setfield(L, -2, "target");
		lua_pushstring(L, cluau::Version());
		lua_setfield(L, -2, "version");
	}

} // namespace cluau::runtime::detail
