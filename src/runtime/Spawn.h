#pragma once

// Starting child processes for process.run, process.exec and process.spawn:
// from an argument list (no shell: each argument reaches the program as it
// is) or a shell command line, with the standard streams captured, merged
// or inherited, and an optional time limit.

#include <memory>
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
	};

	struct SpawnResult
	{
		int Code = 0; // the exit code (128 + the signal for a POSIX child a signal ended)
		std::string Output;
		std::string Error;
		bool TimedOut = false;
	};

	// Runs the child to its end. False (with `why`) when it cannot be started.
	bool Spawn(const SpawnRequest& request, SpawnResult& result, std::string& why);

	// A child running alongside the script (process.spawn): its standard
	// output read as it comes, its standard input written as the script
	// goes. Its standard error is inherited or merged into the output.
	class Child
	{
	public:
		// `pipeInput`: the script writes the child's standard input
		// (otherwise the child reads the script's).
		static std::unique_ptr<Child> Start(const SpawnRequest& request, bool pipeInput, std::string& why);
		~Child();

		bool Read(std::string& chunk);    // the next output there is; false at its end
		bool ReadLine(std::string& line); // a line without its newline; false at the end
		bool Write(const std::string& text, std::string& why);
		void CloseInput();
		int Wait(); // closes the input, keeps the rest of the output for Read, returns the exit code
		void Kill();
		long Id() const;

		struct Impl;

	private:
		explicit Child(std::unique_ptr<Impl> impl);
		bool Fill(); // more output into the buffer; false at its end
		std::unique_ptr<Impl> m_Impl;
		std::string m_Buffer;
		bool m_Ended = false;
		bool m_Waited = false;
		int m_Code = 0;
	};

} // namespace cluau::runtime::detail
