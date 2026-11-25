// Fuzz targets (libFuzzer) for what reads untrusted input: the translator
// and the formatter, and the standard library's decoders and parsers. Each
// target is this file built with CLUAU_FUZZ_TARGET set to its name (see
// CMakeLists.txt, CLUAU_FUZZ). An error the input causes is fine; a crash,
// a hang or a sanitizer's report is a bug.
//
//   cmake -S . -B build-fuzz -DCLUAU_FUZZ=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz
//   build-fuzz/fuzz_gunzip -max_total_time=60 -max_len=4096

#include "cluau/cluau.h"
#include "cluau/format.h"
#include "cluau/runtime.h"

#include "Luau/Compiler.h"
#include "lua.h"
#include "lualib.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace {

	// The Luau each target runs: a function of the input (errors caught).
	const char* Script()
	{
		const std::string target = CLUAU_FUZZ_TARGET;
		if (target == "gunzip")
			return "local c = require('@std/compress') return function(s) pcall(c.gunzip, s) end";
		if (target == "inflate")
			return "local c = require('@std/compress') return function(s) pcall(c.inflate, s) pcall(c.inflate, s, { raw = true }) end";
		if (target == "zip")
			return "local zip = require('@std/zip') return function(s)\n"
				   "  local ok, archive = pcall(zip.fromString, s)\n"
				   "  if not ok then return end\n"
				   "  local listed, entries = pcall(archive.entries, archive)\n"
				   "  if not listed then return end\n"
				   "  for _, e in entries do if e.size < 1048576 then pcall(archive.read, archive, e.name) end end\n"
				   "end";
		if (target == "base64")
			return "local e = require('@std/encoding') return function(s) pcall(e.fromBase64, s) pcall(e.fromHex, s) end";
		if (target == "json")
			return "local j = require('@std/json') return function(s) local ok, v = pcall(j.decode, s) if ok then pcall(j.encode, v) end end";
		if (target == "csv")
			return "local c = require('@std/csv') return function(s) pcall(c.parse, s) pcall(c.records, s) end";
		if (target == "regex")
			return "local r = require('@std/regex') return function(s)\n"
				   "  local cut = string.find(s, '\\n', 1, true) or #s + 1\n"
				   "  local ok, re = pcall(r.compile, string.sub(s, 1, cut - 1), 'i')\n"
				   "  if not ok then return end\n"
				   "  local subject = string.sub(s, cut + 1)\n"
				   "  pcall(function() for m in re:matches(subject) do end end)\n"
				   "  pcall(re.replace, re, subject, '[$0]')\n"
				   "  pcall(re.split, re, subject)\n"
				   "end";
		return nullptr;
	}

	struct Target
	{
		std::unique_ptr<cluau::runtime::Runtime> Runtime;
		int Function = 0;
	};

	Target& Load()
	{
		static Target target = [] {
			Target t;
			cluau::runtime::Options options;
			options.Native = false;
			t.Runtime = std::make_unique<cluau::runtime::Runtime>(options);
			const char* source = Script();
			if (!source)
				return t;
			lua_State* L = t.Runtime->State();
			const std::string bytecode = Luau::compile(source);
			if (luau_load(L, "=fuzz", bytecode.data(), bytecode.size(), 0) != 0 || lua_pcall(L, 0, 1, 0) != 0)
			{
				std::fprintf(stderr, "fuzz: the target's script failed: %s\n", lua_tostring(L, -1));
				std::abort();
			}
			t.Function = lua_ref(L, -1);
			lua_pop(L, 1);
			return t;
		}();
		return target;
	}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const std::string input(reinterpret_cast<const char*>(data), size);
	const std::string target = CLUAU_FUZZ_TARGET;
	if (target == "translate")
	{
		cluau::TranslateOptions script;
		script.Script = true;
		(void)cluau::Translate(input);
		(void)cluau::Translate(input, script);
		(void)cluau::Format(input);
		return 0;
	}
	Target& t = Load();
	if (!t.Function)
		return 0;
	lua_State* L = t.Runtime->State();
	lua_getref(L, t.Function);
	lua_pushlstring(L, input.data(), input.size());
	if (lua_pcall(L, 1, 0, 0) != 0)
		lua_pop(L, 1);
	lua_gc(L, LUA_GCCOLLECT, 0);
	return 0;
}
