#pragma once

// The standard modules `require("@std/<name>")` returns, and the globals
// scripts get beyond Luau's own libraries (`input`, `repr`).

#include <cstddef>
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
	// How many Ctrl-Cs have come so far (never reset): a job's thread
	// compares it with the count when its work began.
	unsigned InterruptCount();

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

	// Pushes @std/term's table (the terminal, colours).
	void OpenTerm(lua_State* L);

	// Pushes @std/random's table (seeded random numbers).
	void OpenRandom(lua_State* L);

	// Pushes @std/_entropy's table: `bytes(count)`, the operating system's
	// random bytes, for the standard modules written in cluau (@std/uuid),
	// in every build (@std/crypto has them only when it is built).
	void OpenEntropy(lua_State* L);

	// Fills `out` with `size` bytes from the operating system's random
	// source (false when it gave none).
	bool SystemRandom(void* out, size_t size);

	// Pushes @std/compress's and @std/zip's tables (when the runtime is built with them).
	void OpenCompress(lua_State* L);
	void OpenZip(lua_State* L);

	// After a read of standard input came back empty-handed: when a Ctrl-C
	// stopped it, raises the `interrupted` error (and does not return).
	void RaiseIfInterrupted(lua_State* L);

	// Called with the exit status when a script calls `process.exit`,
	// before the process ends (the debugger reports the end); null for none.
	void SetExitHook(void (*hook)(int code));

	// A line of standard input without its line end (false at the end, or
	// at a Ctrl-C), from the buffer io.readLine and input() read through.
	bool ReadStandardInputLine(std::string& line);

} // namespace cluau::runtime::detail
