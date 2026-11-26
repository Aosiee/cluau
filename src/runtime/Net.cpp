// @std/net: TCP connections and servers. Each wait (connecting, accepting,
// reading, writing) runs as a job (Jobs.h): in a task it waits on its own
// while the other tasks run. A wait polls in short steps, so a timeout
// holds and a close from another task ends it on every system.

#include "Std.h"
#include "Jobs.h"
#ifdef CLUAU_HAS_MBEDTLS
#include "Tls.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#endif

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <climits>
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

#ifdef CLUAU_HAS_MBEDTLS
		// A TLS connection's state over its socket (which is non-blocking:
		// mbedTLS runs one call at a time, holding Lock, and the waits for
		// the socket happen between calls, so a read and a write in two
		// tasks both go ahead).
		struct TlsState
		{
			mbedtls_ssl_context Ssl;
			mbedtls_ssl_config Config;
			mbedtls_x509_crt Trusted, Own;
			mbedtls_pk_context Key;
			mbedtls_entropy_context Entropy;
			mbedtls_ctr_drbg_context Random;
			std::mutex Lock;
			// Why the socket's last send or receive failed (the system's
			// words), set under Lock.
			std::string IoError;

			TlsState()
			{
				mbedtls_ssl_init(&Ssl);
				mbedtls_ssl_config_init(&Config);
				mbedtls_x509_crt_init(&Trusted);
				mbedtls_x509_crt_init(&Own);
				mbedtls_pk_init(&Key);
				mbedtls_entropy_init(&Entropy);
				mbedtls_ctr_drbg_init(&Random);
			}
			~TlsState()
			{
				mbedtls_ssl_free(&Ssl);
				mbedtls_ssl_config_free(&Config);
				mbedtls_x509_crt_free(&Trusted);
				mbedtls_x509_crt_free(&Own);
				mbedtls_pk_free(&Key);
				mbedtls_ctr_drbg_free(&Random);
				mbedtls_entropy_free(&Entropy);
			}
		};

		std::string TlsError(int rc)
		{
			char text[200] = {};
			mbedtls_strerror(rc, text, sizeof(text));
			return text;
		}
#else
		struct TlsState
		{
		};
#endif

		// Seconds on the steady clock (a deadline's unit).
		double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

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
			std::atomic<double> Timeout{ 0 };  // seconds a wait may take (0: no limit)
			std::atomic<double> Deadline{ 0 }; // when every wait ends (Now(); 0: none)
			std::string LocalHost, RemoteHost;
			int LocalPort = 0, RemotePort = 0;
			std::unique_ptr<TlsState> Tls; // a TLS connection's
			std::string CertFile, KeyFile;  // a TLS server's
			bool IsTls = false;
			// An accepted TLS connection's handshake, made by its first read
			// or write (on that connection's own job, so a client that sends
			// nothing holds up only itself, not the server's accepts).
			std::shared_ptr<Socket> HandshakeWith; // the server
			std::mutex Handshaking;
			bool HandshakeDone = true;
			std::string HandshakeFailed;

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
				if (Closed.load())
					return;
#ifdef CLUAU_HAS_MBEDTLS
				// The peer is told (when no wait is using the connection: one
				// that is may be making the handshake, which sets Tls).
				if (Users == 0 && Tls)
				{
					std::lock_guard<std::mutex> tls(Tls->Lock);
					mbedtls_ssl_close_notify(&Tls->Ssl);
				}
#endif
				Closed = true;
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

		// Late: the socket's deadline passed.
		enum class Ready { Yes, TimedOut, Late, Closed, Failed };

		bool PastDeadline(const Socket& socket)
		{
			const double deadline = socket.Deadline;
			return deadline > 0 && Now() >= deadline;
		}

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

		// Until the socket can be read (or written), in steps of 100 ms: for
		// at most `timeout` seconds (0: no limit), and no later than `limit`
		// (a time from Now(), 0: none; also TimedOut) or the socket's deadline.
		Ready WaitFor(const Socket& socket, bool write, double timeout, double limit = 0)
		{
			const double started = Now();
			double until = timeout > 0 ? started + timeout : 0;
			if (limit > 0 && (until == 0 || limit < until))
				until = limit;
			for (;;)
			{
				if (socket.Closed)
					return Ready::Closed;
				// Before the poll too: data that keeps coming does not put
				// the deadline off.
				if (PastDeadline(socket))
					return Ready::Late;
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
				if (until > 0 && Now() >= until)
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
			case Ready::Late: return "the time setDeadline gave has passed";
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
				if (!lua_isnumber(L, -1) || !(lua_tonumber(L, -1) > 0))
					luaL_error(L, "%s: options.timeout is a number of seconds", fn);
				timeout = lua_tonumber(L, -1);
			}
			lua_pop(L, 1);
			return timeout;
		}

		// ---- TLS ---------------------------------------------------------------

		struct TlsOptions
		{
			bool Enabled = false;
			bool Verify = true;
			std::string CaFile, ServerName;
		};

		// A non-blocking call that found nothing to do yet (try again once
		// the socket is ready).
		bool WouldBlock()
		{
#ifdef _WIN32
			return WSAGetLastError() == WSAEWOULDBLOCK;
#else
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
		}

