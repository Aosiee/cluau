// @std/random: seeded random numbers for simulations, games, tests and
// sampling (not for secrets: @std/crypto's randomBytes is). The generator
// is xoshiro256** (Blackman and Vigna), its state filled from a seed by
// splitmix64, so a seed gives the same numbers on every platform. The
// module's functions share one generator, seeded from the operating
// system on first use; random.new makes generators of their own.
//
// Everything is computed from the generator's 64-bit outputs with integer
// arithmetic or IEEE operations that round the same way everywhere (no
// fused multiply-add the compiler may or may not form: where one is
// wanted, std::fma is written out). The one library function, the
// logarithm in `normal`, only decides whether a pair of draws is kept.

#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace cluau::runtime::detail {

	bool SystemRandom(void* out, size_t size)
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
			if (n < 0 && errno == EINTR)
				continue;
			if (n < 0)
				return false;
			at += n;
			size -= (size_t)n;
		}
		return true;
#endif
	}

	namespace {

		constexpr const char* kRandomMeta = "cluau.Random";

		// 2^53: past it, not every whole number is a double.
		constexpr double kExact = 9007199254740992.0;

		struct Generator
		{
			uint64_t State[4];
			// The shared generator is seeded from the system on first use.
			bool Seeded;
		};

		uint64_t SplitMix64(uint64_t& x)
		{
			uint64_t z = (x += 0x9e3779b97f4a7c15ULL);
			z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
			z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
			return z ^ (z >> 31);
		}

		uint64_t Rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

		// xoshiro256**'s next output.
		uint64_t Next(Generator& g)
		{
			uint64_t* s = g.State;
			const uint64_t result = Rotl(s[1] * 5, 7) * 9;
			const uint64_t t = s[1] << 17;
			s[2] ^= s[0];
			s[3] ^= s[1];
			s[1] ^= s[2];
			s[0] ^= s[3];
			s[2] ^= t;
			s[3] = Rotl(s[3], 45);
			return result;
		}

		// Four splitmix64 outputs, which are never all zero (the one state
		// xoshiro cannot leave).
		void SeedWith(Generator& g, uint64_t seed)
		{
			for (uint64_t& word : g.State)
				word = SplitMix64(seed);
			g.Seeded = true;
		}

		void SeedFromSystem(lua_State* L, Generator& g, const char* fn)
		{
			uint64_t words[4] = {};
			if (!SystemRandom(words, sizeof(words)))
				luaL_error(L, "%s: the system gave no random bytes to seed the generator", fn);
			if ((words[0] | words[1] | words[2] | words[3]) == 0)
				SeedWith(g, 0);
			else
				std::copy(words, words + 4, g.State);
			g.Seeded = true;
		}

		// A number as Luau prints it, for an error message.
		std::string Shown(lua_State* L, double value)
		{
			lua_pushnumber(L, value);
			std::string text = lua_tostring(L, -1);
			lua_pop(L, 1);
			return text;
		}

		// In [0, 1), from the top 53 bits: every value a multiple of 2^-53.
		double Float(Generator& g) { return (double)(Next(g) >> 11) * (1.0 / kExact); }

		// Uniform in [0, n), for 1 <= n <= 2^53: the fewest top bits that can
		// hold n - 1, drawn again while they are n or more (never biased, as
		// taking a remainder would be; at most two draws on average).
		uint64_t Below(Generator& g, uint64_t n)
		{
			if (n <= 1)
				return 0;
			int bits = 0;
			for (uint64_t top = n - 1; top != 0; top >>= 1)
				bits++;
			for (;;)
			{
				const uint64_t x = Next(g) >> (64 - bits);
				if (x < n)
					return x;
			}
		}

		// The generator a call works on, and where its own arguments start:
		// a module function's is the shared one (its upvalue), a method's
		// is its first argument.
		struct Call
		{
			Generator& G;
			int At;
		};

		Call Begin(lua_State* L, const char* fn)
		{
			if (lua_type(L, lua_upvalueindex(1)) == LUA_TUSERDATA)
			{
				Generator& shared = *static_cast<Generator*>(lua_touserdata(L, lua_upvalueindex(1)));
				if (!shared.Seeded)
					SeedFromSystem(L, shared, fn);
				return { shared, 1 };
			}
			return { *static_cast<Generator*>(luaL_checkudata(L, 1, kRandomMeta)), 2 };
		}

		// A seed: a whole number from -2^63 to 2^63, negative ones taken as
		// their 64-bit two's complement.
		uint64_t SeedArgument(lua_State* L, int index, const char* fn)
		{
			const double seed = luaL_checknumber(L, index);
			if (seed != std::floor(seed) || seed < -9223372036854775808.0 || seed >= 9223372036854775808.0)
				luaL_error(L, "%s: the seed is a whole number from -2^63 to 2^63, not %s", fn, Shown(L, seed).c_str());
			return (uint64_t)(int64_t)seed;
		}

		double Finite(lua_State* L, int index, const char* fn, const char* what)
		{
			const double value = luaL_checknumber(L, index);
			if (!std::isfinite(value))
				luaL_error(L, "%s: %s is a finite number, not %s", fn, what, Shown(L, value).c_str());
			return value;
		}

		int64_t WholeArgument(lua_State* L, int index, const char* fn, const char* what)
		{
			const double value = luaL_checknumber(L, index);
			if (value != std::floor(value) || std::fabs(value) > kExact)
				luaL_error(L, "%s: %s is a whole number from -2^53 to 2^53, not %s", fn, what, Shown(L, value).c_str());
			return (int64_t)value;
		}

		size_t ListLength(lua_State* L, int index)
		{
			luaL_checktype(L, index, LUA_TTABLE);
			return (size_t)lua_objlen(L, index);
		}

		void PushGenerator(lua_State* L)
		{
			Generator* g = static_cast<Generator*>(lua_newuserdata(L, sizeof(Generator)));
			g->Seeded = false;
			luaL_getmetatable(L, kRandomMeta);
			lua_setmetatable(L, -2);
		}

		// ---- the functions, on the shared generator or a Random ------------

		// new(seed?): a generator of its own, seeded with `seed` (the same
		// numbers every run) or from the system.
		int RandomNew(lua_State* L)
		{
			const bool seeded = !lua_isnoneornil(L, 1);
			const uint64_t seed = seeded ? SeedArgument(L, 1, "random.new") : 0;
			PushGenerator(L);
			Generator& g = *static_cast<Generator*>(lua_touserdata(L, -1));
			if (seeded)
				SeedWith(g, seed);
			else
				SeedFromSystem(L, g, "random.new");
			return 1;
		}

		int RandomSeed(lua_State* L)
		{
			const Call c = Begin(L, "random.seed");
			if (lua_isnoneornil(L, c.At))
				SeedFromSystem(L, c.G, "random.seed");
			else
				SeedWith(c.G, SeedArgument(L, c.At, "random.seed"));
			return 0;
		}

		int RandomRandom(lua_State* L)
		{
			const Call c = Begin(L, "random.random");
			lua_pushnumber(L, Float(c.G));
			return 1;
		}

		// integer(min, max): a whole number from min to max, both included.
		int RandomInteger(lua_State* L)
		{
			const Call c = Begin(L, "random.integer");
			const int64_t low = WholeArgument(L, c.At, "random.integer", "min");
			const int64_t high = WholeArgument(L, c.At + 1, "random.integer", "max");
			if (low > high)
				luaL_error(L, "random.integer: min (%s) is greater than max (%s)", Shown(L, (double)low).c_str(), Shown(L, (double)high).c_str());
			const uint64_t count = (uint64_t)(high - low) + 1;
			if (count > (uint64_t)kExact)
				luaL_error(L, "random.integer: the range from %s to %s holds more than 2^53 numbers", Shown(L, (double)low).c_str(),
					Shown(L, (double)high).c_str());
			lua_pushnumber(L, (double)(low + (int64_t)Below(c.G, count)));
			return 1;
		}

		// uniform(a, b): from a to b. a + (b - a) * u is written as
		// a - u * a + u * b, so a range wider than the largest double still
		// works; rounding can reach b, never past it.
		int RandomUniform(lua_State* L)
		{
			const Call c = Begin(L, "random.uniform");
			const double a = Finite(L, c.At, "random.uniform", "a");
			const double b = Finite(L, c.At + 1, "random.uniform", "b");
			const double u = Float(c.G);
			const double x = std::fma(u, b, std::fma(-u, a, a));
			lua_pushnumber(L, std::clamp(x, std::min(a, b), std::max(a, b)));
			return 1;
		}

		// normal(mean?, deviation?): Kinderman and Monahan's ratio of
		// uniforms (as Python's random.normalvariate). The value is a ratio
		// of exactly rounded operations; the logarithm only decides whether
		// a pair is kept.
		int RandomNormal(lua_State* L)
		{
			const Call c = Begin(L, "random.normal");
			const double mean = lua_isnoneornil(L, c.At) ? 0.0 : Finite(L, c.At, "random.normal", "the mean");
			const double deviation = lua_isnoneornil(L, c.At + 1) ? 1.0 : Finite(L, c.At + 1, "random.normal", "the deviation");
			if (deviation < 0)
				luaL_error(L, "random.normal: the deviation is 0 or more, not %s", Shown(L, deviation).c_str());
			constexpr double kMagic = 1.7155277699214135; // 4 * exp(-1/2) / sqrt(2)
			double z = 0;
			for (;;)
			{
				const double u1 = Float(c.G);
				const double u2 = 1.0 - Float(c.G); // in (0, 1]
				z = kMagic * (u1 - 0.5) / u2;
				if (z * z / 4.0 <= -std::log(u2))
					break;
			}
			lua_pushnumber(L, std::fma(z, deviation, mean));
			return 1;
		}

		int RandomChoice(lua_State* L)
		{
			const Call c = Begin(L, "random.choice");
			const size_t n = ListLength(L, c.At);
			if (n == 0)
				luaL_error(L, "random.choice: the list is empty");
			lua_rawgeti(L, c.At, (int)(1 + Below(c.G, n)));
			return 1;
		}

		// shuffle(list): in place, by Fisher and Yates: each order equally likely.
		int RandomShuffle(lua_State* L)
		{
			const Call c = Begin(L, "random.shuffle");
			const size_t n = ListLength(L, c.At);
			for (size_t i = n; i >= 2; i--)
			{
				const int j = (int)(1 + Below(c.G, i));
				if (j == (int)i)
					continue;
				lua_rawgeti(L, c.At, (int)i);
				lua_rawgeti(L, c.At, j);
				lua_rawseti(L, c.At, (int)i);
				lua_rawseti(L, c.At, j);
			}
			return 0;
		}

		// sample(list, count): `count` items from distinct places, in the
		// order drawn: the first `count` steps of a Fisher-Yates shuffle of
		// the places. For a long list and a small count the places moved are
		// kept in a map instead of a list of them all; the draws are the same.
		int RandomSample(lua_State* L)
		{
			const Call c = Begin(L, "random.sample");
			const size_t n = ListLength(L, c.At);
			const double wanted = luaL_checknumber(L, c.At + 1);
			if (wanted != std::floor(wanted) || wanted < 0)
				luaL_error(L, "random.sample: the count is a whole number from 0, not %s", Shown(L, wanted).c_str());
			if (wanted > (double)n)
				luaL_error(L, "random.sample: the count (%s) is more than the list's %d items", Shown(L, wanted).c_str(), (int)n);
			const size_t count = (size_t)wanted;
			lua_createtable(L, (int)count, 0);
			const int out = lua_gettop(L);
			if (count * 4 >= n)
			{
				std::vector<size_t> places(n);
				for (size_t i = 0; i < n; i++)
					places[i] = i + 1;
				for (size_t i = 0; i < count; i++)
				{
					std::swap(places[i], places[i + Below(c.G, n - i)]);
					lua_rawgeti(L, c.At, (int)places[i]);
					lua_rawseti(L, out, (int)i + 1);
				}
			}
			else
			{
				std::unordered_map<size_t, size_t> moved;
				const auto at = [&](size_t i) {
					const auto it = moved.find(i);
					return it == moved.end() ? i + 1 : it->second;
				};
				for (size_t i = 0; i < count; i++)
				{
					const size_t j = i + Below(c.G, n - i);
					const size_t picked = at(j);
					moved[j] = at(i);
					lua_rawgeti(L, c.At, (int)picked);
					lua_rawseti(L, out, (int)i + 1);
				}
			}
			return 1;
		}

		// weighted(list, weights): an item, each as likely as its weight.
		int RandomWeighted(lua_State* L)
		{
			const Call c = Begin(L, "random.weighted");
			const size_t n = ListLength(L, c.At);
			const size_t weights = ListLength(L, c.At + 1);
			if (n != weights)
				luaL_error(L, "random.weighted: the list has %d items and the weights %d (one weight for each item)", (int)n, (int)weights);
			if (n == 0)
				luaL_error(L, "random.weighted: the list is empty");
			std::vector<double> sums(n);
			double total = 0;
			for (size_t i = 0; i < n; i++)
			{
				lua_rawgeti(L, c.At + 1, (int)i + 1);
				if (lua_type(L, -1) != LUA_TNUMBER)
					luaL_error(L, "random.weighted: weight %d is a %s, not a number", (int)i + 1, luaL_typename(L, -1));
				const double weight = lua_tonumber(L, -1);
				lua_pop(L, 1);
				if (!(weight >= 0) || !std::isfinite(weight))
					luaL_error(L, "random.weighted: weight %d is %s (a weight is a finite number, 0 or more)", (int)i + 1, Shown(L, weight).c_str());
				total += weight;
				sums[i] = total;
			}
			if (!std::isfinite(total))
				luaL_error(L, "random.weighted: the weights add up to more than the largest number");
			if (total == 0)
				luaL_error(L, "random.weighted: every weight is 0");
			const double target = Float(c.G) * total;
			// The first item whose running total passes the target: never one
			// of weight 0, and the last of weight above 0 should rounding
			// leave the target at the total.
			size_t picked = (size_t)(std::upper_bound(sums.begin(), sums.end(), target) - sums.begin());
			if (picked == n)
				picked = (size_t)(std::lower_bound(sums.begin(), sums.end(), total) - sums.begin());
			lua_rawgeti(L, c.At, (int)picked + 1);
			return 1;
		}

		const luaL_Reg kRandom[] = {
			{ "seed", RandomSeed },
			{ "random", RandomRandom },
			{ "integer", RandomInteger },
			{ "uniform", RandomUniform },
			{ "normal", RandomNormal },
			{ "choice", RandomChoice },
			{ "shuffle", RandomShuffle },
			{ "sample", RandomSample },
			{ "weighted", RandomWeighted },
			{ nullptr, nullptr },
		};

	} // namespace

	void OpenRandom(lua_State* L)
	{
		// A Random's methods: the functions with no shared generator.
		luaL_newmetatable(L, kRandomMeta);
		lua_pushstring(L, "Random");
		lua_setfield(L, -2, "__type");
		lua_newtable(L);
		for (const luaL_Reg* f = kRandom; f->name; f++)
		{
			lua_pushboolean(L, false);
			lua_pushcclosure(L, f->func, f->name, 1);
			lua_setfield(L, -2, f->name);
		}
		lua_setfield(L, -2, "__index");
		lua_pop(L, 1);

		lua_newtable(L);
		PushGenerator(L);
		const int shared = lua_gettop(L);
		for (const luaL_Reg* f = kRandom; f->name; f++)
		{
			lua_pushvalue(L, shared);
			lua_pushcclosure(L, f->func, f->name, 1);
			lua_setfield(L, -3, f->name);
		}
		lua_pop(L, 1);
		lua_pushcfunction(L, RandomNew, "new");
		lua_setfield(L, -2, "new");
	}

} // namespace cluau::runtime::detail
