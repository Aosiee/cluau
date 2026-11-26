// @std/crypto: hashes (MD5, SHA-1, SHA-2), HMACs, random bytes from the
// operating system, UUIDs, and a comparison that takes the same time
// whatever the strings hold. Text and buffers are both accepted; a digest
// is lowercase hex unless `{ raw = true }` asks for its bytes.

#include "Std.h"
#include "Tls.h"

#include "lua.h"
#include "lualib.h"

#include <mbedtls/md.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <sys/random.h>
#endif

namespace cluau::runtime::detail {

	namespace {

		constexpr const char* kHasherMeta = "cluau.Hasher";

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

		const mbedtls_md_info_t* Algorithm(lua_State* L, int index, const char* fn)
		{
			const char* name = luaL_checkstring(L, index);
			const struct
			{
				const char* Name;
				mbedtls_md_type_t Type;
			} kAlgorithms[] = { { "md5", MBEDTLS_MD_MD5 }, { "sha1", MBEDTLS_MD_SHA1 }, { "sha224", MBEDTLS_MD_SHA224 },
				{ "sha256", MBEDTLS_MD_SHA256 }, { "sha384", MBEDTLS_MD_SHA384 }, { "sha512", MBEDTLS_MD_SHA512 } };
			for (const auto& a : kAlgorithms)
				if (std::strcmp(name, a.Name) == 0)
					if (const mbedtls_md_info_t* info = mbedtls_md_info_from_type(a.Type))
						return info;
			luaL_error(L, "%s: no hash '%s' (the hashes: md5, sha1, sha224, sha256, sha384, sha512)", fn, name);
			return nullptr;
		}

		bool RawOption(lua_State* L, int index)
		{
			if (lua_isnoneornil(L, index))
				return false;
			luaL_checktype(L, index, LUA_TTABLE);
			lua_getfield(L, index, "raw");
			const bool raw = lua_toboolean(L, -1);
			lua_pop(L, 1);
			return raw;
		}

		int PushDigest(lua_State* L, const unsigned char* digest, size_t size, bool raw)
		{
			if (raw)
			{
				lua_pushlstring(L, reinterpret_cast<const char*>(digest), size);
				return 1;
			}
			static const char kHex[] = "0123456789abcdef";
			std::string hex(size * 2, '0');
			for (size_t i = 0; i < size; i++)
			{
				hex[2 * i] = kHex[digest[i] >> 4];
				hex[2 * i + 1] = kHex[digest[i] & 15];
			}
			lua_pushlstring(L, hex.data(), hex.size());
			return 1;
		}

		int CryptoHash(lua_State* L)
		{
			StartMbedtls(); // on first use, on the script's thread
			const mbedtls_md_info_t* info = Algorithm(L, 1, "crypto.hash");
			const std::string_view data = Bytes(L, 2);
			unsigned char digest[MBEDTLS_MD_MAX_SIZE];
			if (mbedtls_md(info, reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest) != 0)
				luaL_error(L, "crypto.hash: the hash failed");
			return PushDigest(L, digest, mbedtls_md_get_size(info), RawOption(L, 3));
		}

		int CryptoHmac(lua_State* L)
		{
			StartMbedtls(); // on first use, on the script's thread
			const mbedtls_md_info_t* info = Algorithm(L, 1, "crypto.hmac");
			const std::string_view key = Bytes(L, 2);
			const std::string_view data = Bytes(L, 3);
			unsigned char digest[MBEDTLS_MD_MAX_SIZE];
			if (mbedtls_md_hmac(info, reinterpret_cast<const unsigned char*>(key.data()), key.size(), reinterpret_cast<const unsigned char*>(data.data()),
					data.size(), digest) != 0)
				luaL_error(L, "crypto.hmac: the hash failed");
			return PushDigest(L, digest, mbedtls_md_get_size(info), RawOption(L, 4));
		}

		// ---- a hash fed in parts -------------------------------------------

		struct HasherBox
		{
			mbedtls_md_context_t Context;
			bool Finished;
		};

		HasherBox& CheckHasher(lua_State* L, const char* fn)
		{
			HasherBox* box = static_cast<HasherBox*>(luaL_checkudata(L, 1, kHasherMeta));
			if (box->Finished)
				luaL_error(L, "%s: the hasher has given its digest already", fn);
			return *box;
		}

		int HasherUpdate(lua_State* L)
		{
			HasherBox& box = CheckHasher(L, "hasher:update");
			const std::string_view data = Bytes(L, 2);
			mbedtls_md_update(&box.Context, reinterpret_cast<const unsigned char*>(data.data()), data.size());
			lua_settop(L, 1);
			return 1;
		}

