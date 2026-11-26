#pragma once

// Blocking work that lets @std/task's other tasks run meanwhile: a
// download, a program run, a read of standard input, a sleep.
//
// The work runs on a thread of its own and touches no Lua state; what it
// leaves is a function that pushes the results (or raises the error) on
// the script's thread. Where the call is made decides how it waits:
//
// - in a task, the task yields (the call's C function yields, marked as
//   waiting on the job); @std/task resumes it once the job is done, and the
//   function's continuation (AwaitContinue) pushes the results there;
// - in the script's own code, with @std/task loaded, the waiting tasks run
//   until the job is done;
// - otherwise (no @std/task, or a call that cannot yield) the work runs
//   right there, as an ordinary blocking call.

#include <functional>

struct lua_State;

namespace cluau::runtime::detail {

	// Pushes the results on the script's thread (or raises the error);
	// returns how many.
	using JobResult = std::function<int(lua_State*)>;
	// Runs on its own thread: no Lua state.
	using JobWork = std::function<JobResult()>;
	// Called once when the job is dropped (its task cancelled, or the wait
	// for it ended by an error) instead of its results being taken: a read
	// gives back what it got. It may run on the job's thread or the
	// script's, while the work runs or after; nothing else is called then.
	using JobUnclaimed = std::function<void()>;

	// The C function's return: `return Await(L, work);`. The function is
	// registered with AwaitContinue as its continuation.
	int Await(lua_State* L, JobWork work, JobUnclaimed unclaimed = {});
	int AwaitContinue(lua_State* L, int status);

	// Pushes the table @std/task drives jobs with (require("@std/_jobs")):
	// marker (what a waiting call yields first), done(id), pending(),
	// completed() (how many have finished), wait(seconds?, since?) (until a
	// job finishes, after `since` when given, or the time passes),
	// forget(id), register(tasksByThread, runUntilJob).
	void OpenJobs(lua_State* L);

} // namespace cluau::runtime::detail
