#pragma once

// Starting a child process for process.run / process.exec: from an
// argument list (no shell: each argument reaches the program as it is) or a
// shell command line, with its standard streams captured, merged or
// inherited.

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
	};

	struct SpawnResult
	{
		int Code = 0; // the exit code (128 + the signal for a POSIX child a signal ended)
		std::string Output;
		std::string Error;
	};

	// Runs the child to its end. False (with `why`) when it cannot be started.
	bool Spawn(const SpawnRequest& request, SpawnResult& result, std::string& why);

} // namespace cluau::runtime::detail
