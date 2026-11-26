#include "Debugger.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

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
			// A count past int's range is refused, not wrapped.
			const auto parsed = std::from_chars(text.data() + digits, text.data() + i, count);
			if (parsed.ec != std::errc() || parsed.ptr != text.data() + i)
				return false;
			skip();
			return i == text.size() && (op != "%" || count > 0);
		}

		// The length of the UTF-8 character at `s` (of `n` bytes); 0 when
		// the bytes there are not one.
		size_t Utf8Length(const unsigned char* s, size_t n)
		{
			const unsigned char c = s[0];
			size_t length = 0;
			unsigned min = 0, code = 0;
			if (c < 0x80)
				return 1;
			if (c >= 0xC2 && c <= 0xDF)
				length = 2, min = 0x80, code = c & 0x1F;
			else if (c >= 0xE0 && c <= 0xEF)
				length = 3, min = 0x800, code = c & 0x0F;
			else if (c >= 0xF0 && c <= 0xF4)
				length = 4, min = 0x10000, code = c & 0x07;
			else
				return 0;
			if (n < length)
				return 0;
			for (size_t i = 1; i < length; i++)
			{
				if ((s[i] & 0xC0) != 0x80)
					return 0;
				code = (code << 6) | (s[i] & 0x3F);
			}
			if (code < min || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
				return 0;
			return length;
		}

		// A string as a quoted literal: escapes for quotes, backslashes,
		// control characters and bytes that are not UTF-8 (\xFE); cut at a
		// character's end, with "..." before the closing quote, once it is
		// `limit` bytes long.
		std::string Quoted(const char* s, size_t len, size_t limit = 80)
		{
			std::string out = "\"";
			size_t i = 0;
			while (i < len && out.size() < limit)
			{
				const unsigned char c = (unsigned char)s[i];
				char escape[8];
				switch (c)
				{
				case '"': out += "\\\""; break;
				case '\\': out += "\\\\"; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				case '\0': out += "\\0"; break;
				default:
					if (c < 0x20 || c == 0x7F)
					{
						std::snprintf(escape, sizeof(escape), "\\x%02X", c);
						out += escape;
					}
					else if (c >= 0x80)
					{
						const size_t n = Utf8Length((const unsigned char*)s + i, len - i);
						if (n == 0)
						{
							std::snprintf(escape, sizeof(escape), "\\x%02X", c);
							out += escape;
						}
						else
						{
							out.append(s + i, n);
							i += n - 1;
						}
					}
					else
						out += (char)c;
				}
				i++;
			}
			return out + (i < len ? "...\"" : "\"");
		}

		bool IsName(const std::string& s)
		{
			if (s.empty() || !(std::isalpha((unsigned char)s[0]) || s[0] == '_'))
				return false;
			return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '_'; });
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
				return Quoted(s, len);
			}
			case LUA_TVECTOR:
			{
				const float* v = lua_tovector(S, index);
				std::string out = "vector(";
				for (int i = 0; i < LUA_VECTOR_SIZE; i++)
				{
					char number[32];
					std::snprintf(number, sizeof(number), "%.9g", (double)v[i]);
					out += (i ? ", " : "") + std::string(number);
				}
				return out + ")";
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

		// The number of lines in a file; nullopt when it cannot be read.
		std::optional<int> LineCount(const std::string& file)
		{
			std::ifstream in(fs::u8path(file), std::ios::binary);
			if (!in)
				return std::nullopt;
			int lines = 0;
			char last = '\n';
			char buffer[65536];
			while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0)
			{
				const std::streamsize n = in.gcount();
				lines += (int)std::count(buffer, buffer + n, '\n');
				last = buffer[n - 1];
			}
			return lines + (last != '\n' ? 1 : 0);
		}

		// The file's lines (without their line breaks); empty when it cannot be read.
		std::vector<std::string> FileLines(const std::string& file)
		{
			std::vector<std::string> lines;
			std::ifstream in(fs::u8path(file), std::ios::binary);
			for (std::string line; std::getline(in, line);)
			{
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				lines.push_back(std::move(line));
			}
			return lines;
		}

		// The line only closes a block: `}`, `})`, `},` (in Luau `end`,
		// `end)`), and perhaps a comment.
		bool OnlyCloses(const std::string& text, bool luau)
		{
			std::string code = text;
			const size_t comment = code.find(luau ? "--" : "//");
			if (comment != std::string::npos)
				code.erase(comment);
			bool any = false;
			for (size_t i = 0; i < code.size(); i++)
			{
				const char c = code[i];
				if (std::isspace((unsigned char)c) || c == ',' || c == ';' || c == ')' || c == ']')
					continue;
				if (c == '}')
					any = true;
				else if (luau && code.compare(i, 3, "end") == 0 && (i + 3 == code.size() || !std::isalnum((unsigned char)code[i + 3])) &&
						 (i == 0 || !std::isalnum((unsigned char)code[i - 1])))
				{
					any = true;
					i += 2;
				}
				else
					return false;
			}
			return any;
		}

		double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

		// How long an expression (a watch, a condition, a value's __tostring) may run.
		constexpr double kEvaluationSeconds = 2;

		// Why a breakpoint's line is not in the file ("" when it is).
		std::string PastEndOf(const std::optional<int>& lines, int line)
		{
			if (!lines || line <= *lines)
				return {};
			return "the file has " + std::to_string(*lines) + (*lines == 1 ? " line" : " lines") + ": there is no code at line " + std::to_string(line);
		}

		// What keeps a breakpoint from pausing ("" for nothing); `bytecode`:
		// its condition, compiled.
		std::string ProblemOf(const Breakpoint& b, const std::optional<int>& lines, bool luau, std::string* bytecode)
		{
			if (std::string past = PastEndOf(lines, b.Line); !past.empty())
				return past;
			if (!b.Condition.empty())
			{
				std::string error;
				std::string compiled = CompileExpression(b.Condition, luau, error);
				if (compiled.empty())
					return "the condition does not compile: " + error;
				if (bytecode)
					*bytecode = std::move(compiled);
			}
			std::string op;
			int count = 0;
			if (!b.HitCondition.empty() && !ParseHitCondition(b.HitCondition, op, count))
				return "the hit count is a number up to 2147483647, or one after >=, ==, >, <, <= or % (\"% 3\": every third hit)";
			return {};
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
		cb->postresume = PostResumeCallback;
		cb->debuginterrupt = InterruptedCallback;
		SetDebugHooks(m_Main, this);
		lua_checkstack(m_Main, 2);
		lua_getglobal(m_Main, "coroutine");
		if (lua_istable(m_Main, -1))
		{
			lua_rawgetfield(m_Main, -1, "resume");
			if (lua_isfunction(m_Main, -1))
				m_ResumeRef = lua_ref(m_Main, -1);
			lua_pop(m_Main, 1);
		}
		lua_pop(m_Main, 1);
		for (auto [name, ref] : { std::pair<const char*, int*>{ "pcall", &m_PcallRef }, { "xpcall", &m_XpcallRef } })
		{
			lua_getglobal(m_Main, name);
			if (lua_isfunction(m_Main, -1))
				*ref = lua_ref(m_Main, -1);
			lua_pop(m_Main, 1);
		}
	}

	Debugger::~Debugger()
	{
		EndLineStep();
		EndEntry();
		SetDebugHooks(m_Main, nullptr);
		lua_Callbacks* cb = lua_callbacks(m_Main);
		cb->userdata = nullptr;
		cb->debugbreak = nullptr;
		cb->debugstep = nullptr;
		cb->interrupt = nullptr;
		cb->debugprotectederror = nullptr;
		cb->preresume = nullptr;
		cb->postresume = nullptr;
		cb->debuginterrupt = nullptr;
		for (const Chunk& chunk : m_Chunks)
			lua_unref(m_Main, chunk.Ref);
		for (int ref : { m_ResumeRef, m_PcallRef, m_XpcallRef })
			if (ref)
				lua_unref(m_Main, ref);
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
		if (Debugger* d = Of(L))
		{
			d->m_Resuming.push_back(L);
			if (L == d->m_StepThread)
				lua_singlestep(L, 1);
		}
	}

	void Debugger::PostResumeCallback(lua_State* L)
	{
		if (Debugger* d = Of(L))
			d->OnResumed(L);
	}

	bool DebuggerAttached(lua_State* L) { return lua_callbacks(L)->userdata != nullptr; }

	void DebugWaiting(lua_State* L)
	{
		if (Debugger* d = static_cast<Debugger*>(lua_callbacks(L)->userdata))
			d->Waiting(L);
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
		for (auto it = m_Breakpoints.begin(); it != m_Breakpoints.end();)
			if (it->File == canonical)
			{
				m_Conditions.erase(it->Id);
				it = m_Breakpoints.erase(it);
			}
			else
				++it;

		const bool luau = EndsWith(canonical, ".luau");
		const std::optional<int> lines = LineCount(canonical);
		const std::vector<std::string> text = FileLines(canonical);
		for (Breakpoint& b : breakpoints)
		{
			if (b.Id == 0)
				b.Id = m_NextId++;
			b.File = canonical;
			b.Hits = 0;
			b.ResolvedLine = 0;
			b.Closing = b.Line >= 1 && (size_t)b.Line <= text.size() && OnlyCloses(text[(size_t)b.Line - 1], luau);
			std::string bytecode;
			b.Problem = ProblemOf(b, lines, luau, &bytecode);
			if (b.Problem.empty() && !bytecode.empty())
				m_Conditions[b.Id] = std::move(bytecode);
			m_Breakpoints.push_back(b);
		}
		ArmFile(canonical);
		std::vector<Breakpoint> out;
		for (const Breakpoint& b : m_Breakpoints)
			if (b.File == canonical)
				out.push_back(b);
		return out;
	}

	void Debugger::MarkPastEnd(const std::string& file, std::vector<Breakpoint>& breakpoints)
	{
		const std::string canonical = Canonical(file);
		const std::optional<int> lines = LineCount(canonical);
		for (Breakpoint& b : breakpoints)
			if (b.Problem.empty())
				b.Problem = ProblemOf(b, lines, EndsWith(canonical, ".luau"), nullptr);
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

	int Debugger::Resolve(lua_State* L, int index, const Breakpoint& b, std::vector<int>& stray)
	{
		// Luau arms the line, or the next one with code, and says which.
		const int next = lua_breakpoint(L, index, b.Line, 1);
		if (next == b.Line || !b.Closing)
			return next;
		// A line that only closes a block: the block's last line with code
		// (where a breakpoint on a loop's `}` pauses on each pass), not the
		// code after the block. The nearest line before it with code is the
		// first, going up, that Luau resolves to a line before this one.
		if (next > 0)
			stray.push_back(next);
		for (int line = b.Line - 1; line >= 1 && line >= b.Line - 1000; line--)
		{
			const int at = lua_breakpoint(L, index, line, 1);
			if (at > 0 && at < b.Line)
				return at;
		}
		return next;
	}

	// Luau patches one BREAK per line: a line is armed while a breakpoint
	// resolves to it, in every chunk loaded from the file.
	std::vector<Breakpoint*> Debugger::ArmFile(const std::string& file, Chunk* only, lua_State* L, int index)
	{
		struct Before
		{
			Breakpoint* B;
			int Line;
			bool Verified;
		};
		std::vector<Before> before;
		for (Breakpoint& b : m_Breakpoints)
			if (b.File == file)
				before.push_back({ &b, b.ResolvedLine, b.Verified() });
		for (Chunk& chunk : m_Chunks)
		{
			if (chunk.File != file || (only && &chunk != only))
				continue;
			lua_State* S = only ? L : m_Main;
			int at = index;
			if (!only)
			{
				lua_checkstack(m_Main, 2);
				lua_getref(m_Main, chunk.Ref);
				at = -1;
			}
			at = lua_absindex(S, at);
			std::vector<int> want, stray;
			for (Breakpoint& b : m_Breakpoints)
			{
				if (b.File != file)
					continue;
				const int line = Resolve(S, at, b, stray);
				if (line > 0)
				{
					b.ResolvedLine = line;
					if (b.Problem.empty())
						want.push_back(line);
					else
						stray.push_back(line);
				}
				else if (b.ResolvedLine == 0 && b.Problem.empty())
					b.Problem = "there is no code at line " + std::to_string(b.Line) + " or after it";
			}
			auto wanted = [&](int line) { return std::find(want.begin(), want.end(), line) != want.end(); };
			for (const std::vector<int>* list : { &chunk.Armed, &stray })
				for (int line : *list)
					if (!wanted(line))
						lua_breakpoint(S, at, line, 0);
			for (int line : want)
				lua_breakpoint(S, at, line, 1);
			chunk.Armed = std::move(want);
			if (!only)
				lua_pop(m_Main, 1);
		}
		// Taking a line out takes out a step's breakpoint there too.
		RearmSteps();
		std::vector<Breakpoint*> changed;
		for (const Before& b : before)
			if (b.B->ResolvedLine != b.Line || b.B->Verified() != b.Verified)
				changed.push_back(b.B);
		return changed;
	}

	void Debugger::Rearm()
	{
		for (const Chunk& chunk : m_Chunks)
		{
			if (chunk.Armed.empty())
				continue;
			lua_checkstack(m_Main, 2);
			lua_getref(m_Main, chunk.Ref);
			for (int line : chunk.Armed)
				lua_breakpoint(m_Main, -1, line, 1);
			lua_pop(m_Main, 1);
		}
	}

	void Debugger::RearmSteps()
	{
		lua_checkstack(m_Main, 2);
		if (m_LineStep.Active)
			for (const auto& [ref, line] : m_LineStep.Armed)
			{
				lua_getref(m_Main, ref);
				if (lua_isfunction(m_Main, -1))
					lua_breakpoint(m_Main, -1, line, 1);
				lua_pop(m_Main, 1);
			}
		if (m_Entry.Thread)
		{
			lua_getref(m_Main, m_Entry.Function);
			if (lua_isfunction(m_Main, -1))
				lua_breakpoint(m_Main, -1, m_Entry.Line, 1);
			lua_pop(m_Main, 1);
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
		if (!chunk.File.empty())
			chunk.Lines = LineCount(chunk.File).value_or(0);
		m_Chunks.push_back(chunk);
		if (chunk.File.empty())
			return;
		// The file's breakpoints are verified now (moved to the line with code).
		for (Breakpoint* b : ArmFile(m_Chunks.back().File, &m_Chunks.back(), L, -1))
			if (m_Callbacks.Changed)
				m_Callbacks.Changed(*b);
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

	bool Debugger::PastEnd(const std::string& name, int line) const
	{
		const Chunk* chunk = ChunkNamed(name);
		return chunk && chunk->Lines > 0 && line > chunk->Lines;
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
		if (L == m_SkipBreakThread)
		{
			// The BREAK the thread resumes at runs first: another line's
			// means that one was not run again.
			m_SkipBreakThread = nullptr;
			if (line == m_SkipBreakLine)
				return;
		}
		if (L == m_Entry.Thread && L == m_StepThread && line == m_Entry.Line && lua_stackdepth(L) == m_Entry.Depth && lua_checkstack(L, 2))
		{
			lua_Debug fn{};
			bool same = false;
			if (lua_getinfo(L, 0, "f", &fn))
			{
				lua_getref(L, m_Entry.Function);
				same = lua_rawequal(L, -1, -2) != 0;
				lua_pop(L, 2);
			}
			if (same)
			{
				Pause(L, { PauseReason::Step, {}, {} }, true, true);
				return;
			}
		}
		if (m_LineStep.Active && L == m_LineStep.Thread && LineStepStops(L, line))
		{
			EndLineStep();
			Pause(L, { PauseReason::Step, {}, {} }, true, false);
			return;
		}
		// Every breakpoint at the line counts its hit (and logs); the pause
		// names each that pauses.
		const std::string file = chunk->File;
		std::vector<int> pausing;
		for (size_t i = 0; i < m_Breakpoints.size(); i++)
		{
			Breakpoint& b = m_Breakpoints[i];
			if (b.File != file || b.ResolvedLine != line || !b.Problem.empty())
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
					m_Callbacks.Log(FormatLog(L, m_Breakpoints[i]) + "\n");
				continue;
			}
			pausing.push_back(b.Id);
		}
		if (!pausing.empty())
			Pause(L, { PauseReason::Breakpoint, std::move(pausing), {} }, true, false);
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
			Pause(L, { PauseReason::Entry, {}, {} }, false, true);
			return;
		}
		if (!m_StepThread)
			return;
		// Another thread single-steps when the stepped thread resumed it
		// (it inherits the mode): a step into stops in it too (a coroutine
		// resumed, a function coroutine.wrap made, called).
		if (L != m_StepThread && m_StepKind != StepKind::Into)
			return;
		lua_Debug ar{};
		if (!lua_getinfo(L, 0, "sl", &ar) || !IsLuaFrame(ar))
			return;
		const int depth = lua_stackdepth(L);
		const int line = ar.currentline;
		bool stop = L != m_StepThread;
		switch (m_StepKind)
		{
		case StepKind::Into: stop = stop || depth != m_StepDepth || line != m_StepLine; break;
		case StepKind::Over: stop = depth < m_StepDepth || (depth == m_StepDepth && line != m_StepLine); break;
		case StepKind::Out: stop = depth < m_StepDepth; break;
		}
		// Through the standard modules and the runtime's own code, to the
		// script's next line (not the end of a file, its implicit return).
		if (!stop || !IsUserChunk(ChunkOf(ar)) || PastEnd(ChunkOf(ar), line))
			return;
		m_SkipBreakThread = nullptr;
		Pause(L, { PauseReason::Step, {}, {} }, false, true);
	}

	void Debugger::OnInterrupt(lua_State* L)
	{
		// An expression that does not return (a loop, a slow __tostring)
		// is stopped: its error is the evaluation's result.
		if (m_Evaluating)
		{
			if (Now() > m_EvaluationDeadline)
			{
				lua_rawcheckstack(L, 1);
				lua_pushstring(L, "the expression ran for more than two seconds: it was stopped");
				lua_error(L);
			}
			return;
		}
		if (m_Paused)
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
		Pause(L, { PauseReason::Pause, {}, {} }, true, false);
	}

	void Debugger::Waiting(lua_State* L)
	{
		if (m_Paused || m_Evaluating)
			return;
		if (m_Callbacks.Poll)
			m_Callbacks.Poll();
		if (m_Stopping)
		{
			lua_rawcheckstack(L, 1);
			luaL_error(L, "the debugger stopped the script");
		}
	}

	lua_State* Debugger::ResumerOf(lua_State* co, bool* byResume)
	{
		std::vector<lua_State*> candidates;
		if (!m_Resuming.empty())
			candidates.push_back(m_Resuming.back());
		// A resumer broken out of the VM with the coroutine (it is resumed after it).
		for (const auto& [parent, child] : m_BrokenChild)
			if (child == co)
				candidates.push_back(parent);
		for (lua_State* R : candidates)
		{
			lua_Debug ar{};
			if (R == co || !lua_checkstack(R, 3) || !lua_getinfo(R, 0, "sf", &ar))
				continue;
			bool resume = false, wrap = false;
			if (lua_iscfunction(R, -1))
			{
				if (m_ResumeRef)
				{
					lua_getref(R, m_ResumeRef);
					resume = lua_rawequal(R, -1, -2) != 0;
					lua_pop(R, 1);
				}
				// coroutine.wrap's function holds its coroutine.
				if (!resume && lua_getupvalue(R, -1, 1))
				{
					wrap = lua_tothread(R, -1) == co;
					lua_pop(R, 1);
				}
			}
			lua_pop(R, 1);
			lua_Debug caller{};
			if ((resume || wrap) && lua_getinfo(R, 1, "s", &caller) && IsLuaFrame(caller) && IsUserChunk(ChunkOf(caller)))
			{
				if (byResume)
					*byResume = resume;
				return R;
			}
		}
		return nullptr;
	}

	int Debugger::UserFrame(lua_State* L)
	{
		lua_Debug ar{};
		for (int level = 0; lua_getinfo(L, level, "s", &ar); level++)
			if (IsLuaFrame(ar) && IsUserChunk(ChunkOf(ar)))
				return level;
		return -1;
	}

	// The coroutine was resumed by coroutine.resume (not by the runtime, as
	// the script's own thread is).
	bool Debugger::ResumedFromLua()
	{
		if (m_Resuming.empty() || !m_ResumeRef)
			return false;
		lua_State* R = m_Resuming.back();
		lua_Debug ar{};
		if (!lua_checkstack(R, 3) || !lua_getinfo(R, 0, "f", &ar))
			return false;
		lua_getref(R, m_ResumeRef);
		const bool resume = lua_rawequal(R, -1, -2) != 0;
		lua_pop(R, 2);
		return resume;
	}

	// The coroutine stopped running: it yielded, returned or failed.
	void Debugger::OnResumed(lua_State* co)
	{
		if (!m_Resuming.empty() && m_Resuming.back() == co)
			m_Resuming.pop_back();
		const int status = lua_status(co);
		if (m_Paused || m_Evaluating || m_Stopping || status == LUA_BREAK)
			return;
		const bool failed = status != LUA_OK && status != LUA_YIELD;
		if (!failed && co != m_StepThread)
			return;
		bool byResume = false;
		lua_State* resumer = ResumerOf(co, &byResume);
		auto message = [&] {
			return lua_isstring(co, -1) ? std::string(lua_tostring(co, -1)) : std::string("(an error value of type ") + luaL_typename(co, -1) + ")";
		};
		auto leaveStep = [&] {
			if (co == m_StepThread)
			{
				lua_singlestep(co, 0);
				m_StepThread = nullptr;
			}
		};
		// coroutine.resume catches the coroutine's error: a caught error
		// (coroutine.wrap's call raises it again, where it is caught or not).
		if (failed && m_BreakOnCaught && resumer && byResume)
		{
			leaveStep();
			Pause(co, { PauseReason::CaughtError, {}, message() }, false, false);
			return;
		}
		// A task's error: @std/task's scheduler raises it again, after the
		// task's frames, so the pause is where it was raised, in the task.
		if (failed && !resumer && m_BreakOnUncaught && ResumedFromLua() && UserFrame(co) >= 0)
		{
			leaveStep();
			m_TaskError = message();
			Pause(co, { PauseReason::Error, {}, m_TaskError }, false, false);
			return;
		}
		if (co != m_StepThread)
			return;
		// A task stepped through that waits (task.wait, a blocking call)
		// keeps the step: resumed by the scheduler, it goes on single-stepping
		// to its next line.
		if (!resumer && status == LUA_YIELD)
			return;
		// The coroutine stepped through left before the step stopped: the
		// step goes on where it was resumed, at that function's next line
		// (for a task that ended, the script's code below the scheduler).
		m_StepThread = nullptr;
		lua_singlestep(co, 0);
		if (resumer)
		{
			lua_Debug ar{};
			const int line = lua_getinfo(resumer, 1, "l", &ar) ? ar.currentline : 0;
			StartLineStep(resumer, line, 1, true);
		}
		else if (!m_Resuming.empty())
		{
			lua_State* below = m_Resuming.back();
			const int level = UserFrame(below);
			lua_Debug ar{};
			if (level >= 0 && lua_getinfo(below, level, "l", &ar))
				StartLineStep(below, ar.currentline, level, true);
		}
	}

	// A caught error pauses when the script's code is between where it was
	// raised and the pcall that catches it, or calls that pcall: not for
	// the errors a standard module raises and catches itself.
	bool Debugger::CaughtInScript(lua_State* L)
	{
		lua_Debug ar{};
		lua_checkstack(L, 3);
		for (int level = 0; lua_getinfo(L, level, "sf", &ar); level++)
		{
			const bool lua = IsLuaFrame(ar);
			bool catches = false;
			if (!lua)
				for (int ref : { m_PcallRef, m_XpcallRef })
					if (ref)
					{
						lua_getref(L, ref);
						catches = catches || lua_rawequal(L, -1, -2) != 0;
						lua_pop(L, 1);
					}
			lua_pop(L, 1);
			if (lua && IsUserChunk(ChunkOf(ar)))
				return true;
			if (catches)
			{
				lua_Debug caller{};
				for (int up = level + 1; lua_getinfo(L, up, "s", &caller); up++)
					if (IsLuaFrame(caller))
						return IsUserChunk(ChunkOf(caller));
				return false;
			}
		}
		return false;
	}

	void Debugger::OnProtectedError(lua_State* L)
	{
		if (!m_BreakOnCaught || m_Paused || m_Evaluating || m_Stopping || !CaughtInScript(L))
			return;
		std::string message = lua_isstring(L, -1) ? lua_tostring(L, -1) : std::string("(an error value of type ") + luaL_typename(L, -1) + ")";
		Pause(L, { PauseReason::CaughtError, {}, std::move(message) }, false, false);
	}

	void Debugger::OnUncaught(lua_State* T, const std::string& message)
	{
		// A task's error ends the script: it paused already, in the task.
		const bool paused = !m_TaskError.empty() && message.find(m_TaskError) != std::string::npos;
		m_TaskError.clear();
		if (!m_BreakOnUncaught || m_Paused || m_Stopping || paused)
			return;
		Pause(T, { PauseReason::Error, {}, message }, false, false);
	}

	void Debugger::Pause(lua_State* L, PauseInfo pause, bool fromBreak, bool stepping)
	{
		const bool wasStepping = stepping || L == m_StepThread;
		// A pause in another thread (a coroutine stepped into) ends the step.
		if (m_StepThread && L != m_StepThread)
			lua_singlestep(m_StepThread, 0);
		m_StepThread = nullptr;
		EndLineStep();
		EndEntry();
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
				StartLineStep(L, line);
				return;
			}
			m_StepThread = L;
			m_StepDepth = lua_stackdepth(L);
			m_StepLine = line;
			lua_singlestep(L, 1);
			if (stepping && m_StepKind == StepKind::Into && !lua_isyieldable(L))
				ArmEntry(L, line, !fromBreak);
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

	void Debugger::StartLineStep(lua_State* L, int line, int level, bool here)
	{
		LineStep& step = m_LineStep;
		step = LineStep{};
		step.Active = true;
		step.Thread = L;
		step.Kind = m_StepKind;
		step.Line = line;
		lua_checkstack(L, 2);
		lua_Debug frame{};
		while (lua_getinfo(L, level, "s", &frame) && !IsLuaFrame(frame))
			level++;
		step.Depth = lua_stackdepth(L) - level;
		// Every line of `level`'s function: lua_breakpoint arms the next line
		// with code at or after the one asked, and says which.
		auto armAll = [&](int level, int& ref) {
			lua_Debug ar{};
			if (!lua_getinfo(L, level, "f", &ar))
				return;
			for (int at = 1, armed; (armed = lua_breakpoint(L, -1, at, 1)) > 0; at = armed + 1)
				step.Armed.push_back({ 0, armed });
			ref = lua_ref(L, -1);
			for (auto& armed : step.Armed)
				if (armed.first == 0)
					armed.first = ref;
			lua_pop(L, 1);
		};
		if (step.Kind != StepKind::Out || here)
		{
			armAll(level, step.Function);
			for (const auto& [ref, armed] : step.Armed)
				step.FirstLine = step.FirstLine ? std::min(step.FirstLine, armed) : armed;
		}
		// The Lua function it returns to (past the C call), for the step
		// that leaves the function.
		lua_Debug ar{};
		for (int caller = level + 1; lua_getinfo(L, caller, "s", &ar); caller++)
			if (IsLuaFrame(ar))
			{
				step.CallerDepth = step.Depth - (caller - level);
				armAll(caller, step.Caller);
				break;
			}
	}

	bool Debugger::LineStepStops(lua_State* L, int line)
	{
		const LineStep& step = m_LineStep;
		lua_Debug ar{};
		if (!lua_checkstack(L, 2) || !lua_getinfo(L, 0, "f", &ar))
			return false;
		auto is = [&](int ref) {
			if (!ref)
				return false;
			lua_getref(L, ref);
			const bool same = lua_rawequal(L, -1, -2) != 0;
			lua_pop(L, 1);
			return same;
		};
		const int depth = lua_stackdepth(L);
		// A step into stops at the function's next call too (a callback C
		// calls again starts at its first line).
		const bool again = step.Kind == StepKind::Into && line == step.FirstLine && depth == step.Depth;
		const bool stop = (is(step.Function) && depth <= step.Depth && (line != step.Line || depth < step.Depth || again)) ||
						  (is(step.Caller) && depth <= step.CallerDepth);
		lua_pop(L, 1);
		return stop;
	}

	void Debugger::ArmEntry(lua_State* L, int line, bool atInstruction)
	{
		lua_Debug ar{};
		if (!lua_checkstack(L, 2) || !lua_getinfo(L, 0, "fa", &ar))
			return;
		const int first = lua_breakpoint(L, -1, 1, 1);
		if (first <= 0)
		{
			lua_pop(L, 1);
			return;
		}
		m_Entry.Thread = L;
		m_Entry.Function = lua_ref(L, -1);
		m_Entry.Line = first;
		m_Entry.Depth = lua_stackdepth(L);
		lua_pop(L, 1);
		// Paused before that line's first instruction (a function's first,
		// past a vararg function's preamble), the thread runs the BREAK
		// there now.
		if (atInstruction && line == first && !ar.isvararg)
		{
			m_SkipBreakThread = L;
			m_SkipBreakLine = line;
		}
	}

	void Debugger::EndEntry()
	{
		if (!m_Entry.Thread)
			return;
		lua_getref(m_Main, m_Entry.Function);
		if (lua_isfunction(m_Main, -1))
			lua_breakpoint(m_Main, -1, m_Entry.Line, 0);
		lua_pop(m_Main, 1);
		lua_unref(m_Main, m_Entry.Function);
		m_Entry = Entry{};
		Rearm();
	}

	// The step's breakpoints come out; the editor's go back where they share a line.
	void Debugger::EndLineStep()
	{
		LineStep& step = m_LineStep;
		if (!step.Active)
			return;
		step.Active = false;
		for (const auto& [ref, line] : step.Armed)
		{
			lua_getref(m_Main, ref);
			if (lua_isfunction(m_Main, -1))
				lua_breakpoint(m_Main, -1, line, 0);
			lua_pop(m_Main, 1);
		}
		for (int ref : { step.Function, step.Caller })
			if (ref)
				lua_unref(m_Main, ref);
		step.Armed.clear();
		Rearm();
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
			const bool was = BeginEvaluation();
			const int status = lua_pcall(S, 1, 1, 0);
			EndEvaluation(was);
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
		if (!more)
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
			out += (shown++ ? ", " : "") + KeyName(S, -2) + " = " + Shallow(S, -1);
			lua_pop(S, 1);
		}
		return out + (more ? ", ...}" : "}");
	}

	// A table's key as a field's name: a name as itself, another string
	// quoted, any other key in brackets (["a b"], ["[1]"], [1]).
	std::string Debugger::KeyName(lua_State* S, int index)
	{
		index = lua_absindex(S, index);
		if (lua_type(S, index) == LUA_TSTRING)
		{
			size_t len = 0;
			const char* s = lua_tolstring(S, index, &len);
			const std::string key(s, len);
			if (IsName(key))
				return key;
		}
		lua_pushvalue(S, index);
		std::string key = "[" + Shallow(S, -1) + "]";
		lua_pop(S, 1);
		return key;
	}

	// The value on top of the scratch thread, popped; a table keeps a handle.
	Variable Debugger::Describe(const std::string& name)
	{
		lua_State* S = m_Scratch;
		const int top = lua_gettop(S);
		Variable variable;
		variable.Name = name;
		variable.Value = Format(S, -1);
		lua_settop(S, top); // Format leaves the stack as it found it
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
		constexpr size_t kShown = 500;
		size_t entries = 0;
		lua_pushnil(S);
		while (lua_next(S, table) != 0)
		{
			if (++entries > kShown)
			{
				lua_pop(S, 1);
				continue; // counted
			}
			const std::string key = KeyName(S, -2);
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
		if (entries > kShown)
		{
			Variable rest;
			rest.Name = "...";
			rest.Value = std::to_string(entries - kShown) + " more of " + std::to_string(entries) + " entries (an expression shows any of them)";
			rest.Type = "";
			out.push_back(std::move(rest));
		}
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

	bool Debugger::BeginEvaluation()
	{
		const bool was = m_Evaluating;
		if (!was)
		{
			m_Evaluating = true;
			m_EvaluationDeadline = Now() + kEvaluationSeconds;
		}
		return was;
	}

	void Debugger::EndEvaluation(bool was) { m_Evaluating = was; }

	namespace {

		// An expression's environment's __index: a name the frame declares
		// is its value (nil too), any other the frame's globals'.
		int FrameIndex(lua_State* T)
		{
			lua_pushvalue(T, 2);
			lua_rawget(T, lua_upvalueindex(1)); // declared?
			const bool declared = lua_toboolean(T, -1) != 0;
			lua_pop(T, 1);
			lua_pushvalue(T, 2);
			if (declared)
				lua_rawget(T, lua_upvalueindex(2));
			else
				lua_gettable(T, lua_upvalueindex(3));
			return 1;
		}

	} // namespace

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
		lua_checkstack(T, 16);

		// The environment: the frame's locals, then its upvalues, over its
		// globals. The names the frame declares are kept apart, so that a
		// local holding nil hides a global of its name.
		lua_newtable(T);
		const int env = lua_gettop(T);
		lua_newtable(T);
		const int declared = lua_gettop(T);
		lua_newtable(T);
		const int values = lua_gettop(T);
		auto declare = [&](const char* name) {
			// The value on top of T, unless a later local of the name has one.
			lua_pushstring(T, name);
			lua_pushboolean(T, 1);
			lua_rawset(T, declared);
			lua_setfield(T, values, name);
		};
		for (int n = 1;; n++)
		{
			const char* name = lua_getlocal(L, level, n);
			if (!name)
				break;
			lua_xmove(L, T, 1);
			if (name[0] == '(')
				lua_pop(T, 1);
			else
				declare(name); // a later local of the same name shadows an earlier one
		}
		lua_Debug ar{};
		int varargs = 0;
		if (lua_getinfo(L, level, "fa", &ar))
		{
			for (int n = 1;; n++)
			{
				const char* name = lua_getupvalue(L, -1, n);
				if (!name)
					break;
				lua_xmove(L, T, 1);
				lua_getfield(T, declared, name);
				const bool shadowed = lua_toboolean(T, -1) != 0;
				lua_pop(T, 1);
				if (shadowed || !*name)
					lua_pop(T, 1);
				else
					declare(name);
			}
			lua_getfenv(L, -1);
			lua_xmove(L, T, 1);
			lua_pop(L, 1);
			// The frame's `...`: its arguments past the named ones.
			if (ar.isvararg)
				for (int n = ar.nparams + 1; lua_checkstack(L, 1) && lua_getargument(L, level, n); n++)
					varargs++;
		}
		else
			lua_pushvalue(T, LUA_GLOBALSINDEX);
		const int globals = lua_gettop(T);
		lua_newtable(T); // the metatable
		lua_pushvalue(T, declared);
		lua_pushvalue(T, values);
		lua_pushvalue(T, globals);
		lua_pushcclosure(T, FrameIndex, "index", 3);
		lua_setfield(T, -2, "__index");
		lua_setmetatable(T, env);
		lua_settop(T, env);

		const bool was = BeginEvaluation();
		int status = luau_load(T, "=debugger", bytecode.data(), bytecode.size(), env);
		if (status == 0)
		{
			lua_checkstack(T, varargs + 1);
			lua_xmove(L, T, varargs); // pushed in order, the first lowest
			status = lua_pcall(T, varargs, 1, 0);
		}
		else
			lua_pop(L, varargs);
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
		EndEvaluation(was);
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
