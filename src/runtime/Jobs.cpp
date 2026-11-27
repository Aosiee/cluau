#include "Jobs.h"
#include "Debugger.h"
#include "Profile.h"
#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cluau::runtime::detail {

	namespace {

		constexpr const char* kTasksRegistry = "_cluau_task_threads"; // @std/task's tasks, by thread
		constexpr const char* kRunUntilRegistry = "_cluau_task_run_until_job";

		int g_Marker = 0; // its address marks a yield that waits on a job

		struct Job
		{
			// The runtime whose script waits on it (its state's main
			// thread): the jobs are the process's, and pending() counts a
			// runtime's own (a worker's tasks do not wait on its parent's).
			lua_State* Owner = nullptr;
			bool Done = false;
			JobResult Result;
			JobUnclaimed Unclaimed; // when nothing takes the results
		};

		struct Jobs
		{
			std::mutex Mutex;
			std::condition_variable Finished;
			std::map<int, std::shared_ptr<Job>> All;
			int Next = 1;
			unsigned Completed = 0; // bumps on each finish (wait's wake-up; wraps)
		};

		// Never destroyed: a job's thread is detached and can still be
		// finishing (and notifying) while the process exits.
		Jobs& State()
		{
			static Jobs* jobs = new Jobs;
			return *jobs;
		}

		// The error a job's work threw (out of memory, a thread that could
		// not be made), raised on the script's thread instead.
		JobResult Failed(std::string message)
		{
			return [message = std::move(message)](lua_State* L) -> int {
				luaL_error(L, "%s", message.c_str());
				return 0;
			};
		}

		int Start(lua_State* L, JobWork work, JobUnclaimed unclaimed)
		{
			Jobs& jobs = State();
			auto job = std::make_shared<Job>();
			job->Owner = lua_mainthread(L);
			job->Unclaimed = std::move(unclaimed);
			int id = 0;
			{
				std::lock_guard<std::mutex> lock(jobs.Mutex);
				id = jobs.Next++;
				jobs.All[id] = job;
			}
			try
			{
				std::thread([job, work = std::move(work)] {
					JobResult result;
					try
					{
						result = work();
					}
					catch (const std::bad_alloc&)
					{
						result = Failed("not enough memory");
					}
					catch (const std::exception& e)
					{
						result = Failed(e.what());
					}
					catch (...)
					{
						result = Failed("the work failed");
					}
					Jobs& jobs = State();
					{
						std::lock_guard<std::mutex> lock(jobs.Mutex);
						job->Result = std::move(result);
						job->Done = true;
						jobs.Completed++;
					}
					jobs.Finished.notify_all();
				}).detach();
			}
			catch (...)
			{
				// No thread: the job would never finish.
				std::lock_guard<std::mutex> lock(jobs.Mutex);
				jobs.All.erase(id);
				throw;
			}
			return id;
		}

		// Drops the job (its thread finishes on its own); what it got, or
		// will get, goes back where it came from.
		void Drop(int id)
		{
			Jobs& jobs = State();
			JobUnclaimed unclaimed;
			{
				std::lock_guard<std::mutex> lock(jobs.Mutex);
				auto it = jobs.All.find(id);
				if (it == jobs.All.end())
					return;
				unclaimed = std::move(it->second->Unclaimed);
				jobs.All.erase(it);
			}
			if (unclaimed)
				unclaimed();
		}

		// The job's results, pushed (waiting for it, if it is not done).
		int Take(lua_State* L, int id)
		{
			Jobs& jobs = State();
			std::shared_ptr<Job> job;
			{
				std::unique_lock<std::mutex> lock(jobs.Mutex);
				auto it = jobs.All.find(id);
				if (it == jobs.All.end())
					luaL_error(L, "no such job (%d)", id);
				job = it->second;
				jobs.Finished.wait(lock, [&] { return job->Done; });
				jobs.All.erase(it);
			}
			return job->Result(L);
		}

		bool IsTask(lua_State* L)
		{
			lua_getfield(L, LUA_REGISTRYINDEX, kTasksRegistry);
			if (!lua_istable(L, -1))
			{
				lua_pop(L, 1);
				return false;
			}
			lua_pushthread(L);
			lua_rawget(L, -2);
			const bool task = !lua_isnil(L, -1);
			lua_pop(L, 2);
			return task;
		}

		int JobsDone(lua_State* L)
		{
			const int id = (int)luaL_checkinteger(L, 1);
			Jobs& jobs = State();
			std::lock_guard<std::mutex> lock(jobs.Mutex);
			auto it = jobs.All.find(id);
			lua_pushboolean(L, it == jobs.All.end() || it->second->Done);
			return 1;
		}

		int JobsPending(lua_State* L)
		{
			Jobs& jobs = State();
			lua_State* owner = lua_mainthread(L);
			std::lock_guard<std::mutex> lock(jobs.Mutex);
			int pending = 0;
			for (const auto& [id, job] : jobs.All)
				pending += job->Done || job->Owner != owner ? 0 : 1;
			lua_pushinteger(L, pending);
			return 1;
		}

		// How many jobs have finished so far: a later wait(seconds, since)
		// returns at once when one has finished since.
		int JobsCompleted(lua_State* L)
		{
			Jobs& jobs = State();
			std::lock_guard<std::mutex> lock(jobs.Mutex);
			lua_pushnumber(L, (double)jobs.Completed);
			return 1;
		}

		// Until a job finishes (after `since`, a count from completed(); none:
		// from the call on), or `seconds` pass (none, or too many to count:
		// no limit); a Ctrl-C stops the wait.
		int JobsWait(lua_State* L)
		{
			const double seconds = luaL_optnumber(L, 1, HUGE_VAL);
			if (!(seconds > 0)) // none left (or NaN)
				return 0;
			const bool limited = seconds < 1e9;
			const auto until = std::chrono::steady_clock::now() +
				std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(limited ? seconds : 0));
			Jobs& jobs = State();
			std::unique_lock<std::mutex> lock(jobs.Mutex);
			const unsigned seen = lua_isnoneornil(L, 2) ? jobs.Completed : (unsigned)luaL_checknumber(L, 2);
			while (jobs.Completed == seen)
			{
				auto step = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
				if (limited && until < step)
					step = until;
				jobs.Finished.wait_until(lock, step);
				if (InterruptPending())
				{
					lock.unlock();
					RaiseIfInterrupted(L);
					return 0;
				}
				lock.unlock();
				DebugWaiting(L); // a debugger's requests are read meanwhile
				lock.lock();
				if (limited && std::chrono::steady_clock::now() >= until)
					break;
			}
			return 0;
		}

		int JobsForget(lua_State* L)
		{
			Drop((int)luaL_checkinteger(L, 1));
			return 0;
		}

		int JobsRegister(lua_State* L)
		{
			luaL_checktype(L, 1, LUA_TTABLE);
			luaL_checktype(L, 2, LUA_TFUNCTION);
			lua_pushvalue(L, 1);
			lua_setfield(L, LUA_REGISTRYINDEX, kTasksRegistry);
			lua_pushvalue(L, 2);
			lua_setfield(L, LUA_REGISTRYINDEX, kRunUntilRegistry);
			return 0;
		}

	} // namespace

	int Await(lua_State* L, JobWork work, JobUnclaimed unclaimed)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kRunUntilRegistry);
		const bool tasks = lua_isfunction(L, -1);
		if (!tasks && DebuggerAttached(L))
		{
			// Under the debugger the call runs on a job's thread while this
			// one reads the editor's requests (a pause, breakpoints, the end).
			lua_pop(L, 1);
			const int id = Start(L, std::move(work), std::move(unclaimed));
			Jobs& jobs = State();
			for (;;)
			{
				{
					std::unique_lock<std::mutex> lock(jobs.Mutex);
					const std::shared_ptr<Job> job = jobs.All[id];
					if (jobs.Finished.wait_for(lock, std::chrono::milliseconds(100), [&] { return job->Done; }))
						break;
				}
				try
				{
					DebugWaiting(L);
				}
				catch (...)
				{
					Drop(id); // its thread finishes on its own
					throw;
				}
			}
			return Take(L, id);
		}
		if (!tasks || (IsTask(L) && !lua_isyieldable(L)))
		{
			lua_pop(L, 1);
			JobResult finish = work();
			SampleBlocking(L); // the profiler's sample, with the calling line
			return finish(L);
		}
		const int id = Start(L, std::move(work), std::move(unclaimed));
		if (IsTask(L))
		{
			lua_pop(L, 1);
			lua_pushlightuserdata(L, &g_Marker);
			lua_pushinteger(L, id);
			return lua_yield(L, 2);
		}
		// The script's own code: the tasks run until the job is done. A
		// task's error (or a Ctrl-C) can end that wait: the job is dropped
		// then, or each later wait would find it done and return at once.
		lua_pushinteger(L, id);
		if (lua_pcall(L, 1, 0, 0) != LUA_OK)
		{
			Drop(id);
			lua_error(L);
		}
		return Take(L, id);
	}

	int AwaitContinue(lua_State* L, int)
	{
		// @std/task resumes the task with the job's id.
		return Take(L, (int)luaL_checkinteger(L, -1));
	}

	void OpenJobs(lua_State* L)
	{
		lua_newtable(L);
		lua_pushlightuserdata(L, &g_Marker);
		lua_setfield(L, -2, "marker");
		const luaL_Reg functions[] = { { "done", JobsDone }, { "pending", JobsPending }, { "wait", JobsWait }, { "forget", JobsForget },
			{ "completed", JobsCompleted },
			{ "register", JobsRegister }, { nullptr, nullptr } };
		for (const luaL_Reg* f = functions; f->name; f++)
		{
			lua_pushcfunction(L, f->func, f->name);
			lua_setfield(L, -2, f->name);
		}
	}

} // namespace cluau::runtime::detail
