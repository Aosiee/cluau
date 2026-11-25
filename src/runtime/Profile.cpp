#include "Profile.h"

#include "lua.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace cluau::runtime::detail {

	Profiler::Profiler()
		: m_Start(std::chrono::steady_clock::now())
	{
		m_Timer = std::thread([this] {
			while (m_Running.load(std::memory_order_relaxed))
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				m_Ticks.fetch_add(1, std::memory_order_relaxed);
			}
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

	void Profiler::Sample(lua_State* L)
	{
		const int ticks = m_Ticks.exchange(0, std::memory_order_relaxed);
		if (ticks <= 0)
			return;
		struct Frame
		{
			size_t Function;
			std::string Line; // file:line, for a Lua frame
		};
		std::vector<Frame> frames; // innermost first
		lua_Debug ar{};
		for (int level = 0; lua_getinfo(L, level, "sln", &ar); level++)
		{
			const bool c = ar.what && std::strcmp(ar.what, "C") == 0;
			const char* source = ar.source ? ar.source : "";
			// The runtime's own code (the prelude, the debugger's require)
			// counts toward what called it.
			if (!c && source[0] == '=')
				continue;
			const std::string chunk = source[0] == '@' ? source + 1 : source;
			const std::string name = ar.name && *ar.name ? ar.name : c ? "(C function)" : "(anonymous)";
			const std::string where = c ? "[C]" : chunk + ":" + std::to_string(ar.linedefined);
			const std::string key = name + " " + where;
			auto it = m_Index.find(key);
			if (it == m_Index.end())
			{
				it = m_Index.emplace(key, m_Functions.size()).first;
				m_Functions.push_back({ name, where });
			}
			frames.push_back({ it->second, c ? std::string() : chunk + ":" + std::to_string(ar.currentline) });
		}
		if (frames.empty())
			return;
		// A file's top level: the outermost Lua frame with no name.
		for (auto it = frames.rbegin(); it != frames.rend(); ++it)
			if (m_Functions[it->Function].Where != "[C]")
			{
				Function& fn = m_Functions[it->Function];
				if (fn.Name == "(anonymous)")
				{
					const std::string key = "(top level) " + fn.Where;
					auto top = m_Index.find(key);
					if (top == m_Index.end())
					{
						top = m_Index.emplace(key, m_Functions.size()).first;
						m_Functions.push_back({ "(top level)", fn.Where.substr(0, fn.Where.rfind(':')) });
					}
					it->Function = top->second;
				}
				break;
			}

		m_Samples += ticks;
		m_Functions[frames.front().Function].Self += ticks;
		std::set<size_t> seen;
		for (const Frame& f : frames)
			if (seen.insert(f.Function).second)
				m_Functions[f.Function].Total += ticks;
		for (const Frame& f : frames)
			if (!f.Line.empty())
			{
				m_Lines[f.Line] += ticks;
				break;
			}
		std::string stack;
		for (auto it = frames.rbegin(); it != frames.rend(); ++it)
		{
			const Function& fn = m_Functions[it->Function];
			stack += (stack.empty() ? "" : ";") + fn.Name + " (" + fn.Where + ")";
		}
		m_Stacks[stack] += ticks;
	}

	std::string Profiler::Report(size_t top) const
	{
		char buffer[512];
		std::string out;
		const double total = (double)std::max(1L, m_Samples);
		std::snprintf(buffer, sizeof(buffer), "profile: %.3f s, %ld ms sampled\n", m_Seconds, m_Samples);
		out += buffer;
		if (m_Samples == 0)
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

		std::vector<std::pair<std::string, long>> lines(m_Lines.begin(), m_Lines.end());
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
