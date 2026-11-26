#include "DapServer.h"
#include "Std.h"

#include "lua.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define CLUAU_DUP _dup
#define CLUAU_DUP2 _dup2
#define CLUAU_READ(fd, buf, n) _read(fd, buf, (unsigned)(n))
#define CLUAU_WRITE(fd, buf, n) _write(fd, buf, (unsigned)(n))
#define CLUAU_CLOSE _close
#else
#include <fcntl.h>
#include <unistd.h>
#define CLUAU_DUP dup
#define CLUAU_DUP2 dup2
#define CLUAU_READ(fd, buf, n) read(fd, buf, n)
#define CLUAU_WRITE(fd, buf, n) write(fd, buf, n)
#define CLUAU_CLOSE close
#endif

namespace fs = std::filesystem;

namespace cluau::runtime::detail {

	namespace {

		// Scope references sit above the debugger's table handles.
		constexpr int kScopeBase = 1000000;

		DapServer* g_Running = nullptr; // the server whose script runs (process.exit reports to it)

		std::string Utf8(const fs::path& path)
		{
			const auto text = path.u8string();
			return std::string(text.begin(), text.end());
		}

		std::string BaseName(const std::string& path)
		{
			const size_t slash = path.find_last_of("/\\");
			return slash == std::string::npos ? path : path.substr(slash + 1);
		}

		const char* ReasonName(PauseReason reason)
		{
			switch (reason)
			{
			case PauseReason::Breakpoint: return "breakpoint";
			case PauseReason::Step: return "step";
			case PauseReason::Entry: return "entry";
			case PauseReason::Pause: return "pause";
			case PauseReason::Error:
			case PauseReason::CaughtError: return "exception";
			}
			return "pause";
		}

	} // namespace

	DapServer::DapServer(Io io, Options base)
		: m_Io(std::move(io))
		, m_Base(std::move(base))
	{
	}

	DapServer::~DapServer()
	{
		if (g_Running == this)
		{
			SetExitHook(nullptr);
			g_Running = nullptr;
		}
	}

	// ---- Messages ------------------------------------------------------------

	void DapServer::Receive(std::string message)
	{
		{
			std::lock_guard<std::mutex> lock(m_QueueMutex);
			m_Queue.push_back(std::move(message));
			m_Queued++;
		}
		m_QueueReady.notify_one();
	}

	void DapServer::EndOfInput()
	{
		{
			std::lock_guard<std::mutex> lock(m_QueueMutex);
			m_InputEnded = true;
			m_Queued++;
		}
		m_QueueReady.notify_one();
	}

	std::optional<std::string> DapServer::Wait()
	{
		std::unique_lock<std::mutex> lock(m_QueueMutex);
		m_QueueReady.wait(lock, [&] { return !m_Queue.empty() || m_InputEnded; });
		if (m_Queue.empty())
			return std::nullopt;
		std::string message = std::move(m_Queue.front());
		m_Queue.pop_front();
		m_Queued--;
		return message;
	}

	// While the script runs: the messages waiting, handled now.
	void DapServer::Poll()
	{
		while (m_Queued > 0)
		{
			std::string message;
			{
				std::lock_guard<std::mutex> lock(m_QueueMutex);
				if (m_Queue.empty())
				{
					if (m_InputEnded)
					{
						m_Queued = 0;
						m_Disconnected = true;
						Stop();
					}
					return;
				}
				message = std::move(m_Queue.front());
				m_Queue.pop_front();
				m_Queued--;
			}
			Handle(message);
		}
	}

	void DapServer::Send(Json message)
	{
		std::lock_guard<std::mutex> lock(m_SendMutex);
		Json out = Json::Object();
		out.Set("seq", m_Seq++);
		for (const auto& [key, value] : message.Members())
			out.Set(key, value);
		if (m_Io.Send)
			m_Io.Send(out.Dump());
	}

	void DapServer::Respond(const Json& request, bool success, Json body, const std::string& message)
	{
		Json response = Json::Object();
		response.Set("type", "response");
		response.Set("request_seq", request["seq"].Int());
		response.Set("success", success);
		response.Set("command", request["command"].String());
		if (!message.empty())
			response.Set("message", message);
		response.Set("body", std::move(body));
		Send(std::move(response));
	}

