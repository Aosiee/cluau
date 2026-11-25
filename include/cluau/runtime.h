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

		lua_State* State() const;

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

} // namespace cluau::runtime
