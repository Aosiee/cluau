#include "Spawn.h"
#include "Std.h" // InterruptPending, InterruptCount

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <thread>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

#else

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

// The process's environment (a child's starts from it).
extern char** environ;

#endif

namespace cluau::runtime::detail {

	namespace {

		using Clock = std::chrono::steady_clock;

		// After a child is killed, how long its output is still read: what it
		// started may hold the pipes open (and outside its group, live on).
		constexpr auto kGrace = std::chrono::milliseconds(250);

		// The moment a time limit runs out (none, or one too far to count:
		// the far future).
		Clock::time_point Deadline(double seconds)
		{
			if (!(seconds > 0) || !(seconds < 1e9))
				return Clock::time_point::max();
			return Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
		}

		// Milliseconds left before `deadline` (-1: no limit).
		int Remaining(Clock::time_point deadline)
		{
			if (deadline == Clock::time_point::max())
				return -1;
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
			return left < 0 ? 0 : (int)std::min<long long>(left, 1 << 30);
		}

		// A Ctrl-C has come since the count was `since` (or is waiting to
		// stop the script).
		bool Interrupted(unsigned since) { return InterruptPending() || InterruptCount() != since; }

		void Pause(int milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }

	} // namespace

	std::mutex& EnvironmentLock()
	{
		static std::mutex* lock = new std::mutex; // never destroyed: jobs may launch while the process exits
		return *lock;
	}

#ifdef _WIN32

	namespace {

		std::wstring Wide(const std::string& s)
		{
			if (s.empty())
				return {};
			const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
			std::wstring out((size_t)n, L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
			return out;
		}

		std::string Narrow(const std::wstring& s)
		{
			if (s.empty())
				return {};
			const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
			std::string out((size_t)n, '\0');
			WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
			return out;
		}

		std::string LastError()
		{
			const DWORD code = GetLastError();
			char* text = nullptr;
			FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
				(LPSTR)&text, 0, nullptr);
			std::string out = text ? text : "error " + std::to_string(code);
			if (text)
				LocalFree(text);
			while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '.'))
				out.pop_back();
			return out;
		}

		// One argument as the C runtime's command-line parser reads it back.
		void AppendQuoted(std::wstring& line, const std::wstring& arg)
		{
			if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
			{
				line += arg;
				return;
			}
			line += L'"';
			for (size_t i = 0;; i++)
			{
				size_t backslashes = 0;
				while (i < arg.size() && arg[i] == L'\\')
				{
					i++;
					backslashes++;
				}
				if (i == arg.size())
				{
					line.append(backslashes * 2, L'\\');
					break;
				}
				if (arg[i] == L'"')
				{
					line.append(backslashes * 2 + 1, L'\\');
					line += L'"';
				}
				else
				{
					line.append(backslashes, L'\\');
					line += arg[i];
				}
			}
			line += L'"';
		}

		// The environment block: the script's variables with the request's
		// on top (names compare without case, as Windows does).
		std::wstring EnvironmentBlock(const std::vector<std::pair<std::string, std::string>>& overrides)
		{
			struct NoCase
			{
				bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; }
			};
			std::map<std::wstring, std::wstring, NoCase> vars;
			if (wchar_t* block = GetEnvironmentStringsW())
			{
				for (const wchar_t* p = block; *p; p += wcslen(p) + 1)
				{
					const std::wstring entry = p;
					const size_t eq = entry.find(L'=', 1); // `=C:=...` entries start with '='
					if (eq != std::wstring::npos)
						vars[entry.substr(0, eq)] = entry.substr(eq + 1);
				}
				FreeEnvironmentStringsW(block);
			}
			for (const auto& [name, value] : overrides)
				vars[Wide(name)] = Wide(value);
			std::wstring out;
			for (const auto& [name, value] : vars)
			{
				out += name + L"=" + value;
				out += L'\0';
			}
			out += L'\0';
			return out;
		}

		// A variable as the child will see it: the request's, or the script's.
		std::string ChildVariable(const SpawnRequest& request, const char* name)
		{
			for (const auto& [key, value] : request.Env)
				if (_stricmp(key.c_str(), name) == 0)
					return value;
			const std::wstring wide = Wide(name);
			const DWORD n = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
			if (n == 0)
				return {};
			std::wstring value(n, L'\0');
			value.resize(GetEnvironmentVariableW(wide.c_str(), value.data(), n));
			return Narrow(value);
		}

		std::string Directory(UINT (WINAPI* get)(LPWSTR, UINT))
		{
			wchar_t buffer[MAX_PATH + 1];
			const UINT n = get(buffer, MAX_PATH + 1);
			return n > 0 && n <= MAX_PATH ? Narrow(std::wstring(buffer, n)) : std::string();
		}

		bool IsFile(const std::string& path)
		{
			const DWORD attributes = GetFileAttributesW(Wide(path).c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
		}

		void Close(HANDLE& h)
		{
			if (h)
			{
				CloseHandle(h);
				h = nullptr;
			}
		}

		// A started child: the parent's ends of its pipes, and a job object
		// holding it and everything it starts (so a kill reaches them all).
		struct Started
		{
			PROCESS_INFORMATION Process{};
			HANDLE Job = nullptr;
			HANDLE In = nullptr;  // the child's standard input (we write)
			HANDLE Out = nullptr; // its output (we read)
			HANDLE Err = nullptr; // its standard error, captured on its own
			bool Reaped = false;  // it has ended, with Code (both under the children's lock)
			int Code = 0;
		};

		// Kills the child and the rest of its job (`afterEnd`: the rest even
		// when the child has ended). The children's lock is held.
		void KillLocked(Started& started, bool afterEnd)
		{
			if (started.Job && (!started.Reaped || afterEnd))
				TerminateJobObject(started.Job, 1);
			else if (!started.Reaped)
				TerminateProcess(started.Process.hProcess, 1);
		}

		bool ReapLocked(Started& started)
		{
			if (started.Reaped)
				return true;
			if (WaitForSingleObject(started.Process.hProcess, 0) != WAIT_OBJECT_0)
				return false;
			DWORD code = 0;
			GetExitCodeProcess(started.Process.hProcess, &code);
			started.Code = (int)code;
			started.Reaped = true;
			return true;
		}

	} // namespace