#ifdef CLUAU_HAS_MBEDTLS

		// mbedTLS's view of the (non-blocking) socket. A failure keeps the
		// system's reason, for the error the script sees.
		int TlsSend(void* context, const unsigned char* data, size_t size)
		{
			Socket& socket = *static_cast<Socket*>(context);
#ifdef _WIN32
			const int n = ::send(socket.Handle, reinterpret_cast<const char*>(data), (int)std::min<size_t>(size, 1 << 30), 0);
#else
			const ssize_t n = ::send(socket.Handle, data, size, kSendFlags);
#endif
			if (n < 0)
			{
				if (WouldBlock())
					return MBEDTLS_ERR_SSL_WANT_WRITE;
				socket.Tls->IoError = LastError();
				return MBEDTLS_ERR_NET_SEND_FAILED;
			}
			return (int)n;
		}

		int TlsReceive(void* context, unsigned char* data, size_t size)
		{
			Socket& socket = *static_cast<Socket*>(context);
			const auto n = ::recv(socket.Handle, reinterpret_cast<char*>(data), (int)std::min<size_t>(size, 1 << 30), 0);
			if (n < 0)
			{
				if (WouldBlock())
					return MBEDTLS_ERR_SSL_WANT_READ;
				socket.Tls->IoError = LastError();
				return MBEDTLS_ERR_NET_RECV_FAILED;
			}
			return (int)n;
		}

		// One TLS call, retried as the socket becomes ready: its result, or
		// false with the reason the wait ended (or the socket failed). The
		// waits end by `limit` too (a time from Now(), 0: none).
		template <typename Call>
		bool TlsCall(Socket& socket, double timeout, Call call, int& rc, std::string& why, double limit = 0)
		{
			for (;;)
			{
				if (PastDeadline(socket))
				{
					why = Why(Ready::Late, timeout);
					return false;
				}
				{
					std::lock_guard<std::mutex> lock(socket.Tls->Lock);
					rc = call(socket.Tls->Ssl);
					if ((rc == MBEDTLS_ERR_NET_RECV_FAILED || rc == MBEDTLS_ERR_NET_SEND_FAILED) && !socket.Tls->IoError.empty())
					{
						why = socket.Tls->IoError;
						return false;
					}
				}
				if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
					return true;
				const Ready ready = WaitFor(socket, rc == MBEDTLS_ERR_SSL_WANT_WRITE, timeout, limit);
				if (ready != Ready::Yes)
				{
					why = Why(ready, timeout);
					return false;
				}
			}
		}

		// The handshake, as a client (`options`) or a server (`server`'s
		// certificate and key): "" when it succeeded, else the reason. It has
		// `timeout` seconds in all, however its messages arrive.
		std::string Handshake(Socket& socket, double timeout, const TlsOptions* options, const Socket* server, const std::string& host)
		{
			const double limit = Now() + timeout;
			auto tls = std::make_unique<TlsState>();
			int rc = mbedtls_ctr_drbg_seed(&tls->Random, mbedtls_entropy_func, &tls->Entropy, nullptr, 0);
			if (rc == 0)
				rc = mbedtls_ssl_config_defaults(&tls->Config, server ? MBEDTLS_SSL_IS_SERVER : MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
					MBEDTLS_SSL_PRESET_DEFAULT);
			if (rc != 0)
				return TlsError(rc);
			mbedtls_ssl_conf_rng(&tls->Config, mbedtls_ctr_drbg_random, &tls->Random);
			if (server)
			{
				rc = mbedtls_x509_crt_parse_file(&tls->Own, server->CertFile.c_str());
				if (rc < 0)
					return "cannot read the certificate '" + server->CertFile + "': " + TlsError(rc);
				rc = mbedtls_pk_parse_keyfile(&tls->Key, server->KeyFile.c_str(), nullptr, mbedtls_ctr_drbg_random, &tls->Random);
				if (rc < 0)
					return "cannot read the key '" + server->KeyFile + "': " + TlsError(rc);
				rc = mbedtls_ssl_conf_own_cert(&tls->Config, &tls->Own, &tls->Key);
				if (rc != 0)
					return TlsError(rc);
			}
			else
			{
				std::string error;
				if (options->Verify)
				{
					if (!LoadTrusted(tls->Trusted, options->CaFile, error))
						return error;
					mbedtls_ssl_conf_ca_chain(&tls->Config, &tls->Trusted, nullptr);
				}
				mbedtls_ssl_conf_authmode(&tls->Config, options->Verify ? MBEDTLS_SSL_VERIFY_REQUIRED : MBEDTLS_SSL_VERIFY_NONE);
			}
			if ((rc = mbedtls_ssl_setup(&tls->Ssl, &tls->Config)) != 0)
				return TlsError(rc);
			if (!server)
			{
				const std::string& name = options->ServerName.empty() ? host : options->ServerName;
				if ((rc = mbedtls_ssl_set_hostname(&tls->Ssl, name.c_str())) != 0)
					return TlsError(rc);
			}
			mbedtls_ssl_set_bio(&tls->Ssl, &socket, TlsSend, TlsReceive, nullptr);
			SetBlocking(socket.Handle, false);
			socket.Tls = std::move(tls);
			std::string why;
			if (!TlsCall(socket, timeout, [](mbedtls_ssl_context& ssl) { return mbedtls_ssl_handshake(&ssl); }, rc, why, limit))
				return why;
			if (rc != 0)
			{
				const uint32_t flags = mbedtls_ssl_get_verify_result(&socket.Tls->Ssl);
				if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags != 0)
				{
					char info[512] = {};
					mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
					std::string text = info;
					while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
						text.pop_back();
					for (char& c : text)
						if (c == '\n')
							c = ';';
					return "the server's certificate is not trusted (" + text + ")";
				}
				return TlsError(rc);
			}
			return {};
		}
