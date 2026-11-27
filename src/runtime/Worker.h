#pragma once

// @std/worker: scripts that run in parallel, each in a runtime (a Luau VM)
// of its own on a thread of its own. Values cross between a worker and the
// script that started it by copy.
//
// Worker.cpp has the module, the copies and the threads; Runtime.cpp runs a
// worker's script (RunWorker) and, at the end of a run, waits for the
// workers it started (FinishWorkers) or stops them (StopWorkers).

#include "cluau/runtime.h"

#include <functional>
#include <string>

struct lua_State;

namespace cluau::runtime::detail {

	// Pushes @std/worker's table.
	void OpenWorker(lua_State* L);

	// ---- what Runtime.cpp does for a worker ------------------------------

	// What a worker's runtime is made with: its script (an absolute path)
	// and its options, from the starting runtime's.
	struct WorkerLaunch
	{
		std::string File;
		std::string Name; // the file as errors show it
		Options Opts;
	};

	// The file worker.start(path) names, resolved as `require("./path")`
	// resolves from the calling chunk's directory (a `.cluau` or `.luau`
	// extension may be left off), and the options for its runtime. Raises
	// an error (naming `fn`) when there is no such file.
	WorkerLaunch PrepareWorker(lua_State* L, const std::string& path, const char* fn);

	// How a worker's script ended, when it failed: the message, the stack
	// (the frames' text, "" for none), and whether a task's stack it is.
	struct WorkerFailure
	{
		bool Failed = false;
		std::string Message;
		std::string Trace;
		bool InTask = false;
	};

	// Runs the worker's script in a runtime of its own on the calling
	// thread, as `cluau script` would, its tasks and its own workers to
	// their end; an error is given in `failure` instead of being printed.
	// `prepare` gets the runtime's state before the script runs; `results`
	// gets the thread holding what the script returned, and gives back ""
	// or why they cannot be kept (which fails the worker).
	void RunWorker(const WorkerLaunch& launch, const std::function<void(lua_State*)>& prepare,
		const std::function<std::string(lua_State*)>& results, WorkerFailure& failure);

	// ---- the end of a run -------------------------------------------------

	// The runtime on L has started workers that it has not yet waited for.
	bool HasWorkers(lua_State* L);

	// A lua_CFunction, run once the script's code and its tasks are done:
	// the workers still running are told nothing more comes from the
	// script (their receive gives nil once their messages are read) and
	// waited for. It returns the error text of the first failure that
	// nothing joined, which fails the run, or nothing.
	int FinishWorkers(lua_State* L);

	// After a run failed: the workers it started are terminated, and their
	// threads waited for. No Lua error is raised.
	void StopWorkers(lua_State* L);

	// ---- stopping ---------------------------------------------------------

	// On a worker's thread: why its script is to stop ("interrupted" after
	// a Ctrl-C, or worker:terminate's or process.exit's message), else null.
	// Off a worker's thread (the main script's, a job's): null.
	const char* WorkerStopping();
	// The calling thread is a worker's (the one its script runs on).
	bool InWorker();

	// process.exit in a worker: ends the worker's script with that status
	// (it does not return). Elsewhere it returns at once.
	void WorkerExit(lua_State* L, int status);
	// process.exit in the main script: every worker in the process is
	// stopped at its next safepoint, and waited for, before the process
	// ends.
	void StopAllWorkers();

} // namespace cluau::runtime::detail
