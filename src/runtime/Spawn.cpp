#include "Spawn.h"

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
#include <unistd.h>

#endif

namespace cluau::runtime::detail {

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

	} // namespace

	bool Spawn(const SpawnRequest& request, SpawnResult& result, std::string& why)
	{
		std::wstring line;
		if (request.Shell)
		{
			const char* comspec = std::getenv("ComSpec");
			line = Wide(comspec ? comspec : "cmd.exe");
			line = L"\"" + line + L"\" /d /s /c \"" + Wide(request.Args[0]) + L"\"";
		}
		else
			for (size_t i = 0; i < request.Args.size(); i++)
			{
				if (i)
					line += L' ';
				AppendQuoted(line, Wide(request.Args[i]));
			}

		SECURITY_ATTRIBUTES inherit{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
		HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
		auto closeAll = [&] {
			for (HANDLE* h : { &inRead, &inWrite, &outRead, &outWrite, &errRead, &errWrite })
				if (*h)
				{
					CloseHandle(*h);
					*h = nullptr;
				}
		};
		auto makePipe = [&](HANDLE& read, HANDLE& write, bool parentWrites) {
			if (!CreatePipe(&read, &write, &inherit, 0))
				return false;
			SetHandleInformation(parentWrites ? write : read, HANDLE_FLAG_INHERIT, 0);
			return true;
		};
		const bool capture = request.CaptureOutput || request.Error == Stream::Merge;
		if ((request.Input && !makePipe(inRead, inWrite, true)) || (capture && !makePipe(outRead, outWrite, false)) ||
			(request.Error == Stream::Capture && !makePipe(errRead, errWrite, false)))
		{
			why = LastError();
			closeAll();
			return false;
		}

		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = request.Input ? inRead : GetStdHandle(STD_INPUT_HANDLE);
		startup.hStdOutput = capture ? outWrite : GetStdHandle(STD_OUTPUT_HANDLE);
		startup.hStdError = request.Error == Stream::Capture ? errWrite
			: request.Error == Stream::Merge                 ? outWrite
															  : GetStdHandle(STD_ERROR_HANDLE);
		std::wstring env;
		if (!request.Env.empty())
			env = EnvironmentBlock(request.Env);
		const std::wstring cwd = Wide(request.Cwd);
		PROCESS_INFORMATION process{};
		std::fflush(stdout);
		std::fflush(stderr);
		const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT,
			env.empty() ? nullptr : env.data(), cwd.empty() ? nullptr : cwd.c_str(), &startup, &process);
		if (!started)
		{
			why = LastError();
			closeAll();
			return false;
		}
		// The child's ends belong to the child now.
		for (HANDLE* h : { &inRead, &outWrite, &errWrite })
			if (*h)
			{
				CloseHandle(*h);
				*h = nullptr;
			}
		std::thread outThread, errThread, inThread;
		if (outRead)
			outThread = std::thread(ReadAll, outRead, &result.Output);
		if (errRead)
			errThread = std::thread(ReadAll, errRead, &result.Error);
		if (inWrite)
		{
			const std::string& input = *request.Input;
			HANDLE handle = inWrite;
			inThread = std::thread([handle, &input] {
				size_t at = 0;
				DWORD wrote = 0;
				while (at < input.size() && WriteFile(handle, input.data() + at, (DWORD)std::min<size_t>(input.size() - at, 1 << 16), &wrote, nullptr))
					at += wrote;
				CloseHandle(handle);
			});
			inWrite = nullptr; // the thread closes it
		}
		WaitForSingleObject(process.hProcess, INFINITE);
		for (std::thread* t : { &inThread, &outThread, &errThread })
			if (t->joinable())
				t->join();
		DWORD code = 0;
		GetExitCodeProcess(process.hProcess, &code);
		result.Code = (int)code;
		CloseHandle(process.hProcess);
		CloseHandle(process.hThread);
		closeAll();
		return true;
	}

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

	} // namespace

	bool Spawn(const SpawnRequest& request, SpawnResult& result, std::string& why)
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
		if ((request.Input && !MakePipe(in)) || (capture && !MakePipe(out)) || (request.Error == Stream::Capture && !MakePipe(err)) ||
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
			// The child: its streams, directory and environment, then the
			// program. A failure is reported through the status pipe.
			if (request.Input)
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

		// The parent.
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

		// Feed standard input and drain the outputs together (a child that
		// fills one pipe while the script writes another would wait forever).
		void (*previous)(int) = std::signal(SIGPIPE, SIG_IGN);
		size_t written = 0;
		if (in[1] >= 0 && request.Input->empty())
			CloseFd(in[1]);
		char buffer[4096];
		while (in[1] >= 0 || out[0] >= 0 || err[0] >= 0)
		{
			pollfd fds[3];
			int n = 0;
			if (in[1] >= 0)
				fds[n++] = { in[1], POLLOUT, 0 };
			if (out[0] >= 0)
				fds[n++] = { out[0], POLLIN, 0 };
			if (err[0] >= 0)
				fds[n++] = { err[0], POLLIN, 0 };
			if (poll(fds, (nfds_t)n, -1) < 0)
			{
				if (errno == EINTR)
					continue;
				break;
			}
			for (int k = 0; k < n; k++)
			{
				if (!fds[k].revents)
					continue;
				if (fds[k].fd == in[1])
				{
					const ssize_t w = write(in[1], request.Input->data() + written, request.Input->size() - written);
					if (w > 0)
						written += (size_t)w;
					if (w < 0 || written == request.Input->size())
						CloseFd(in[1]);
					continue;
				}
				int& fd = fds[k].fd == out[0] ? out[0] : err[0];
				std::string& into = &fd == &out[0] ? result.Output : result.Error;
				const ssize_t got = read(fd, buffer, sizeof(buffer));
				if (got > 0)
					into.append(buffer, (size_t)got);
				else if (got == 0 || errno != EINTR)
					CloseFd(fd);
			}
		}
		std::signal(SIGPIPE, previous);
		closeAll();
		int wait = 0;
		while (waitpid(pid, &wait, 0) < 0 && errno == EINTR)
		{
		}
		if (WIFEXITED(wait))
			result.Code = WEXITSTATUS(wait);
		else if (WIFSIGNALED(wait))
			result.Code = 128 + WTERMSIG(wait);
		else
			result.Code = wait;
		return true;
	}

#endif

} // namespace cluau::runtime::detail