#else

	namespace {

		void CloseFd(int& fd)
		{
			if (fd >= 0)
			{
				close(fd);
				fd = -1;
			}
		}

		bool MakePipe(int fds[2])
		{
			if (pipe(fds) != 0)
				return false;
			fcntl(fds[0], F_SETFD, FD_CLOEXEC);
			fcntl(fds[1], F_SETFD, FD_CLOEXEC);
			return true;
		}

		// A started child: its process (in a group of its own when it may
		// be killed, so a kill reaches what it starts) and the parent's ends
		// of its pipes.
		struct Started
		{
			pid_t Pid = -1;
			bool Group = false;
			int In = -1;         // the child's standard input (we write)
			int Out = -1;        // its output (we read)
			int Err = -1;        // its standard error, captured on its own
			bool Reaped = false; // it has ended, with Code (both under the children's lock)
			int Code = 0;
		};

		int ExitCode(int wait)
		{
			if (WIFEXITED(wait))
				return WEXITSTATUS(wait);
			if (WIFSIGNALED(wait))
				return 128 + WTERMSIG(wait);
			return wait;
		}

		// Kills the child and its group (`afterEnd`: the group even when the
		// child has ended, for what it left running). The children's lock is
		// held: a child is reaped under it too, so its pid is never one the
		// system has given another process since.
		void KillLocked(Started& started, bool afterEnd)
		{
			if (started.Pid <= 0)
				return;
			if (started.Group && (!started.Reaped || afterEnd))
				kill(-started.Pid, SIGKILL);
			if (!started.Reaped)
				kill(started.Pid, SIGKILL);
		}

		bool ReapLocked(Started& started)
		{
			if (started.Reaped)
				return true;
			int wait = 0;
			const pid_t done = waitpid(started.Pid, &wait, WNOHANG);
			if (done == started.Pid)
				started.Code = ExitCode(wait);
			else if (!(done < 0 && errno == ECHILD)) // ECHILD: reaped already (SIGCHLD ignored), its code lost
				return false;
			started.Reaped = true;
			return true;
		}

		// While a thread writes to a child: a write to one that has gone is
		// an error (EPIPE), not the SIGPIPE that would end the script. The
		// signal is blocked on this thread only and, when a write raised it,
		// taken back before it is unblocked.
		class SigpipeGuard
		{
		public:
			SigpipeGuard()
			{
				sigemptyset(&m_Pipe);
				sigaddset(&m_Pipe, SIGPIPE);
				pthread_sigmask(SIG_BLOCK, &m_Pipe, &m_Old);
				sigset_t pending;
				sigemptyset(&pending);
				m_WasPending = sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1;
			}
			~SigpipeGuard()
			{
				sigset_t pending;
				sigemptyset(&pending);
				if (m_Broken && !m_WasPending && sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1)
				{
					int signal = 0;
					sigwait(&m_Pipe, &signal);
				}
				pthread_sigmask(SIG_SETMASK, &m_Old, nullptr);
			}
			SigpipeGuard(const SigpipeGuard&) = delete;
			SigpipeGuard& operator=(const SigpipeGuard&) = delete;
			void Broken() { m_Broken = true; } // a write failed with EPIPE

		private:
			sigset_t m_Pipe;
			sigset_t m_Old;
			bool m_WasPending = false;
			bool m_Broken = false;
		};

	} // namespace

#endif

	namespace {

		// The children some wait is on (killed if the process exits: nothing
		// would read what they give), and on POSIX those dropped while still
		// running (reaped later, not left as zombies).
		struct Children
		{
			std::mutex Lock; // also around every kill and reap
			std::map<Started*, int> Busy;
			std::set<Started*> Live; // every process.spawn child not yet dropped
#ifndef _WIN32
			std::vector<pid_t> Abandoned;
#endif
		};

		Children& Registry()
		{
			static Children* children = new Children; // never destroyed: used at exit
			return *children;
		}

		void KillBusy()
		{
			Children& children = Registry();
			std::lock_guard<std::mutex> lock(children.Lock);
			for (auto& [started, count] : children.Busy)
				KillLocked(*started, false);
		}

		// Marks the child as waited on for the scope's life.
		class Busy
		{
		public:
			explicit Busy(Started& started)
				: m_Started(started)
			{
				static std::once_flag once;
				std::call_once(once, [] { std::atexit(KillBusy); });
				Children& children = Registry();
				std::lock_guard<std::mutex> lock(children.Lock);
				children.Busy[&m_Started]++;
			}
			~Busy()
			{
				Children& children = Registry();
				std::lock_guard<std::mutex> lock(children.Lock);
				if (--children.Busy[&m_Started] == 0)
					children.Busy.erase(&m_Started);
			}
			Busy(const Busy&) = delete;
			Busy& operator=(const Busy&) = delete;

		private:
			Started& m_Started;
		};

		void Track(Started& started)
		{
			Children& children = Registry();
			std::lock_guard<std::mutex> lock(children.Lock);
			children.Live.insert(&started);
		}

		void Untrack(Started& started)
		{
			Children& children = Registry();
			std::lock_guard<std::mutex> lock(children.Lock);
			children.Live.erase(&started);
		}

		void KillTree(Started& started, bool afterEnd = false)
		{
			std::lock_guard<std::mutex> lock(Registry().Lock);
			KillLocked(started, afterEnd);
		}

		// The child has ended: its code into `code`.
		bool Reaped(Started& started, int& code)
		{
			std::lock_guard<std::mutex> lock(Registry().Lock);
			if (!ReapLocked(started))
				return false;
			code = started.Code;
			return true;
		}

		bool Reaped(Started& started)
		{
			int code = 0;
			return Reaped(started, code);
		}

	} // namespace

