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

	// Builds the standard modules written in C++ into the registry, and
	// sets the global `input`. In the sandbox (`sandbox`), @std/fs and
	// @std/http are left out and @std/process keeps only `args`,
	// `platform`, `script`, `exit` and `sleep`.
	void OpenStd(lua_State* L, const std::vector<std::string>& args, bool sandbox);

	// The standard modules the sandbox does without (no file or network
	// access, no programs run): the name after `@std/`.
	bool SandboxLeavesOut(const std::string& name);

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
	// Pushes @std/regex's table (when the runtime is built with it).
	void OpenRegex(lua_State* L);

	// Pushes @std/net's table (TCP connections and servers).
	void OpenNet(lua_State* L);

	// Pushes @std/sqlite's table (when the runtime is built with it).
	void OpenSqlite(lua_State* L);

	// Pushes @std/crypto's table (when the runtime is built with it).
	void OpenCrypto(lua_State* L);

	// Pushes @std/encoding's table (base64, hex).
	void OpenEncoding(lua_State* L);

	// Pushes @std/compress's and @std/zip's tables (when the runtime is built with them).
	void OpenCompress(lua_State* L);
	void OpenZip(lua_State* L);

	// After a read of standard input came back empty-handed: when a Ctrl-C
	// stopped it, raises the `interrupted` error (and does not return).
	void RaiseIfInterrupted(lua_State* L);

	// Called with the exit status when a script calls `process.exit`,
	// before the process ends (the debugger reports the end); null for none.
	void SetExitHook(void (*hook)(int code));

} // namespace cluau::runtime::detail