#endif

		TlsOptions ReadTlsOptions(lua_State* L, int index, const char* fn)
		{
			TlsOptions tls;
			if (!lua_istable(L, index))
				return tls;
			lua_getfield(L, index, "tls");
			tls.Enabled = lua_toboolean(L, -1);
			lua_getfield(L, index, "verify");
			tls.Verify = lua_isnil(L, -1) || lua_toboolean(L, -1);
			lua_getfield(L, index, "caFile");
			if (lua_isstring(L, -1))
				tls.CaFile = lua_tostring(L, -1);
			lua_getfield(L, index, "serverName");
			if (lua_isstring(L, -1))
				tls.ServerName = lua_tostring(L, -1);
			lua_pop(L, 4);
#ifndef CLUAU_HAS_MBEDTLS
			if (tls.Enabled)
				luaL_error(L, "%s: this cluau is built without TLS", fn);
#else
			(void)fn;
#endif
			return tls;
		}

		// ---- net.connect -------------------------------------------------

		int NetConnect(lua_State* L)
		{
			const std::string host = luaL_checkstring(L, 1);
			const int port = (int)luaL_checkinteger(L, 2);
			if (port <= 0 || port > 65535)
				luaL_argerror(L, 2, "a port from 1 to 65535");
			const double timeout = TimeoutOption(L, 3, "net.connect");
			const TlsOptions tls = ReadTlsOptions(L, 3, "net.connect");
			StartSockets();
#ifdef CLUAU_HAS_MBEDTLS
			if (tls.Enabled)
				StartMbedtls(); // here, on the script's thread, before a job uses it
#endif
			return Await(L, [host, port, timeout, tls]() -> JobResult {
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
						int error = 0;
						socklen_t length = sizeof(error);
#ifdef _WIN32
						// Before Windows 10 2004, WSAPoll does not report a
						// connect that failed: its error is looked at each step.
						const double until = timeout > 0 ? Now() + timeout : 0;
						Ready ready;
						for (;;)
						{
							ready = WaitFor(*socket, true, 0.1);
							length = sizeof(error);
							getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
							if (ready != Ready::TimedOut || error != 0 || (until > 0 && Now() >= until))
								break;
						}
#else
						const Ready ready = WaitFor(*socket, true, timeout);
						getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length);
#endif
						if (ready != Ready::Yes || error != 0)
						{
#ifdef _WIN32
							why = error != 0 ? (WSASetLastError(error), LastError()) : Why(ready, timeout);
#else
							why = error != 0 ? std::strerror(error) : Why(ready, timeout);
#endif
							CloseHandle(s);
							socket->Handle = kNoSocket;
							continue;
						}
					}
					break; // the socket stays non-blocking: each wait has its time limit
				}
				freeaddrinfo(found);
				if (socket->Handle == kNoSocket)
					return Failure("net.connect: cannot connect to " + where + ": " + why);
				Prepare(socket->Handle);
				socket->Addresses();
