// @std/net: TCP connections and servers. Each wait (connecting, accepting,
// reading, writing) runs as a job (Jobs.h): in a task it waits on its own
// while the other tasks run. A wait polls in short steps, so a timeout
// holds and a close from another task ends it on every system.

#include "Std.h"
#include "Jobs.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle kNoSocket = INVALID_SOCKET;
#define CLUAU_POLL WSAPoll
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kNoSocket = -1;
#define CLUAU_POLL poll
#endif

namespace cluau::runtime::detail {

	namespace {

		constexpr const char* kSocketMeta = "cluau.Socket";
		constexpr const char* kServerMeta = "cluau.Server";

		std::string LastError()
		{
#ifdef _WIN32
			const int code = WSAGetLastError();
			char* text = nullptr;
			FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)code, 0,
				(LPSTR)&text, 0, nullptr);
			std::string out = text ? text : "error " + std::to_string(code);
			LocalFree(text);
			while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '.'))
				out.pop_back();
			return out;
#else
			return std::strerror(errno);
#endif
		}

		void CloseHandle(SocketHandle s)
		{
#ifdef _WIN32
			closesocket(s);
#else
			::close(s);
#endif
		}

		void StartSockets()
		{
#ifdef _WIN32
			static const bool started = [] {
				WSADATA data;
				return WSAStartup(MAKEWORD(2, 2), &data) == 0;
			}();
			(void)started;
#endif
		}

		// A write to a peer that has gone is an error, not SIGPIPE (which
		// is left alone for the programs the script runs).
#if defined(MSG_NOSIGNAL)
		constexpr int kSendFlags = MSG_NOSIGNAL;
#else
		constexpr int kSendFlags = 0;
