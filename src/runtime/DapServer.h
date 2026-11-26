#pragma once

// The Debug Adapter Protocol over the debugger: what `cluau debug` speaks
// on standard input and output. The editor launches a script (`launch`
// with its path), sets breakpoints, and is told of each pause (`stopped`),
// then reads the paused stack, its scopes and variables, evaluates
// expressions and continues or steps. The script's output arrives as
// `output` events; its exit as `exited` and `terminated`.
//
// Messages arrive on any thread (Receive) and are handled on the thread
// that calls Run, which also runs the script: while it runs, requests are
// handled as the script reaches Luau's safepoints; while it is paused, as
// they arrive. Send is called with each outgoing message's text, from Run's
// thread or from whichever thread calls Output.

#include "Debugger.h"
#include "Json.h"

#include "cluau/runtime.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cluau::runtime::detail {

	class DapServer
	{
	public:
		struct Io
		{
			// One message's JSON text, to frame and write.
			std::function<void(const std::string&)> Send;
			// Before the end is reported: the script's output written so far
			// has all been given to Output.
			std::function<void()> FinishOutput;
		};

		// `base`: the options every launch starts from (the search path).
		DapServer(Io io, Options base = {});
		~DapServer();

		// A message from the editor (any thread).
		void Receive(std::string message);
		// The editor's side closed (any thread).
		void EndOfInput();
		// The script's output: an `output` event in `category` ("stdout",
		// "stderr", "console").
		void Output(const std::string& category, const std::string& text);

		// Handles messages, and runs the launched script, until the editor
		// disconnects; returns the exit status.
		int Run();
		// The script has ended (or Run has returned).
		bool Finished() const { return m_Finished; }
		// The session ends without the script's end (it did not reach a
		// safepoint to stop at): `terminated`, from any thread.
		void Abandon();

	private:
		struct Pending
		{
			std::vector<Breakpoint> Breakpoints;
		};

		std::optional<std::string> Wait();
		void Poll();
		void Handle(const std::string& text);
		void Respond(const Json& request, bool success, Json body = Json::Object(), const std::string& message = {});
		void Event(const std::string& name, Json body = Json::Object());
		void Send(Json message);

		void Launch(const Json& request);
		void RunScript();
		void OnPaused(const PauseInfo& pause);
		void Ended(int code);
		// `exited` (with the status) and `terminated`, once.
		void Report(int code);
		void Stop();

		Json BreakpointJson(const Breakpoint& b) const;
		Json SourceOf(const Frame& frame);
		int Line(int line) const { return m_LinesFrom1 ? line : line - 1; }
		int LineIn(int line) const { return m_LinesFrom1 ? line : line + 1; }

		static void ExitHook(int code);

		Io m_Io;
		Options m_Base;

		std::mutex m_QueueMutex;
		std::condition_variable m_QueueReady;
		std::deque<std::string> m_Queue;
		bool m_InputEnded = false;
		std::atomic<int> m_Queued{ 0 };

		std::mutex m_SendMutex;
		int m_Seq = 1;

		bool m_LinesFrom1 = true;
		bool m_ColumnsFrom1 = true;
		bool m_Configured = false;
		bool m_Launched = false;
		bool m_Started = false;
		std::atomic<bool> m_Ended{ false }; // `exited` and `terminated` sent
		bool m_Disconnected = false;
		bool m_Stopping = false;
		std::atomic<bool> m_Finished{ false };

		// The launch's settings.
		std::string m_Program;
		std::vector<std::string> m_Args;
		std::string m_Cwd;
		bool m_StopOnEntry = false;
		bool m_NoDebug = false;
		bool m_Sandbox = false;
		bool m_Test = false;   // the program's @std/test cases, as `cluau test` runs them
		std::string m_Filter;  // and only those whose names hold this (-k)

		// Breakpoints by file as the editor names it, and the debugger's view.
		std::map<std::string, std::vector<Breakpoint>> m_Breakpoints;
		int m_NextBreakpoint = 1;
		bool m_BreakOnUncaught = true;
		bool m_BreakOnCaught = false;

		std::unique_ptr<Runtime> m_Runtime;
		std::unique_ptr<Debugger> m_Debugger;
		// While paused: the stack, the editor's names for its files, the
		// pause, and whether a continue or step was chosen.
		std::vector<Frame> m_Stack;
		std::map<std::string, std::string> m_EditorPaths; // canonical -> as the editor names it
		std::map<int, std::string> m_SourceRefs; // sourceReference -> chunk name
		PauseInfo m_Pause;
		bool m_Paused = false;
		bool m_Resumed = false;
	};

} // namespace cluau::runtime::detail
