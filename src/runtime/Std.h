#pragma once

// The standard modules `require("@std/<name>")` returns, and the globals
// scripts get beyond Luau's own libraries (`input`, `repr`).

#include <map>
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

	// The standard modules written in cluau (src/runtime/std/<name>.cluau),
	// by name: loaded on their first require, typed from their source.
	const std::map<std::string, std::string>& StdSources();

	// The value at `index` as JSON text (@std/json's encoding), for
	// @std/http's `json` option.
	std::string JsonEncodeValue(lua_State* L, int index);

	// A Ctrl-C is waiting to stop the script (a long transfer checks).
	bool InterruptPending();

	// Pushes @std/http's table (when the runtime is built with it).
	void OpenHttp(lua_State* L);

	// After a read of standard input came back empty-handed: when a Ctrl-C
	// stopped it, raises the `interrupted` error (and does not return).
	void RaiseIfInterrupted(lua_State* L);

} // namespace cluau::runtime::detail
