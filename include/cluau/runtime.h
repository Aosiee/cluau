#pragma once

// cluau_runtime - runs cluau as a scripting language, the way `python
// script.py` runs Python: a .cluau file is translated (script mode), compiled
// and run on Luau's VM, with `require` for .cluau / .luau modules and the
// standard modules (`@std/fs`, `@std/path`, `@std/process`, `@std/io`,
// `@std/json`). The translator (cluau.h) does not need it; the runtime builds
// against Luau's VM and compiler.
//
// Errors are reported as `file:line: message` with a stack backtrace; the
// translation keeps every line on its line, so the lines are the .cluau
// file's.

#include "cluau/cluau.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace cluau::runtime {

	struct Options
	{
		// `process.args`: the script's own arguments (not the script's path).
		std::vector<std::string> Args;
		// Directories a bare `require("name")` searches after the main
		// script's directory (the CLUAU_PATH environment variable's entries
		// are added by the command line).
		std::vector<std::string> SearchPath;
		// Ctrl-C (SIGINT) stops the running script with an `interrupted`
		// error instead of ending the process (a second one ends it). The
		// command line sets it; a host that handles SIGINT itself leaves it.
		bool CatchInterrupt = false;
		// Scripts and modules compile to native code (Luau's code generator,
		// on x64 and arm64; elsewhere they are interpreted either way).
		bool Native = true;
		// The sandbox: no file, network or process access. @std/fs and
		// @std/http are not available, @std/process has only `args`,
		// `platform`, `script`, `exit` and `sleep`, and `require` loads
		// modules only from under the main script's directory, its
		// cluau_modules and the search path. Luau itself has no file or
		// process functions, so the script reaches nothing else.
		bool Sandbox = false;
		// Count the times each line runs (`cluau test --coverage`): read
		// back by Coverage(). The code is interpreted.
		bool Coverage = false;
		// Sample where the time goes (`cluau profile`): when the script
		// ends (or calls process.exit) the report goes to stderr, with the
		// `ProfileTop` busiest functions, and the folded stacks to the file
		// `ProfileFolded` names (none for "").
		bool Profile = false;
		int ProfileTop = 20;
		std::string ProfileFolded;
	};

	class Runtime
	{
	public:
		explicit Runtime(Options options = {});
		~Runtime();
		Runtime(const Runtime&) = delete;
		Runtime& operator=(const Runtime&) = delete;

		// Runs a script file. The exit status: 0, or 1 after an error (which
		// is reported on stderr). `process.exit(n)` ends the process itself.
		int RunFile(const std::string& path);
		// Runs source text (`cluau -e`, a script on stdin). `name` is what
		// errors call it ("-e", "stdin"); relative requires resolve from the
		// current directory.
		int RunSource(const std::string& source, const std::string& name);
		// The interactive prompt: each input is translated in REPL mode
		// (top-level `let` names persist), an expression's value is printed.
		int Repl();

		// Runs a test file (`cluau test`): its @std/test cases matching
		// `filter` (a part of their names, any case; "" for all) report as
		// they run. False when the file itself fails (it does not load, or
		// an error escapes its cases).
		struct TestCounts
		{
			int Passed = 0;
			int Failed = 0;
			int Skipped = 0;
		};
		bool RunTest(const std::string& path, const std::string& filter, TestCounts& counts);

		lua_State* State() const;

		// With Options::Coverage: for each file run (its absolute path), its
		// lines with code (1-based) and the times each ran.
		std::map<std::string, std::map<int, int>> Coverage() const;

		// ---- the host's modules ---------------------------------------
		//
		// A host gives its scripts modules of its own beside @std, under an
		// alias of its choosing: `require("@game/world")`. The name is
		// `@<alias>/<name>` (any alias but `std`); registering a name again
		// replaces it. Each is loaded on its first require and kept, as
		// files are. `cluau check` types them through CheckOptions::Modules.
		// False when the name is not of that form.

		// A module made in C++: `open` is called (with no arguments) on the
		// first require and returns the module's value.
		bool AddModule(const std::string& name, int (*open)(lua_State*));
		// A module written in cluau (its source text), run on the first
		// require; errors name it by `name`.
		bool AddModuleSource(const std::string& name, const std::string& source);

		struct Impl; // the runtime's state (Runtime.cpp)

	private:
		std::unique_ptr<Impl> m_Impl;
	};

	// The text `repr(value)` gives: strings quoted, tables shown with their
	// contents (nested tables to a few levels, cycles marked).
	std::string Repr(lua_State* L, int index);

	// ---- checking (`cluau check`) ---------------------------------------

	struct CheckOptions
	{
		// As Options::SearchPath: where bare `require` names are looked for
		// after each checked script's directory.
		std::vector<std::string> SearchPath;
		// Luau's strict mode only, without cluau's strict rules (`any` needs
		// no exemption, parameters need no annotation): `cluau check --loose`.
		bool Loose = false;
		// Luau's lint warnings too (unused locals, shadowing, unreachable
		// code, ...): `cluau check --lint`.
		bool Lint = false;
		// The host's modules (Runtime::AddModule), by name ("@game/world"):
		// each is Luau source whose returned value has the module's type,
		// for instance
		//     export type Entity = { name: string, health: number }
		//     local m: any = nil
		//     return m :: { spawn: (name: string) -> Entity }
		// (types declared with `export type` can be named as
		// `world.Entity`). `cluau check --module <name>=<file>` reads one.
		std::map<std::string, std::string> Modules;
		// Modules written in cluau (Runtime::AddModuleSource): typed from
		// their source, as files are.
		std::map<std::string, std::string> ModuleSources;
	};

	struct CheckFinding
	{
		std::string File;    // as errors show it (relative to the working directory when under it)
		Range At;            // source positions (0-based)
		std::string Code;    // "cluau" (a translation error), "SyntaxError", "TypeError", or a strict rule's code
		std::string Message;
		bool Warning = false;
	};

	// Type-checks scripts without running them: each file and every module
	// it requires (resolved as `require` resolves them at run time), in
	// Luau's strict mode for .cluau files, with the standard modules' and
	// the runtime globals' types, and cluau's strict rules (docs/syntax.md
	// "Strict typing"). A .luau module is checked in its own mode
	// (nonstrict unless it says `--!strict`). The findings are sorted by
	// file and position.
	std::vector<CheckFinding> Check(const std::vector<std::string>& files, const CheckOptions& options = {});

	// ---- the language server (`cluau lsp`) --------------------------------

	// Speaks the Language Server Protocol on standard input and output
	// until the editor ends the session: the checker's findings as the
	// files change, completion, hover, signature help, go to definition,
	// references and renaming of locals, the outline and formatting. The
	// options are the checker's. Returns the exit status.
	int ServeLanguageServer(const CheckOptions& options = {});

	// ---- the debugger (`cluau debug`) -------------------------------------

	// Speaks the Debug Adapter Protocol on standard input and output: the
	// editor launches a script, sets breakpoints (with conditions, hit
	// counts and log messages), and steps through it, reading the paused
	// frames' variables and evaluating expressions in them. The script's
	// output comes to the editor as output events; its standard input is
	// empty. `options` is where each launch starts (the search path); the
	// launch gives the script, its arguments and its directory. Returns the
	// exit status.
	int ServeDebugAdapter(const Options& options = {});

} // namespace cluau::runtime