#ifdef _WIN32

	namespace {

		// One launch at a time: the pipes are inheritable while a child is
		// made, and another child made then would hold their ends too.
		std::mutex g_Launching;

		bool Launch(const SpawnRequest& request, bool pipeInput, Started& started, std::string& why)
		{
			std::lock_guard<std::mutex> launching(g_Launching);
			std::wstring application; // "": the command line's first word is the program
			std::wstring line;
			if (request.Shell)
			{
				const wchar_t* comspec = _wgetenv(L"ComSpec");
				line = L"\"" + std::wstring(comspec ? comspec : L"cmd.exe") + L"\" /d /s /c \"" + Wide(request.Args[0]) + L"\"";
			}
			else
			{
				// The program from the system's directories, then PATH (with
				// PATHEXT): CreateProcess's own order, but for the current
				// directory, which it looks in first. (The system's own
				// programs, such as its tar, come before others of the same
				// name on PATH, as they always have.)
				const std::string& name = request.Args[0];
				std::string path;
				for (const std::string& dir : { Directory(GetSystemDirectoryW), Directory(GetWindowsDirectoryW) })
					if (!dir.empty())
						path += dir + ";";
				path += ChildVariable(request, "PATH");
				std::string program = FindProgram(name, path, ChildVariable(request, "PATHEXT"), IsFile);
				if (program.empty())
				{
					if (name.find_first_of(":\\/") == std::string::npos)
					{
						why = "no such program on PATH";
						return false;
					}
					program = name; // a path: CreateProcess says what is wrong with it
				}
				if (IsBatchFile(program))
				{
					// cmd.exe runs it, and reads its arguments with rules of its
					// own: they are written so that it takes each as it is.
					const std::string system = Directory(GetSystemDirectoryW);
					const std::string cmd = (system.empty() ? std::string("C:\\Windows\\System32") : system) + "\\cmd.exe";
					std::string rest;
					if (!BatchCommandLine(program, std::vector<std::string>(request.Args.begin() + 1, request.Args.end()), rest, why))
						return false;
					application = Wide(cmd);
					line = L"\"" + application + L"\" " + Wide(rest);
				}
				else
				{
					application = Wide(program);
					for (size_t i = 0; i < request.Args.size(); i++)
					{
						if (i)
							line += L' ';
						AppendQuoted(line, Wide(request.Args[i]));
					}
				}
			}

			SECURITY_ATTRIBUTES inherit{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
			HANDLE inRead = nullptr, outWrite = nullptr, errWrite = nullptr;
			auto fail = [&] {
				why = LastError();
				for (HANDLE* h : { &inRead, &outWrite, &errWrite, &started.In, &started.Out, &started.Err })
					Close(*h);
				return false;
			};
			auto makePipe = [&](HANDLE& read, HANDLE& write, bool parentWrites) {
				if (!CreatePipe(&read, &write, &inherit, 0))
					return false;
				SetHandleInformation(parentWrites ? write : read, HANDLE_FLAG_INHERIT, 0);
				return true;
			};
			const bool capture = request.CaptureOutput || request.Error == Stream::Merge;
			if ((pipeInput && !makePipe(inRead, started.In, true)) || (capture && !makePipe(started.Out, outWrite, false)) ||
				(request.Error == Stream::Capture && !makePipe(started.Err, errWrite, false)))
				return fail();

			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdInput = pipeInput ? inRead : GetStdHandle(STD_INPUT_HANDLE);
			startup.hStdOutput = capture ? outWrite : GetStdHandle(STD_OUTPUT_HANDLE);
			startup.hStdError = request.Error == Stream::Capture ? errWrite
				: request.Error == Stream::Merge                 ? outWrite
																  : GetStdHandle(STD_ERROR_HANDLE);
			std::wstring env;
			if (!request.Env.empty())
			{
				std::lock_guard<std::mutex> environment(EnvironmentLock());
				env = EnvironmentBlock(request.Env);
			}
			const std::wstring cwd = Wide(request.Cwd);
			std::fflush(stdout);
			std::fflush(stderr);
			const DWORD flags = CREATE_SUSPENDED | (env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT);
			if (!CreateProcessW(application.empty() ? nullptr : application.c_str(), line.data(), nullptr, nullptr, TRUE, flags,
					env.empty() ? nullptr : env.data(), cwd.empty() ? nullptr : cwd.c_str(), &startup, &started.Process))
				return fail();
			// The job holds it and what it starts; where it cannot be put in
			// one (a job that allows none inside), a kill ends the child alone.
			started.Job = CreateJobObjectW(nullptr, nullptr);
			if (started.Job && !AssignProcessToJobObject(started.Job, started.Process.hProcess))
				Close(started.Job);
			ResumeThread(started.Process.hThread);
			// The child's ends belong to the child now.
			for (HANDLE* h : { &inRead, &outWrite, &errWrite })
				Close(*h);
			return true;
		}

		void Release(Started& started)
		{
			for (HANDLE* h : { &started.In, &started.Out, &started.Err, &started.Job, &started.Process.hProcess, &started.Process.hThread })
				Close(*h);
		}

		// Reads the pipe to its end, or until `giveUp` (the pipe is looked at
		// now and then, so a reader never blocks past it).
		void ReadAll(HANDLE pipe, std::string* into, const std::atomic<bool>* giveUp)
		{
			char buffer[4096];
			for (int pause = 1;;)
			{
				DWORD available = 0;
				if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
					return; // every writer has closed it
				if (available > 0)
				{
					DWORD got = 0;
					if (!ReadFile(pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || got == 0)
						return;
					into->append(buffer, got);
					pause = 1;
					continue;
				}
				if (*giveUp)
					return;
				Sleep((DWORD)pause);
				pause = std::min(pause * 2, 20);
			}
		}

	} // namespace

	bool Spawn(const SpawnRequest& request, SpawnResult& result, std::string& why)
	{
		Started started;
		if (!Launch(request, request.Input.has_value(), started, why))
			return false;
		Busy busy(started);
		std::atomic<bool> giveUp{ false };
		std::atomic<int> reading{ 0 };
		std::thread outThread, errThread, inThread;
		auto reader = [&](HANDLE pipe, std::string* into) {
			reading++;
			return std::thread([pipe, into, &giveUp, &reading] {
				ReadAll(pipe, into, &giveUp);
				reading--;
			});
		};
		if (started.Out)
			outThread = reader(started.Out, &result.Output);
		if (started.Err)
			errThread = reader(started.Err, &result.Error);
		if (started.In)
		{
			const std::string& input = *request.Input;
			HANDLE handle = started.In;
			started.In = nullptr; // the thread closes it
			inThread = std::thread([handle, &input] {
				size_t at = 0;
				DWORD wrote = 0;
				while (at < input.size() && WriteFile(handle, input.data() + at, (DWORD)std::min<size_t>(input.size() - at, 1 << 16), &wrote, nullptr))
					at += wrote;
				CloseHandle(handle);
			});
		}
		const Clock::time_point deadline = Deadline(request.Timeout);
		Clock::time_point stopReading = Clock::time_point::max();
		bool killed = false;
		// Past the limit (a child that has ended by then finished in time,
		// and what it left running goes), or at a Ctrl-C: the job is killed.
		auto stop = [&](bool limit) {
			if (killed)
				return;
			killed = true;
			if (limit)
				result.TimedOut = !Reaped(started);
			else
				result.Interrupted = true;
			KillTree(started, true);
			stopReading = Clock::now() + kGrace;
		};
		auto check = [&] {
			if (killed)
				return;
			if (Interrupted(request.Interrupts))
				stop(false);
			else if (Remaining(deadline) == 0)
				stop(true);
		};
		// The child, in steps: a Ctrl-C is looked for between them.
		while (!Reaped(started))
		{
			check();
			int step = 100;
			const int left = Remaining(deadline);
			if (!killed && left >= 0 && left < step)
				step = left;
			WaitForSingleObject(started.Process.hProcess, (DWORD)step);
		}
		// Its output, to its end: past the limit or a kill, a moment more at
		// most (what it started may hold the pipes).
		while (reading > 0)
		{
			check();
			if (killed && Clock::now() >= stopReading)
				break;
			Sleep(5);
		}
		giveUp = true;
		for (std::thread* t : { &inThread, &outThread, &errThread })
			if (t->joinable())
				t->join();
		Reaped(started, result.Code);
		Release(started);
		return true;
	}

	struct Child::Impl
	{
		Started Process;
	};

	std::unique_ptr<Child> Child::Start(const SpawnRequest& request, bool pipeInput, std::string& why)
	{
		auto impl = std::make_unique<Impl>();
		SpawnRequest r = request;
		r.CaptureOutput = true;
		if (!Launch(r, pipeInput, impl->Process, why))
			return nullptr;
		return std::unique_ptr<Child>(new Child(std::move(impl)));
	}

	Child::Fill Child::FillFor(int milliseconds)
	{
		HANDLE out = m_Impl->Process.Out;
		const Clock::time_point until = Clock::now() + std::chrono::milliseconds(milliseconds);
		for (int pause = 1;; pause = std::min(pause * 2, 20))
		{
			DWORD available = 0;
			if (!out || !PeekNamedPipe(out, nullptr, 0, nullptr, &available, nullptr))
				break; // the end
			if (available > 0)
			{
				char buffer[4096];
				DWORD got = 0;
				if (!ReadFile(out, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || got == 0)
					break;
				std::lock_guard<std::mutex> output(m_Output);
				m_Buffer.append(buffer, got);
				m_Fills++;
				return Fill::Data;
			}
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
			if (left <= 0)
				return Fill::Idle;
			Sleep((DWORD)std::min<long long>(pause, left));
		}
		std::lock_guard<std::mutex> output(m_Output);
		m_Ended = true;
		m_Fills++;
		return Fill::End;
	}

	bool Child::Write(const std::string& text, std::string& why, unsigned interrupts)
	{
		Busy busy(m_Impl->Process);
		std::lock_guard<std::mutex> writing(m_Writing);
		HANDLE in = nullptr;
		{
			std::lock_guard<std::mutex> input(m_Input);
			in = m_Impl->Process.In;
			if (!in)
			{
				why = "its input is closed";
				return false;
			}
			m_WriteRunning = true;
		}
		bool ok = true;
		size_t at = 0;
		DWORD wrote = 0;
		while (at < text.size())
		{
			if (Interrupted(interrupts))
			{
				Kill();
				why = "interrupted";
				ok = false;
				break;
			}
			if (!WriteFile(in, text.data() + at, (DWORD)std::min<size_t>(text.size() - at, 1 << 16), &wrote, nullptr))
			{
				why = LastError();
				ok = false;
				break;
			}
			at += wrote;
		}
		std::lock_guard<std::mutex> input(m_Input);
		m_WriteRunning = false;
		if (m_CloseWanted)
		{
			m_CloseWanted = false;
			Close(m_Impl->Process.In);
		}
		return ok;
	}

	void Child::CloseInput()
	{
		std::lock_guard<std::mutex> input(m_Input);
		if (m_WriteRunning)
			m_CloseWanted = true; // the write closes it when it stops
		else
			Close(m_Impl->Process.In);
	}

	long Child::Id() const { return (long)m_Impl->Process.Process.dwProcessId; }

	Child::~Child()
	{
		Untrack(m_Impl->Process);
		Release(m_Impl->Process);
	}

#else

	namespace {

		// One launch at a time: a child made while another's pipes are being
		// set up would hold their ends (and delay the other's end of output).
		std::mutex g_Launching;

		// Reaps the children dropped while running that have ended since.
		void ReapAbandoned()
		{
			Children& children = Registry();
			std::lock_guard<std::mutex> lock(children.Lock);
			auto& abandoned = children.Abandoned;
			abandoned.erase(std::remove_if(abandoned.begin(), abandoned.end(),
								[](pid_t pid) {
									const pid_t done = waitpid(pid, nullptr, WNOHANG);
									return done == pid || (done < 0 && errno == ECHILD);
								}),
				abandoned.end());
		}

		// Reads exactly `size` bytes (false at the end first).
		bool ReadFully(int fd, void* into, size_t size)
		{
			size_t at = 0;
			while (at < size)
			{
				const ssize_t got = read(fd, static_cast<char*>(into) + at, size - at);
				if (got > 0)
					at += (size_t)got;
				else if (!(got < 0 && errno == EINTR))
					return false;
			}
			return true;
		}

		bool Launch(const SpawnRequest& request, bool pipeInput, bool ownGroup, Started& started, std::string& why)
		{
			ReapAbandoned();
			// A child reading the terminal stays in the terminal's group: in a
			// group of its own its reads would stop it (SIGTTIN), and Ctrl-C
			// would not reach it.
			if (!pipeInput && isatty(0))
				ownGroup = false;
			std::lock_guard<std::mutex> launching(g_Launching);
			// The environment is not changed while it is copied, nor while the
			// child is made (the child's own search of PATH reads it).
			std::lock_guard<std::mutex> environment(EnvironmentLock());
			// The child's environment, made here: after fork the child only
			// switches to it (other threads may hold the allocator's lock).
			std::vector<std::string> envText;
			std::vector<char*> envp;
			if (!request.Env.empty())
			{
				for (char** e = environ; *e; e++)
				{
					const char* eq = std::strchr(*e, '=');
					const std::string name = eq ? std::string(*e, (size_t)(eq - *e)) : std::string(*e);
					if (std::none_of(request.Env.begin(), request.Env.end(), [&](const auto& kv) { return kv.first == name; }))
						envText.push_back(*e);
				}
				for (const auto& [name, value] : request.Env)
					envText.push_back(name + "=" + value);
				for (std::string& e : envText)
					envp.push_back(e.data());
				envp.push_back(nullptr);
			}
			std::vector<std::string> args = request.Shell ? std::vector<std::string>{ "/bin/sh", "-c", request.Args[0] } : request.Args;
			std::vector<char*> argv;
			for (std::string& a : args)
				argv.push_back(a.data());
			argv.push_back(nullptr);
			// The script's own files and sockets are not the child's: every
			// descriptor past the standard three closes when it starts.
			long openMax = sysconf(_SC_OPEN_MAX);
			if (openMax < 0 || openMax > (1 << 16))
				openMax = 1 << 16;

			int in[2] = { -1, -1 }, out[2] = { -1, -1 }, err[2] = { -1, -1 }, status[2] = { -1, -1 };
			auto closeAll = [&] {
				for (int* fd : { &in[0], &in[1], &out[0], &out[1], &err[0], &err[1], &status[0], &status[1] })
					CloseFd(*fd);
			};
			const bool capture = request.CaptureOutput || request.Error == Stream::Merge;
			if ((pipeInput && !MakePipe(in)) || (capture && !MakePipe(out)) || (request.Error == Stream::Capture && !MakePipe(err)) ||
				!MakePipe(status))
			{
				why = std::strerror(errno);
				closeAll();
				return false;
			}
			std::fflush(stdout);
			std::fflush(stderr);
			const pid_t pid = fork();
			if (pid < 0)
			{
				why = std::strerror(errno);
				closeAll();
				return false;
			}
			if (pid == 0)
			{
				// The child: its group, streams, directory and environment,
				// then the program. A failure is reported through the status
				// pipe.
				if (ownGroup)
					setpgid(0, 0);
				std::signal(SIGPIPE, SIG_DFL);
				if (pipeInput)
					dup2(in[0], 0);
				if (capture)
					dup2(out[1], 1);
				if (request.Error == Stream::Capture)
					dup2(err[1], 2);
				else if (request.Error == Stream::Merge)
					dup2(out[1], 2);
				bool marked = false;
#if defined(__linux__) && defined(SYS_close_range)
				marked = syscall(SYS_close_range, 3u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */) == 0;
#endif
				for (int fd = 3; !marked && fd < openMax; fd++)
					fcntl(fd, F_SETFD, FD_CLOEXEC);
				int failure = 0;
				char tag = 'x'; // 'd': the directory, 'x': the program
				if (!request.Cwd.empty() && chdir(request.Cwd.c_str()) != 0)
				{
					failure = errno;
					tag = 'd';
				}
				if (!envp.empty())
					environ = envp.data();
				if (!failure)
				{
					execvp(argv[0], argv.data());
					failure = errno;
				}
				ssize_t ignored = write(status[1], &tag, 1);
				ignored = write(status[1], &failure, sizeof(failure));
				(void)ignored;
				_exit(127);
			}
			if (ownGroup)
				setpgid(pid, pid); // also here: the parent may kill before the child runs
			CloseFd(in[0]);
			CloseFd(out[1]);
			CloseFd(err[1]);
			CloseFd(status[1]);
			char tag = 0;
			int failure = 0;
			if (ReadFully(status[0], &tag, 1) && ReadFully(status[0], &failure, sizeof(failure)))
			{
				while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR)
				{
				}
				why = (tag == 'd' ? "cannot enter '" + request.Cwd + "': " : std::string()) + std::strerror(failure);
				closeAll();
				return false;
			}
			CloseFd(status[0]);
			started.Pid = pid;
			started.Group = ownGroup;
			started.In = in[1];
			started.Out = out[0];
			started.Err = err[0];
			return true;
		}

	} // namespace

	bool Spawn(const SpawnRequest& request, SpawnResult& result, std::string& why)
	{
		Started child;
		if (!Launch(request, request.Input.has_value(), request.Timeout > 0, child, why))
			return false;
		Busy busy(child);
		const Clock::time_point deadline = Deadline(request.Timeout);
		Clock::time_point stopReading = Clock::time_point::max();
		bool killed = false;
		// Past the limit (a child that has ended by then finished in time,
		// and what it left running goes), or at a Ctrl-C: the child is
		// killed, with its group.
		auto stop = [&](bool limit) {
			if (killed)
				return;
			killed = true;
			if (limit)
				result.TimedOut = !Reaped(child);
			else
			{
				result.Interrupted = true;
				// In the terminal's group the child had the Ctrl-C too: a
				// moment to end on its own first.
				for (int i = 0; i < 25 && !child.Group && !Reaped(child); i++)
					Pause(10);
			}
			KillTree(child, true);
			stopReading = Clock::now() + kGrace;
		};
		auto check = [&] {
			if (killed)
				return;
			if (Interrupted(request.Interrupts))
				stop(false);
			else if (Remaining(deadline) == 0)
				stop(true);
		};

		// Feed standard input and drain the outputs together (a child that
		// fills one pipe while the script writes another would wait forever):
		// the input pipe does not block, so a write takes what fits.
		size_t written = 0;
		if (child.In >= 0 && request.Input->empty())
			CloseFd(child.In);
		std::optional<SigpipeGuard> sigpipe;
		if (child.In >= 0)
		{
			fcntl(child.In, F_SETFL, fcntl(child.In, F_GETFL) | O_NONBLOCK);
			sigpipe.emplace();
		}
		char buffer[4096];
		while (child.In >= 0 || child.Out >= 0 || child.Err >= 0)
		{
			check();
			if (killed && Clock::now() >= stopReading)
				break;
			pollfd fds[3];
			int n = 0;
			if (child.In >= 0)
				fds[n++] = { child.In, POLLOUT, 0 };
			if (child.Out >= 0)
				fds[n++] = { child.Out, POLLIN, 0 };
			if (child.Err >= 0)
				fds[n++] = { child.Err, POLLIN, 0 };
			// In steps, for the limit and a Ctrl-C (which may reach another thread).
			int step = 100;
			const int left = Remaining(killed ? stopReading : deadline);
			if (left >= 0 && left < step)
				step = left;
			const int ready = poll(fds, (nfds_t)n, step);
			if (ready < 0)
			{
				if (errno == EINTR)
					continue;
				break;
			}
			for (int k = 0; k < n; k++)
			{
				if (!fds[k].revents)
					continue;
				if (fds[k].fd == child.In)
				{
					const ssize_t w = write(child.In, request.Input->data() + written, request.Input->size() - written);
					if (w > 0)
						written += (size_t)w;
					const bool again = w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
					if (w < 0 && errno == EPIPE)
						sigpipe->Broken();
					if ((w < 0 && !again) || written == request.Input->size())
						CloseFd(child.In);
					continue;
				}
				int& fd = fds[k].fd == child.Out ? child.Out : child.Err;
				std::string& into = &fd == &child.Out ? result.Output : result.Error;
				const ssize_t got = read(fd, buffer, sizeof(buffer));
				if (got > 0)
					into.append(buffer, (size_t)got);
				else if (got == 0 || errno != EINTR)
					CloseFd(fd);
			}
		}
		for (int* fd : { &child.In, &child.Out, &child.Err })
			CloseFd(*fd);
		// The child itself (its output not piped, or ended): within the limit,
		// and a Ctrl-C ends it.
		for (int pause = 1; !Reaped(child, result.Code); pause = std::min(pause * 2, 20))
		{
			check();
			Pause(pause);
		}
		return true;
	}

	struct Child::Impl
	{
		Started Process;
	};

	std::unique_ptr<Child> Child::Start(const SpawnRequest& request, bool pipeInput, std::string& why)
	{
		auto impl = std::make_unique<Impl>();
		SpawnRequest r = request;
		r.CaptureOutput = true;
		if (!Launch(r, pipeInput, true, impl->Process, why))
			return nullptr;
		// Written in steps (a Ctrl-C, or a close, is looked for between them).
		if (impl->Process.In >= 0)
			fcntl(impl->Process.In, F_SETFL, fcntl(impl->Process.In, F_GETFL) | O_NONBLOCK);
		return std::unique_ptr<Child>(new Child(std::move(impl)));
	}

	Child::Fill Child::FillFor(int milliseconds)
	{
		const int fd = m_Impl->Process.Out;
		if (fd >= 0)
		{
			pollfd p = { fd, POLLIN, 0 };
			const int ready = poll(&p, 1, milliseconds);
			if (ready == 0 || (ready < 0 && errno == EINTR))
				return Fill::Idle;
			char buffer[4096];
			const ssize_t got = ready < 0 ? -1 : read(fd, buffer, sizeof(buffer));
			if (got > 0)
			{
				std::lock_guard<std::mutex> output(m_Output);
				m_Buffer.append(buffer, (size_t)got);
				m_Fills++;
				return Fill::Data;
			}
			if (got < 0 && (errno == EINTR || errno == EAGAIN))
				return Fill::Idle;
		}
		std::lock_guard<std::mutex> output(m_Output);
		m_Ended = true;
		m_Fills++;
		return Fill::End;
	}

	bool Child::Write(const std::string& text, std::string& why, unsigned interrupts)
	{
		Busy busy(m_Impl->Process);
		std::lock_guard<std::mutex> writing(m_Writing);
		int fd = -1;
		{
			std::lock_guard<std::mutex> input(m_Input);
			fd = m_Impl->Process.In;
			if (fd < 0)
			{
				why = "its input is closed";
				return false;
			}
			m_WriteRunning = true;
		}
		bool ok = true;
		{
			SigpipeGuard sigpipe;
			size_t at = 0;
			while (at < text.size())
			{
				if (Interrupted(interrupts))
				{
					Kill();
					why = "interrupted";
					ok = false;
					break;
				}
				{
					std::lock_guard<std::mutex> input(m_Input);
					if (m_CloseWanted)
					{
						why = "its input was closed during the write";
						ok = false;
						break;
					}
				}
				const ssize_t w = write(fd, text.data() + at, text.size() - at);
				if (w > 0)
					at += (size_t)w;
				else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				{
					pollfd p = { fd, POLLOUT, 0 };
					poll(&p, 1, 100);
				}
				else if (!(w < 0 && errno == EINTR))
				{
					if (w < 0 && errno == EPIPE)
						sigpipe.Broken();
					why = w < 0 ? std::strerror(errno) : "nothing was written";
					ok = false;
					break;
				}
			}
		}
		std::lock_guard<std::mutex> input(m_Input);
		m_WriteRunning = false;
		if (m_CloseWanted)
		{
			m_CloseWanted = false;
			CloseFd(m_Impl->Process.In);
		}
		return ok;
	}

	void Child::CloseInput()
	{
		std::lock_guard<std::mutex> input(m_Input);
		if (m_WriteRunning)
			m_CloseWanted = true; // the write stops, and closes it
		else
			CloseFd(m_Impl->Process.In);
	}

	long Child::Id() const { return (long)m_Impl->Process.Pid; }

	Child::~Child()
	{
		CloseFd(m_Impl->Process.In);
		CloseFd(m_Impl->Process.Out);
		// Reaped when it has ended; a child still running is left to run, and
		// reaped once it ends (at a later launch).
		Children& children = Registry();
		std::lock_guard<std::mutex> lock(children.Lock);
		children.Live.erase(&m_Impl->Process);
		if (m_Impl->Process.Pid > 0 && !ReapLocked(m_Impl->Process))
			children.Abandoned.push_back(m_Impl->Process.Pid);
	}

#endif

	Child::Child(std::unique_ptr<Impl> impl)
		: m_Impl(std::move(impl))
	{
		Track(m_Impl->Process);
	}

	void KillChildren()
	{
		Children& children = Registry();
		std::lock_guard<std::mutex> lock(children.Lock);
		for (Started* started : children.Live)
			KillLocked(*started, false);
#ifndef _WIN32
		// Dropped while running: not yet reaped, so each pid is still the child's.
		for (pid_t pid : children.Abandoned)
			if (waitpid(pid, nullptr, WNOHANG) == 0)
				kill(pid, SIGKILL);
		// Reaped, so none is left a zombie of a host that goes on running (a
		// killed child ends at once; a second at most is waited for).
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		bool waiting = true;
		while (waiting && std::chrono::steady_clock::now() < until)
		{
			waiting = false;
			for (Started* started : children.Live)
				waiting = !ReapLocked(*started) || waiting;
			auto& abandoned = children.Abandoned;
			abandoned.erase(std::remove_if(abandoned.begin(), abandoned.end(),
								[](pid_t pid) {
									const pid_t done = waitpid(pid, nullptr, WNOHANG);
									return done == pid || (done < 0 && errno == ECHILD);
								}),
				abandoned.end());
			waiting = waiting || !abandoned.empty();
			if (waiting)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
#endif
	}

	void Child::Fetch(int milliseconds, unsigned fills)
	{
		std::unique_lock<std::timed_mutex> reading(m_Reading, std::chrono::milliseconds(milliseconds));
		if (!reading.owns_lock())
			return; // another thread reads: what it gets is shared out
		{
			std::lock_guard<std::mutex> output(m_Output);
			if (m_Fills != fills || m_Ended)
				return; // more came while this one waited its turn
		}
		FillFor(milliseconds);
	}

	bool Child::Read(std::string& chunk, unsigned interrupts, const ReadClaim& claim)
	{
		Busy busy(m_Impl->Process);
		for (;;)
		{
			unsigned fills = 0;
			{
				std::lock_guard<std::mutex> output(m_Output);
				if (!m_Buffer.empty())
				{
					if (claim && !claim())
						return false;
					chunk.swap(m_Buffer);
					m_Buffer.clear();
					return true;
				}
				if (m_Ended)
					return false;
				fills = m_Fills;
			}
			if (Interrupted(interrupts))
			{
				Kill();
				return false;
			}
			Fetch(100, fills);
		}
	}

	bool Child::ReadLine(std::string& line, unsigned interrupts, const ReadClaim& claim)
	{
		Busy busy(m_Impl->Process);
		for (;;)
		{
			unsigned fills = 0;
			{
				std::lock_guard<std::mutex> output(m_Output);
				const size_t nl = m_Buffer.find('\n');
				if (nl != std::string::npos || (m_Ended && !m_Buffer.empty()))
				{
					if (claim && !claim())
						return false;
					const size_t n = nl != std::string::npos ? nl + 1 : m_Buffer.size();
					line.assign(m_Buffer, 0, n);
					m_Buffer.erase(0, n);
					return true;
				}
				if (m_Ended)
					return false;
				fills = m_Fills;
			}
			if (Interrupted(interrupts))
			{
				Kill();
				return false;
			}
			Fetch(100, fills);
		}
	}

	void Child::Unread(const std::string& text)
	{
		std::lock_guard<std::mutex> output(m_Output);
		m_Buffer.insert(0, text);
		m_Fills++;
	}

	bool Child::Ready(bool line)
	{
		std::lock_guard<std::mutex> output(m_Output);
		return m_Ended || (line ? m_Buffer.find('\n') != std::string::npos : !m_Buffer.empty());
	}

	bool Child::TryReap(int& code) { return Reaped(m_Impl->Process, code); }

	int Child::Wait(unsigned interrupts)
	{
		Busy busy(m_Impl->Process);
		CloseInput();
		int code = 0;
		bool killed = false;
		// The output is read meanwhile: a child that fills its pipe would
		// never end. The wait is for the child, not for the pipe's end
		// (what it started may hold the pipe open).
		for (int pause = 1; !TryReap(code);)
		{
			if (!killed && Interrupted(interrupts))
			{
				Kill();
				killed = true;
			}
			unsigned fills = 0;
			bool ended = false;
			{
				std::lock_guard<std::mutex> output(m_Output);
				fills = m_Fills;
				ended = m_Ended;
			}
			if (ended)
			{
				Pause(pause);
				pause = std::min(pause * 2, 20);
			}
			else
				Fetch(20, fills);
		}
		// What it wrote before it ended (a moment's worth: what it started
		// may go on writing, for Read).
		std::unique_lock<std::timed_mutex> reading(m_Reading, std::try_to_lock);
		const Clock::time_point until = Clock::now() + std::chrono::milliseconds(50);
		while (reading.owns_lock() && Clock::now() < until && FillFor(0) == Fill::Data)
		{
		}
		return code;
	}

	void Child::Kill() { KillTree(m_Impl->Process); }

	// ---- Windows program lookup and batch files ---------------------------

	namespace {

		std::vector<std::string> SplitList(const std::string& list)
		{
			std::vector<std::string> out;
			size_t start = 0;
			while (start <= list.size())
			{
				const size_t end = std::min(list.find(';', start), list.size());
				std::string item = list.substr(start, end - start);
				if (item.size() >= 2 && item.front() == '"' && item.back() == '"')
					item = item.substr(1, item.size() - 2);
				if (!item.empty())
					out.push_back(item);
				start = end + 1;
			}
			return out;
		}

		// A directory named from the root (`C:\...`, `\\server\...`), not
		// from the current one.
		bool IsAbsoluteDirectory(const std::string& dir)
		{
			if (dir.size() >= 3 && std::isalpha((unsigned char)dir[0]) && dir[1] == ':' && (dir[2] == '\\' || dir[2] == '/'))
				return true;
			return dir.size() >= 2 && (dir[0] == '\\' || dir[0] == '/') && (dir[1] == '\\' || dir[1] == '/');
		}

	} // namespace

	std::string FindProgram(const std::string& name, const std::string& path, const std::string& pathext,
		const std::function<bool(const std::string&)>& isFile)
	{
		std::vector<std::string> extensions = SplitList(pathext);
		if (extensions.empty())
			extensions = { ".COM", ".EXE", ".BAT", ".CMD" };
		// The file as it is when it has an extension, then with each of PATHEXT's.
		auto find = [&](const std::string& file) -> std::string {
			const size_t last = file.find_last_of(":\\/.");
			if (last != std::string::npos && file[last] == '.' && isFile(file))
				return file;
			for (const std::string& extension : extensions)
				if (isFile(file + extension))
					return file + extension;
			return {};
		};
		if (name.empty())
			return {};
		if (name.find_first_of(":\\/") != std::string::npos)
			return find(name);
		for (std::string dir : SplitList(path))
		{
			if (!IsAbsoluteDirectory(dir))
				continue;
			if (dir.back() != '\\' && dir.back() != '/')
				dir += '\\';
			const std::string found = find(dir + name);
			if (!found.empty())
				return found;
		}
		return {};
	}

	bool IsBatchFile(const std::string& program)
	{
		// Windows drops trailing dots and spaces from a file name.
		std::string name = program;
		while (!name.empty() && (name.back() == '.' || name.back() == ' '))
			name.pop_back();
		if (name.size() < 4)
			return false;
		std::string extension = name.substr(name.size() - 4);
		for (char& c : extension)
			c = (char)std::tolower((unsigned char)c);
		return extension == ".bat" || extension == ".cmd";
	}

	bool BatchCommandLine(const std::string& script, const std::vector<std::string>& args, std::string& line, std::string& why)
	{
		// As Rust's standard library does it. Command extensions on (the `%`
		// escape below needs them), delayed expansion off (no `!VAR!`), no
		// AutoRun, and the whole command in one more pair of quotes, which
		// cmd.exe takes off.
		if (script.find('"') != std::string::npos || (!script.empty() && script.back() == '\\'))
		{
			why = "a batch file's path cannot hold '\"' or end in '\\'";
			return false;
		}
		line = "/e:ON /v:OFF /d /c \"\"" + script + "\"";
		for (const std::string& arg : args)
		{
			// A line break would end the command there.
			if (arg.find_first_of("\r\n") != std::string::npos)
			{
				why = "an argument to a batch file cannot hold a line break";
				return false;
			}
			line += ' ';
			// Quoted unless it holds only characters cmd.exe gives no meaning
			// to; also when empty (or it would vanish), or when it ends in a
			// backslash (which would escape a closing quote the script adds).
			bool quote = arg.empty() || arg.back() == '\\';
			for (size_t i = 0; i < arg.size(); i++)
			{
				const unsigned char c = (unsigned char)arg[i];
				if (c < 0x80 && !std::isalnum(c) && !std::strchr("#$*+-./:?@\\_", c))
					quote = true;
				// U+0080..U+009F, control characters too.
				if (c == 0xC2 && i + 1 < arg.size() && (unsigned char)arg[i + 1] >= 0x80 && (unsigned char)arg[i + 1] <= 0x9F)
					quote = true;
			}
			if (quote)
				line += '"';
			size_t backslashes = 0;
			for (const char c : arg)
			{
				if (c == '\\')
					backslashes++;
				else
				{
					if (c == '"')
					{
						// Backslashes before a quote doubled, and the quote doubled.
						line.append(backslashes, '\\');
						line += '"';
					}
					else if (c == '%')
					{
						// `%cd:~,%` expands to nothing, and keeps cmd.exe from
						// reading `%NAME%` as a variable.
						line += "%%cd:~,";
					}
					backslashes = 0;
				}
				line += c;
			}
			if (quote)
			{
				line.append(backslashes, '\\');
				line += '"';
			}
		}
		line += '"';
		return true;
	}

} // namespace cluau::runtime::detail
