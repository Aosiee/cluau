#pragma once

// The debugger under `cluau debug`: line breakpoints (with conditions, hit
// counts and log messages), pausing on errors, stepping, and the paused
// stack's frames, variables and expressions, over Luau's debug API.
//
// A pause is synchronous: Luau's break or step callback (or the error, or
// the interrupt callback for a requested pause) calls the host's Paused,
// which returns once the host chose to continue or step. Meanwhile the
// script's thread is stopped in place and its frames are read directly.
//
// Breakpoints belong to files (absolute paths): Luau patches a BREAK into
// every chunk loaded from the file, including chunks loaded after the
// breakpoint was set. The runtime compiles to bytecode only while a
// debugger is attached (native code has no breakpoints).
//
// Luau enters single-step mode only as a thread enters the VM, so a step
// from a breakpoint breaks the thread out of the VM (the runtime resumes
// it at once). Inside a call made from C (a sort comparator, a metamethod)
// the thread cannot break out: there a step arms a breakpoint on every
// line of the function and of the Lua function below the C call, and
// stops at the first of them reached.
//
// Expressions (conditions, log messages, watches, the console) are cluau
// (or Luau, in a .luau file), compiled as `return (<expression>)` and run
// in a fresh thread whose environment is the frame's locals, then its
// upvalues, over its globals. They read; nothing is written back.

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace cluau::runtime::detail {

	// What the runtime tells a debugger while one is attached.
	class DebugHooks
	{
	public:
		virtual ~DebugHooks() = default;
		// A chunk loaded (its function on top of L's stack): `name` is the
		// name errors give it, `file` its absolute path ("" for a chunk with
		// no file: a standard or host module, `-e`).
		virtual void OnLoaded(lua_State* L, const std::string& name, const std::string& file) = 0;
		// The script ended in an error; its thread keeps the stack.
		virtual void OnUncaught(lua_State* T, const std::string& message) = 0;
		// The script's thread came back broken out of the VM: resumes what
		// broke (a coroutine before the threads resuming it); returns T's
		// status (still LUA_BREAK while a coroutine remains broken).
		virtual int ResumeBroken(lua_State* T) = 0;
	};

	// The runtime's side (Runtime.cpp). Attaching also replaces `require`
	// with one written in Luau that runs each module's chunk itself, so
	// that a step can enter a module's top level.
	void SetDebugHooks(lua_State* L, DebugHooks* hooks);
	// `return (<expression>)` as bytecode, translated from cluau unless
	// `luau`; "" with the error otherwise.
	std::string CompileExpression(const std::string& expression, bool luau, std::string& error);
	// A loaded chunk's source text, by the name errors give it.
	std::string ChunkSource(lua_State* L, const std::string& name);

	struct Breakpoint
	{
		int Id = 0;              // the caller's, or assigned by SetBreakpoints when 0
		std::string File;        // absolute
		int Line = 0;            // 1-based, as set
		std::string Condition;   // an expression; "" for every hit
		std::string HitCondition; // "5" or ">= 5" (from the 5th hit), "== 5", "> 5", "% 5" (every 5th)
		std::string LogMessage;  // logs ({expression} replaced by its value) instead of pausing
		// Kept by the debugger:
		int Hits = 0;
		int ResolvedLine = 0;    // where Luau put it (the next line with code); 0 before the file loads
		std::string Problem;     // the condition or hit count does not compile
	};

	enum class PauseReason { Breakpoint, Step, Entry, Pause, Error, CaughtError };

	struct PauseInfo
	{
		PauseReason Reason = PauseReason::Breakpoint;
		int Breakpoint = 0;
		std::string Message; // the error
	};

	struct Frame
	{
		int Level = 0;        // Luau's level (C frames are left out)
		std::string Function; // "Length", "(top level)", "(anonymous)"
		std::string Chunk;    // the name errors give it
		std::string File;     // absolute; "" for a chunk with no file
		int Line = 0;
	};

	struct Variable
	{
		std::string Name;
		std::string Value;
		std::string Type;
		int Children = 0; // a table's handle for Expand (0 for none)
	};

	enum class StepKind { Into, Over, Out };

	class Debugger : public DebugHooks
	{
	public:
		struct Callbacks
		{
			// A pause: returns once Continue or Step was called.
			std::function<void(const PauseInfo&)> Paused;
			// A log message's text, and the debugger's own notes.
			std::function<void(const std::string&)> Log;
			// Called often while the script runs (to read requests).
			std::function<void()> Poll;
			// A breakpoint moved to the line with code, or found a problem.
			std::function<void(const Breakpoint&)> Changed;
		};

		Debugger(lua_State* L, Callbacks callbacks);
		~Debugger() override;
		Debugger(const Debugger&) = delete;
		Debugger& operator=(const Debugger&) = delete;

		// Replaces `file`'s breakpoints; returns them with ids and lines.
		std::vector<Breakpoint> SetBreakpoints(const std::string& file, std::vector<Breakpoint> breakpoints);
		void SetBreakOnErrors(bool uncaught, bool caught);
		// Pause at the first line of the script.
		void StopOnEntry();
		// Pause as soon as possible (from any thread).
		void RequestPause() { m_PauseRequested = true; }
		// End the script: no more pauses, and an error at its next safepoint.
		void Stop() { m_Stopping = true; }

		// ---- While paused (inside Paused) ----------------------------------
		bool IsPaused() const { return m_Paused != nullptr; }
		std::vector<Frame> Stack();
		std::vector<Variable> Locals(int level);
		std::vector<Variable> Upvalues(int level);
		std::vector<Variable> Expand(int handle);
		// The expression's value (Name is empty; a table can be expanded), or
		// false with the error in Value.
		Variable Evaluate(const std::string& expression, int level, bool& ok);
		void Continue();
		void Step(StepKind kind);

		// ---- DebugHooks --------------------------------------------------
		void OnLoaded(lua_State* L, const std::string& name, const std::string& file) override;
		void OnUncaught(lua_State* T, const std::string& message) override;
		int ResumeBroken(lua_State* T) override;

		// The absolute, normalised form breakpoints and chunks compare by.
		static std::string Canonical(const std::string& path);

	private:
		struct Chunk
		{
			std::string Name;
			std::string File; // canonical
			int Ref = 0;
		};
		enum class Resume { None, Continue, Step };

		static Debugger* Of(lua_State* L);
		static void BreakCallback(lua_State* L, lua_Debug* ar);
		static void StepCallback(lua_State* L, lua_Debug* ar);
		static void InterruptCallback(lua_State* L, int gc);
		static void ProtectedErrorCallback(lua_State* L);
		static void PreResumeCallback(lua_State* L);
		static void InterruptedCallback(lua_State* L, lua_Debug* ar);

		void OnBreak(lua_State* L);
		void OnStep(lua_State* L);
		void OnInterrupt(lua_State* L);
		void OnProtectedError(lua_State* L);

		void Arm(const std::string& file, int line);
		// A step where the thread cannot leave the VM: by breakpoints.
		void StartLineStep(lua_State* L, int line);
		void EndLineStep();
		bool LineStepStops(lua_State* L, int line);
		// `fromBreak`: at a BREAK (a step breaks the thread out of the VM
		// and skips that BREAK once on resuming); `stepping`: the thread
		// runs in single-step mode.
		void Pause(lua_State* L, PauseInfo pause, bool fromBreak, bool stepping);
		const Chunk* ChunkNamed(const std::string& name) const;
		bool IsUserChunk(const std::string& name) const;
		bool IsLuauChunk(const std::string& name) const;
		bool Hit(Breakpoint& b) const;
		// `described`: the value kept for the pause (a table expandable).
		bool RunExpression(lua_State* L, int level, const std::string& bytecode, std::string& text, bool* truthy, Variable* described = nullptr);
		std::string FormatLog(lua_State* L, Breakpoint& b);
		std::string Format(lua_State* S, int index);
		Variable Describe(const std::string& name);
		void Release();

		lua_State* m_Main;
		Callbacks m_Callbacks;
		std::vector<Chunk> m_Chunks;
		std::vector<Breakpoint> m_Breakpoints;
		std::map<int, std::string> m_Conditions; // compiled, by breakpoint
		int m_NextId = 1;
		bool m_BreakOnUncaught = true;
		bool m_BreakOnCaught = false;
		bool m_EntryPending = false;
		std::atomic<bool> m_PauseRequested{ false };
		std::atomic<bool> m_Stopping{ false };
		unsigned m_Ticks = 0;
		// A thread broken out because the coroutine it resumed broke: the coroutine.
		std::map<lua_State*, lua_State*> m_BrokenChild;

		lua_State* m_Paused = nullptr;
		PauseInfo m_Pause;
		Resume m_Resume = Resume::None;
		StepKind m_StepKind = StepKind::Over;
		lua_State* m_Scratch = nullptr; // the pause's thread for formatting
		int m_ScratchRef = 0;
		std::vector<int> m_Handles; // the pause's expandable tables (registry refs)
		bool m_Evaluating = false;

		lua_State* m_StepThread = nullptr;
		int m_StepDepth = 0;
		int m_StepLine = 0;
		// A thread broken out at a BREAK runs it again on resuming: that hit is skipped.
		lua_State* m_SkipBreakThread = nullptr;
		int m_SkipBreakLine = 0;

		struct LineStep
		{
			bool Active = false;
			lua_State* Thread = nullptr;
			StepKind Kind = StepKind::Over;
			int Function = 0, Caller = 0; // registry refs (0 for none)
			int Depth = 0, CallerDepth = 0, Line = 0;
			std::vector<std::pair<int, int>> Armed; // function ref, line
		} m_LineStep;
	};

} // namespace cluau::runtime::detail