	void DapServer::Event(const std::string& name, Json body)
	{
		Json event = Json::Object();
		event.Set("type", "event");
		event.Set("event", name);
		event.Set("body", std::move(body));
		Send(std::move(event));
	}

	void DapServer::Output(const std::string& category, const std::string& text)
	{
		if (text.empty())
			return;
		Json body = Json::Object();
		body.Set("category", category);
		body.Set("output", text);
		Event("output", std::move(body));
	}

	// ---- The session ---------------------------------------------------------

	int DapServer::Run()
	{
		struct Done
		{
			std::atomic<bool>& Flag;
			~Done() { Flag = true; }
		} done{ m_Finished };
		while (!m_Disconnected)
		{
			if (m_Configured && m_Launched && !m_Started)
			{
				m_Started = true;
				RunScript();
				continue;
			}
			std::optional<std::string> message = Wait();
			if (!message)
				break;
			Handle(*message);
		}
		return 0;
	}

	void DapServer::Launch(const Json& request)
	{
		const Json& args = request["arguments"];
		m_Program = args["program"].String();
		if (m_Program.empty())
		{
			Respond(request, false, Json::Object(), "the launch configuration names no program (\"program\": the script to run)");
			return;
		}
		m_Cwd = args["cwd"].String();
		std::error_code ec;
		if (!m_Cwd.empty() && !fs::is_directory(fs::u8path(m_Cwd), ec))
		{
			Respond(request, false, Json::Object(), "the launch configuration's cwd is not a directory: " + m_Cwd);
			return;
		}
		fs::path program = fs::u8path(m_Program);
		if (program.is_relative() && !m_Cwd.empty())
			program = fs::u8path(m_Cwd) / program;
		if (!fs::is_regular_file(program, ec))
		{
			Respond(request, false, Json::Object(), "the program is not a file: " + m_Program);
			return;
		}
		m_Program = Debugger::Canonical(Utf8(program));
		if (m_Cwd.empty())
			m_Cwd = Utf8(fs::u8path(m_Program).parent_path());
		m_Args.clear();
		for (const Json& arg : args["args"].Items())
			m_Args.push_back(arg.String());
		m_StopOnEntry = args["stopOnEntry"].Bool();
		m_NoDebug = args["noDebug"].Bool();
		m_Sandbox = args["sandbox"].Bool();
		// A test file's cases run as `cluau test` runs them, unless asked otherwise.
		const std::string& file = m_Program;
		m_Test = args["test"].Bool(file.size() > 11 && file.compare(file.size() - 11, 11, "_test.cluau") == 0);
		m_Filter = args["filter"].String();
		m_Launched = true;
		Respond(request, true);
	}

