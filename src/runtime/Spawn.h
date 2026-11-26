#pragma once

// Starting child processes for process.run, process.exec and process.spawn:
// from an argument list (no shell: each argument reaches the program as it
// is) or a shell command line, with the standard streams captured, merged
// or inherited, and an optional time limit.
//
// A wait on a child (a run, a read, a write, a wait) stops when a Ctrl-C
// comes after the call was made, and kills the child. A child some wait is
// on when the process exits is killed then too: nothing would read what it
// gives.

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cluau::runtime::detail {

	enum class Stream { Inherit, Capture, Merge }; // Merge: standard error into the captured output

	struct SpawnRequest
	{
		std::vector<std::string> Args; // the program and its arguments
		bool Shell = false;            // Args[0] is a command line for the system shell
		std::optional<std::string> Input; // the child's standard input (none: it inherits the script's)
		std::string Cwd;                  // "" = the script's working directory
		std::vector<std::pair<std::string, std::string>> Env; // variables set on top of the script's environment
		bool CaptureOutput = false;
		Stream Error = Stream::Inherit;
		double Timeout = 0; // seconds; past it the child (and what it started) is killed. 0: no limit
		unsigned Interrupts = 0; // InterruptCount() when the call was made: a later Ctrl-C ends the run
	};

	struct SpawnResult
	{
		int Code = 0; // the exit code (128 + the signal for a POSIX child a signal ended)
		std::string Output;
		std::string Error;
		bool TimedOut = false;    // the child was still running at the limit
		bool Interrupted = false; // a Ctrl-C ended the run (the child was killed)
	};

	// Kills every child process.spawn started that is still running: a run
	// that a Ctrl-C ends takes the programs it started with it.
	void KillChildren();

	// Runs the child to its end. False (with `why`) when it cannot be started.
	bool Spawn(const SpawnRequest& request, SpawnResult& result, std::string& why);

	// Held while the environment changes (process.setEnv) and while a launch
	// copies it: a child may be starting on another thread.
	std::mutex& EnvironmentLock();

	// Asked, with the output's lock held, whether a read takes what it found:
	// false when the read was given up (its task cancelled), and the text
	// stays for the next read.
	using ReadClaim = std::function<bool()>;

	// A child running alongside the script (process.spawn): its standard
	// output read as it comes, its standard input written as the script
	// goes. Its standard error is inherited or merged into the output.
	// Any thread may call any method, at the same time as others: reads
	// share the output out between them, and a wait gets the exit code
	// however many there are.
	class Child
	{
	public:
		// `pipeInput`: the script writes the child's standard input
		// (otherwise the child reads the script's).
		static std::unique_ptr<Child> Start(const SpawnRequest& request, bool pipeInput, std::string& why);
		~Child();

		// `interrupts` is InterruptCount() when the call was made: a later
		// Ctrl-C ends the wait and kills the child.

		// The output there is (waiting for some); false at its end.
		bool Read(std::string& chunk, unsigned interrupts, const ReadClaim& claim = {});
		// A line with its line end ("\n", "\r\n", or none for a last line
		// without one); false at the end.
		bool ReadLine(std::string& line, unsigned interrupts, const ReadClaim& claim = {});
		// Puts text back in front of the output still to be read.
		void Unread(const std::string& text);
		// A read (of a line, or of a chunk) would not wait: the output has it
		// already, or has ended.
		bool Ready(bool line);
		bool Write(const std::string& text, std::string& why, unsigned interrupts);
		void CloseInput();
		// Closes the input and waits for the child to end, reading its output
		// meanwhile (kept for Read); returns the exit code.
		int Wait(unsigned interrupts);
		void Kill();
		long Id() const;

		struct Impl;

	private:
		enum class Fill { Data, End, Idle };
		explicit Child(std::unique_ptr<Impl> impl);
		Fill FillFor(int milliseconds); // more output into the buffer; m_Reading held
		void Fetch(int milliseconds, unsigned fills); // one turn at filling the buffer, unless another filled it since `fills`
		bool TryReap(int& code);        // the child has ended (with `code`)
		std::unique_ptr<Impl> m_Impl;
		std::timed_mutex m_Reading; // one thread reads the pipe at a time
		std::mutex m_Output;        // m_Buffer, m_Ended, m_Fills
		std::string m_Buffer;
		bool m_Ended = false;
		unsigned m_Fills = 0; // bumps when the buffer gets more
		std::mutex m_Writing; // one write at a time
		std::mutex m_Input;   // the input's state: a close while a write runs waits for it to stop
		bool m_WriteRunning = false;
		bool m_CloseWanted = false;
	};

	// Windows program lookup and batch files, here (not only in the Windows
	// build) so they can be tested anywhere. Strings are UTF-8.

	// The program `name` names: as it is when it holds a path (a separator
	// or a drive), otherwise the first match in `path`'s directories (`;`
	// between them; relative ones are skipped, so the current directory is
	// never searched unless the name says so). A name without an extension
	// is tried with each of `pathext`'s (";.COM;.EXE..."), as cmd.exe does.
	// "" when there is none.
	std::string FindProgram(const std::string& name, const std::string& path, const std::string& pathext,
		const std::function<bool(const std::string&)>& isFile);

	// The program is a batch file (.bat or .cmd), which Windows runs through
	// cmd.exe.
	bool IsBatchFile(const std::string& program);

	// The rest of cmd.exe's command line (after its own name) that runs the
	// batch file `script` with `args`, each reaching it as one argument with
	// nothing in it seen by cmd.exe as its own syntax. False (with `why`)
	// for what cannot be passed that way.
	bool BatchCommandLine(const std::string& script, const std::vector<std::string>& args, std::string& line, std::string& why);

} // namespace cluau::runtime::detail