#endif

		void Prepare(SocketHandle s)
		{
			int on = 1;
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
#ifdef SO_NOSIGPIPE
			setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
		}

		void SetBlocking(SocketHandle s, bool blocking)
		{
#ifdef _WIN32
			u_long nonblocking = blocking ? 0 : 1;
			ioctlsocket(s, FIONBIO, &nonblocking);
#else
			const int flags = fcntl(s, F_GETFL, 0);
			fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
		}

		// "host:port" of an address.
		std::string Describe(const sockaddr* address, std::string* host = nullptr, int* port = nullptr)
		{
			char text[INET6_ADDRSTRLEN] = {};
			int number = 0;
			if (address->sa_family == AF_INET)
			{
				const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(address);
				inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text));
				number = ntohs(in->sin_port);
			}
			else if (address->sa_family == AF_INET6)
			{
				const sockaddr_in6* in = reinterpret_cast<const sockaddr_in6*>(address);
				inet_ntop(AF_INET6, &in->sin6_addr, text, sizeof(text));
				number = ntohs(in->sin6_port);
			}
			if (host)
				*host = text;
			if (port)
				*port = number;
			return std::string(text) + ":" + std::to_string(number);
		}

		// A socket shared by the script's handle and the jobs using it: it is
		// closed when the last of them lets go; Close ends the waits on it.
		struct Socket
		{
			SocketHandle Handle = kNoSocket;
			std::atomic<bool> Closed{ false };
			std::mutex Reading; // one read at a time (the buffer is the reads')
			std::mutex Writing;
			std::string Buffer; // read, not yet given out
			bool Ended = false;
			double Timeout = 0; // seconds a wait may take (0: no limit)
			std::string LocalHost, RemoteHost;
			int LocalPort = 0, RemotePort = 0;

			std::mutex Using;
			int Users = 0; // the jobs waiting on it now

			~Socket()
			{
				if (Handle != kNoSocket)
					CloseHandle(Handle);
			}

			// Closing ends the waits on it (they poll for it), and the handle
			// is closed as soon as none uses it (no other socket gets its
			// number under a wait). A closed server stops listening.
			void Close()
			{
				std::lock_guard<std::mutex> lock(Using);
				if (Closed.exchange(true))
					return;
#ifdef _WIN32
				shutdown(Handle, SD_BOTH);
#else
				shutdown(Handle, SHUT_RDWR);
#endif
				if (Users == 0 && Handle != kNoSocket)
				{
					CloseHandle(Handle);
					Handle = kNoSocket;
				}
			}

			// A job's use of the handle: false when it is closed already.
			bool Enter()
			{
				std::lock_guard<std::mutex> lock(Using);
				if (Closed)
					return false;
				Users++;
				return true;
			}

			void Leave()
			{
				std::lock_guard<std::mutex> lock(Using);
				if (--Users == 0 && Closed && Handle != kNoSocket)
				{
					CloseHandle(Handle);
					Handle = kNoSocket;
				}
			}

			void Addresses()
			{
				sockaddr_storage address{};
				socklen_t length = sizeof(address);
				if (getsockname(Handle, reinterpret_cast<sockaddr*>(&address), &length) == 0)
					Describe(reinterpret_cast<sockaddr*>(&address), &LocalHost, &LocalPort);
				length = sizeof(address);
				if (getpeername(Handle, reinterpret_cast<sockaddr*>(&address), &length) == 0)
					Describe(reinterpret_cast<sockaddr*>(&address), &RemoteHost, &RemotePort);
			}
		};

		enum class Ready { Yes, TimedOut, Closed, Failed };

		// A job's hold on a socket while it waits on it.
		struct Use
		{
			Socket& Of;
			bool Entered;
			explicit Use(Socket& socket)
				: Of(socket)
				, Entered(socket.Enter())
			{
			}
			~Use()
			{
				if (Entered)
					Of.Leave();
			}
		};

		// Until the socket can be read (or written), in steps of 100 ms.
		Ready WaitFor(const Socket& socket, bool write, double timeout)
		{
			const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
			for (;;)
			{
				if (socket.Closed)
					return Ready::Closed;
				pollfd entry{};
				entry.fd = socket.Handle;
				entry.events = write ? POLLOUT : POLLIN;
				const int n = CLUAU_POLL(&entry, 1, 100);
				if (n > 0)
					return socket.Closed ? Ready::Closed : (entry.revents & POLLNVAL) ? Ready::Failed : Ready::Yes;
				if (n < 0)
				{
#ifndef _WIN32
					if (errno == EINTR)
						continue;
#endif
					return Ready::Failed;
				}
				if (timeout > 0 && std::chrono::steady_clock::now() >= until)
					return Ready::TimedOut;
				if (InterruptPending())
					return Ready::Closed;
			}
		}

		std::string Why(Ready ready, double timeout)
		{
			switch (ready)
			{
			case Ready::TimedOut: return "no answer within " + std::to_string(timeout).substr(0, 6) + " seconds";
			case Ready::Closed: return "the socket is closed";
			default: return LastError();
			}
		}

		// The job's failure: an error raised on the script's thread (a Ctrl-C
		// as `interrupted`).
		JobResult Failure(std::string message)
		{
			return [message](lua_State* L) -> int {
				RaiseIfInterrupted(L);
				luaL_error(L, "%s", message.c_str());
				return 0;
			};
		}

		struct SocketBox
		{
			std::shared_ptr<Socket>* Shared = nullptr;
		};

		void PushSocket(lua_State* L, std::shared_ptr<Socket> socket, const char* meta)
		{
			SocketBox* box = static_cast<SocketBox*>(lua_newuserdatadtor(L, sizeof(SocketBox), [](lua_State*, void* p) {
				SocketBox* box = static_cast<SocketBox*>(p);
				if (box->Shared)
				{
					(*box->Shared)->Close();
					delete box->Shared;
				}
			}));
			box->Shared = new std::shared_ptr<Socket>(std::move(socket));
			luaL_getmetatable(L, meta);
			lua_setmetatable(L, -2);
		}

		std::shared_ptr<Socket> Check(lua_State* L, int index, const char* meta)
		{
			SocketBox* box = static_cast<SocketBox*>(luaL_checkudata(L, index, meta));
			return *box->Shared;
		}

		double TimeoutOption(lua_State* L, int index, const char* fn)
		{
			if (lua_isnoneornil(L, index))
				return 0;
			luaL_checktype(L, index, LUA_TTABLE);
			lua_getfield(L, index, "timeout");
			double timeout = 0;
			if (!lua_isnil(L, -1))
			{
				if (!lua_isnumber(L, -1) || lua_tonumber(L, -1) <= 0)
					luaL_error(L, "%s: options.timeout is a number of seconds", fn);
				timeout = lua_tonumber(L, -1);
			}
			lua_pop(L, 1);
			return timeout;
		}

		// ---- net.connect -------------------------------------------------

		int NetConnect(lua_State* L)
		{
			const std::string host = luaL_checkstring(L, 1);
			const int port = (int)luaL_checkinteger(L, 2);
			if (port <= 0 || port > 65535)
				luaL_argerror(L, 2, "a port from 1 to 65535");
			const double timeout = TimeoutOption(L, 3, "net.connect");
			StartSockets();
			return Await(L, [host, port, timeout]() -> JobResult {
				const std::string where = host + ":" + std::to_string(port);
				addrinfo hints{};
				hints.ai_family = AF_UNSPEC;
				hints.ai_socktype = SOCK_STREAM;
				addrinfo* found = nullptr;
				if (const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found); rc != 0)
					return Failure("net.connect: cannot find " + host + ": " + gai_strerror(rc));
				std::string why = "no address";
				auto socket = std::make_shared<Socket>();
				socket->Timeout = timeout;
				for (addrinfo* a = found; a; a = a->ai_next)
				{
					const SocketHandle s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
					if (s == kNoSocket)
					{
						why = LastError();
						continue;
					}
					// Connect without blocking, then wait (the timeout holds).
					SetBlocking(s, false);
					const int rc = ::connect(s, a->ai_addr, (socklen_t)a->ai_addrlen);
#ifdef _WIN32
					const bool pending = rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK;
#else
					const bool pending = rc != 0 && errno == EINPROGRESS;
#endif
					if (rc != 0 && !pending)
					{
						why = LastError();
						CloseHandle(s);
						continue;
					}
					socket->Handle = s;
					if (pending)
					{
						const Ready ready = WaitFor(*socket, true, timeout);
						int error = 0;
						socklen_t length = sizeof(error);
						getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
						if (ready != Ready::Yes || error != 0)
						{
#ifdef _WIN32
							why = ready != Ready::Yes ? Why(ready, timeout) : (WSASetLastError(error), LastError());
#else
							why = ready != Ready::Yes ? Why(ready, timeout) : std::strerror(error);
#endif
							CloseHandle(s);
							socket->Handle = kNoSocket;
							continue;
						}
					}
					SetBlocking(s, true);
					break;
				}
				freeaddrinfo(found);
				if (socket->Handle == kNoSocket)
					return Failure("net.connect: cannot connect to " + where + ": " + why);
				Prepare(socket->Handle);
				socket->Addresses();
				return [socket](lua_State* L) {
					PushSocket(L, socket, kSocketMeta);
					return 1;
				};
			});
		}

		// ---- net.listen --------------------------------------------------

		int NetListen(lua_State* L)
		{
			const std::string host = luaL_checkstring(L, 1);
			const int port = (int)luaL_checkinteger(L, 2);
			if (port < 0 || port > 65535)
				luaL_argerror(L, 2, "a port from 0 (any free one) to 65535");
			int backlog = 64;
			if (lua_istable(L, 3))
			{
				lua_getfield(L, 3, "backlog");
				if (lua_isnumber(L, -1))
					backlog = std::max(1, (int)lua_tointeger(L, -1));
				lua_pop(L, 1);
			}
			StartSockets();
			const std::string where = host + ":" + std::to_string(port);
			addrinfo hints{};
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_flags = AI_PASSIVE;
			addrinfo* found = nullptr;
			if (const int rc = getaddrinfo(host.empty() ? nullptr : host.c_str(), std::to_string(port).c_str(), &hints, &found); rc != 0)
				luaL_error(L, "net.listen: cannot find %s: %s", host.c_str(), gai_strerror(rc));
			std::string why = "no address";
			auto server = std::make_shared<Socket>();
			for (addrinfo* a = found; a && server->Handle == kNoSocket; a = a->ai_next)
			{
				const SocketHandle s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
				if (s == kNoSocket)
				{
					why = LastError();
					continue;
				}
				int on = 1;
#ifdef _WIN32
				setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
#else
				setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
				if (::bind(s, a->ai_addr, (socklen_t)a->ai_addrlen) != 0 || ::listen(s, backlog) != 0)
				{
					why = LastError();
					CloseHandle(s);
					continue;
				}
				server->Handle = s;
			}
			freeaddrinfo(found);
			if (server->Handle == kNoSocket)
				luaL_error(L, "net.listen: cannot listen on %s: %s", where.c_str(), why.c_str());
			server->Addresses();
			PushSocket(L, server, kServerMeta);
			return 1;
		}

		int ServerAccept(lua_State* L)
		{
			std::shared_ptr<Socket> server = Check(L, 1, kServerMeta);
			const double timeout = TimeoutOption(L, 2, "server:accept");
			return Await(L, [server, timeout]() -> JobResult {
				Use use(*server);
				if (!use.Entered)
					return Failure("server:accept: the socket is closed");
				const Ready ready = WaitFor(*server, false, timeout);
				if (ready != Ready::Yes)
					return Failure("server:accept: " + Why(ready, timeout));
				const SocketHandle s = ::accept(server->Handle, nullptr, nullptr);
				if (s == kNoSocket)
					return Failure("server:accept: " + LastError());
				auto socket = std::make_shared<Socket>();
				socket->Handle = s;
				Prepare(s);
				socket->Addresses();
				return [socket](lua_State* L) {
					PushSocket(L, socket, kSocketMeta);
					return 1;
				};
			});
		}

		int ServerClose(lua_State* L)
		{
			Check(L, 1, kServerMeta)->Close();
			return 0;
		}

		int ServerIndex(lua_State* L)
		{
			std::shared_ptr<Socket> server = Check(L, 1, kServerMeta);
			const char* key = luaL_checkstring(L, 2);
			if (std::strcmp(key, "port") == 0)
				lua_pushinteger(L, server->LocalPort);
			else if (std::strcmp(key, "host") == 0)
				lua_pushstring(L, server->LocalHost.c_str());
			else
			{
				lua_getmetatable(L, 1);
				lua_getfield(L, -1, "methods");
				lua_getfield(L, -1, key);
			}
			return 1;
		}

		// ---- a connection -------------------------------------------------

		// More of the stream into the buffer; false at its end. Called with
		// the reading lock held.
		bool Fill(Socket& socket, std::string& why)
		{
			if (socket.Ended)
				return false;
			const Ready ready = WaitFor(socket, false, socket.Timeout);
			if (ready != Ready::Yes)
			{
				why = Why(ready, socket.Timeout);
				return false;
			}
			char chunk[16384];
			const auto n = ::recv(socket.Handle, chunk, (int)sizeof(chunk), 0);
			if (n < 0)
			{
				why = LastError();
				return false;
			}
			if (n == 0)
			{
				socket.Ended = true;
				return false;
			}
			socket.Buffer.append(chunk, (size_t)n);
			return true;
		}

		enum class ReadKind { Some, Line, All };

		int AwaitRead(lua_State* L, std::shared_ptr<Socket> socket, ReadKind kind, size_t count, const char* fn)
		{
			return Await(L, [socket, kind, count, fn]() -> JobResult {
				std::lock_guard<std::mutex> reading(socket->Reading);
				Use use(*socket);
				if (!use.Entered && socket->Buffer.empty())
					return Failure(std::string(fn) + ": the socket is closed");
				std::string why;
				auto text = std::make_shared<std::string>();
				bool got = false;
				if (kind == ReadKind::Some)
				{
					if (socket->Buffer.empty())
						Fill(*socket, why);
					if (!socket->Buffer.empty())
					{
						const size_t n = std::min(count, socket->Buffer.size());
						*text = socket->Buffer.substr(0, n);
						socket->Buffer.erase(0, n);
						got = true;
					}
				}
				else if (kind == ReadKind::Line)
				{
					size_t newline;
					while ((newline = socket->Buffer.find('\n')) == std::string::npos && Fill(*socket, why))
					{
					}
					if (newline != std::string::npos)
					{
						*text = socket->Buffer.substr(0, newline);
						socket->Buffer.erase(0, newline + 1);
						if (!text->empty() && text->back() == '\r')
							text->pop_back();
						got = true;
					}
					else if (!socket->Buffer.empty() && why.empty())
					{
						*text = std::move(socket->Buffer); // the last line, with no line break
						socket->Buffer.clear();
						got = true;
					}
				}
				else
				{
					while (Fill(*socket, why))
					{
					}
					*text = std::move(socket->Buffer);
					socket->Buffer.clear();
					got = why.empty();
				}
				if (!why.empty() && !got)
					return Failure(std::string(fn) + ": " + why);
				return [text, got](lua_State* L) {
					if (got)
						lua_pushlstring(L, text->data(), text->size());
					else
						lua_pushnil(L);
					return 1;
				};
			});
		}

		int SocketRead(lua_State* L)
		{
			std::shared_ptr<Socket> socket = Check(L, 1, kSocketMeta);
			const int count = (int)luaL_optinteger(L, 2, 65536);
			if (count < 1)
				luaL_argerror(L, 2, "a number of bytes from 1");
			return AwaitRead(L, socket, ReadKind::Some, (size_t)count, "socket:read");
		}

		int SocketReadLine(lua_State* L) { return AwaitRead(L, Check(L, 1, kSocketMeta), ReadKind::Line, 0, "socket:readLine"); }

		int SocketReadAll(lua_State* L) { return AwaitRead(L, Check(L, 1, kSocketMeta), ReadKind::All, 0, "socket:readAll"); }

		int SocketLinesNext(lua_State* L)
		{
			return AwaitRead(L, Check(L, lua_upvalueindex(1), kSocketMeta), ReadKind::Line, 0, "socket:lines");
		}

		int SocketLines(lua_State* L)
		{
			Check(L, 1, kSocketMeta);
			lua_pushvalue(L, 1);
			lua_pushcclosurek(L, SocketLinesNext, "lines", 1, AwaitContinue);
			return 1;
		}

		int SocketWrite(lua_State* L)
		{
			std::shared_ptr<Socket> socket = Check(L, 1, kSocketMeta);
			std::string data;
			const int n = lua_gettop(L);
			for (int i = 2; i <= n; i++)
			{
				size_t len = 0;
				const char* text = luaL_checklstring(L, i, &len);
				data.append(text, len);
			}
			return Await(L, [socket, data]() -> JobResult {
				std::lock_guard<std::mutex> writing(socket->Writing);
				Use use(*socket);
				if (!use.Entered)
					return Failure("socket:write: the socket is closed");
				size_t at = 0;
				while (at < data.size())
				{
					const Ready ready = WaitFor(*socket, true, socket->Timeout);
					if (ready != Ready::Yes)
						return Failure("socket:write: " + Why(ready, socket->Timeout));
#ifdef _WIN32
					const int sent = ::send(socket->Handle, data.data() + at, (int)std::min<size_t>(data.size() - at, 1 << 30), 0);
#else
					const ssize_t sent = ::send(socket->Handle, data.data() + at, data.size() - at, kSendFlags);
#endif
					if (sent < 0)
						return Failure("socket:write: " + LastError());
					at += (size_t)sent;
				}
				return [](lua_State*) { return 0; };
			});
		}

		int SocketClose(lua_State* L)
		{
			Check(L, 1, kSocketMeta)->Close();
			return 0;
		}

		int SocketSetTimeout(lua_State* L)
		{
			std::shared_ptr<Socket> socket = Check(L, 1, kSocketMeta);
			const double seconds = luaL_optnumber(L, 2, 0);
			if (seconds < 0)
				luaL_argerror(L, 2, "a number of seconds (none, or 0: no limit)");
			socket->Timeout = seconds;
			return 0;
		}

		int SocketIndex(lua_State* L)
		{
			std::shared_ptr<Socket> socket = Check(L, 1, kSocketMeta);
			const char* key = luaL_checkstring(L, 2);
			if (std::strcmp(key, "localHost") == 0)
				lua_pushstring(L, socket->LocalHost.c_str());
			else if (std::strcmp(key, "localPort") == 0)
				lua_pushinteger(L, socket->LocalPort);
			else if (std::strcmp(key, "remoteHost") == 0)
				lua_pushstring(L, socket->RemoteHost.c_str());
			else if (std::strcmp(key, "remotePort") == 0)
				lua_pushinteger(L, socket->RemotePort);
			else
			{
				lua_getmetatable(L, 1);
				lua_getfield(L, -1, "methods");
				lua_getfield(L, -1, key);
			}
			return 1;
		}

		void Type(lua_State* L, const char* meta, const char* name, lua_CFunction index, const luaL_Reg* methods, bool awaiting)
		{
			luaL_newmetatable(L, meta);
			lua_pushstring(L, name);
			lua_setfield(L, -2, "__type");
			lua_pushcfunction(L, index, "index");
			lua_setfield(L, -2, "__index");
			lua_newtable(L);
			for (const luaL_Reg* m = methods; m->name; m++)
			{
				const bool waits = awaiting && std::strcmp(m->name, "close") != 0 && std::strcmp(m->name, "lines") != 0 &&
								   std::strcmp(m->name, "setTimeout") != 0;
				if (waits)
					lua_pushcclosurek(L, m->func, m->name, 0, AwaitContinue);
				else
					lua_pushcfunction(L, m->func, m->name);
				lua_setfield(L, -2, m->name);
			}
			lua_setfield(L, -2, "methods");
			lua_pop(L, 1);
		}

	} // namespace

	void OpenNet(lua_State* L)
	{
		const luaL_Reg socketMethods[] = { { "read", SocketRead }, { "readLine", SocketReadLine }, { "readAll", SocketReadAll },
			{ "lines", SocketLines }, { "write", SocketWrite }, { "close", SocketClose }, { "setTimeout", SocketSetTimeout }, { nullptr, nullptr } };
		const luaL_Reg serverMethods[] = { { "accept", ServerAccept }, { "close", ServerClose }, { nullptr, nullptr } };
		Type(L, kSocketMeta, "Socket", SocketIndex, socketMethods, true);
		Type(L, kServerMeta, "Server", ServerIndex, serverMethods, true);

		lua_newtable(L);
		lua_pushcclosurek(L, NetConnect, "connect", 0, AwaitContinue);
		lua_setfield(L, -2, "connect");
		lua_pushcfunction(L, NetListen, "listen");
		lua_setfield(L, -2, "listen");
	}

} // namespace cluau::runtime::detail