	void DapServer::RunScript()
	{
		std::error_code ec;
		if (!m_Cwd.empty())
			fs::current_path(fs::u8path(m_Cwd), ec);
		Options options = m_Base;
		options.Args = m_Args;
		options.Native = false;
		options.CatchInterrupt = false;
		options.Sandbox = options.Sandbox || m_Sandbox;
		m_Runtime = std::make_unique<Runtime>(options);
		if (!m_NoDebug)
		{
			Debugger::Callbacks callbacks;
			callbacks.Paused = [this](const PauseInfo& pause) { OnPaused(pause); };
			callbacks.Log = [this](const std::string& text) { Output("console", text); };
			callbacks.Poll = [this] { Poll(); };
			callbacks.Changed = [this](const Breakpoint& b) {
				Json body = Json::Object();
				body.Set("reason", "changed");
				body.Set("breakpoint", BreakpointJson(b));
				Event("breakpoint", std::move(body));
			};
			m_Debugger = std::make_unique<Debugger>(m_Runtime->State(), std::move(callbacks));
			m_Debugger->SetBreakOnErrors(m_BreakOnUncaught, m_BreakOnCaught);
			for (auto& [file, breakpoints] : m_Breakpoints)
			{
				m_EditorPaths[Debugger::Canonical(file)] = file;
				breakpoints = m_Debugger->SetBreakpoints(file, breakpoints);
				for (const Breakpoint& b : breakpoints)
					if (!b.Problem.empty())
					{
						Json body = Json::Object();
						body.Set("reason", "changed");
						body.Set("breakpoint", BreakpointJson(b));
						Event("breakpoint", std::move(body));
					}
			}
			if (m_StopOnEntry)
				m_Debugger->StopOnEntry();
		}
		g_Running = this;
		SetExitHook(ExitHook);
		Json process = Json::Object();
		process.Set("name", BaseName(m_Program));
		process.Set("startMethod", "launch");
		process.Set("isLocalProcess", true);
		Event("process", std::move(process));

		// Named as a run from its directory names it, when it is under it.
		std::string name = m_Program;
		const fs::path rel = fs::u8path(m_Program).lexically_relative(fs::current_path(ec));
		if (!ec && !rel.empty() && *rel.begin() != "..")
			name = Utf8(rel);
		int code = 0;
		if (m_Test)
		{
			Runtime::TestCounts counts;
			const bool ran = m_Runtime->RunTest(name, m_Filter, counts);
			// After the cases' own output (every output event of it sent).
			std::fflush(stdout);
			std::fflush(stderr);
			if (m_Io.FinishOutput && !m_Ended)
				m_Io.FinishOutput();
			std::string summary = std::to_string(counts.Passed) + " passed";
			if (counts.Failed)
				summary += ", " + std::to_string(counts.Failed) + " failed";
			if (counts.Skipped)
				summary += ", " + std::to_string(counts.Skipped) + " skipped";
			if (!ran)
				summary += "; the file did not finish";
			Output("console", summary + "\n");
			code = !ran || counts.Failed || counts.Passed + counts.Skipped == 0 ? 1 : 0;
		}
		else
			code = m_Runtime->RunFile(name);

		SetExitHook(nullptr);
		g_Running = nullptr;
		m_Debugger.reset();
		m_Runtime.reset();
		Ended(code);
	}

	void DapServer::ExitHook(int code)
	{
		if (DapServer* server = g_Running)
		{
			g_Running = nullptr;
			server->Ended(code);
		}
	}

	void DapServer::Ended(int code)
	{
		m_Finished = true;
		Report(code);
	}

	void DapServer::Report(int code)
	{
		if (m_Ended.exchange(true))
			return;
		if (m_Io.FinishOutput)
			m_Io.FinishOutput();
		Json exited = Json::Object();
		exited.Set("exitCode", code);
		Event("exited", std::move(exited));
		Event("terminated");
	}

	void DapServer::Abandon()
	{
		if (!m_Finished.exchange(true) && !m_Ended)
			Event("terminated");
	}

	void DapServer::Stop()
	{
		m_Stopping = true;
		if (m_Debugger)
			m_Debugger->Stop();
		if (m_Paused)
			m_Resumed = true;
		// Ended as asked: the script stops at its next safepoint (the error
		// that stops it is not the script's failure, and is not reported).
		if (m_Started && !m_Ended)
			Report(0);
	}

	// ---- A pause ----------------------------------------------------------------

	void DapServer::OnPaused(const PauseInfo& pause)
	{
		if (m_Stopping)
			return;
		m_Pause = pause;
		m_Stack = m_Debugger->Stack();
		m_Paused = true;
		m_Resumed = false;
		Json body = Json::Object();
		body.Set("reason", ReasonName(pause.Reason));
		body.Set("threadId", 1);
		body.Set("allThreadsStopped", true);
		if (pause.Reason == PauseReason::Breakpoint)
			body.Set("hitBreakpointIds", Json::Array().Push(pause.Breakpoint));
		if (!pause.Message.empty())
		{
			body.Set("description", pause.Reason == PauseReason::CaughtError ? "Paused on a caught error" : "Paused on an error");
			body.Set("text", pause.Message);
		}
		Event("stopped", std::move(body));
		while (!m_Resumed)
		{
			std::optional<std::string> message = Wait();
			if (!message)
			{
				m_Disconnected = true;
				Stop();
				break;
			}
			Handle(*message);
		}
		m_Paused = false;
		m_Stack.clear();
		if (!m_Stopping)
		{
			Json continued = Json::Object();
			continued.Set("threadId", 1);
			continued.Set("allThreadsContinued", true);
			Event("continued", std::move(continued));
		}
	}

