#pragma once

// The standard modules `require("@std/<name>")` returns, and the globals
// scripts get beyond Luau's own libraries (`input`, `repr`).

#include <string>
#include <vector>

struct lua_State;

namespace cluau::runtime::detail {

	// The registry field holding the standard modules ({ fs = ..., ... }).
	inline constexpr const char* kStdRegistry = "_cluau_std";

	// Builds `@std/fs`, `@std/path`, `@std/process`, `@std/io` and
	// `@std/json` into the registry, and sets the globals `input` and `repr`.
	void OpenStd(lua_State* L, const std::vector<std::string>& args);

	// `process.script`: the running script's path ("-e", "stdin" for source
	// text, "" in the REPL).
	void SetScript(lua_State* L, const std::string& script);

	// The standard modules' names, for error messages ("fs, io, ...").
	const char* StdNames();

} // namespace cluau::runtime::detail
