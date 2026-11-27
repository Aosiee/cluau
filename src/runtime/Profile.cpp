#include "Profile.h"

#include "lua.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif
#endif

namespace cluau::runtime::detail {

	Profiler::Profiler()
		: m_Start(std::chrono::steady_clock::now())
		, m_Last(m_Start)
	{
		m_Timer = std::thread([this] {
#ifdef _WIN32
			// Windows sleeps a timer period (15.6 ms unless asked for less).
			timeBeginPeriod(1);
#endif
			while (m_Running.load(std::memory_order_relaxed))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				m_Due.store(true, std::memory_order_relaxed);
			}
#ifdef _WIN32
			timeEndPeriod(1);
#endif
		});
	}

	Profiler::~Profiler() { Stop(); }

	void Profiler::Stop()
	{
		if (!m_Running.exchange(false))
			return;
		m_Timer.join();
		m_Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_Start).count();
	}

	void Profiler::Sample(lua_State* L, bool chunk)
	{
		if (!m_Due.exchange(false, std::memory_order_relaxed) || !m_Running.load(std::memory_order_relaxed))
			return;
		// The sample stands for the time since the last one.
		const auto now = std::chrono::steady_clock::now();
		const long long weight = std::chrono::duration_cast<std::chrono::microseconds>(now - m_Last).count();
		m_Last = now;
		if (weight <= 0)
			return;
		struct Frame
		{
			size_t Function;
			std::string Line; // file:line, for a Lua frame
		};
		struct Raw
		{
			bool C = false;
			bool Std = false;
			std::string Name, Chunk;
			int Defined = 0, Line = 0;
		};
		std::vector<Raw> raw; // innermost first
		lua_Debug ar{};
		bool script = false; // a frame of the script's own
		for (int level = 0; lua_getinfo(L, level, "sln", &ar); level++)
		{
			const bool c = ar.what && std::strcmp(ar.what, "C") == 0;
			const char* source = ar.source ? ar.source : "";
			// The runtime's own code (the prelude, the debugger's require)
			// counts toward what called it.
			if (!c && source[0] == '=')
				continue;
			const std::string chunk = source[0] == '@' ? source + 1 : source;
			const bool std = !c && chunk.rfind("@std/", 0) == 0;
			script = script || (!c && !std);
			raw.push_back({ c, std, ar.name && *ar.name ? ar.name : "", chunk, ar.linedefined, ar.currentline });
		}
		// So do the standard modules written in cluau (@std/task's wait),
		// as a C function does, under the script's code.
		// Without the script's code on the stack, they run for the runtime
		// itself (the tasks the script's end waits for): that time is in
		// the total but in no function.
		const auto stdFrame = [](const Raw& f) { return f.Std; };
		if (!script && std::any_of(raw.begin(), raw.end(), stdFrame))
			return;
		raw.erase(std::remove_if(raw.begin(), raw.end(), stdFrame), raw.end());
		std::vector<Frame> frames; // innermost first
		for (size_t i = 0; i < raw.size(); i++)
		{
			const Raw& f = raw[i];
			std::string name = !f.Name.empty() ? f.Name : f.C ? "(C function)" : "(anonymous)";
			std::string where = f.C ? "[C]" : f.Chunk + ":" + std::to_string(f.Defined);
			// A file's top level: a module's, which `require` runs, or the
			// outermost Lua frame of a thread that runs a chunk (a
			// coroutine's or a task's outermost frame is its function).
			if (!f.C && f.Name.empty())
			{
				const bool required = i + 1 < raw.size() && raw[i + 1].C && raw[i + 1].Name == "require";
				bool outermost = chunk;
				for (size_t outer = i + 1; outer < raw.size(); outer++)
					outermost = outermost && raw[outer].C;
				if (required || outermost)
				{
					name = "(top level)";
					where = f.Chunk;
				}
			}
			const std::string key = name + " " + where;
			auto it = m_Index.find(key);
			if (it == m_Index.end())
			{
				it = m_Index.emplace(key, m_Functions.size()).first;
				m_Functions.push_back({ name, where });
			}
			frames.push_back({ it->second, f.C ? std::string() : f.Chunk + ":" + std::to_string(f.Line) });
		}
		if (frames.empty())
			return;

		m_Sampled += weight;
		m_Functions[frames.front().Function].Self += weight;
		std::set<size_t> seen;
		for (const Frame& f : frames)
			if (seen.insert(f.Function).second)
				m_Functions[f.Function].Total += weight;
		for (const Frame& f : frames)
			if (!f.Line.empty())
			{
				m_Lines[f.Line] += weight;
				break;
			}
		std::string stack;
		for (auto it = frames.rbegin(); it != frames.rend(); ++it)
		{
			const Function& fn = m_Functions[it->Function];
			stack += (stack.empty() ? "" : ";") + fn.Name + " (" + fn.Where + ")";
		}
		m_Stacks[stack] += weight;
	}

	std::string Profiler::Report(size_t top) const
	{
		char buffer[512];
		std::string out;
		const double total = (double)std::max(1LL, m_Sampled);
		std::snprintf(buffer, sizeof(buffer), "profile: %.3f s, %lld ms sampled\n", m_Seconds, m_Sampled / 1000);
		out += buffer;
		if (m_Sampled == 0)
			return out + "  (the script ran too briefly to sample)\n";

		std::vector<const Function*> functions;
		for (const Function& fn : m_Functions)
			functions.push_back(&fn);
		std::sort(functions.begin(), functions.end(), [](const Function* a, const Function* b) {
			return a->Self != b->Self ? a->Self > b->Self : a->Total > b->Total;
		});
		out += "\n   self   total  function\n";
		for (size_t i = 0; i < functions.size() && i < top; i++)
		{
			const Function& fn = *functions[i];
			if (fn.Self == 0 && i > 0)
				break;
			std::snprintf(buffer, sizeof(buffer), "  %5.1f%%  %5.1f%%  %s (%s)\n", 100.0 * fn.Self / total, 100.0 * fn.Total / total, fn.Name.c_str(),
				fn.Where.c_str());
			out += buffer;
		}

		std::vector<std::pair<std::string, long long>> lines(m_Lines.begin(), m_Lines.end());
		std::sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		out += "\n   self  line\n";
		for (size_t i = 0; i < lines.size() && i < std::max<size_t>(1, top / 2); i++)
		{
			std::snprintf(buffer, sizeof(buffer), "  %5.1f%%  %s\n", 100.0 * lines[i].second / total, lines[i].first.c_str());
			out += buffer;
		}
		return out;
	}

	std::string Profiler::Folded() const
	{
		std::string out;
		for (const auto& [stack, count] : m_Stacks)
			out += stack + " " + std::to_string(count) + "\n";
		return out;
	}

} // namespace cluau::runtime::detail
