#include "Jobs.h"
#include "Debugger.h"
#include "Profile.h"
#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace cluau::runtime::detail {

	namespace {

		constexpr const char* kTasksRegistry = "_cluau_task_threads"; // @std/task's tasks, by thread
		constexpr const char* kRunUntilRegistry = "_cluau_task_run_until_job";

		int g_Marker = 0; // its address marks a yield that waits on a job

		struct Job
		{
			bool Done = false;
			JobResult Result;
		};

		struct Jobs
		{
			std::mutex Mutex;
			std::condition_variable Finished;
			std::map<int, std::shared_ptr<Job>> All;
			int Next = 1;
			int Completed = 0; // bumps on each finish (wait's wake-up)
		};

		// Never destroyed: a job's thread is detached and can still be
		// finishing (and notifying) while the process exits.
		Jobs& State()
		{
			static Jobs* jobs = new Jobs;
			return *jobs;
		}

		int Start(JobWork work)
		{
			Jobs& jobs = State();
			auto job = std::make_shared<Job>();
			int id = 0;
			{
				std::lock_guard<std::mutex> lock(jobs.Mutex);
				id = jobs.Next++;
				jobs.All[id] = job;
			}
			std::thread([job, work = std::move(work)] {
				JobResult result = work();
				Jobs& jobs = State();
				{
					std::lock_guard<std::mutex> lock(jobs.Mutex);
					job->Result = std::move(result);
					job->Done = true;
					jobs.Completed++;
				}
				jobs.Finished.notify_all();
			}).detach();
			return id;
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
			std::lock_guard<std::mutex> lock(jobs.Mutex);
			int pending = 0;
			for (const auto& [id, job] : jobs.All)
				pending += job->Done ? 0 : 1;
			lua_pushinteger(L, pending);
			return 1;
		}

		// Until a job finishes (from the call on), or `seconds` pass (none:
		// no limit); a Ctrl-C stops the wait.
		int JobsWait(lua_State* L)
		{
			const bool limited = !lua_isnoneornil(L, 1);
			const auto until = std::chrono::steady_clock::now() +
				std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(limited ? luaL_checknumber(L, 1) : 0));
			Jobs& jobs = State();
			std::unique_lock<std::mutex> lock(jobs.Mutex);
			const int seen = jobs.Completed;
			bool anyDone = false;
			for (const auto& [id, job] : jobs.All)
				anyDone = anyDone || job->Done;
			while (!anyDone && jobs.Completed == seen)
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
			const int id = (int)luaL_checkinteger(L, 1);
			Jobs& jobs = State();
			std::lock_guard<std::mutex> lock(jobs.Mutex);
			jobs.All.erase(id); // its thread finishes on its own
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

	int Await(lua_State* L, JobWork work)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kRunUntilRegistry);
		const bool tasks = lua_isfunction(L, -1);
		if (!tasks || (IsTask(L) && !lua_isyieldable(L)))
		{
			lua_pop(L, 1);
			JobResult finish = work();
			SampleBlocking(L); // the profiler's sample, with the calling line
			return finish(L);
		}
		const int id = Start(std::move(work));
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
			{
				Jobs& jobs = State();
				std::lock_guard<std::mutex> lock(jobs.Mutex);
				jobs.All.erase(id); // its thread finishes on its own
			}
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
			{ "register", JobsRegister }, { nullptr, nullptr } };
		for (const luaL_Reg* f = functions; f->name; f++)
		{
			lua_pushcfunction(L, f->func, f->name);
			lua_setfield(L, -2, f->name);
		}
	}

} // namespace cluau::runtime::detail