	Json DapServer::BreakpointJson(const Breakpoint& b) const
	{
		Json out = Json::Object();
		out.Set("id", b.Id);
		out.Set("verified", b.Problem.empty());
		out.Set("line", Line(b.ResolvedLine ? b.ResolvedLine : b.Line));
		if (!b.Problem.empty())
			out.Set("message", b.Problem);
		return out;
	}

	Json DapServer::SourceOf(const Frame& frame)
	{
		Json source = Json::Object();
		if (!frame.File.empty())
		{
			auto known = m_EditorPaths.find(frame.File);
			const std::string path = known != m_EditorPaths.end() ? known->second : frame.File;
			source.Set("name", BaseName(path));
			source.Set("path", path);
			return source;
		}
		// A chunk with no file (a standard module): its text by reference.
		int ref = 0;
		for (const auto& [id, chunk] : m_SourceRefs)
			if (chunk == frame.Chunk)
				ref = id;
		if (!ref)
		{
			ref = (int)m_SourceRefs.size() + 1;
			m_SourceRefs[ref] = frame.Chunk;
		}
		source.Set("name", frame.Chunk);
		source.Set("sourceReference", ref);
		source.Set("presentationHint", "deemphasize");
		return source;
	}

	// ---- Requests ----------------------------------------------------------------