		int HasherDigest(lua_State* L)
		{
			HasherBox& box = CheckHasher(L, "hasher:digest");
			unsigned char digest[MBEDTLS_MD_MAX_SIZE];
			mbedtls_md_finish(&box.Context, digest);
			box.Finished = true;
			return PushDigest(L, digest, mbedtls_md_get_size(mbedtls_md_info_from_ctx(&box.Context)), RawOption(L, 2));
		}

		int CryptoHasher(lua_State* L)
		{
			StartMbedtls(); // on first use, on the script's thread
			const mbedtls_md_info_t* info = Algorithm(L, 1, "crypto.hasher");
			HasherBox* box = static_cast<HasherBox*>(lua_newuserdatadtor(L, sizeof(HasherBox), [](lua_State*, void* p) {
				mbedtls_md_free(&static_cast<HasherBox*>(p)->Context);
			}));
			mbedtls_md_init(&box->Context);
			box->Finished = false;
			luaL_getmetatable(L, kHasherMeta);
			lua_setmetatable(L, -2);
			if (mbedtls_md_setup(&box->Context, info, 0) != 0 || mbedtls_md_starts(&box->Context) != 0)
				luaL_error(L, "crypto.hasher: cannot start the hash");
			return 1;
		}

		// ---- randomness ----------------------------------------------------

		bool Random(void* out, size_t size)
		{
#ifdef _WIN32
			return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__)
			arc4random_buf(out, size);
			return true;
#else
			unsigned char* at = static_cast<unsigned char*>(out);
			while (size > 0)
			{
				const ssize_t n = getrandom(at, size, 0);
				if (n < 0)
					return false;
				at += n;
				size -= (size_t)n;
			}
			return true;
#endif
		}

		int CryptoRandomBytes(lua_State* L)
		{
			const double n = luaL_checknumber(L, 1);
			const int count = n == std::floor(n) && n >= 0 && n <= (1 << 26) ? (int)n : -1;
			if (count < 0)
				luaL_argerror(L, 1, "a number of bytes from 0 to 64 MiB");
			std::string bytes((size_t)count, '\0');
			if (count > 0 && !Random(bytes.data(), bytes.size()))
				luaL_error(L, "crypto.randomBytes: the system gave no random bytes");
			lua_pushlstring(L, bytes.data(), bytes.size());
			return 1;
		}

		// A random (version 4) UUID: 8-4-4-4-12 hex digits.
		int CryptoUuid(lua_State* L)
		{
			unsigned char b[16];
			if (!Random(b, sizeof(b)))
				luaL_error(L, "crypto.uuid: the system gave no random bytes");
			b[6] = (unsigned char)((b[6] & 0x0F) | 0x40);
			b[8] = (unsigned char)((b[8] & 0x3F) | 0x80);
			char text[37];
			std::snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5], b[6],
				b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
			lua_pushstring(L, text);
			return 1;
		}

		// Equal strings, compared in a time that does not depend on where
		// they first differ (for secrets: tokens, signatures).
		int CryptoEqual(lua_State* L)
		{
			const std::string_view a = Bytes(L, 1);
			const std::string_view b = Bytes(L, 2);
			unsigned char difference = a.size() == b.size() ? 0 : 1;
			const size_t n = std::max(a.size(), b.size());
			for (size_t i = 0; i < n; i++)
				difference |= (unsigned char)((i < a.size() ? a[i] : 0) ^ (i < b.size() ? b[i] : 0));
			lua_pushboolean(L, difference == 0);
			return 1;
		}

	} // namespace

	void OpenCrypto(lua_State* L)
	{
		luaL_newmetatable(L, kHasherMeta);
		lua_pushstring(L, "Hasher");
		lua_setfield(L, -2, "__type");
		lua_newtable(L);
		lua_pushcfunction(L, HasherUpdate, "update");
		lua_setfield(L, -2, "update");
		lua_pushcfunction(L, HasherDigest, "digest");
		lua_setfield(L, -2, "digest");
		lua_setfield(L, -2, "__index");
		lua_pop(L, 1);

		lua_newtable(L);
		const luaL_Reg functions[] = { { "hash", CryptoHash }, { "hmac", CryptoHmac }, { "hasher", CryptoHasher }, { "randomBytes", CryptoRandomBytes },
			{ "uuid", CryptoUuid }, { "equal", CryptoEqual }, { nullptr, nullptr } };
		for (const luaL_Reg* f = functions; f->name; f++)
		{
			lua_pushcfunction(L, f->func, f->name);
			lua_setfield(L, -2, f->name);
		}
	}

} // namespace cluau::runtime::detail
