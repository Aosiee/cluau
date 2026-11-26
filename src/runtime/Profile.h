#pragma once

// The sampling profiler under `cluau profile`: a timer marks a sample due
// about once a millisecond; at the script's next safepoint (Luau's
// interrupt callback, at calls and loop back-edges) the stack is recorded,
// weighted by the time since the last sample (measured, so a coarse timer
// makes the samples fewer but not the time less). A blocking C call (a
// sleep, a read, a download) takes its sample before it returns, so its
// time goes to the line that made the call.
//
// The report is each function's own time (its code, and the C functions
// it calls) and its total time (with what it calls), and the lines with
// the most time; the folded stacks (`outer;inner;innermost count` per
// line, the count in microseconds) are what flame graph tools read.

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

struct lua_State;

namespace cluau::runtime::detail {

	class Profiler
	{
	public:
		Profiler();
		~Profiler();
		Profiler(const Profiler&) = delete;
		Profiler& operator=(const Profiler&) = delete;

		// A sample is waiting to be taken.
		bool Due() const { return m_Due.load(std::memory_order_relaxed); }
		// `chunk`: L's outermost frame is a chunk's (L is the thread a
		// script, a module or a prompt input runs on, not a coroutine).
		void Sample(lua_State* L, bool chunk);
		// The timer stops; the report covers the time until now.
		void Stop();

		std::string Report(size_t top) const;
		std::string Folded() const;

	private:
		struct Function
		{
			std::string Name;
			std::string Where; // file:line it is defined at, or [C]
			long long Self = 0; // microseconds, as every weight here
			long long Total = 0;
		};

		std::atomic<bool> m_Due{ false };
		std::atomic<bool> m_Running{ true };
		std::thread m_Timer;
		std::chrono::steady_clock::time_point m_Start;
		std::chrono::steady_clock::time_point m_Last; // the last sample (or the start)
		double m_Seconds = 0;
		long long m_Sampled = 0; // the time the samples cover
		std::vector<Function> m_Functions;
		std::map<std::string, size_t> m_Index;     // name and place -> m_Functions
		std::map<std::string, long long> m_Lines;  // file:line -> its own time
		std::map<std::string, long long> m_Stacks; // folded stack -> time
	};

	// After a blocking call of the standard library (a sleep, a read, a
	// wait) and before it returns: the sample due, if the running script
	// is being profiled, with the line that made the call.
	void SampleBlocking(lua_State* L);

} // namespace cluau::runtime::detail