#ifdef CLUAU_HAS_MBEDTLS
				if (tls.Enabled)
				{
					if (const std::string failed = Handshake(*socket, timeout > 0 ? timeout : 30, &tls, nullptr, host); !failed.empty())
						return Failure("net.connect: TLS with " + where + ": " + failed);
					socket->IsTls = true;
				}
#else
				(void)tls;
#endif
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
			std::string certFile, keyFile;
			if (lua_istable(L, 3))
			{
				lua_getfield(L, 3, "backlog");
				if (lua_isnumber(L, -1))
					backlog = std::max(1, (int)lua_tointeger(L, -1));
				lua_pop(L, 1);
				// tls = { certFile, keyFile }: the server's certificate (chain) and key, PEM.
				lua_getfield(L, 3, "tls");
				if (lua_istable(L, -1))
				{
					lua_getfield(L, -1, "certFile");
					lua_getfield(L, -2, "keyFile");
					if (!lua_isstring(L, -2) || !lua_isstring(L, -1))
						luaL_error(L, "net.listen: options.tls is { certFile = <PEM file>, keyFile = <PEM file> }");
					certFile = lua_tostring(L, -2);
					keyFile = lua_tostring(L, -1);
					lua_pop(L, 2);
#ifndef CLUAU_HAS_MBEDTLS
					luaL_error(L, "net.listen: this cluau is built without TLS");
#endif
				}
				else if (!lua_isnil(L, -1))
					luaL_error(L, "net.listen: options.tls is { certFile = <PEM file>, keyFile = <PEM file> }");
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
				// Non-blocking: of two accepts woken for one client, the other
				// goes back to waiting (within its time limit).
				SetBlocking(s, false);
			}
			freeaddrinfo(found);
			if (server->Handle == kNoSocket)
				luaL_error(L, "net.listen: cannot listen on %s: %s", where.c_str(), why.c_str());
			server->Addresses();
			server->CertFile = certFile;
			server->KeyFile = keyFile;
#ifdef CLUAU_HAS_MBEDTLS
			if (!certFile.empty())
			{
				StartMbedtls();
				// Read now, so a missing or broken file, or a key that is not the
				// certificate's, is this call's error (not every handshake's).
				mbedtls_x509_crt cert;
				mbedtls_x509_crt_init(&cert);
				mbedtls_pk_context key;
				mbedtls_pk_init(&key);
				mbedtls_ctr_drbg_context random;
				mbedtls_ctr_drbg_init(&random);
				mbedtls_entropy_context entropy;
				mbedtls_entropy_init(&entropy);
				std::string failed;
				if (const int rc = mbedtls_x509_crt_parse_file(&cert, certFile.c_str()); rc < 0)
					failed = "cannot read the certificate '" + certFile + "': " + TlsError(rc);
				else
				{
					int keyRc = mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy, nullptr, 0);
					if (keyRc == 0)
						keyRc = mbedtls_pk_parse_keyfile(&key, keyFile.c_str(), nullptr, mbedtls_ctr_drbg_random, &random);
					if (keyRc < 0)
						failed = "cannot read the key '" + keyFile + "': " + TlsError(keyRc);
					else if (mbedtls_pk_check_pair(&cert.pk, &key, mbedtls_ctr_drbg_random, &random) != 0)
						failed = "the key '" + keyFile + "' is not the key of the certificate '" + certFile + "'";
				}
				mbedtls_x509_crt_free(&cert);
				mbedtls_pk_free(&key);
				mbedtls_ctr_drbg_free(&random);
				mbedtls_entropy_free(&entropy);
				if (!failed.empty())
				{
					server->Close();
					luaL_error(L, "net.listen: %s", failed.c_str());
				}
			}
