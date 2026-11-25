#include "Debugger.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace cluau::runtime::detail {

	namespace {

		std::string ChunkOf(const lua_Debug& ar)
		{
			const char* source = ar.source ? ar.source : "";
			return source[0] == '@' ? std::string(source + 1) : std::string(source);
		}

		bool IsLuaFrame(const lua_Debug& ar) { return ar.what && std::strcmp(ar.what, "C") != 0; }

		bool EndsWith(const std::string& s, const char* suffix)
		{
			const size_t n = std::strlen(suffix);
			return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
		}

		// A hit condition: an operator (">=" when there is none) and a count.
		bool ParseHitCondition(const std::string& text, std::string& op, int& count)
		{
			size_t i = 0;
			auto skip = [&] {
				while (i < text.size() && std::isspace((unsigned char)text[i]))
					i++;
			};
			skip();
			for (const char* candidate : { ">=", "<=", "==", ">", "<", "%", "=" })
				if (text.compare(i, std::strlen(candidate), candidate) == 0)
				{
					op = candidate;
					i += op.size();
					break;
				}
			if (op.empty())
				op = ">=";
			else if (op == "=")
				op = "==";
			skip();
			const size_t digits = i;
			while (i < text.size() && std::isdigit((unsigned char)text[i]))
				i++;
			if (i == digits)
				return false;
			count = std::atoi(text.c_str() + digits);
			skip();
			return i == text.size() && (op != "%" || count > 0);
		}

		// The text a table shows before it is opened: its class, its first
		// few entries (nested tables as {...}).
		std::string Shallow(lua_State* S, int index)
		{
			switch (lua_type(S, index))
			{
			case LUA_TSTRING:
			{
				size_t len = 0;
				const char* s = lua_tolstring(S, index, &len);
				std::string out = "\"";
				for (size_t i = 0; i < len && out.size() < 80; i++)
				{
					const char c = s[i];
					out += c == '"' ? "\\\"" : c == '\\' ? "\\\\" : c == '\n' ? "\\n" : c == '\t' ? "\\t" : std::string(1, c);
				}
				return out + (len > 78 ? "...\"" : "\"");
			}
			case LUA_TTABLE: return "{...}";
			case LUA_TNIL: return "nil";
			case LUA_TBOOLEAN: return lua_toboolean(S, index) ? "true" : "false";
			case LUA_TNUMBER:
			{
				// tolstring turns the value it reads into a string: a copy's.
				lua_pushvalue(S, index);
				size_t len = 0;
				const char* s = lua_tolstring(S, -1, &len);
				std::string out(s, len);
				lua_pop(S, 1);
				return out;
			}
			default: return lua_typename(S, lua_type(S, index));
			}
		}

		int ToStringProtected(lua_State* S)
		{
			size_t len = 0;
			const char* s = luaL_tolstring(S, 1, &len);
			lua_pushlstring(S, s, len);
			return 1;
		}

	} // namespace

	Debugger::Debugger(lua_State* L, Callbacks callbacks)
		: m_Main(lua_mainthread(L)), m_Callbacks(std::move(callbacks))
	{
		lua_Callbacks* cb = lua_callbacks(m_Main);
		cb->userdata = this;
		cb->debugbreak = BreakCallback;
		cb->debugstep = StepCallback;
		cb->interrupt = InterruptCallback;
		cb->debugprotectederror = ProtectedErrorCallback;
		cb->preresume = PreResumeCallback;
		cb->debuginterrupt = InterruptedCallback;
		SetDebugHooks(m_Main, this);
	}

	Debugger::~Debugger()
	{
		SetDebugHooks(m_Main, nullptr);
		lua_Callbacks* cb = lua_callbacks(m_Main);
		cb->userdata = nullptr;
		cb->debugbreak = nullptr;
		cb->debugstep = nullptr;
		cb->interrupt = nullptr;
		cb->debugprotectederror = nullptr;
		cb->preresume = nullptr;
		cb->debuginterrupt = nullptr;
		for (const Chunk& chunk : m_Chunks)
			lua_unref(m_Main, chunk.Ref);
	}

	Debugger* Debugger::Of(lua_State* L) { return static_cast<Debugger*>(lua_callbacks(L)->userdata); }

	void Debugger::BreakCallback(lua_State* L, lua_Debug*)
	{
		if (Debugger* d = Of(L))
			d->OnBreak(L);
	}

	void Debugger::StepCallback(lua_State* L, lua_Debug*)
	{
		if (Debugger* d = Of(L))
			d->OnStep(L);
	}

	void Debugger::InterruptCallback(lua_State* L, int gc)
	{
		if (gc >= 0)
			return;
		if (Debugger* d = Of(L))
			d->OnInterrupt(L);
	}

	void Debugger::ProtectedErrorCallback(lua_State* L)
	{
		if (Debugger* d = Of(L))
			d->OnProtectedError(L);
	}

	// coroutine.resume gives the coroutine its resumer's single-step mode:
	// a coroutine being stepped through keeps its own.
	void Debugger::PreResumeCallback(lua_State* L)
	{
		if (Debugger* d = Of(L); d && L == d->m_StepThread)
			lua_singlestep(L, 1);
	}

	// coroutine.resume breaks its own thread out when the coroutine broke
	// out: the coroutine is resumed first (ResumeBroken).
	void Debugger::InterruptedCallback(lua_State* L, lua_Debug* ar)
	{
		if (Debugger* d = Of(L); d && ar && ar->userdata)
			d->m_BrokenChild[L] = static_cast<lua_State*>(ar->userdata);
	}

	int Debugger::ResumeBroken(lua_State* T)
	{
		lua_State* at = T;
		for (;;)
		{
			auto child = m_BrokenChild.find(at);
			if (child == m_BrokenChild.end() || lua_status(child->second) != LUA_BREAK)
				break;
			at = child->second;
		}
		if (at == T)
		{
			m_BrokenChild.clear();
			return lua_resume(T, nullptr, 0);
		}
		// The innermost broken coroutine runs on to its yield or end; its
		// resumers then see it finished.
		lua_resume(at, nullptr, 0);
		return LUA_BREAK;
	}

	std::string Debugger::Canonical(const std::string& path)
	{
		if (path.empty())
			return {};
		std::error_code ec;
		fs::path p = fs::weakly_canonical(fs::u8path(path), ec);
		if (ec)
			p = fs::absolute(fs::u8path(path), ec).lexically_normal();
		const auto text = p.u8string();
		return std::string(text.begin(), text.end());
	}

	// ---- Breakpoints --------------------------------------------------------

	std::vector<Breakpoint> Debugger::SetBreakpoints(const std::string& file, std::vector<Breakpoint> breakpoints)
	{
		const std::string canonical = Canonical(file);
		std::vector<int> oldLines;
		for (auto it = m_Breakpoints.begin(); it != m_Breakpoints.end();)
			if (it->File == canonical)
			{
				oldLines.push_back(it->Line);
				m_Conditions.erase(it->Id);
				it = m_Breakpoints.erase(it);
			}
			else
				++it;
		for (int line : oldLines)
			Arm(canonical, line);

		const bool luau = EndsWith(canonical, ".luau");
		std::vector<Breakpoint> out;
		for (Breakpoint& b : breakpoints)
		{
			if (b.Id == 0)
				b.Id = m_NextId++;
			b.File = canonical;
			b.Hits = 0;
			b.ResolvedLine = 0;
			b.Problem.clear();
			if (!b.Condition.empty())
			{
				std::string error;
				std::string bytecode = CompileExpression(b.Condition, luau, error);
				if (bytecode.empty())
					b.Problem = "the condition does not compile: " + error;
				else
					m_Conditions[b.Id] = std::move(bytecode);
			}
			std::string op;
			int count = 0;
			if (b.Problem.empty() && !b.HitCondition.empty() && !ParseHitCondition(b.HitCondition, op, count))
				b.Problem = "the hit count is a number, or one after >=, ==, >, <, <= or % (\"% 3\": every third hit)";
			m_Breakpoints.push_back(b);
		}
		for (const Breakpoint& b : m_Breakpoints)
			if (b.File == canonical)
				Arm(canonical, b.Line);
		for (const Breakpoint& b : m_Breakpoints)
			if (b.File == canonical)
				out.push_back(b);
		return out;
	}

	void Debugger::SetBreakOnErrors(bool uncaught, bool caught)
	{
		m_BreakOnUncaught = uncaught || caught;
		m_BreakOnCaught = caught;
	}

	void Debugger::StopOnEntry()
	{
		m_EntryPending = true;
		lua_singlestep(m_Main, 1); // the script's thread inherits it
	}

	// Luau patches one BREAK per line: the line is armed while any
	// breakpoint names it, in every chunk loaded from the file.
	void Debugger::Arm(const std::string& file, int line)
	{
		const bool armed = std::any_of(m_Breakpoints.begin(), m_Breakpoints.end(), [&](const Breakpoint& b) { return b.File == file && b.Line == line; });
		for (const Chunk& chunk : m_Chunks)
		{
			if (chunk.File != file)
				continue;
			lua_getref(m_Main, chunk.Ref);
			const int resolved = lua_breakpoint(m_Main, -1, line, armed ? 1 : 0);
			lua_pop(m_Main, 1);
			if (resolved > 0)
				for (Breakpoint& b : m_Breakpoints)
					if (b.File == file && b.Line == line)
						b.ResolvedLine = resolved;
		}
	}

	void Debugger::OnLoaded(lua_State* L, const std::string& name, const std::string& file)
	{
		Chunk chunk;
		chunk.Name = name;
		chunk.File = Canonical(file);
		lua_pushvalue(L, -1);
		chunk.Ref = lua_ref(L, -1);
		lua_pop(L, 1);
		m_Chunks.push_back(chunk);
		if (chunk.File.empty())
			return;
		for (Breakpoint& b : m_Breakpoints)
		{
			if (b.File != chunk.File)
				continue;
			const int resolved = lua_breakpoint(L, -1, b.Line, 1);
			if (resolved > 0 && resolved != b.ResolvedLine)
			{
				const bool moved = resolved != (b.ResolvedLine ? b.ResolvedLine : b.Line);
				b.ResolvedLine = resolved;
				if (moved && m_Callbacks.Changed)
					m_Callbacks.Changed(b);
			}
		}
	}

	const Debugger::Chunk* Debugger::ChunkNamed(const std::string& name) const
	{
		for (auto it = m_Chunks.rbegin(); it != m_Chunks.rend(); ++it)
			if (it->Name == name)
				return &*it;
		return nullptr;
	}

	// A chunk of the script's own: one loaded from a file (not a standard
	// module, the runtime's prelude or its require).
	bool Debugger::IsUserChunk(const std::string& name) const
	{
		const Chunk* chunk = ChunkNamed(name);
		return chunk && !chunk->File.empty();
	}

	bool Debugger::IsLuauChunk(const std::string& name) const
	{
		const Chunk* chunk = ChunkNamed(name);
		return chunk && EndsWith(chunk->File, ".luau");
	}

	bool Debugger::Hit(Breakpoint& b) const
	{
		if (b.HitCondition.empty())
			return true;
		std::string op;
		int count = 0;
		if (!ParseHitCondition(b.HitCondition, op, count))
			return false;
		if (op == ">=")
			return b.Hits >= count;
		if (op == "==")
			return b.Hits == count;
		if (op == ">")
			return b.Hits > count;
		if (op == "<")
			return b.Hits < count;
		if (op == "<=")
			return b.Hits <= count;
		return b.Hits % count == 0;
	}

	// ---- The VM's callbacks --------------------------------------------------

	void Debugger::OnBreak(lua_State* L)
	{
		if (m_Evaluating || m_Paused || m_Stopping)
			return;
		lua_Debug ar{};
		if (!lua_getinfo(L, 0, "sl", &ar))
			return;
		const Chunk* chunk = ChunkNamed(ChunkOf(ar));
		if (!chunk || chunk->File.empty())
			return;
		const int line = ar.currentline;
		if (L == m_SkipBreakThread && line == m_SkipBreakLine)
		{
			m_SkipBreakThread = nullptr;
			return;
		}
		const std::string file = chunk->File;
		for (size_t i = 0; i < m_Breakpoints.size(); i++)
		{
			Breakpoint& b = m_Breakpoints[i];
			if (b.File != file || (b.ResolvedLine ? b.ResolvedLine : b.Line) != line || !b.Problem.empty())
				continue;
			if (auto condition = m_Conditions.find(b.Id); condition != m_Conditions.end())
			{
				std::string text;
				bool truthy = false;
				if (!RunExpression(L, 0, condition->second, text, &truthy))
				{
					if (m_Callbacks.Log)
						m_Callbacks.Log(b.File + ":" + std::to_string(line) + ": the breakpoint's condition failed: " + text + "\n");
					continue;
				}
				if (!truthy)
					continue;
			}
			b.Hits++;
			if (!Hit(b))
				continue;
			if (!b.LogMessage.empty())
			{
				if (m_Callbacks.Log)
					m_Callbacks.Log(FormatLog(L, b) + "\n");
				continue;
			}
			const int id = b.Id;
			Pause(L, { PauseReason::Breakpoint, id, {} }, true, false);
			return;
		}
	}

	void Debugger::OnStep(lua_State* L)
	{
		if (m_Paused || m_Evaluating || m_Stopping)
			return;
		if (m_EntryPending)
		{
			lua_Debug ar{};
			if (!lua_getinfo(L, 0, "sl", &ar) || !IsLuaFrame(ar) || !IsUserChunk(ChunkOf(ar)))
				return;
			m_EntryPending = false;
			lua_singlestep(m_Main, 0);
			Pause(L, { PauseReason::Entry, 0, {} }, false, true);
			return;
		}
		if (L != m_StepThread)
			return;
		lua_Debug ar{};
		if (!lua_getinfo(L, 0, "sl", &ar) || !IsLuaFrame(ar))
			return;
		const int depth = lua_stackdepth(L);
		const int line = ar.currentline;
		bool stop = false;
		switch (m_StepKind)
		{
		case StepKind::Into: stop = depth != m_StepDepth || line != m_StepLine; break;
		case StepKind::Over: stop = depth < m_StepDepth || (depth == m_StepDepth && line != m_StepLine); break;
		case StepKind::Out: stop = depth < m_StepDepth; break;
		}
		// Through the standard modules and the runtime's own code, to the
		// script's next line.
		if (!stop || !IsUserChunk(ChunkOf(ar)))
			return;
		m_SkipBreakThread = nullptr;
		Pause(L, { PauseReason::Step, 0, {} }, false, true);
	}

	void Debugger::OnInterrupt(lua_State* L)
	{
		if (m_Paused || m_Evaluating)
			return;
		// Requests are read every so many safepoints (thousands a millisecond).
		if (m_Callbacks.Poll && (++m_Ticks % 1024 == 0 || m_PauseRequested))
			m_Callbacks.Poll();
		if (m_Stopping)
		{
			lua_rawcheckstack(L, 1);
			luaL_error(L, "the debugger stopped the script");
		}
		if (!m_PauseRequested)
			return;
		// In the script's own code (a pause inside a standard module waits
		// for it to return).
		lua_Debug ar{};
		if (!lua_getinfo(L, 0, "sl", &ar) || !IsLuaFrame(ar) || !IsUserChunk(ChunkOf(ar)))
			return;
		m_PauseRequested = false;
		m_SkipBreakThread = L;
		m_SkipBreakLine = ar.currentline;
		Pause(L, { PauseReason::Pause, 0, {} }, true, false);
	}

	void Debugger::OnProtectedError(lua_State* L)
	{
		if (!m_BreakOnCaught || m_Paused || m_Evaluating || m_Stopping)
			return;
		std::string message = lua_isstring(L, -1) ? lua_tostring(L, -1) : std::string("(an error value of type ") + luaL_typename(L, -1) + ")";
		Pause(L, { PauseReason::CaughtError, 0, std::move(message) }, false, false);
	}

	void Debugger::OnUncaught(lua_State* T, const std::string& message)
	{
		if (!m_BreakOnUncaught || m_Paused || m_Stopping)
			return;
		Pause(T, { PauseReason::Error, 0, message }, false, false);
	}

	void Debugger::Pause(lua_State* L, PauseInfo pause, bool fromBreak, bool stepping)
	{
		const bool wasStepping = stepping || L == m_StepThread;
		if (L == m_StepThread)
			m_StepThread = nullptr;
		m_Scratch = lua_newthread(m_Main);
		m_ScratchRef = lua_ref(m_Main, -1);
		lua_pop(m_Main, 1);
		m_Paused = L;
		m_Pause = std::move(pause);
		m_Resume = Resume::None;
		std::fflush(stdout);
		std::fflush(stderr);

		if (m_Callbacks.Paused)
			m_Callbacks.Paused(m_Pause);

		Release();
		m_Paused = nullptr;
		const Resume resume = m_Resume;
		m_Resume = Resume::None;
		const bool errored = m_Pause.Reason == PauseReason::Error || m_Pause.Reason == PauseReason::CaughtError;
		if (errored)
			return;
		lua_Debug ar{};
		const int line = lua_getinfo(L, 0, "l", &ar) ? ar.currentline : 0;
		if (resume == Resume::Step)
		{
			if (!stepping && !lua_isyieldable(L))
			{
				if (m_Callbacks.Log)
					m_Callbacks.Log("cluau debug: cannot step inside a call made from C (a sort comparator, a metamethod): continuing\n");
				return;
			}
			m_StepThread = L;
			m_StepDepth = lua_stackdepth(L);
			m_StepLine = line;
			lua_singlestep(L, 1);
			if (!stepping)
			{
				// Single-step mode starts as the thread re-enters the VM.
				if (fromBreak)
				{
					m_SkipBreakThread = L;
					m_SkipBreakLine = line;
				}
				lua_break(L);
			}
			return;
		}
		// A continue after a step leaves single-step mode (by breaking out,
		// where it can), so the script runs at full speed again.
		lua_singlestep(L, 0);
		if (wasStepping && lua_isyieldable(L))
		{
			if (fromBreak)
			{
				m_SkipBreakThread = L;
				m_SkipBreakLine = line;
			}
			lua_break(L);
		}
	}

	void Debugger::Release()
	{
		for (int ref : m_Handles)
			lua_unref(m_Main, ref);
		m_Handles.clear();
		if (m_ScratchRef)
			lua_unref(m_Main, m_ScratchRef);
		m_ScratchRef = 0;
		m_Scratch = nullptr;
	}

	void Debugger::Continue() { m_Resume = Resume::Continue; }

	void Debugger::Step(StepKind kind)
	{
		m_StepKind = kind;
		m_Resume = Resume::Step;
	}

	// ---- The paused stack ------------------------------------------------------

	std::vector<Frame> Debugger::Stack()
	{
		std::vector<Frame> out;
		if (!m_Paused)
			return out;
		lua_Debug ar{};
		for (int level = 0; lua_getinfo(m_Paused, level, "sln", &ar); level++)
		{
			if (!IsLuaFrame(ar))
				continue;
			Frame frame;
			frame.Level = level;
			frame.Chunk = ChunkOf(ar);
			// The runtime's own code (the prelude, require) is left out.
			if (!frame.Chunk.empty() && frame.Chunk[0] == '=')
				continue;
			bool topLevel = false;
			if (const Chunk* chunk = ChunkNamed(frame.Chunk))
			{
				frame.File = chunk->File;
				// The chunk's own function: a file's top level.
				lua_Debug fn{};
				if (lua_checkstack(m_Paused, 2) && lua_getinfo(m_Paused, level, "f", &fn))
				{
					lua_getref(m_Paused, chunk->Ref);
					topLevel = lua_rawequal(m_Paused, -1, -2) != 0;
					lua_pop(m_Paused, 2);
				}
			}
			frame.Line = ar.currentline;
			frame.Function = ar.name && *ar.name ? ar.name : topLevel ? "(top level)" : "(anonymous)";
			out.push_back(std::move(frame));
		}
		return out;
	}

	// The value at `index` of S as one line: strings quoted, a table as its
	// class and first entries (or its own __tostring), a function by name.
	std::string Debugger::Format(lua_State* S, int index)
	{
		index = lua_absindex(S, index);
		lua_checkstack(S, 8);
		const int type = lua_type(S, index);
		if (type == LUA_TFUNCTION)
		{
			lua_Debug ar{};
			lua_pushvalue(S, index);
			std::string out = "function";
			if (lua_getinfo(S, -1, "sn", &ar))
			{
				if (ar.name && *ar.name)
					out += std::string(" ") + ar.name;
				const std::string chunk = ChunkOf(ar);
				if (ar.linedefined > 0 && !chunk.empty() && chunk[0] != '=')
					out += " (" + chunk + ":" + std::to_string(ar.linedefined) + ")";
			}
			lua_pop(S, 1);
			return out;
		}
		if (type != LUA_TTABLE && type != LUA_TUSERDATA)
			return Shallow(S, index);

		std::string prefix;
		bool custom = false;
		if (lua_getmetatable(S, index))
		{
			lua_rawgetfield(S, -1, "__tostring");
			custom = !lua_isnil(S, -1);
			lua_pop(S, 1);
			lua_rawgetfield(S, -1, "__name");
			if (lua_isstring(S, -1))
				prefix = std::string(lua_tostring(S, -1)) + " ";
			lua_pop(S, 2);
		}
		if (custom || type == LUA_TUSERDATA)
		{
			lua_pushcfunction(S, ToStringProtected, "tostring");
			lua_pushvalue(S, index);
			const bool was = m_Evaluating;
			m_Evaluating = true;
			const int status = lua_pcall(S, 1, 1, 0);
			m_Evaluating = was;
			std::string out = status == 0 && lua_isstring(S, -1) ? lua_tostring(S, -1) : prefix + "(its __tostring failed)";
			lua_pop(S, 1);
			return out;
		}
		std::string out = prefix + "{";
		int shown = 0;
		bool more = false;
		const int n = lua_objlen(S, index);
		for (int i = 1; i <= n; i++)
		{
			if (shown == 6)
			{
				more = true;
				break;
			}
			lua_rawgeti(S, index, i);
			out += (shown++ ? ", " : "") + Shallow(S, -1);
			lua_pop(S, 1);
		}
		lua_pushnil(S);
		while (!more && lua_next(S, index))
		{
			if (lua_type(S, -2) == LUA_TNUMBER)
			{
				const double k = lua_tonumber(S, -2);
				if (k >= 1 && k <= n && k == (double)(int)k)
				{
					lua_pop(S, 1);
					continue;
				}
			}
			if (shown == 6)
			{
				more = true;
				lua_pop(S, 2);
				break;
			}
			std::string key;
			if (lua_type(S, -2) == LUA_TSTRING)
				key = lua_tostring(S, -2);
			else
			{
				lua_pushvalue(S, -2);
				key = "[" + Shallow(S, -1) + "]";
				lua_pop(S, 1);
			}
			out += (shown++ ? ", " : "") + key + " = " + Shallow(S, -1);
			lua_pop(S, 1);
		}
		return out + (more ? ", ...}" : "}");
	}

	// The value on top of the scratch thread, popped; a table keeps a handle.
	Variable Debugger::Describe(const std::string& name)
	{
		lua_State* S = m_Scratch;
		Variable variable;
		variable.Name = name;
		variable.Value = Format(S, -1);
		variable.Type = lua_typename(S, lua_type(S, -1));
		if (lua_istable(S, -1))
		{
			if (lua_getmetatable(S, -1))
			{
				lua_rawgetfield(S, -1, "__name");
				if (lua_isstring(S, -1))
					variable.Type = lua_tostring(S, -1);
				lua_pop(S, 2);
			}
			lua_pushnil(S);
			const bool any = lua_next(S, -2) != 0;
			if (any)
				lua_pop(S, 2);
			if (any)
			{
				lua_pushvalue(S, -1);
				m_Handles.push_back(lua_ref(S, -1));
				lua_pop(S, 1);
				variable.Children = (int)m_Handles.size();
			}
		}
		lua_pop(S, 1);
		return variable;
	}

	std::vector<Variable> Debugger::Locals(int level)
	{
		std::vector<Variable> out;
		if (!m_Paused || !lua_checkstack(m_Paused, 2) || !lua_checkstack(m_Scratch, 8))
			return out;
		for (int n = 1;; n++)
		{
			const char* name = lua_getlocal(m_Paused, level, n);
			if (!name)
				break;
			lua_xmove(m_Paused, m_Scratch, 1);
			// The compiler's temporaries, and the translation's own locals.
			if (name[0] == '(' || std::strncmp(name, "__", 2) == 0)
			{
				lua_pop(m_Scratch, 1);
				continue;
			}
			out.push_back(Describe(name));
		}
		// A later local of the same name shadows an earlier one: keep the last.
		std::vector<Variable> visible;
		for (auto it = out.rbegin(); it != out.rend(); ++it)
			if (std::none_of(visible.begin(), visible.end(), [&](const Variable& v) { return v.Name == it->Name; }))
				visible.push_back(*it);
		std::reverse(visible.begin(), visible.end());
		return visible;
	}

	std::vector<Variable> Debugger::Upvalues(int level)
	{
		std::vector<Variable> out;
		lua_Debug ar{};
		if (!m_Paused || !lua_checkstack(m_Paused, 3) || !lua_checkstack(m_Scratch, 8) || !lua_getinfo(m_Paused, level, "f", &ar))
			return out;
		for (int n = 1;; n++)
		{
			const char* name = lua_getupvalue(m_Paused, -1, n);
			if (!name)
				break;
			lua_xmove(m_Paused, m_Scratch, 1);
			if (std::strncmp(name, "__", 2) == 0)
			{
				lua_pop(m_Scratch, 1);
				continue;
			}
			out.push_back(Describe(*name ? name : "?"));
		}
		lua_pop(m_Paused, 1);
		return out;
	}

	std::vector<Variable> Debugger::Expand(int handle)
	{
		std::vector<Variable> out;
		if (!m_Paused || handle <= 0 || handle > (int)m_Handles.size())
			return out;
		lua_State* S = m_Scratch;
		lua_checkstack(S, 8);
		lua_getref(S, m_Handles[(size_t)handle - 1]);
		const int table = lua_gettop(S);
		lua_pushnil(S);
		while (lua_next(S, table) != 0 && out.size() < 500)
		{
			std::string key;
			if (lua_type(S, -2) == LUA_TSTRING)
				key = lua_tostring(S, -2);
			else
			{
				lua_pushvalue(S, -2);
				key = "[" + Shallow(S, -1) + "]";
				lua_pop(S, 1);
			}
			lua_pushvalue(S, -1);
			out.push_back(Describe(key));
			lua_pop(S, 1);
		}
		lua_settop(S, table - 1);
		// Named fields by name, then the array part in its order.
		std::stable_sort(out.begin(), out.end(), [](const Variable& a, const Variable& b) {
			const bool an = !a.Name.empty() && a.Name[0] == '[';
			const bool bn = !b.Name.empty() && b.Name[0] == '[';
			return an != bn ? !an : an ? false : a.Name < b.Name;
		});
		return out;
	}

	Variable Debugger::Evaluate(const std::string& expression, int level, bool& ok)
	{
		ok = false;
		Variable result;
		if (!m_Paused)
		{
			result.Value = "the script is not paused";
			return result;
		}
		lua_Debug ar{};
		const bool luau = lua_getinfo(m_Paused, level, "s", &ar) && IsLuauChunk(ChunkOf(ar));
		std::string error;
		const std::string bytecode = CompileExpression(expression, luau, error);
		if (bytecode.empty())
		{
			result.Value = error;
			return result;
		}
		ok = RunExpression(m_Paused, level, bytecode, result.Value, nullptr, &result);
		return result;
	}

	bool Debugger::RunExpression(lua_State* L, int level, const std::string& bytecode, std::string& text, bool* truthy, Variable* described)
	{
		if (!lua_checkstack(L, 4) || !lua_checkstack(m_Main, 2))
		{
			text = "the stack is full";
			return false;
		}
		lua_State* T = lua_newthread(m_Main);
		const int threadRef = lua_ref(m_Main, -1);
		lua_pop(m_Main, 1);
		lua_checkstack(T, 8);

		// The environment: the frame's locals, then its upvalues, over its globals.
		lua_newtable(T);
		const int env = lua_gettop(T);
		for (int n = 1;; n++)
		{
			const char* name = lua_getlocal(L, level, n);
			if (!name)
				break;
			lua_xmove(L, T, 1);
			if (name[0] == '(')
				lua_pop(T, 1);
			else
				lua_setfield(T, env, name);
		}
		lua_Debug ar{};
		if (lua_getinfo(L, level, "f", &ar))
		{
			for (int n = 1;; n++)
			{
				const char* name = lua_getupvalue(L, -1, n);
				if (!name)
					break;
				lua_xmove(L, T, 1);
				lua_getfield(T, env, name);
				const bool shadowed = !lua_isnil(T, -1);
				lua_pop(T, 1);
				if (shadowed || !*name)
					lua_pop(T, 1);
				else
					lua_setfield(T, env, name);
			}
			lua_getfenv(L, -1);
			lua_xmove(L, T, 1);
			lua_pop(L, 1);
		}
		else
			lua_pushvalue(T, LUA_GLOBALSINDEX);
		lua_newtable(T);
		lua_insert(T, -2);
		lua_setfield(T, -2, "__index");
		lua_setmetatable(T, env);

		const bool was = m_Evaluating;
		m_Evaluating = true;
		int status = luau_load(T, "=debugger", bytecode.data(), bytecode.size(), env);
		if (status == 0)
			status = lua_pcall(T, 0, 1, 0);
		const bool ok = status == 0;
		if (ok)
		{
			if (truthy)
				*truthy = lua_toboolean(T, -1) != 0;
			if (described && m_Scratch)
			{
				lua_checkstack(m_Scratch, 8);
				lua_xmove(T, m_Scratch, 1);
				*described = Describe("");
				text = described->Value;
			}
			else
				text = Format(T, -1);
		}
		else
			text = lua_isstring(T, -1) ? lua_tostring(T, -1) : "the expression failed";
		m_Evaluating = was;
		lua_settop(T, 0);
		lua_unref(m_Main, threadRef);
		return ok;
	}

	std::string Debugger::FormatLog(lua_State* L, Breakpoint& b)
	{
		std::string out;
		const std::string& message = b.LogMessage;
		const bool luau = EndsWith(b.File, ".luau");
		for (size_t i = 0; i < message.size(); i++)
		{
			if (message[i] != '{')
			{
				out += message[i];
				continue;
			}
			const size_t close = message.find('}', i);
			if (close == std::string::npos)
			{
				out += message.substr(i);
				break;
			}
			const std::string expression = message.substr(i + 1, close - i - 1);
			std::string error;
			const std::string bytecode = CompileExpression(expression, luau, error);
			std::string value;
			if (bytecode.empty())
				value = "<" + error + ">";
			else if (!RunExpression(L, 0, bytecode, value, nullptr))
				value = "<" + value + ">";
			// A string reads as itself, not quoted.
			else if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
				value = value.substr(1, value.size() - 2);
			out += value;
			i = close;
		}
		return out;
	}

} // namespace cluau::runtime::detail
