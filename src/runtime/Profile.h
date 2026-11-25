#pragma once

// The sampling profiler under `cluau profile`: a timer counts milliseconds;
// at the script's next safepoint (Luau's interrupt callback, at calls and
// loop back-edges) the stack is recorded, weighted by the milliseconds
// counted since the last sample. Time inside a long C call (a sleep, a
// read, a download) goes to the stack that made the call.
//
// The report is each function's own time (its code, and the C functions
// it calls) and its total time (with what it calls), and the lines with
// the most time; the folded stacks (`outer;inner;innermost count` per
// line) are what flame graph tools read.

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
		bool Due() const { return m_Ticks.load(std::memory_order_relaxed) > 0; }
		void Sample(lua_State* L);
		// The timer stops; the report covers the time until now.
		void Stop();

		std::string Report(size_t top) const;
		std::string Folded() const;

	private:
		struct Function
		{
			std::string Name;
			std::string Where; // file:line it is defined at, or [C]
			long Self = 0;
			long Total = 0;
		};

		std::atomic<int> m_Ticks{ 0 };
		std::atomic<bool> m_Running{ true };
		std::thread m_Timer;
		std::chrono::steady_clock::time_point m_Start;
		double m_Seconds = 0;
		long m_Samples = 0; // in milliseconds of weight
		std::vector<Function> m_Functions;
		std::map<std::string, size_t> m_Index;  // name and place -> m_Functions
		std::map<std::string, long> m_Lines;    // file:line -> its own time
		std::map<std::string, long> m_Stacks;   // folded stack -> time
	};

} // namespace cluau::runtime::detail