#endif
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
				std::shared_ptr<Socket> socket;
				for (;;)
				{
					const Ready ready = WaitFor(*server, false, timeout);
					if (ready != Ready::Yes)
						return Failure("server:accept: " + Why(ready, timeout));
					const SocketHandle s = ::accept(server->Handle, nullptr, nullptr);
					if (s == kNoSocket)
					{
						// Another accept took the client, or it left before
						// being taken: wait for the next.
#ifdef _WIN32
						if (WouldBlock() || WSAGetLastError() == WSAECONNRESET)
							continue;
#else
						if (WouldBlock() || errno == ECONNABORTED || errno == EPROTO)
							continue;
#endif
						return Failure("server:accept: " + LastError());
					}
					socket = std::make_shared<Socket>();
					socket->Handle = s;
					SetBlocking(s, false); // not inherited from the listener everywhere
					Prepare(s);
					socket->Addresses();
#ifdef CLUAU_HAS_MBEDTLS
					if (!server->CertFile.empty())
					{
						socket->HandshakeWith = server;
						socket->HandshakeDone = false;
						socket->IsTls = true;
					}
#endif
					break;
				}
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
			else if (std::strcmp(key, "tls") == 0)
				lua_pushboolean(L, !server->CertFile.empty());
			else
			{
				lua_getmetatable(L, 1);
				lua_getfield(L, -1, "methods");
				lua_getfield(L, -1, key);
			}
			return 1;
		}

		// ---- a connection -------------------------------------------------

		// An accepted TLS connection's handshake, the first time it is used:
		// false (with the reason) when it failed.
		bool EnsureHandshake(Socket& socket, std::string& why)
		{
#ifdef CLUAU_HAS_MBEDTLS
			std::lock_guard<std::mutex> lock(socket.Handshaking);
			if (!socket.HandshakeDone)
			{
				socket.HandshakeDone = true;
				const double timeout = socket.Timeout;
				socket.HandshakeFailed = Handshake(socket, timeout > 0 ? timeout : 30, nullptr, socket.HandshakeWith.get(), {});
				socket.HandshakeWith.reset();
			}
			if (!socket.HandshakeFailed.empty())
			{
				why = "TLS: " + socket.HandshakeFailed;
				return false;
			}
#else
			(void)socket;
			(void)why;
#endif
			return true;
		}

		// More of the stream into the buffer; false at its end. Called with
		// the reading lock held.
		bool Fill(Socket& socket, std::string& why)
		{
			if (socket.Ended)
				return false;
			// A client whose handshake failed is a connection that ended (as a
			// server sees one that left): its reads find the end.
			if (std::string failed; !EnsureHandshake(socket, failed))
			{
				socket.Ended = true;
				return false;
			}
#ifdef CLUAU_HAS_MBEDTLS
			if (socket.Tls)
			{
				unsigned char chunk[16384];
				for (;;)
				{
					int rc = 0;
					if (!TlsCall(socket, socket.Timeout, [&](mbedtls_ssl_context& ssl) { return mbedtls_ssl_read(&ssl, chunk, sizeof(chunk)); }, rc, why))
						return false;
					if (rc > 0)
					{
						socket.Buffer.append(reinterpret_cast<const char*>(chunk), (size_t)rc);
						return true;
					}
					if (rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
						continue;
					if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_NET_CONN_RESET)
					{
						socket.Ended = true;
						return false;
					}
					why = TlsError(rc);
					return false;
				}
			}
#endif
			if (PastDeadline(socket))
			{
				why = Why(Ready::Late, socket.Timeout);
				return false;
			}
			char chunk[16384];
			auto n = ::recv(socket.Handle, chunk, (int)sizeof(chunk), 0);
			while (n < 0 && WouldBlock())
			{
				const Ready ready = WaitFor(socket, false, socket.Timeout);
				if (ready != Ready::Yes)
				{
					why = Why(ready, socket.Timeout);
					return false;
				}
				n = ::recv(socket.Handle, chunk, (int)sizeof(chunk), 0);
			}
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
				if (!use.Entered)
				{
					// Closed: what was read before is still given out, and no
					// more is read (the handle may be gone).
					if (socket->Buffer.empty())
						return Failure(std::string(fn) + ": the socket is closed");
					socket->Ended = true;
				}
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
					// `count`: the longest line taken, without its line break
					// (0: no limit); a longer one is an error, and stays
					// unread. Each search starts where the last one ended.
					const std::string& buffer = socket->Buffer;
					size_t newline;
					size_t searched = 0;
					auto room = [&] {
						// A line of `count` bytes may still be followed by \r\n.
						return count == 0 || buffer.size() <= count || (buffer.size() == count + 1 && buffer.back() == '\r');
					};
					while ((newline = buffer.find('\n', searched)) == std::string::npos && room())
					{
						searched = buffer.size();
						if (!Fill(*socket, why))
							break;
					}
					if (count > 0)
					{
						const size_t length = newline == std::string::npos ? buffer.size()
											  : newline > 0 && buffer[newline - 1] == '\r' ? newline - 1
																						   : newline;
						if (length > count)
							return Failure(std::string(fn) + ": a line longer than " + std::to_string(count) + " bytes");
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
			const double count = luaL_optnumber(L, 2, 65536);
			if (!(count >= 1 && count <= INT_MAX) || count != std::floor(count))
				luaL_argerror(L, 2, "a number of bytes from 1");
			return AwaitRead(L, socket, ReadKind::Some, (size_t)count, "socket:read");
		}

		int SocketReadLine(lua_State* L)
		{
			std::shared_ptr<Socket> socket = Check(L, 1, kSocketMeta);
			const double limit = luaL_optnumber(L, 2, 0);
			if (!(limit >= 0) || limit != std::floor(limit))
				luaL_argerror(L, 2, "the longest line, a whole number of bytes (none: no limit)");
			// One beyond any line a buffer could hold is no limit at all.
			const size_t count = limit >= 9007199254740992.0 ? 0 : (size_t)limit;
			return AwaitRead(L, socket, ReadKind::Line, count, "socket:readLine");
		}

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
				if (std::string why; !EnsureHandshake(*socket, why))
					return Failure("socket:write: " + why);
				size_t at = 0;
#ifdef CLUAU_HAS_MBEDTLS
				if (socket->Tls)
				{
					while (at < data.size())
					{
						int rc = 0;
						std::string why;
						const unsigned char* from = reinterpret_cast<const unsigned char*>(data.data()) + at;
						const size_t left = data.size() - at;
						if (!TlsCall(*socket, socket->Timeout, [&](mbedtls_ssl_context& ssl) { return mbedtls_ssl_write(&ssl, from, left); }, rc, why))
							return Failure("socket:write: " + why);
						if (rc < 0)
							return Failure("socket:write: " + TlsError(rc));
						at += (size_t)rc;
					}
					return [](lua_State*) { return 0; };
				}
#endif
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
					{
						if (WouldBlock())
							continue; // the buffer filled again: wait (within the time limit)
						return Failure("socket:write: " + LastError());
					}
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
			if (!(seconds >= 0))
				luaL_argerror(L, 2, "a number of seconds (none, or 0: no limit)");
			socket->Timeout = std::isinf(seconds) ? 0 : seconds;
			return 0;
		}

		// Every wait on the socket (the ones under way too) ends `seconds`
		// from now, however often data comes; none (or nil): no deadline.
		int SocketSetDeadline(lua_State* L)
		{
			std::shared_ptr<Socket> socket = Check(L, 1, kSocketMeta);
			if (lua_isnoneornil(L, 2))
			{
				socket->Deadline = 0;
				return 0;
			}
			const double seconds = luaL_checknumber(L, 2);
			if (!(seconds >= 0))
				luaL_argerror(L, 2, "a number of seconds (none: no deadline)");
			socket->Deadline = std::isinf(seconds) ? 0 : Now() + std::max(seconds, 1e-9);
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
			else if (std::strcmp(key, "tls") == 0)
				lua_pushboolean(L, socket->IsTls);
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
								   std::strcmp(m->name, "setTimeout") != 0 && std::strcmp(m->name, "setDeadline") != 0;
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
			{ "lines", SocketLines }, { "write", SocketWrite }, { "close", SocketClose }, { "setTimeout", SocketSetTimeout },
			{ "setDeadline", SocketSetDeadline }, { nullptr, nullptr } };
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
