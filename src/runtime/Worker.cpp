// @std/worker: scripts that run in parallel. A worker is a script file run
// in a runtime (a Luau VM) of its own on a thread of its own; the script
// that started it and the worker share nothing, and the values they send
// each other (and the worker's arguments and results) are copied: encoded
// into bytes on one side and decoded into new values on the other.
//
// Each worker has two queues of messages, one each way, under one mutex.
// A blocking call (a receive, a join) waits as the standard library's other
// blocking calls do (Jobs.h): in a task the task waits and the others run.

#include "Worker.h"
#include "Debugger.h"
#include "Jobs.h"
#include "Std.h"

#include "lua.h"
#include "lualib.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace cluau::runtime::detail {

	namespace {

		// A thread with the stack a script's own thread has (8 MiB) rather
		// than the platform's default for new threads (512 KiB on macOS, 1
		// MiB on Windows): a worker's script nests as deeply as the main
		// script's, in Luau's C calls, the compiler and the translator.
		class WorkerThread
		{
		public:
			WorkerThread() = default;
			WorkerThread(const WorkerThread&) = delete;
			WorkerThread& operator=(const WorkerThread&) = delete;

			template <typename F>
			explicit WorkerThread(F&& body)
			{
				auto* run = new std::function<void()>(std::forward<F>(body));
#if defined(_WIN32)
				m_Handle = CreateThread(nullptr, kStack, &WorkerThread::Start, run, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
				if (m_Handle == nullptr)
#else
				pthread_attr_t attributes;
				pthread_attr_init(&attributes);
				pthread_attr_setstacksize(&attributes, kStack);
				const int failed = pthread_create(&m_Thread, &attributes, &WorkerThread::Start, run);
				pthread_attr_destroy(&attributes);
				m_Started = failed == 0;
				if (!m_Started)
#endif
				{
					delete run;
					throw std::runtime_error("the system could not start a thread");
				}
			}

			WorkerThread& operator=(WorkerThread&& other) noexcept
			{
				std::swap(m_Handle, other.m_Handle);
				std::swap(m_Thread, other.m_Thread);
				std::swap(m_Started, other.m_Started);
				return *this;
			}

			~WorkerThread()
			{
				if (joinable())
					std::terminate(); // as std::thread does: a thread is joined before it goes
			}

			bool joinable() const
			{
#if defined(_WIN32)
				return m_Handle != nullptr;
#else
				return m_Started;
#endif
			}

			void join()
			{
#if defined(_WIN32)
				WaitForSingleObject(m_Handle, INFINITE);
				CloseHandle(m_Handle);
				m_Handle = nullptr;
#else
				pthread_join(m_Thread, nullptr);
				m_Started = false;
#endif
			}

		private:
			static constexpr size_t kStack = 8 * 1024 * 1024;

#if defined(_WIN32)
			static DWORD WINAPI Start(void* data)
#else
			static void* Start(void* data)
#endif
			{
				std::unique_ptr<std::function<void()>> run(static_cast<std::function<void()>*>(data));
				(*run)();
				return 0;
			}

			void* m_Handle = nullptr; // Windows: the thread's HANDLE
#if !defined(_WIN32)
			pthread_t m_Thread {};
#else
			int m_Thread = 0;
#endif
			bool m_Started = false;
		};

	} // namespace


	namespace {

		constexpr const char* kWorkerMeta = "cluau.Worker";
		constexpr const char* kStartedRegistry = "_cluau_workers"; // the workers a runtime started (Started)

		// What a worker's script sees when it is stopped (a `catch` there
		// gets it; the next safepoint raises it again).
		constexpr const char* kTerminated = "the worker was terminated";
		constexpr const char* kExited = "the worker called process.exit";
		constexpr const char* kInterrupted = "interrupted"; // as the runtime's Ctrl-C

		// Why a worker is to stop (Link::Stop).
		enum StopReason : int
		{
			kRunning = 0,
			kTerminate = 1, // worker:terminate, or its parent's run failed or exits
			kExit = 2,      // its script called process.exit
		};

		enum class Ending
		{
			Running,
			Returned,   // the script finished: Results holds what it returned
			Failed,     // an error ended it: Message, Trace
			Terminated, // worker:terminate (join gives nothing)
			Exited,     // process.exit(ExitStatus)
		};

		// A worker and the script that started it (its parent).
		struct Link : std::enable_shared_from_this<Link>
		{
			std::string Name; // the script as errors show it
			std::string Args; // encoded: the count, then each argument
			unsigned Interrupts = 0; // InterruptCount() when it started: a Ctrl-C since stops it
			std::atomic<int> Stop{ kRunning };

			std::mutex Mutex; // everything below but Thread and Joined
			std::condition_variable ToWorkerCv; // a message for the worker, the parent's end, a stop
			std::condition_variable ToParentCv; // a message for the parent, the worker's end
			std::deque<std::string> ToWorker, ToParent; // encoded messages
			bool ParentDone = false; // nothing more comes to the worker: its parent's script ended, or its Worker was collected
			bool HandleGone = false; // the Worker was collected: the worker's messages have no reader
			Ending End = Ending::Running;
			std::string Results; // Returned: encoded, the count then each value
			std::string Message, Trace;
			bool InTask = false;
			int ExitStatus = 0;

			// The parent's thread's alone: it starts and joins the thread.
			WorkerThread Thread;
			bool Joined = false; // worker:join has given its outcome (or the run's end has)
		};

		// The worker whose script runs on this thread (null elsewhere).
		thread_local Link* t_Self = nullptr;

		// Every worker running in the process, for process.exit to stop.
		// Never destroyed: a worker's thread can be finishing while the
		// process exits.
		struct Live
		{
			std::mutex Mutex;
			std::condition_variable Ended;
			std::set<Link*> All;
		};

		Live& Everyone()
		{
			static Live* live = new Live;
			return *live;
		}

		void RequestStop(Link& link, int why)
		{
			int running = kRunning;
			link.Stop.compare_exchange_strong(running, why);
			{
				// A waiter between looking at Stop and waiting sees it.
				std::lock_guard<std::mutex> lock(link.Mutex);
			}
			link.ToWorkerCv.notify_all();
		}

		// The worker's receive gives nil once its messages are read.
		void CloseToWorker(Link& link)
		{
			{
				std::lock_guard<std::mutex> lock(link.Mutex);
				link.ParentDone = true;
			}
			link.ToWorkerCv.notify_all();
		}

		bool Ended(Link& link)
		{
			std::lock_guard<std::mutex> lock(link.Mutex);
			return link.End != Ending::Running;
		}

		// An outcome worker:join raises (an error, or a status other than 0).
		bool Failing(Link& link)
		{
			std::lock_guard<std::mutex> lock(link.Mutex);
			return link.End == Ending::Failed || (link.End == Ending::Exited && link.ExitStatus != 0);
		}

		// After a wait that a Ctrl-C (or the worker's own stop) ended: the
		// error that says so, even when the Ctrl-C was taken elsewhere
		// meanwhile.
		[[noreturn]] void RaiseStopped(lua_State* L)
		{
			RaiseIfInterrupted(L);
			lua_rawcheckstack(L, 1);
			lua_pushstring(L, kInterrupted);
			lua_error(L);
		}

		// ---- copies ---------------------------------------------------------
		//
		// A value is encoded as a tag byte and what follows it: a number's 8
		// bytes, a string's or a buffer's length and bytes, a table's json
		// mark, its border (for the array part), its count of pairs and the
		// pairs. Both ends are this process, so a number is its bytes.

		constexpr int kMaxDepth = 1000; // tables in tables, as @std/json decodes

		constexpr char kNil = 'n';
		constexpr char kFalse = 'f';
		constexpr char kTrue = 't';
		constexpr char kNumber = 'd';
		constexpr char kString = 's';
		constexpr char kBuffer = 'b';
		constexpr char kJsonNull = 'z';
		constexpr char kTable = 'T';
		constexpr char kNoMark = '-';

		void PutSize(std::string& out, uint64_t n)
		{
			while (n >= 0x80)
			{
				out += (char)(0x80 | (n & 0x7f));
				n >>= 7;
			}
			out += (char)n;
		}

		bool IsIdentifier(const char* s, size_t len)
		{
			if (len == 0 || std::isdigit((unsigned char)s[0]))
				return false;
			for (size_t i = 0; i < len; i++)
				if (!(std::isalnum((unsigned char)s[i]) || s[i] == '_'))
					return false;
			return true;
		}

		// What a value is, for an error: "a function", "a Worker".
		std::string Kind(lua_State* L, int index)
		{
			if (lua_type(L, index) == LUA_TUSERDATA && luaL_getmetafield(L, index, "__type"))
			{
				std::string kind = lua_isstring(L, -1) ? std::string("a ") + lua_tostring(L, -1) : "a userdata";
				lua_pop(L, 1);
				return kind;
			}
			return std::string("a ") + lua_typename(L, lua_type(L, index));
		}

		class Encoder
		{
		public:
			Encoder(lua_State* L, std::string& out)
				: m_L(L)
				, m_Out(out)
			{
			}

			// Appends the value at `index`, which errors call `root`
			// ("value", "args[2]"); false, with Error saying why, for a value
			// that cannot be sent.
			bool Put(int index, std::string root)
			{
				m_Root = std::move(root);
				return Value(lua_absindex(m_L, index), 0);
			}

			std::string Error;

		private:
			bool Value(int index, int depth)
			{
				switch (lua_type(m_L, index))
				{
				case LUA_TNIL:
					m_Out += kNil;
					return true;
				case LUA_TBOOLEAN:
					m_Out += lua_toboolean(m_L, index) ? kTrue : kFalse;
					return true;
				case LUA_TNUMBER:
				{
					const double d = lua_tonumber(m_L, index);
					char bytes[sizeof(double)];
					std::memcpy(bytes, &d, sizeof(double));
					m_Out += kNumber;
					m_Out.append(bytes, sizeof(double));
					return true;
				}
				case LUA_TSTRING:
				{
					size_t len = 0;
					const char* s = lua_tolstring(m_L, index, &len);
					m_Out += kString;
					PutSize(m_Out, len);
					m_Out.append(s, len);
					return true;
				}
				case LUA_TBUFFER:
				{
					size_t len = 0;
					const void* data = lua_tobuffer(m_L, index, &len);
					m_Out += kBuffer;
					PutSize(m_Out, len);
					m_Out.append(static_cast<const char*>(data), len);
					return true;
				}
				case LUA_TLIGHTUSERDATA:
					if (JsonNullAt(m_L, index))
					{
						m_Out += kJsonNull;
						return true;
					}
					break;
				case LUA_TTABLE:
					return Table(index, depth);
				default:
					break;
				}
				return Fail(Kind(m_L, index) + " at " + Path(m_Keys.size()) + " cannot be sent");
			}

			bool Table(int index, int depth)
			{
				const void* self = lua_topointer(m_L, index);
				if (std::find(m_Open.begin(), m_Open.end(), self) != m_Open.end())
					return Fail("the table at " + Path(m_Keys.size()) + " is inside itself: a cycle cannot be sent");
				if (depth >= kMaxDepth || !lua_checkstack(m_L, 4))
					return Fail("the value nests tables more than " + std::to_string(kMaxDepth) + " deep, too deep to send");
				char mark = kNoMark;
				if (lua_getmetatable(m_L, index))
				{
					lua_pop(m_L, 1);
					mark = JsonMarkAt(m_L, index);
					if (!mark)
						return Fail("a table with a metatable (an instance of a class, say) at " + Path(m_Keys.size()) +
							" cannot be sent: only plain tables are copied");
				}
				if (!m_Seen.insert(self).second)
					return Fail("the table at " + Path(m_Keys.size()) + " is also elsewhere in the value: a table is copied once, so it cannot be sent twice");
				m_Open.push_back(self);
				m_Out += kTable;
				m_Out += mark;
				PutSize(m_Out, (uint64_t)std::max(0, lua_objlen(m_L, index)));
				// The count of pairs, filled in once they are written.
				const size_t countAt = m_Out.size();
				m_Out.append(sizeof(uint64_t), '\0');
				uint64_t count = 0;
				m_Keys.push_back(0);
				lua_pushnil(m_L);
				while (lua_next(m_L, index))
				{
					const int value = lua_gettop(m_L);
					m_Keys.back() = value - 1;
					if (!Key(value - 1, depth) || !Value(value, depth + 1))
						return false;
					lua_pop(m_L, 1);
					count++;
				}
				m_Keys.pop_back();
				m_Open.pop_back();
				std::memcpy(&m_Out[countAt], &count, sizeof(count));
				return true;
			}

			// A key is a value as any other, but an error names the table
			// it is in.
			bool Key(int index, int depth)
			{
				switch (lua_type(m_L, index))
				{
				case LUA_TBOOLEAN:
				case LUA_TNUMBER:
				case LUA_TSTRING:
				case LUA_TBUFFER:
				case LUA_TTABLE:
					return Value(index, depth + 1);
				case LUA_TLIGHTUSERDATA:
					if (JsonNullAt(m_L, index))
						return Value(index, depth + 1);
					break;
				default:
					break;
				}
				return Fail(Kind(m_L, index) + " as a key in " + Path(m_Keys.size() - 1) + " cannot be sent");
			}

			bool Fail(std::string why)
			{
				Error = std::move(why);
				return false;
			}

			// Where the value is: the root, then the keys of the first
			// `levels` tables it is in (`value.handlers[2]`).
			std::string Path(size_t levels) const
			{
				std::string out = m_Root;
				for (size_t i = 0; i < levels; i++)
					out += Segment(m_Keys[i]);
				return out;
			}

			std::string Segment(int key) const
			{
				switch (lua_type(m_L, key))
				{
				case LUA_TSTRING:
				{
					size_t len = 0;
					const char* s = lua_tolstring(m_L, key, &len);
					if (IsIdentifier(s, len))
						return "." + std::string(s, len);
					std::string out = "[\"";
					for (size_t i = 0; i < len && i < 40; i++)
					{
						const unsigned char c = (unsigned char)s[i];
						if (c == '"' || c == '\\')
							out += '\\';
						if (c < 0x20 || c == 0x7f)
						{
							char escaped[8];
							std::snprintf(escaped, sizeof(escaped), "\\%03d", c);
							out += escaped;
						}
						else
							out += (char)c;
					}
					return out + (len > 40 ? "...\"]" : "\"]");
				}
				case LUA_TNUMBER:
				{
					const double d = lua_tonumber(m_L, key);
					char text[40];
					if (d == std::floor(d) && std::fabs(d) < 1e15)
						std::snprintf(text, sizeof(text), "[%.0f]", d);
					else
						std::snprintf(text, sizeof(text), "[%.17g]", d);
					return text;
				}
				case LUA_TBOOLEAN:
					return lua_toboolean(m_L, key) ? "[true]" : "[false]";
				default:
					return "[" + Kind(m_L, key) + "]";
				}
			}

			lua_State* m_L;
			std::string& m_Out;
			std::string m_Root;
			std::vector<int> m_Keys;                // each open table's current key, on the stack
			std::vector<const void*> m_Open;        // the tables being copied (one met again is a cycle)
			std::unordered_set<const void*> m_Seen; // every table copied so far
		};

		// Reads what an Encoder wrote (so it trusts it), pushing the values.
		class Decoder
		{
		public:
			Decoder(lua_State* L, const std::string& bytes)
				: m_L(L)
				, m_Bytes(bytes)
			{
			}

			uint64_t Size()
			{
				uint64_t n = 0;
				for (int shift = 0;; shift += 7)
				{
					const unsigned char c = (unsigned char)m_Bytes[m_At++];
					n |= (uint64_t)(c & 0x7f) << shift;
					if (!(c & 0x80))
						return n;
				}
			}

			void Value()
			{
				const char tag = m_Bytes[m_At++];
				switch (tag)
				{
				case kNil:
					lua_pushnil(m_L);
					return;
				case kFalse:
				case kTrue:
					lua_pushboolean(m_L, tag == kTrue);
					return;
				case kNumber:
				{
					double d = 0;
					std::memcpy(&d, m_Bytes.data() + m_At, sizeof(double));
					m_At += sizeof(double);
					lua_pushnumber(m_L, d);
					return;
				}
				case kString:
				{
					const size_t len = (size_t)Size();
					lua_pushlstring(m_L, m_Bytes.data() + m_At, len);
					m_At += len;
					return;
				}
				case kBuffer:
				{
					const size_t len = (size_t)Size();
					void* data = lua_newbuffer(m_L, len);
					if (len > 0)
						std::memcpy(data, m_Bytes.data() + m_At, len);
					m_At += len;
					return;
				}
				case kJsonNull:
					PushJsonNull(m_L);
					return;
				default: // kTable
				{
					const char mark = m_Bytes[m_At++];
					const uint64_t border = Size();
					uint64_t count = 0;
					std::memcpy(&count, m_Bytes.data() + m_At, sizeof(count));
					m_At += sizeof(count);
					luaL_checkstack(m_L, 3, "the value nests too deep");
					const uint64_t array = std::min(border, count);
					lua_createtable(m_L, (int)std::min<uint64_t>(array, INT_MAX), (int)std::min<uint64_t>(count - array, INT_MAX));
					for (uint64_t i = 0; i < count; i++)
					{
						Value();
						Value();
						lua_rawset(m_L, -3);
					}
					if (mark != kNoMark)
						SetJsonMark(m_L, -1, mark);
					return;
				}
				}
			}

		private:
			lua_State* m_L;
			const std::string& m_Bytes;
			size_t m_At = 0;
		};

		// The one value a send takes (nil is refused: receive's nil means
		// that no message came).
		std::string EncodeMessage(lua_State* L, int index, const char* fn)
		{
			if (lua_isnoneornil(L, index))
				luaL_error(L, "%s: nil cannot be sent (receive gives nil for no message)", fn);
			std::string bytes;
			Encoder encoder(L, bytes);
			if (!encoder.Put(index, "value"))
				luaL_error(L, "%s: %s", fn, encoder.Error.c_str());
			return bytes;
		}

		int PushMessage(lua_State* L, const std::string& bytes)
		{
			Decoder decoder(L, bytes);
			decoder.Value();
			return 1;
		}

		// ---- waiting ------------------------------------------------------

		double Seconds(lua_State* L, int index, const char* fn)
		{
			if (lua_isnoneornil(L, index))
				return HUGE_VAL;
			const double seconds = luaL_checknumber(L, index);
			if (seconds != seconds)
				luaL_error(L, "%s: the time is NaN, not a number of seconds", fn);
			return std::max(0.0, seconds);
		}

		// No more can come: to the parent once the worker has ended; to the
		// worker once its parent sends no more, or it is to stop.
		bool Over(const Link& link, bool toParent) { return toParent ? link.End != Ending::Running : link.ParentDone || link.Stop != kRunning; }

		// The next message one way (the parent's when `toParent`), waiting
		// `seconds` at most (math.huge or more: no limit); nil when none came.
		int Receive(lua_State* L, Link& link, bool toParent, double seconds)
		{
			// A message already there is taken here: a thread for each
			// message would cost more than the message.
			std::string bytes;
			bool took = false;
			bool none = false;
			{
				std::lock_guard<std::mutex> lock(link.Mutex);
				std::deque<std::string>& queue = toParent ? link.ToParent : link.ToWorker;
				if (!queue.empty())
				{
					bytes = std::move(queue.front());
					queue.pop_front();
					took = true;
				}
				else
					none = seconds <= 0 || Over(link, toParent);
			}
			if (took)
				return PushMessage(L, bytes);
			if (none)
			{
				if (!toParent)
					RaiseIfInterrupted(L); // a worker that is to stop stops here
				lua_pushnil(L);
				return 1;
			}
			// A receive whose task is cancelled takes nothing, or gives back
			// what it took (first in line again).
			struct Taking
			{
				bool Dropped = false;
				bool Took = false;
				bool Interrupted = false;
				std::string Bytes;
			};
			auto taking = std::make_shared<Taking>();
			std::shared_ptr<Link> shared = link.shared_from_this();
			const unsigned interrupts = InterruptCount();
			auto work = [shared, toParent, seconds, taking, interrupts]() -> JobResult {
				using Clock = std::chrono::steady_clock;
				const bool forever = !(seconds < 1e9);
				const Clock::time_point until =
					forever ? Clock::time_point::max() : Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
				{
					std::unique_lock<std::mutex> lock(shared->Mutex);
					std::deque<std::string>& queue = toParent ? shared->ToParent : shared->ToWorker;
					std::condition_variable& arrived = toParent ? shared->ToParentCv : shared->ToWorkerCv;
					while (!taking->Dropped)
					{
						if (!queue.empty())
						{
							taking->Bytes = std::move(queue.front());
							queue.pop_front();
							taking->Took = true;
							break;
						}
						if (Over(*shared, toParent))
							break;
						const Clock::time_point now = Clock::now();
						if (now >= until)
							break;
						// In steps, so that a Ctrl-C ends the wait.
						if (InterruptPending() || InterruptCount() != interrupts)
						{
							taking->Interrupted = true;
							break;
						}
						Clock::time_point step = now + std::chrono::milliseconds(100);
						if (until < step)
							step = until;
						arrived.wait_until(lock, step);
					}
				}
				return [taking, toParent](lua_State* L) -> int {
					if (taking->Took)
						return PushMessage(L, taking->Bytes);
					if (taking->Interrupted)
						RaiseStopped(L);
					if (!toParent)
						RaiseIfInterrupted(L);
					lua_pushnil(L);
					return 1;
				};
			};
			auto unclaimed = [shared, toParent, taking] {
				{
					std::lock_guard<std::mutex> lock(shared->Mutex);
					taking->Dropped = true;
					if (taking->Took)
					{
						(toParent ? shared->ToParent : shared->ToWorker).push_front(std::move(taking->Bytes));
						taking->Took = false;
					}
				}
				(toParent ? shared->ToParentCv : shared->ToWorkerCv).notify_all();
			};
			return Await(L, std::move(work), std::move(unclaimed));
		}

		// ---- the workers a runtime started ---------------------------------

		struct Started
		{
			std::vector<std::shared_ptr<Link>> All; // in the order started: those not joined yet
			// The first that failed with its Worker collected unjoined: the
			// run's end raises its error.
			std::shared_ptr<Link> Unjoined;
		};

		// Every worker terminated, and its thread waited for (no Lua).
		void StopAll(Started& started)
		{
			for (const std::shared_ptr<Link>& link : started.All)
			{
				RequestStop(*link, kTerminate);
				CloseToWorker(*link);
			}
			for (const std::shared_ptr<Link>& link : started.All)
				if (link->Thread.joinable())
					link->Thread.join();
			started.All.clear();
		}

		Started* GetStarted(lua_State* L, bool make)
		{
			lua_getfield(L, LUA_REGISTRYINDEX, kStartedRegistry);
			Started* started = static_cast<Started*>(lua_touserdata(L, -1));
			lua_pop(L, 1);
			if (started || !make)
				return started;
			void* memory = lua_newuserdatadtor(L, sizeof(Started), [](lua_State*, void* p) {
				// The runtime is closing: its workers are stopped.
				Started* started = static_cast<Started*>(p);
				StopAll(*started);
				started->~Started();
			});
			started = new (memory) Started();
			lua_setfield(L, LUA_REGISTRYINDEX, kStartedRegistry);
			return started;
		}

		// The workers that have ended: their threads are waited for, and
		// those whose Worker is gone are let go (the first failure among
		// them kept for the run's end).
		void Sweep(Started& started)
		{
			std::vector<std::shared_ptr<Link>> kept;
			for (std::shared_ptr<Link>& link : started.All)
			{
				bool ended = false;
				bool gone = false;
				{
					std::lock_guard<std::mutex> lock(link->Mutex);
					ended = link->End != Ending::Running;
					gone = link->HandleGone;
				}
				if (ended && link->Thread.joinable())
					link->Thread.join();
				if (!ended || !gone)
					kept.push_back(std::move(link));
				else if (!started.Unjoined && Failing(*link))
					started.Unjoined = std::move(link);
			}
			started.All = std::move(kept);
		}

		// The error a worker that has ended raises in join ("" for none). Its
		// fields were written before its end was set, and not after.
		std::string FailureText(Link& link)
		{
			Ending end;
			{
				std::lock_guard<std::mutex> lock(link.Mutex);
				end = link.End;
			}
			if (end == Ending::Exited && link.ExitStatus != 0)
				return "the worker " + link.Name + " ended with process.exit(" + std::to_string(link.ExitStatus) + ")";
			if (end != Ending::Failed)
				return {};
			std::string text = "the worker " + link.Name + " failed: " + link.Message;
			if (!link.Trace.empty())
				text += std::string("\nstack backtrace (in the worker") + (link.InTask ? "'s task" : "") + "):\n" + link.Trace;
			return text;
		}

		// What a worker that has ended gives its join: its script's results,
		// nothing (terminated, or process.exit(0)), or its error raised.
		int Outcome(lua_State* L, Link& link)
		{
			const std::string failure = FailureText(link);
			if (!failure.empty())
			{
				// A Ctrl-C ends the script as a Ctrl-C does (status 130).
				if (link.Message == kInterrupted && InterruptCount() != link.Interrupts)
				{
					lua_pushstring(L, kInterrupted);
					lua_error(L);
				}
				luaL_error(L, "%s", failure.c_str());
			}
			bool returned = false;
			{
				std::lock_guard<std::mutex> lock(link.Mutex);
				returned = link.End == Ending::Returned;
			}
			if (!returned)
				return 0;
			Decoder decoder(L, link.Results);
			const uint64_t n = decoder.Size();
			if (n > INT_MAX - 1 || !lua_checkstack(L, (int)n + 1))
				luaL_error(L, "worker:join: the worker returned too many values");
			for (uint64_t i = 0; i < n; i++)
				decoder.Value();
			return (int)n;
		}

		// The first join: the thread is waited for and the run no longer
		// keeps the worker. Then (and again on a later join) its outcome.
		int Joined(lua_State* L, const std::shared_ptr<Link>& link)
		{
			if (!link->Joined)
			{
				link->Joined = true;
				if (link->Thread.joinable())
					link->Thread.join();
				if (Started* started = GetStarted(L, false))
				{
					auto& all = started->All;
					all.erase(std::remove(all.begin(), all.end(), link), all.end());
				}
			}
			return Outcome(L, *link);
		}

		// ---- the worker's thread ------------------------------------------

		// Luau's interrupt callback in a worker's runtime (the runtime's own
		// is called after it): a worker that is to stop stops at its next
		// safepoint.
		void WorkerInterrupt(lua_State* L, int gc)
		{
			if (gc >= 0)
				return;
			if (const char* why = WorkerStopping())
			{
				lua_rawcheckstack(L, 1);
				lua_pushstring(L, why);
				lua_error(L);
			}
		}

		// The worker's runtime, before its script runs: the module says it
		// is a worker and holds its arguments.
		void Prepare(lua_State* L, const Link& link)
		{
			lua_callbacks(L)->interrupt = WorkerInterrupt;
			lua_getfield(L, LUA_REGISTRYINDEX, kStdRegistry);
			lua_getfield(L, -1, "worker");
			lua_pushboolean(L, 1);
			lua_setfield(L, -2, "isWorker");
			Decoder decoder(L, link.Args);
			const uint64_t n = decoder.Size();
			lua_createtable(L, (int)n, 0);
			for (uint64_t i = 1; i <= n; i++)
			{
				decoder.Value();
				lua_rawseti(L, -2, (int)i);
			}
			lua_setfield(L, -2, "args");
			lua_pop(L, 2);
		}

		std::string EncodeResults(lua_State* T, std::string& out)
		{
			const int n = lua_gettop(T);
			PutSize(out, (uint64_t)n);
			for (int i = 1; i <= n; i++)
			{
				Encoder encoder(T, out);
				if (!encoder.Put(i, "results[" + std::to_string(i) + "]"))
					return encoder.Error + " (what the script returns is copied to worker:join)";
			}
			return {};
		}

		void Main(std::shared_ptr<Link> link, WorkerLaunch launch)
		{
			t_Self = link.get();
			WorkerFailure failure;
			std::string results;
			try
			{
				RunWorker(
					launch, [&link](lua_State* L) { Prepare(L, *link); }, [&results](lua_State* T) { return EncodeResults(T, results); }, failure);
			}
			catch (const std::bad_alloc&)
			{
				failure.Failed = true;
				failure.Message = "not enough memory";
			}
			catch (const std::exception& e)
			{
				failure.Failed = true;
				failure.Message = e.what();
			}
			catch (...)
			{
				failure.Failed = true;
				failure.Message = "the worker's runtime failed";
			}
			t_Self = nullptr;
			{
				std::lock_guard<std::mutex> lock(link->Mutex);
				const int stop = link->Stop.load();
				if (stop == kTerminate)
					link->End = Ending::Terminated;
				else if (stop == kExit)
					link->End = Ending::Exited;
				else if (failure.Failed)
				{
					link->End = Ending::Failed;
					link->Message = std::move(failure.Message);
					link->Trace = std::move(failure.Trace);
					link->InTask = failure.InTask;
				}
				else
				{
					link->End = Ending::Returned;
					link->Results = std::move(results);
				}
				link->ToWorker.clear(); // nothing reads them now
			}
			link->ToParentCv.notify_all();
			Live& live = Everyone();
			{
				std::lock_guard<std::mutex> lock(live.Mutex);
				live.All.erase(link.get());
			}
			live.Ended.notify_all();
		}

		// ---- the Worker (the parent's handle) -------------------------------

		struct Handle
		{
			std::shared_ptr<Link> Shared;
		};

		const std::shared_ptr<Link>& CheckWorker(lua_State* L, int index)
		{
			return static_cast<Handle*>(luaL_checkudata(L, index, kWorkerMeta))->Shared;
		}

		// worker.start(path, ...): the script at `path` running as a worker,
		// `...` (copied) its worker.args.
		int Start(lua_State* L)
		{
			const std::string path = luaL_checkstring(L, 1);
			const int n = lua_gettop(L) - 1;
			std::string args;
			PutSize(args, (uint64_t)n);
			for (int i = 1; i <= n; i++)
			{
				Encoder encoder(L, args);
				if (!encoder.Put(i + 1, "args[" + std::to_string(i) + "]"))
					luaL_error(L, "worker.start: %s", encoder.Error.c_str());
			}
			WorkerLaunch launch = PrepareWorker(L, path, "worker.start");
			Started& started = *GetStarted(L, true);
			Sweep(started);

			auto link = std::make_shared<Link>();
			link->Name = launch.Name;
			link->Args = std::move(args);
			link->Interrupts = InterruptCount();
			void* memory = lua_newuserdatadtor(L, sizeof(Handle), [](lua_State*, void* p) {
				// Nothing can send to the worker or receive from it now.
				Handle* handle = static_cast<Handle*>(p);
				Link& link = *handle->Shared;
				{
					std::lock_guard<std::mutex> lock(link.Mutex);
					link.ParentDone = true;
					link.HandleGone = true;
					link.ToParent.clear();
				}
				link.ToWorkerCv.notify_all();
				handle->~Handle();
			});
			new (memory) Handle{ link };
			luaL_getmetatable(L, kWorkerMeta);
			lua_setmetatable(L, -2);
			started.All.push_back(link);

			std::string failed;
			Live& live = Everyone();
			{
				std::lock_guard<std::mutex> lock(live.Mutex);
				live.All.insert(link.get());
			}
			try
			{
				link->Thread = WorkerThread([link, launch = std::move(launch)]() mutable { Main(link, std::move(launch)); });
			}
			catch (const std::exception& e)
			{
				failed = e.what();
			}
			if (!failed.empty())
			{
				{
					std::lock_guard<std::mutex> lock(live.Mutex);
					live.All.erase(link.get());
				}
				{
					std::lock_guard<std::mutex> lock(link->Mutex);
					link->End = Ending::Terminated;
				}
				link->Joined = true;
				started.All.pop_back();
				luaL_error(L, "worker.start: cannot start a thread for '%s': %s", path.c_str(), failed.c_str());
			}
			return 1;
		}

		int WorkerSend(lua_State* L)
		{
			Link& link = *CheckWorker(L, 1);
			std::string bytes = EncodeMessage(L, 2, "worker:send");
			{
				std::lock_guard<std::mutex> lock(link.Mutex);
				// A worker that has ended (or is to stop) reads nothing more.
				if (link.End != Ending::Running || link.Stop != kRunning)
					return 0;
				link.ToWorker.push_back(std::move(bytes));
			}
			link.ToWorkerCv.notify_all();
			return 0;
		}

		int WorkerReceive(lua_State* L)
		{
			Link& link = *CheckWorker(L, 1);
			return Receive(L, link, true, Seconds(L, 2, "worker:receive"));
		}

		int WorkerJoin(lua_State* L)
		{
			std::shared_ptr<Link> link = CheckWorker(L, 1);
			if (Ended(*link))
				return Joined(L, link);
			auto dropped = std::make_shared<std::atomic<bool>>(false); // its task was cancelled: the wait ends
			const unsigned interrupts = InterruptCount();
			auto work = [link, dropped, interrupts]() -> JobResult {
				bool ended = false;
				{
					std::unique_lock<std::mutex> lock(link->Mutex);
					while (!(ended = link->End != Ending::Running) && !*dropped && !InterruptPending() && InterruptCount() == interrupts)
						link->ToParentCv.wait_for(lock, std::chrono::milliseconds(100));
				}
				return [link, ended](lua_State* L) -> int {
					if (!ended)
						RaiseStopped(L);
					return Joined(L, link);
				};
			};
			return Await(L, std::move(work), [link, dropped] {
				*dropped = true;
				{
					std::lock_guard<std::mutex> lock(link->Mutex);
				}
				link->ToParentCv.notify_all();
			});
		}

		int WorkerTerminate(lua_State* L)
		{
			Link& link = *CheckWorker(L, 1);
			if (!Ended(link))
				RequestStop(link, kTerminate);
			return 0;
		}

		// A Worker's fields: `done`, and its methods (the upvalue).
		int WorkerIndex(lua_State* L)
		{
			Link& link = *CheckWorker(L, 1);
			if (lua_type(L, 2) == LUA_TSTRING && std::strcmp(lua_tostring(L, 2), "done") == 0)
			{
				lua_pushboolean(L, Ended(link));
				return 1;
			}
			lua_pushvalue(L, 2);
			lua_rawget(L, lua_upvalueindex(1));
			return 1;
		}

		// ---- the module's functions in a worker ----------------------------

		Link& Self(lua_State* L, const char* fn)
		{
			if (!t_Self)
				luaL_error(L, "%s: not in a worker (in the script that started one, its Worker's methods talk to it)", fn);
			return *t_Self;
		}

		int ModuleSend(lua_State* L)
		{
			Link& self = Self(L, "worker.send");
			std::string bytes = EncodeMessage(L, 1, "worker.send");
			{
				std::lock_guard<std::mutex> lock(self.Mutex);
				if (self.HandleGone)
					return 0; // nothing can read it
				self.ToParent.push_back(std::move(bytes));
			}
			self.ToParentCv.notify_all();
			return 0;
		}

		int ModuleReceive(lua_State* L)
		{
			Link& self = Self(L, "worker.receive");
			return Receive(L, self, false, Seconds(L, 1, "worker.receive"));
		}

	} // namespace

	void OpenWorker(lua_State* L)
	{
		luaL_newmetatable(L, kWorkerMeta);
		lua_newtable(L);
		lua_pushcfunction(L, WorkerSend, "send");
		lua_setfield(L, -2, "send");
		lua_pushcclosurek(L, WorkerReceive, "receive", 0, AwaitContinue);
		lua_setfield(L, -2, "receive");
		lua_pushcclosurek(L, WorkerJoin, "join", 0, AwaitContinue);
		lua_setfield(L, -2, "join");
		lua_pushcfunction(L, WorkerTerminate, "terminate");
		lua_setfield(L, -2, "terminate");
		lua_pushcclosure(L, WorkerIndex, "__index", 1);
		lua_setfield(L, -2, "__index");
		lua_pushstring(L, "Worker");
		lua_setfield(L, -2, "__type");
		lua_pop(L, 1);

		lua_newtable(L);
		lua_pushcfunction(L, Start, "start");
		lua_setfield(L, -2, "start");
		lua_pushcfunction(L, ModuleSend, "send");
		lua_setfield(L, -2, "send");
		lua_pushcclosurek(L, ModuleReceive, "receive", 0, AwaitContinue);
		lua_setfield(L, -2, "receive");
		// A worker's runtime sets these before its script runs (Prepare).
		lua_pushboolean(L, 0);
		lua_setfield(L, -2, "isWorker");
		lua_newtable(L);
		lua_setfield(L, -2, "args");
	}

	bool HasWorkers(lua_State* L)
	{
		const Started* started = GetStarted(L, false);
		return started && (!started->All.empty() || started->Unjoined);
	}

	int FinishWorkers(lua_State* L)
	{
		Started* started = GetStarted(L, false);
		if (!started)
			return 0;
		for (const std::shared_ptr<Link>& link : started->All)
			CloseToWorker(*link);
		const unsigned interrupts = InterruptCount();
		for (size_t i = 0; i < started->All.size(); i++)
		{
			const std::shared_ptr<Link> link = started->All[i];
			for (;;)
			{
				{
					std::unique_lock<std::mutex> lock(link->Mutex);
					if (link->ToParentCv.wait_for(lock, std::chrono::milliseconds(100), [&] { return link->End != Ending::Running; }))
						break;
				}
				if (InterruptPending() || InterruptCount() != interrupts)
					RaiseStopped(L);
				DebugWaiting(L); // a debugger's requests are read meanwhile
			}
		}
		std::vector<std::shared_ptr<Link>> all = std::move(started->All);
		started->All.clear();
		std::shared_ptr<Link> failed = std::move(started->Unjoined);
		started->Unjoined.reset();
		for (const std::shared_ptr<Link>& link : all)
		{
			if (link->Thread.joinable())
				link->Thread.join();
			if (!failed && !link->Joined && Failing(*link))
				failed = link;
		}
		// A failure that nothing joined is not left unseen: its text is the
		// run's error (without this function's stack, which says nothing).
		if (!failed)
			return 0;
		failed->Joined = true;
		// A worker a Ctrl-C stopped: the script ends as a Ctrl-C ends it.
		if (failed->Message == kInterrupted && InterruptCount() != failed->Interrupts)
			RaiseStopped(L);
		const std::string text = FailureText(*failed);
		lua_pushlstring(L, text.data(), text.size());
		return 1;
	}

	void StopWorkers(lua_State* L)
	{
		if (Started* started = GetStarted(L, false))
			StopAll(*started);
	}

	bool InWorker() { return t_Self != nullptr; }

	const char* WorkerStopping()
	{
		if (!t_Self)
			return nullptr;
		switch (t_Self->Stop.load(std::memory_order_relaxed))
		{
		case kTerminate:
			return kTerminated;
		case kExit:
			return kExited;
		default:
			break;
		}
		return InterruptCount() != t_Self->Interrupts ? kInterrupted : nullptr;
	}

	void WorkerExit(lua_State* L, int status)
	{
		if (!t_Self)
			return;
		int running = kRunning;
		if (t_Self->Stop.compare_exchange_strong(running, kExit))
		{
			std::lock_guard<std::mutex> lock(t_Self->Mutex);
			t_Self->ExitStatus = status;
		}
		RaiseIfInterrupted(L);
		lua_pushstring(L, kExited); // not reached: the stop is pending
		lua_error(L);
	}

	void StopAllWorkers()
	{
		Live& live = Everyone();
		std::unique_lock<std::mutex> lock(live.Mutex);
		for (Link* link : live.All)
			RequestStop(*link, kTerminate);
		live.Ended.wait(lock, [&] { return live.All.empty(); });
	}

} // namespace cluau::runtime::detail
