// Fuzz targets (libFuzzer) for what reads untrusted input: the translator
// (in each of its modes; the input's first byte picks one) and the
// formatter, and the standard library's decoders and parsers. Each
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

#include <algorithm>
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

	// A host's profile with every hook (a game engine's, say): attributes
	// on fields, renamed field types, signals as members, inheritance, a
	// class line of its own.
	const cluau::ClassProfile& HostProfile()
	{
		static const cluau::ClassProfile profile = [] {
			cluau::ClassProfile p;
			p.TableInit = "{ Properties = {}, Fields = {} }";
			p.BaseMembers = "Entity: Entity, ";
			p.Inheritance = true;
			p.LowerField = [](const cluau::ClassField& f, const std::string& cls, std::string& error) -> std::string {
				if (f.Name == "Refused")
					error = "a field this host refuses";
				const bool exported = std::find(f.Attributes.begin(), f.Attributes.end(), "export") != f.Attributes.end();
				if (f.Default.empty())
					return std::string();
				return exported ? cls + ".Properties." + f.Name + " = { type = \"" + f.Type + "\", default = " + f.Default + " }"
								: cls + ".Fields." + f.Name + " = " + f.Default;
			};
			p.FieldType = [](const cluau::ClassField& f) { return f.Type.rfind("Ref<", 0) == 0 ? std::string("string") : f.Type; };
			p.SignalType = [](const cluau::ClassSignal& s) { return "Signal<(" + s.Params + ")>"; };
			p.Header = [](const cluau::ClassInfo& info, const cluau::ClassMembers& members) {
				cluau::ClassHeader h;
				h.TableInit = "{ Base = " + (info.Base.empty() ? std::string("nil") : info.Base) + " }";
				h.Types = "export type $Class = { " + members.Fields + "}";
				for (const cluau::ClassMembers::Method& m : members.Methods)
					h.Types += " & { " + m.Name + ": " + m.Generics + "(self: $Class" + m.ParamTypes + ") -> " + m.ReturnType + " }";
				return h;
			};
			return p;
		}();
		return profile;
	}

	size_t LineBreaks(const std::string& text) { return (size_t)std::count(text.begin(), text.end(), '\n'); }

	// What the translator promises for any input: every line of the
	// output is the same line of the source, and the map converts
	// positions within their line.
	void CheckTranslation(const std::string& input, const cluau::TranslateResult& r)
	{
		if (LineBreaks(r.Luau) != LineBreaks(input))
		{
			std::fprintf(stderr, "fuzz: the translation has %zu line breaks, the source %zu\n", LineBreaks(r.Luau), LineBreaks(input));
			std::abort();
		}
		// Every so many bytes of the source, and its end.
		const size_t step = 1 + input.size() / 64;
		int line = 0, col = 0;
		for (size_t i = 0; i <= input.size(); i++)
		{
			if (i % step == 0 || i == input.size())
			{
				const cluau::Position luau = r.Map.ToLuau({ line, col });
				const cluau::Position back = r.Map.FromLuau(luau);
				if (luau.Line != line || back.Line != line || luau.Col < 0 || back.Col < 0)
				{
					std::fprintf(stderr, "fuzz: %d:%d maps to %d:%d and back to %d:%d\n", line, col, luau.Line, luau.Col, back.Line, back.Col);
					std::abort();
				}
			}
			if (i < input.size() && input[i] == '\n')
			{
				line++;
				col = 0;
			}
			else
				col++;
		}
	}

	// The translator in the mode the input's first byte picks: a host's
	// class file (the generic profile, a host's own, a manifest), a script,
	// the REPL's input read line by line (as its check for an unfinished
	// input does), or the formatter.
	void FuzzTranslate(const std::string& all)
	{
		if (all.empty())
			return;
		const std::string input = all.substr(1);
		cluau::TranslateOptions options;
		switch ((unsigned char)all[0] % 6)
		{
		case 0: break;
		case 1:
			options.Script = true;
			options.Profile = &cluau::ScriptProfile();
			break;
		case 2:
		{
			// Each line joins the input until it is no longer unfinished.
			options.Script = true;
			options.Repl = true;
			options.Profile = &cluau::ScriptProfile();
			std::string pending;
			for (size_t from = 0; from < input.size();)
			{
				size_t end = input.find('\n', from);
				end = end == std::string::npos ? input.size() : end + 1;
				pending.append(input, from, end - from);
				from = end;
				const cluau::TranslateResult r = cluau::Translate(pending, options);
				CheckTranslation(pending, r);
				if (!r.Unfinished)
					pending.clear();
			}
			return;
		}
		case 3: options.Manifest = true; break;
		case 4: options.Profile = &HostProfile(); break;
		default:
		{
			// Formatting the formatted text changes nothing, and a script
			// that translates still does once formatted.
			const cluau::FormatResult once = cluau::Format(input);
			if (once.HasErrors())
				return;
			const cluau::FormatResult twice = cluau::Format(once.Text);
			if (twice.HasErrors() || twice.Text != once.Text)
			{
				std::fprintf(stderr, "fuzz: formatting the formatted text changes it again%s\n", twice.HasErrors() ? " (an error)" : "");
				std::abort();
			}
			cluau::TranslateOptions script;
			script.Script = true;
			script.Profile = &cluau::ScriptProfile();
			if (!cluau::Translate(input, script).HasErrors() && cluau::Translate(once.Text, script).HasErrors())
			{
				std::fprintf(stderr, "fuzz: the formatted script does not translate\n");
				std::abort();
			}
			return;
		}
		}
		CheckTranslation(input, cluau::Translate(input, options));
	}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	const std::string input(reinterpret_cast<const char*>(data), size);
	const std::string target = CLUAU_FUZZ_TARGET;
	if (target == "translate")
	{
		FuzzTranslate(input);
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
