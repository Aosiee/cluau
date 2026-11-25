#include "Spawn.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <map>
#include <thread>

#else

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#endif

namespace cluau::runtime::detail {

	namespace {

		using Clock = std::chrono::steady_clock;

		// The moment a time limit runs out (none: the far future).
		Clock::time_point Deadline(double seconds)
		{
			if (seconds <= 0)
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

	} // namespace

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

		void ReadAll(HANDLE pipe, std::string* into)
		{
			char buffer[4096];
			DWORD got = 0;
			while (ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) && got > 0)
				into->append(buffer, got);
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
		};

		bool Launch(const SpawnRequest& request, bool pipeInput, Started& started, std::string& why)
		{
			std::wstring line;
			if (request.Shell)
			{
				const char* comspec = std::getenv("ComSpec");
				line = L"\"" + Wide(comspec ? comspec : "cmd.exe") + L"\" /d /s /c \"" + Wide(request.Args[0]) + L"\"";
			}
			else
				for (size_t i = 0; i < request.Args.size(); i++)
				{
					if (i)
						line += L' ';
					AppendQuoted(line, Wide(request.Args[i]));
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
				env = EnvironmentBlock(request.Env);
			const std::wstring cwd = Wide(request.Cwd);
			std::fflush(stdout);
			std::fflush(stderr);
			const DWORD flags = CREATE_SUSPENDED | (env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT);
			if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, flags, env.empty() ? nullptr : env.data(),
					cwd.empty() ? nullptr : cwd.c_str(), &startup, &started.Process))
				return fail();
			started.Job = CreateJobObjectW(nullptr, nullptr);
			if (started.Job)
				AssignProcessToJobObject(started.Job, started.Process.hProcess);
			ResumeThread(started.Process.hThread);
			// The child's ends belong to the child now.
			for (HANDLE* h : { &inRead, &outWrite, &errWrite })
				Close(*h);
			return true;
		}

		void KillTree(Started& started)
		{
			if (started.Job)
				TerminateJobObject(started.Job, 1);
			else
				TerminateProcess(started.Process.hProcess, 1);
		}

		void Release(Started& started)
		{
			for (HANDLE* h : { &started.In, &started.Out, &started.Err, &started.Job, &started.Process.hProcess, &started.Process.hThread })
				Close(*h);
		}

	} // namespace

	bool Spawn(const SpawnRequest& request, SpawnResult& result, std::string& why)
	{
		Started started;
		if (!Launch(request, request.Input.has_value(), started, why))
			return false;
		std::thread outThread, errThread, inThread;
		if (started.Out)
			outThread = std::thread(ReadAll, started.Out, &result.Output);
		if (started.Err)
			errThread = std::thread(ReadAll, started.Err, &result.Error);
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
		const int limit = Remaining(Deadline(request.Timeout));
		if (WaitForSingleObject(started.Process.hProcess, limit < 0 ? INFINITE : (DWORD)limit) == WAIT_TIMEOUT)
		{
			result.TimedOut = true;
			KillTree(started);
			WaitForSingleObject(started.Process.hProcess, INFINITE);
		}
		for (std::thread* t : { &inThread, &outThread, &errThread })
			if (t->joinable())
				t->join();
		DWORD code = 0;
		GetExitCodeProcess(started.Process.hProcess, &code);
		result.Code = (int)code;
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

	bool Child::Fill()
	{
		if (m_Ended || !m_Impl->Process.Out)
			return false;
		char buffer[4096];
		DWORD got = 0;
		if (!ReadFile(m_Impl->Process.Out, buffer, sizeof(buffer), &got, nullptr) || got == 0)
		{
			m_Ended = true;
			return false;
		}
		m_Buffer.append(buffer, got);
		return true;
	}

	bool Child::Write(const std::string& text, std::string& why)
	{
		if (!m_Impl->Process.In)
		{
			why = "its input is closed";
			return false;
		}
		size_t at = 0;
		DWORD wrote = 0;
		while (at < text.size())
		{
			if (!WriteFile(m_Impl->Process.In, text.data() + at, (DWORD)std::min<size_t>(text.size() - at, 1 << 16), &wrote, nullptr))
			{
				why = LastError();
				return false;
			}
			at += wrote;
		}
		return true;
	}

	void Child::CloseInput() { Close(m_Impl->Process.In); }

	int Child::Wait()
	{
		if (m_Waited)
			return m_Code;
		CloseInput();
		while (Fill())
		{
		}
		WaitForSingleObject(m_Impl->Process.Process.hProcess, INFINITE);
		DWORD code = 0;
		GetExitCodeProcess(m_Impl->Process.Process.hProcess, &code);
		m_Code = (int)code;
		m_Waited = true;
		return m_Code;
	}

	void Child::Kill() { KillTree(m_Impl->Process); }

	long Child::Id() const { return (long)m_Impl->Process.Process.dwProcessId; }

	Child::~Child() { Release(m_Impl->Process); }

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
			int In = -1;  // the child's standard input (we write)
			int Out = -1; // its output (we read)
			int Err = -1; // its standard error, captured on its own
		};

		bool Launch(const SpawnRequest& request, bool pipeInput, bool ownGroup, Started& started, std::string& why)
		{
			std::vector<std::string> args = request.Shell ? std::vector<std::string>{ "/bin/sh", "-c", request.Args[0] } : request.Args;
			std::vector<char*> argv;
			for (std::string& a : args)
				argv.push_back(a.data());
			argv.push_back(nullptr);

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
				if (pipeInput)
					dup2(in[0], 0);
				if (capture)
					dup2(out[1], 1);
				if (request.Error == Stream::Capture)
					dup2(err[1], 2);
				else if (request.Error == Stream::Merge)
					dup2(out[1], 2);
				int failure = 0;
				char tag = 'x'; // 'd': the directory, 'x': the program
				if (!request.Cwd.empty() && chdir(request.Cwd.c_str()) != 0)
				{
					failure = errno;
					tag = 'd';
				}
				for (const auto& [name, value] : request.Env)
					setenv(name.c_str(), value.c_str(), 1);
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
			if (read(status[0], &tag, 1) == 1 && read(status[0], &failure, sizeof(failure)) == (ssize_t)sizeof(failure))
			{
				waitpid(pid, nullptr, 0);
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

		void KillTree(const Started& started)
		{
			if (started.Pid <= 0)
				return;
			if (started.Group)
				kill(-started.Pid, SIGKILL);
			kill(started.Pid, SIGKILL);
		}

		int ExitCode(int wait)
		{
			if (WIFEXITED(wait))
				return WEXITSTATUS(wait);
			if (WIFSIGNALED(wait))
				return 128 + WTERMSIG(wait);
			return wait;
		}

		int Reap(pid_t pid)
		{
			int wait = 0;
			while (waitpid(pid, &wait, 0) < 0 && errno == EINTR)
			{
			}
			return ExitCode(wait);
		}

	} // namespace

	bool Spawn(const SpawnRequest& request, SpawnResult& result, std::string& why)
	{
		Started child;
		if (!Launch(request, request.Input.has_value(), request.Timeout > 0, child, why))
			return false;
		const Clock::time_point deadline = Deadline(request.Timeout);
		auto expire = [&] {
			if (!result.TimedOut)
			{
				result.TimedOut = true;
				KillTree(child);
			}
		};

		// Feed standard input and drain the outputs together (a child that
		// fills one pipe while the script writes another would wait forever).
		void (*previous)(int) = std::signal(SIGPIPE, SIG_IGN);
		size_t written = 0;
		if (child.In >= 0 && request.Input->empty())
			CloseFd(child.In);
		char buffer[4096];
		while (child.In >= 0 || child.Out >= 0 || child.Err >= 0)
		{
			pollfd fds[3];
			int n = 0;
			if (child.In >= 0)
				fds[n++] = { child.In, POLLOUT, 0 };
			if (child.Out >= 0)
				fds[n++] = { child.Out, POLLIN, 0 };
			if (child.Err >= 0)
				fds[n++] = { child.Err, POLLIN, 0 };
			const int ready = poll(fds, (nfds_t)n, result.TimedOut ? -1 : Remaining(deadline));
			if (ready < 0)
			{
				if (errno == EINTR)
					continue;
				break;
			}
			if (ready == 0)
			{
				expire(); // the pipes close once the child is gone
				continue;
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
					if (w < 0 || written == request.Input->size())
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
		std::signal(SIGPIPE, previous);
		// A child whose output is not piped: wait for it within the limit.
		if (request.Timeout > 0 && !result.TimedOut)
		{
			int wait = 0;
			for (;;)
			{
				const pid_t done = waitpid(child.Pid, &wait, WNOHANG);
				if (done == child.Pid)
				{
					result.Code = ExitCode(wait);
					return true;
				}
				if (done < 0 && errno != EINTR)
					break;
				if (Remaining(deadline) == 0)
				{
					expire();
					break;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}
		result.Code = Reap(child.Pid);
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
		return std::unique_ptr<Child>(new Child(std::move(impl)));
	}

	bool Child::Fill()
	{
		if (m_Ended || m_Impl->Process.Out < 0)
			return false;
		char buffer[4096];
		for (;;)
		{
			const ssize_t got = read(m_Impl->Process.Out, buffer, sizeof(buffer));
			if (got > 0)
			{
				m_Buffer.append(buffer, (size_t)got);
				return true;
			}
			if (got < 0 && errno == EINTR)
				continue;
			m_Ended = true;
			return false;
		}
	}

	bool Child::Write(const std::string& text, std::string& why)
	{
		if (m_Impl->Process.In < 0)
		{
			why = "its input is closed";
			return false;
		}
		void (*previous)(int) = std::signal(SIGPIPE, SIG_IGN);
		size_t at = 0;
		bool ok = true;
		while (at < text.size())
		{
			const ssize_t w = write(m_Impl->Process.In, text.data() + at, text.size() - at);
			if (w > 0)
				at += (size_t)w;
			else if (!(w < 0 && errno == EINTR))
			{
				why = w < 0 ? std::strerror(errno) : "nothing was written";
				ok = false;
				break;
			}
		}
		std::signal(SIGPIPE, previous);
		return ok;
	}

	void Child::CloseInput() { CloseFd(m_Impl->Process.In); }

	int Child::Wait()
	{
		if (m_Waited)
			return m_Code;
		CloseInput();
		while (Fill())
		{
		}
		m_Code = Reap(m_Impl->Process.Pid);
		m_Waited = true;
		return m_Code;
	}

	void Child::Kill()
	{
		if (!m_Waited)
			KillTree(m_Impl->Process);
	}

	long Child::Id() const { return (long)m_Impl->Process.Pid; }

	Child::~Child()
	{
		CloseFd(m_Impl->Process.In);
		CloseFd(m_Impl->Process.Out);
		// Reaped when it has ended; a child still running is left to run.
		if (!m_Waited && m_Impl->Process.Pid > 0)
			waitpid(m_Impl->Process.Pid, nullptr, WNOHANG);
	}

#endif

	Child::Child(std::unique_ptr<Impl> impl)
		: m_Impl(std::move(impl))
	{
	}

	bool Child::Read(std::string& chunk)
	{
		if (m_Buffer.empty() && !Fill())
			return false;
		chunk.swap(m_Buffer);
		m_Buffer.clear();
		return true;
	}

	bool Child::ReadLine(std::string& line)
	{
		for (;;)
		{
			const size_t nl = m_Buffer.find('\n');
			if (nl != std::string::npos)
			{
				line = m_Buffer.substr(0, nl);
				m_Buffer.erase(0, nl + 1);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				return true;
			}
			if (!Fill())
			{
				if (m_Buffer.empty())
					return false;
				line.swap(m_Buffer);
				m_Buffer.clear();
				return true;
			}
		}
	}

} // namespace cluau::runtime::detail