	void DapServer::Handle(const std::string& text)
	{
		const std::optional<Json> parsed = Json::Parse(text);
		if (!parsed || (*parsed)["type"].String() != "request")
			return;
		const Json& request = *parsed;
		const std::string& command = request["command"].String();
		const Json& args = request["arguments"];

		if (command == "initialize")
		{
			m_LinesFrom1 = args["linesStartAt1"].Bool(true);
			m_ColumnsFrom1 = args["columnsStartAt1"].Bool(true);
			Json caps = Json::Object();
			caps.Set("supportsConfigurationDoneRequest", true);
			caps.Set("supportsConditionalBreakpoints", true);
			caps.Set("supportsHitConditionalBreakpoints", true);
			caps.Set("supportsLogPoints", true);
			caps.Set("supportsEvaluateForHovers", true);
			caps.Set("supportsTerminateRequest", true);
			caps.Set("supportsExceptionInfoRequest", true);
			caps.Set("supportTerminateDebuggee", true);
			Json filters = Json::Array();
			Json uncaught = Json::Object();
			uncaught.Set("filter", "uncaught");
			uncaught.Set("label", "Uncaught Errors");
			uncaught.Set("description", "Pause on an error that ends the script");
			uncaught.Set("default", true);
			filters.Push(std::move(uncaught));
			Json all = Json::Object();
			all.Set("filter", "all");
			all.Set("label", "All Errors");
			all.Set("description", "Pause on every error, including those a try statement catches");
			all.Set("default", false);
			filters.Push(std::move(all));
			caps.Set("exceptionBreakpointFilters", std::move(filters));
			Respond(request, true, std::move(caps));
			Event("initialized");
		}
		else if (command == "launch")
		{
			if (m_Launched)
				Respond(request, false, Json::Object(), "the session has launched its script already");
			else
				Launch(request);
		}
		else if (command == "attach")
			Respond(request, false, Json::Object(), "cluau debug launches scripts (\"request\": \"launch\"); it does not attach to running ones");
		else if (command == "configurationDone")
		{
			m_Configured = true;
			Respond(request, true);
		}
		else if (command == "setBreakpoints")
		{
			const std::string file = args["source"]["path"].String();
			std::vector<Breakpoint> breakpoints;
			for (const Json& item : args["breakpoints"].Items())
			{
				Breakpoint b;
				b.Id = m_NextBreakpoint++;
				b.Line = LineIn(item["line"].Int());
				b.Condition = item["condition"].String();
				b.HitCondition = item["hitCondition"].String();
				b.LogMessage = item["logMessage"].String();
				breakpoints.push_back(std::move(b));
			}
			if (m_Debugger && !file.empty())
			{
				m_EditorPaths[Debugger::Canonical(file)] = file;
				breakpoints = m_Debugger->SetBreakpoints(file, breakpoints);
			}
			else if (!file.empty())
				Debugger::MarkPastEnd(file, breakpoints);
			if (!file.empty())
				m_Breakpoints[file] = breakpoints;
			Json list = Json::Array();
			for (const Breakpoint& b : breakpoints)
				list.Push(BreakpointJson(b));
			Json body = Json::Object();
			body.Set("breakpoints", std::move(list));
			Respond(request, true, std::move(body));
		}
		else if (command == "setExceptionBreakpoints")
		{
			m_BreakOnUncaught = m_BreakOnCaught = false;
			for (const Json& filter : args["filters"].Items())
			{
				if (filter.String() == "uncaught")
					m_BreakOnUncaught = true;
				else if (filter.String() == "all")
					m_BreakOnCaught = true;
			}
			if (m_Debugger)
				m_Debugger->SetBreakOnErrors(m_BreakOnUncaught, m_BreakOnCaught);
			Respond(request, true);
		}
		else if (command == "threads")
		{
			Json thread = Json::Object();
			thread.Set("id", 1);
			thread.Set("name", "main");
			Json body = Json::Object();
			body.Set("threads", Json::Array().Push(std::move(thread)));
			Respond(request, true, std::move(body));
		}
		else if (command == "stackTrace")
		{
			const size_t start = (size_t)std::max(0, args["startFrame"].Int());
			const int levels = args["levels"].Int();
			Json frames = Json::Array();
			for (size_t i = start; i < m_Stack.size() && (levels <= 0 || i < start + (size_t)levels); i++)
			{
				const Frame& frame = m_Stack[i];
				Json item = Json::Object();
				item.Set("id", (int)i + 1);
				item.Set("name", frame.Function);
				item.Set("source", SourceOf(frame));
				item.Set("line", Line(frame.Line));
				item.Set("column", m_ColumnsFrom1 ? 1 : 0);
				frames.Push(std::move(item));
			}
			Json body = Json::Object();
			body.Set("stackFrames", std::move(frames));
			body.Set("totalFrames", m_Stack.size());
			Respond(request, true, std::move(body));
		}
		else if (command == "scopes")
		{
			const int frame = args["frameId"].Int();
			Json scopes = Json::Array();
			if (frame >= 1 && (size_t)frame <= m_Stack.size())
			{
				Json locals = Json::Object();
				locals.Set("name", "Locals");
				locals.Set("presentationHint", "locals");
				locals.Set("variablesReference", kScopeBase + frame * 2);
				locals.Set("expensive", false);
				scopes.Push(std::move(locals));
				Json upvalues = Json::Object();
				upvalues.Set("name", "Upvalues");
				upvalues.Set("variablesReference", kScopeBase + frame * 2 + 1);
				upvalues.Set("expensive", false);
				scopes.Push(std::move(upvalues));
			}
			Json body = Json::Object();
			body.Set("scopes", std::move(scopes));
			Respond(request, true, std::move(body));
		}
		else if (command == "variables")
		{
			const int reference = args["variablesReference"].Int();
			std::vector<Variable> variables;
			if (m_Debugger && m_Paused)
			{
				if (reference >= kScopeBase)
				{
					const int frame = (reference - kScopeBase) / 2;
					if (frame >= 1 && (size_t)frame <= m_Stack.size())
					{
						const int level = m_Stack[(size_t)frame - 1].Level;
						variables = (reference - kScopeBase) % 2 ? m_Debugger->Upvalues(level) : m_Debugger->Locals(level);
					}
				}
				else
					variables = m_Debugger->Expand(reference);
			}
			Json list = Json::Array();
			for (const Variable& v : variables)
			{
				Json item = Json::Object();
				item.Set("name", v.Name);
				item.Set("value", v.Value);
				item.Set("type", v.Type);
				item.Set("variablesReference", v.Children);
				list.Push(std::move(item));
			}
			Json body = Json::Object();
			body.Set("variables", std::move(list));
			Respond(request, true, std::move(body));
		}
		else if (command == "evaluate")
		{
			if (!m_Debugger || !m_Paused)
			{
				Respond(request, false, Json::Object(), "expressions are evaluated in a paused frame (set a breakpoint, or pause the script)");
				return;
			}
			const int frame = args["frameId"].Int(1);
			const int level = frame >= 1 && (size_t)frame <= m_Stack.size() ? m_Stack[(size_t)frame - 1].Level : 0;
			bool ok = false;
			const Variable result = m_Debugger->Evaluate(args["expression"].String(), level, ok);
			if (!ok)
			{
				Respond(request, false, Json::Object(), result.Value);
				return;
			}
			Json body = Json::Object();
			body.Set("result", result.Value);
			body.Set("type", result.Type);
			body.Set("variablesReference", result.Children);
			Respond(request, true, std::move(body));
		}
		else if (command == "exceptionInfo")
		{
			Json body = Json::Object();
			body.Set("exceptionId", "error");
			body.Set("description", m_Pause.Message);
			body.Set("breakMode", m_Pause.Reason == PauseReason::CaughtError ? "always" : "unhandled");
			Respond(request, true, std::move(body));
		}
		else if (command == "source")
		{
			const int reference = args["sourceReference"].Int(args["source"]["sourceReference"].Int());
			auto chunk = m_SourceRefs.find(reference);
			if (chunk == m_SourceRefs.end() || !m_Runtime)
			{
				Respond(request, false, Json::Object(), "no such source");
				return;
			}
			Json body = Json::Object();
			body.Set("content", ChunkSource(m_Runtime->State(), chunk->second));
			Respond(request, true, std::move(body));
		}
		else if (command == "continue" || command == "next" || command == "stepIn" || command == "stepOut")
		{
			if (!m_Paused)
			{
				Respond(request, false, Json::Object(), "the script is not paused");
				return;
			}
			if (command == "continue")
			{
				m_Debugger->Continue();
				Json body = Json::Object();
				body.Set("allThreadsContinued", true);
				Respond(request, true, std::move(body));
			}
			else
			{
				m_Debugger->Step(command == "next" ? StepKind::Over : command == "stepIn" ? StepKind::Into : StepKind::Out);
				Respond(request, true);
			}
			m_Resumed = true;
		}
		else if (command == "pause")
		{
			if (m_Debugger && !m_Paused)
				m_Debugger->RequestPause();
			Respond(request, true);
		}
		else if (command == "terminate" || command == "disconnect")
		{
			Respond(request, true);
			if (command == "disconnect")
				m_Disconnected = true;
			if (m_Started && !m_Ended)
				Stop();
		}
		else
			Respond(request, false, Json::Object(), "cluau debug does not support '" + command + "'");
	}

} // namespace cluau::runtime::detail

namespace cluau::runtime {

	namespace {

		bool WriteAll(int fd, const std::string& text)
		{
			size_t at = 0;
			while (at < text.size())
			{
				const auto n = CLUAU_WRITE(fd, text.data() + at, text.size() - at);
				if (n <= 0)
					return false;
				at += (size_t)n;
			}
			return true;
		}

		// How much of `text` is whole UTF-8 characters (an output event is
		// JSON text: a character split across two reads waits for the rest).
		size_t WholeCharacters(const std::string& text)
		{
			const size_t n = text.size();
			for (size_t back = 1; back <= 4 && back <= n; back++)
			{
				const unsigned char c = (unsigned char)text[n - back];
				if ((c & 0xC0) == 0x80)
					continue; // a continuation byte
				const size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
				return length > back ? n - back : n;
			}
			return n;
		}

		// A stop the script cannot see (it is blocked in a long call: a
		// sleep, a download) ends the process after a grace period.
		void Watchdog(detail::DapServer* server)
		{
			std::thread([server] {
				std::this_thread::sleep_for(std::chrono::seconds(3));
				if (!server->Finished())
				{
					server->Abandon();
					std::_Exit(0);
				}
			}).detach();
		}

	} // namespace

	int ServeDebugAdapter(const Options& options)
	{
		std::fflush(stdout);
		std::fflush(stderr);
		// The protocol keeps the process's own input and output; the
		// script's standard input is empty, and its output (and its
		// programs') goes through pipes into output events.
		const int in = CLUAU_DUP(0);
		const int out = CLUAU_DUP(1);
		int outPipe[2] = { -1, -1 };
		int errPipe[2] = { -1, -1 };
#ifdef _WIN32
		_setmode(in, _O_BINARY);
		_setmode(out, _O_BINARY);
		const int nul = _open("NUL", _O_RDWR);
		const bool piped = _pipe(outPipe, 65536, _O_BINARY) == 0 && _pipe(errPipe, 65536, _O_BINARY) == 0;
#else
		const int nul = open("/dev/null", O_RDWR);
		const bool piped = pipe(outPipe) == 0 && pipe(errPipe) == 0;
#endif
		if (in < 0 || out < 0 || nul < 0 || !piped)
		{
			std::fprintf(stderr, "cluau debug: cannot set up standard input and output\n");
			return 1;
		}
		CLUAU_DUP2(nul, 0);
		CLUAU_DUP2(outPipe[1], 1);
		CLUAU_DUP2(errPipe[1], 2);
		CLUAU_CLOSE(outPipe[1]);
		CLUAU_CLOSE(errPipe[1]);
#ifdef _WIN32
		std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
		std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
#endif

		struct Pumps
		{
			std::mutex Mutex;
			std::condition_variable Done;
			int Finished = 0;
		};
		auto pumps = std::make_shared<Pumps>();

		detail::DapServer::Io io;
		io.Send = [out](const std::string& message) { WriteAll(out, "Content-Length: " + std::to_string(message.size()) + "\r\n\r\n" + message); };
		io.FinishOutput = [pumps, nul] {
			std::fflush(stdout);
			std::fflush(stderr);
			CLUAU_DUP2(nul, 1);
			CLUAU_DUP2(nul, 2);
			// A program the script started may hold the pipes open: the wait is short.
			std::unique_lock<std::mutex> lock(pumps->Mutex);
			pumps->Done.wait_for(lock, std::chrono::seconds(1), [&] { return pumps->Finished == 2; });
		};
		// Kept to the end of the process: the reader may outlive Run.
		detail::DapServer* server = new detail::DapServer(std::move(io), options);

		auto pump = [server, pumps](int fd, const char* category) {
			std::string pending;
			char buffer[4096];
			for (;;)
			{
				const auto n = CLUAU_READ(fd, buffer, sizeof(buffer));
				if (n <= 0)
					break;
				pending.append(buffer, (size_t)n);
				const size_t whole = WholeCharacters(pending);
				server->Output(category, pending.substr(0, whole));
				pending.erase(0, whole);
			}
			server->Output(category, pending);
			std::lock_guard<std::mutex> lock(pumps->Mutex);
			pumps->Finished++;
			pumps->Done.notify_all();
		};
		std::thread(pump, outPipe[0], "stdout").detach();
		std::thread(pump, errPipe[0], "stderr").detach();

		std::thread([server, in] {
			std::string buffer;
			char chunk[8192];
			for (;;)
			{
				// Headers, an empty line, then Content-Length bytes.
				const size_t end = buffer.find("\r\n\r\n");
				if (end != std::string::npos)
				{
					long length = -1;
					size_t at = 0;
					while (at < end)
					{
						size_t eol = buffer.find("\r\n", at);
						if (eol == std::string::npos || eol > end)
							eol = end;
						std::string header = buffer.substr(at, eol - at);
						std::transform(header.begin(), header.end(), header.begin(), [](unsigned char c) { return (char)std::tolower(c); });
						if (header.rfind("content-length:", 0) == 0)
							length = std::strtol(header.c_str() + 15, nullptr, 10);
						at = eol + 2;
					}
					const size_t body = end + 4;
					if (length < 0)
					{
						buffer.erase(0, body);
						continue;
					}
					if (buffer.size() >= body + (size_t)length)
					{
						std::string message = buffer.substr(body, (size_t)length);
						buffer.erase(0, body + (size_t)length);
						const std::optional<detail::Json> parsed = detail::Json::Parse(message);
						const std::string command = parsed ? (*parsed)["command"].String() : std::string();
						server->Receive(std::move(message));
						if (command == "disconnect" || command == "terminate")
							Watchdog(server);
						continue;
					}
				}
				const auto n = CLUAU_READ(in, chunk, sizeof(chunk));
				if (n <= 0)
					break;
				buffer.append(chunk, (size_t)n);
			}
			server->EndOfInput();
			Watchdog(server);
		}).detach();

		const int status = server->Run();
		std::fflush(stdout);
		return status;
	}

} // namespace cluau::runtime
