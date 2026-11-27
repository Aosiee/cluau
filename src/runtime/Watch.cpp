// @std/watch: changes to files and directories, from each system's own
// notifications: inotify on Linux, FSEvents on macOS and
// ReadDirectoryChangesW on Windows.
//
// A watcher gathers what the system reports into a queue of its own, on a
// thread of its own (on macOS, on FSEvents' dispatch queue), so nothing is
// lost while the script is busy; `next` takes from that queue. A wait for
// the next change is a job (Jobs.h): in a task it waits on its own while
// the other tasks run.
//
// A file is watched through its directory, for its name: an editor that
// saves by writing another file and renaming it over the first, or by
// removing the file and writing it again, keeps being followed.

#include "Std.h"
#include "Jobs.h"

#include "lua.h"
#include "lualib.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <future>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <strings.h>
#include <sys/stat.h>
#elif defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace cluau::runtime::detail {

	namespace {

		constexpr const char* kWatcherMeta = "cluau.watcher";

		// The most changes a watcher holds for the script; past that, one
		// "overflow" stands for those it had to let go.
		constexpr size_t kMostQueued = 65536;

		enum class Kind { Created, Modified, Removed, Renamed, Overflow };

		const char* KindName(Kind kind)
		{
			switch (kind)
			{
			case Kind::Created: return "created";
			case Kind::Modified: return "modified";
			case Kind::Removed: return "removed";
			case Kind::Renamed: return "renamed";
			case Kind::Overflow: return "overflow";
			}
			return "modified";
		}

		// What the script gets: the changed path (the watched path joined
		// with the path below it) and, for a rename, its old path.
		struct Event
		{
			Kind What = Kind::Modified;
			std::string Path;
			std::string From;
		};

		// A change as a system reports it: paths relative to the directory
		// watched, with `/` between names.
		struct Change
		{
			Kind What = Kind::Modified;
			std::string Rel;
			std::string FromRel; // a rename's old path
			bool Root = false; // the directory watched has gone
		};

		// A name as a file system compares it: without case on Windows and
		// macOS (as their usual file systems do, for ASCII letters).
		bool SameName(const std::string& a, const std::string& b)
		{
#if defined(_WIN32) || defined(__APPLE__)
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
			{
				unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
				if (x >= 'A' && x <= 'Z')
					x = (unsigned char)(x - 'A' + 'a');
				if (y >= 'A' && y <= 'Z')
					y = (unsigned char)(y - 'A' + 'a');
				if (x != y)
					return false;
			}
			return true;
#else
			return a == b;
#endif
		}

		std::string Join(const std::string& base, const std::string& rel)
		{
			if (rel.empty())
				return base;
			if (base.empty())
				return rel;
			return base.back() == '/' ? base + rel : base + "/" + rel;
		}

		// A system message as the end of an error message: lowercase, with
		// no closing full stop (as @std/fs gives them).
		std::string Lowered(std::string message)
		{
			while (!message.empty() && (message.back() == '.' || message.back() == '\n' || message.back() == '\r' || message.back() == ' '))
				message.pop_back();
			if (message.size() > 1 && message[0] >= 'A' && message[0] <= 'Z' && !(message[1] >= 'A' && message[1] <= 'Z'))
				message[0] = (char)(message[0] - 'A' + 'a');
			return message;
		}

#if defined(__linux__) || defined(__APPLE__)
		std::string ErrnoMessage(int err) { return Lowered(std::generic_category().message(err)); }
#endif

		struct Watcher
		{
			std::string Path; // as given, with `/` and no `/` at its end: `path`, and the events for it
			std::string Base; // what the changed paths are joined to: the directory watched, as given
			std::string Only; // a file watcher's name in Base ("": a directory watcher)
			bool Recursive = true;
			fs::path Dir; // the directory watched, for the system

			std::mutex Lock;
			std::condition_variable Changed;
			std::deque<Event> Queue;
			// Each path's latest event in the queue (a deque's elements stay
			// where they are as others are added and taken at its ends).
			std::unordered_map<std::string, Event*> Latest;
			bool Closed = false; // by close() or the collector: nothing more is given
			bool Ended = false; // the directory watched went: what is queued is still given
			std::string Failure; // why the system stopped reporting, raised once the queue is empty

			std::mutex Stopping;
			bool Stopped = false;

#if defined(__linux__)
			int Fd = -1;
			int Wake[2] = { -1, -1 }; // a byte written to Wake[1] ends the thread
			int RootWd = -1;
			std::unordered_map<int, std::string> Dirs; // watch → directory (relative); the thread's own
			std::thread Thread;
#elif defined(__APPLE__)
			FSEventStreamRef Stream = nullptr;
			dispatch_queue_t DispatchQueue = nullptr;
			std::string Real; // the directory watched with its links resolved, as FSEvents names paths
#elif defined(_WIN32)
			HANDLE Handle = INVALID_HANDLE_VALUE;
			HANDLE StopEvent = nullptr;
			std::wstring WideDir;
			std::thread Thread;
#endif

			~Watcher() { Stop(); }

			// ---- the queue --------------------------------------------------

			// Adds the event, unless the path's latest unread event already
			// stands for it: a second change of the same kind, or a change to
			// a file whose creation is still unread. Called with Lock held.
			void PushLocked(Event event)
			{
				if (Closed)
					return;
				if (event.What != Kind::Overflow && event.What != Kind::Renamed)
				{
					auto latest = Latest.find(event.Path);
					if (latest != Latest.end())
					{
						const Kind last = latest->second->What;
						if (last == event.What || (last == Kind::Created && event.What == Kind::Modified))
							return;
					}
				}
				if (Queue.size() >= kMostQueued)
				{
					if (Queue.back().What != Kind::Overflow)
						Queue.push_back(Event { Kind::Overflow, Path, {} });
					return;
				}
				Queue.push_back(std::move(event));
				if (Queue.back().What != Kind::Overflow)
					Latest[Queue.back().Path] = &Queue.back();
			}

			// The first event in the queue (there is one). Called with Lock held.
			Event TakeLocked()
			{
				Event event = std::move(Queue.front());
				auto latest = Latest.find(event.Path);
				if (latest != Latest.end() && latest->second == &Queue.front())
					Latest.erase(latest);
				Queue.pop_front();
				return event;
			}

			// An event a cancelled wait took, back at the front for the next.
			void GiveBack(Event event)
			{
				{
					std::lock_guard<std::mutex> lock(Lock);
					if (Closed)
						return;
					Queue.push_front(std::move(event));
					if (Queue.front().What != Kind::Overflow && Latest.find(Queue.front().Path) == Latest.end())
						Latest[Queue.front().Path] = &Queue.front();
				}
				Changed.notify_all();
			}

			// The change as the script sees it: false when a file watcher
			// has nothing to say about it (another file in the directory).
			bool Accept(const Change& change, Event& event) const
			{
				event.What = change.What;
				if (change.Root || change.What == Kind::Overflow)
				{
					event.Path = Path;
					return true;
				}
				if (!Only.empty())
				{
					const bool is = SameName(change.Rel, Only);
					const bool was = change.What == Kind::Renamed && SameName(change.FromRel, Only);
					if (!is && !was)
						return false;
					event.Path = is ? Path : Join(Base, change.Rel);
					if (change.What == Kind::Renamed)
						event.From = was ? Path : Join(Base, change.FromRel);
					return true;
				}
				event.Path = Join(Base, change.Rel);
				if (change.What == Kind::Renamed)
					event.From = Join(Base, change.FromRel);
				return true;
			}

			void Deliver(const std::vector<Change>& batch)
			{
				if (batch.empty())
					return;
				{
					std::lock_guard<std::mutex> lock(Lock);
					for (const Change& change : batch)
					{
						Event event;
						if (Accept(change, event))
							PushLocked(std::move(event));
					}
				}
				Changed.notify_all();
			}

			// The directory watched has gone (its "removed" delivered first):
			// the system reports nothing more.
			void MarkEnded()
			{
				{
					std::lock_guard<std::mutex> lock(Lock);
					Ended = true;
				}
				Changed.notify_all();
			}

			void Fail(const std::string& why)
			{
				{
					std::lock_guard<std::mutex> lock(Lock);
					Failure = why;
					Ended = true;
				}
				Changed.notify_all();
			}

			void Close()
			{
				{
					std::lock_guard<std::mutex> lock(Lock);
					Closed = true;
					Latest.clear();
					Queue.clear();
				}
				Changed.notify_all();
				Stop();
			}

			// ---- the system's side ------------------------------------------

#if defined(__linux__)

			static constexpr uint32_t kMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
											  IN_MOVE_SELF | IN_EXCL_UNLINK;

			std::string OsPath(const std::string& rel) const { return rel.empty() ? Dir.string() : Dir.string() + "/" + rel; }

			static std::string WatchError(int err)
			{
				if (err == ENOSPC)
					return "the system's limit on watched directories is reached (fs.inotify.max_user_watches)";
				return ErrnoMessage(err);
			}

			// Watches the directories under `rel` (itself watched already),
			// those made later included. With `report`, each name found is
			// reported as created: they were made before their directory was
			// watched, so no event of the system's tells of them. A
			// directory that cannot be watched is passed over; the system's
			// limit is an error when `error` is given (at the start).
			void WatchTree(const std::string& rel, bool report, std::vector<Change>& batch, std::string* error)
			{
				std::vector<std::string> pending { rel };
				while (!pending.empty())
				{
					const std::string dir = std::move(pending.back());
					pending.pop_back();
					std::error_code ec;
					for (fs::directory_iterator it(OsPath(dir), ec), end; !ec && it != end; it.increment(ec))
					{
						const std::string name = it->path().filename().string();
						const std::string child = dir.empty() ? name : dir + "/" + name;
						if (report)
							batch.push_back(Change { Kind::Created, child, {}, false });
						std::error_code typeEc;
						if (it->symlink_status(typeEc).type() != fs::file_type::directory)
							continue;
						const int wd = inotify_add_watch(Fd, OsPath(child).c_str(), kMask | IN_ONLYDIR | IN_DONT_FOLLOW);
						if (wd >= 0)
						{
							Dirs[wd] = child;
							pending.push_back(child);
						}
						else if (errno == ENOSPC && error)
						{
							*error = WatchError(ENOSPC);
							return;
						}
					}
				}
			}

			// A directory moved within the tree: the watches under it name
			// their new place.
			void Moved(const std::string& from, const std::string& to)
			{
				for (auto& [wd, rel] : Dirs)
					if (rel == from || (rel.size() > from.size() && rel.compare(0, from.size(), from) == 0 && rel[from.size()] == '/'))
						rel = to + rel.substr(from.size());
			}

			// A directory moved out of the tree: nothing under it is watched.
			void Forget(const std::string& from)
			{
				for (auto it = Dirs.begin(); it != Dirs.end();)
				{
					const std::string& rel = it->second;
					if (it->first != RootWd &&
						(rel == from || (rel.size() > from.size() && rel.compare(0, from.size(), from) == 0 && rel[from.size()] == '/')))
					{
						inotify_rm_watch(Fd, it->first);
						it = Dirs.erase(it);
					}
					else
						++it;
				}
			}

			std::string Start()
			{
				Fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
				if (Fd < 0)
					return errno == EMFILE ? "the system's limit on watchers is reached (fs.inotify.max_user_instances)" : ErrnoMessage(errno);
				if (pipe2(Wake, O_CLOEXEC | O_NONBLOCK) != 0)
					return ErrnoMessage(errno);
				RootWd = inotify_add_watch(Fd, Dir.c_str(), kMask | IN_ONLYDIR);
				if (RootWd < 0)
					return WatchError(errno);
				Dirs[RootWd] = "";
				if (Recursive)
				{
					std::vector<Change> none;
					std::string error;
					WatchTree("", false, none, &error);
					if (!error.empty())
						return error;
				}
				Thread = std::thread([this] { Run(); });
				return {};
			}

			struct MovedFrom
			{
				uint32_t Cookie = 0;
				std::string Rel;
				bool IsDir = false;
			};

			// A rename's old name with no new one after it: moved out of the
			// tree, so removed from it.
			void Unpaired(std::optional<MovedFrom>& moved, std::vector<Change>& batch)
			{
				if (!moved)
					return;
				batch.push_back(Change { Kind::Removed, moved->Rel, {}, false });
				if (moved->IsDir && Recursive)
					Forget(moved->Rel);
				moved.reset();
			}

			// One of the system's events into the batch: true when the
			// directory watched has gone.
			bool Handle(const inotify_event& ev, std::optional<MovedFrom>& moved, std::vector<Change>& batch)
			{
				if (ev.mask & IN_Q_OVERFLOW)
				{
					Unpaired(moved, batch);
					batch.push_back(Change { Kind::Overflow, {}, {}, false });
					return false;
				}
				auto dir = Dirs.find(ev.wd);
				if (ev.mask & IN_IGNORED)
				{
					// The watch is gone (its directory removed, or moved out
					// and forgotten); the root's going ends the watcher.
					if (dir != Dirs.end())
					{
						const bool root = ev.wd == RootWd;
						Dirs.erase(dir);
						if (root)
						{
							Unpaired(moved, batch);
							batch.push_back(Change { Kind::Removed, {}, {}, true });
							return true;
						}
					}
					return false;
				}
				if (dir == Dirs.end())
					return false;
				const std::string rel = dir->second;
				if (ev.len == 0 || ev.name[0] == 0)
				{
					// The directory itself: only the root's removal (or its
					// move elsewhere) is news, the others' come from their parent.
					if (ev.wd == RootWd && (ev.mask & (IN_DELETE_SELF | IN_MOVE_SELF)))
					{
						Unpaired(moved, batch);
						batch.push_back(Change { Kind::Removed, {}, {}, true });
						return true;
					}
					return false;
				}
				const std::string path = rel.empty() ? std::string(ev.name) : rel + "/" + ev.name;
				const bool isDir = (ev.mask & IN_ISDIR) != 0;
				if (moved && !((ev.mask & IN_MOVED_TO) && ev.cookie == moved->Cookie))
					Unpaired(moved, batch);
				if (ev.mask & IN_MOVED_FROM)
				{
					moved = MovedFrom { ev.cookie, path, isDir };
				}
				else if (ev.mask & IN_MOVED_TO)
				{
					if (moved)
					{
						batch.push_back(Change { Kind::Renamed, path, moved->Rel, false });
						if (moved->IsDir && Recursive)
							Moved(moved->Rel, path);
						moved.reset();
					}
					else
					{
						batch.push_back(Change { Kind::Created, path, {}, false });
						if (isDir && Recursive)
							WatchNew(path, batch);
					}
				}
				else if (ev.mask & IN_CREATE)
				{
					batch.push_back(Change { Kind::Created, path, {}, false });
					if (isDir && Recursive)
						WatchNew(path, batch);
				}
				else if (ev.mask & IN_DELETE)
					batch.push_back(Change { Kind::Removed, path, {}, false });
				else if ((ev.mask & (IN_MODIFY | IN_ATTRIB)) && !isDir)
					batch.push_back(Change { Kind::Modified, path, {}, false });
				return false;
			}

			// A directory that has come into the tree: watched, with what it
			// already holds reported.
			void WatchNew(const std::string& rel, std::vector<Change>& batch)
			{
				const int wd = inotify_add_watch(Fd, OsPath(rel).c_str(), kMask | IN_ONLYDIR | IN_DONT_FOLLOW);
				if (wd < 0)
					return; // gone already, or past the system's limit
				Dirs[wd] = rel;
				WatchTree(rel, true, batch, nullptr);
			}

			void Run()
			{
				alignas(inotify_event) char buffer[64 * 1024];
				std::optional<MovedFrom> moved;
				for (;;)
				{
					pollfd fds[2] = { { Fd, POLLIN, 0 }, { Wake[0], POLLIN, 0 } };
					// A rename's old name waits briefly for its new one, which
					// the system may hand over in its next read.
					const int ready = poll(fds, 2, moved ? 10 : -1);
					if (ready < 0)
					{
						if (errno == EINTR)
							continue;
						Fail(ErrnoMessage(errno));
						return;
					}
					if (fds[1].revents)
						return;
					std::vector<Change> batch;
					if (ready == 0)
					{
						Unpaired(moved, batch);
						Deliver(batch);
						continue;
					}
					const ssize_t got = read(Fd, buffer, sizeof buffer);
					if (got < 0)
					{
						if (errno == EAGAIN || errno == EINTR)
							continue;
						Fail(ErrnoMessage(errno));
						return;
					}
					bool ended = false;
					for (const char* p = buffer; p < buffer + got && !ended;)
					{
						const inotify_event* ev = reinterpret_cast<const inotify_event*>(p);
						p += sizeof(inotify_event) + ev->len;
						ended = Handle(*ev, moved, batch);
					}
					Deliver(batch);
					if (ended)
					{
						MarkEnded();
						return;
					}
				}
			}

			void Stop()
			{
				std::lock_guard<std::mutex> lock(Stopping);
				if (Stopped)
					return;
				Stopped = true;
				if (Thread.joinable())
				{
					const char byte = 1;
					while (write(Wake[1], &byte, 1) < 0 && errno == EINTR)
					{
					}
					Thread.join();
				}
				for (int fd : { Fd, Wake[0], Wake[1] })
					if (fd >= 0)
						close(fd);
				Fd = Wake[0] = Wake[1] = -1;
			}

#elif defined(__APPLE__)

			// The path below the directory watched ("" for itself): false for
			// a path outside it.
			bool Relative(const std::string& full, std::string& rel) const
			{
				const std::string prefix = Real.back() == '/' ? Real : Real + "/";
				if (full.size() + 1 == prefix.size() && strncasecmp(full.c_str(), prefix.c_str(), full.size()) == 0)
				{
					rel.clear();
					return true;
				}
				if (full.size() > prefix.size() && strncasecmp(full.c_str(), prefix.c_str(), prefix.size()) == 0)
				{
					rel = full.substr(prefix.size());
					return true;
				}
				return false;
			}

			static bool Exists(const std::string& path)
			{
				struct stat st;
				return lstat(path.c_str(), &st) == 0;
			}

			static void Callback(ConstFSEventStreamRef, void* info, size_t count, void* paths, const FSEventStreamEventFlags flags[],
				const FSEventStreamEventId[])
			{
				static_cast<Watcher*>(info)->OnEvents(count, static_cast<char**>(paths), flags);
			}

			// FSEvents gives a path with the kinds of change it has seen since
			// it last reported it, merged into one set of flags: whether the
			// path is there now says which came last (a rename's old name is
			// gone, its new one there).
			void OnEvents(size_t count, char** paths, const FSEventStreamEventFlags flags[])
			{
				{
					std::lock_guard<std::mutex> lock(Lock);
					if (Closed || Ended)
						return;
				}
				constexpr FSEventStreamEventFlags kLost =
					kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped;
				constexpr FSEventStreamEventFlags kModified = kFSEventStreamEventFlagItemModified | kFSEventStreamEventFlagItemInodeMetaMod |
															  kFSEventStreamEventFlagItemFinderInfoMod | kFSEventStreamEventFlagItemChangeOwner |
															  kFSEventStreamEventFlagItemXattrMod;
				std::vector<Change> batch;
				std::optional<std::string> oldName; // a rename's old name, waiting for its new one
				auto unpaired = [&] {
					if (oldName)
						batch.push_back(Change { Kind::Removed, *oldName, {}, false });
					oldName.reset();
				};
				bool ended = false;
				for (size_t i = 0; i < count && !ended; i++)
				{
					const FSEventStreamEventFlags f = flags[i];
					const std::string full = paths[i];
					if (f & kLost)
					{
						unpaired();
						batch.push_back(Change { Kind::Overflow, {}, {}, false });
						continue;
					}
					if (f & kFSEventStreamEventFlagRootChanged)
					{
						if (!Exists(Real))
						{
							unpaired();
							batch.push_back(Change { Kind::Removed, {}, {}, true });
							ended = true;
						}
						continue;
					}
					std::string rel;
					if (!Relative(full, rel))
						continue;
					if (rel.empty())
					{
						// The directory itself: only its going is news.
						if ((f & (kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed)) && !Exists(Real))
						{
							unpaired();
							batch.push_back(Change { Kind::Removed, {}, {}, true });
							ended = true;
						}
						continue;
					}
					if (!Recursive && rel.find('/') != std::string::npos)
						continue;
					const bool exists = Exists(full);
					const bool created = (f & kFSEventStreamEventFlagItemCreated) != 0;
					const bool removed = (f & kFSEventStreamEventFlagItemRemoved) != 0;
					const bool modified = (f & kModified) != 0 && !(f & kFSEventStreamEventFlagItemIsDir);
					if (f & kFSEventStreamEventFlagItemRenamed)
					{
						if (!exists)
						{
							unpaired();
							if (created)
								batch.push_back(Change { Kind::Created, rel, {}, false });
							oldName = rel;
							continue;
						}
						if (oldName)
						{
							batch.push_back(Change { Kind::Renamed, rel, *oldName, false });
							oldName.reset();
						}
						else
							batch.push_back(Change { Kind::Created, rel, {}, false });
						if (modified)
							batch.push_back(Change { Kind::Modified, rel, {}, false });
						continue;
					}
					unpaired();
					if (removed && exists)
					{
						// Removed, then made again.
						batch.push_back(Change { Kind::Removed, rel, {}, false });
						batch.push_back(Change { Kind::Created, rel, {}, false });
						if (modified)
							batch.push_back(Change { Kind::Modified, rel, {}, false });
						continue;
					}
					if (created)
						batch.push_back(Change { Kind::Created, rel, {}, false });
					if (modified && exists)
						batch.push_back(Change { Kind::Modified, rel, {}, false });
					if (removed)
						batch.push_back(Change { Kind::Removed, rel, {}, false });
				}
				unpaired();
				Deliver(batch);
				if (ended)
					MarkEnded();
			}

			std::string Start()
			{
				char real[PATH_MAX];
				if (!realpath(Dir.c_str(), real))
					return ErrnoMessage(errno);
				Real = real;
				CFStringRef path = CFStringCreateWithCString(nullptr, Real.c_str(), kCFStringEncodingUTF8);
				if (!path)
					return "the path is not UTF-8";
				const void* values[] = { path };
				CFArrayRef paths = CFArrayCreate(nullptr, values, 1, &kCFTypeArrayCallBacks);
				CFRelease(path);
				if (!paths)
					return "not enough memory";
				FSEventStreamContext context = { 0, this, nullptr, nullptr, nullptr };
				Stream = FSEventStreamCreate(nullptr, &Watcher::Callback, &context, paths, kFSEventStreamEventIdSinceNow, 0.05,
					kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot);
				CFRelease(paths);
				if (!Stream)
					return "FSEvents cannot watch it";
				DispatchQueue = dispatch_queue_create("org.cluau.watch", DISPATCH_QUEUE_SERIAL);
				FSEventStreamSetDispatchQueue(Stream, DispatchQueue);
				if (!FSEventStreamStart(Stream))
				{
					FSEventStreamInvalidate(Stream);
					FSEventStreamRelease(Stream);
					Stream = nullptr;
					return "FSEvents cannot watch it";
				}
				return {};
			}

			void Stop()
			{
				std::lock_guard<std::mutex> lock(Stopping);
				if (Stopped)
					return;
				Stopped = true;
				if (Stream)
				{
					FSEventStreamStop(Stream);
					FSEventStreamInvalidate(Stream);
					// A callback under way on the queue finishes before the
					// stream (and this watcher) can go.
					dispatch_sync_f(DispatchQueue, nullptr, [](void*) {});
					FSEventStreamRelease(Stream);
					Stream = nullptr;
				}
				if (DispatchQueue)
				{
					dispatch_release(DispatchQueue);
					DispatchQueue = nullptr;
				}
			}

#elif defined(_WIN32)

			static std::string Utf8(const wchar_t* text, size_t length)
			{
				if (length == 0)
					return {};
				const int size = WideCharToMultiByte(CP_UTF8, 0, text, (int)length, nullptr, 0, nullptr, nullptr);
				std::string out(size > 0 ? (size_t)size : 0, '\0');
				if (size > 0)
					WideCharToMultiByte(CP_UTF8, 0, text, (int)length, out.data(), size, nullptr, nullptr);
				return out;
			}

			static std::string SystemMessage(DWORD error) { return Lowered(std::system_category().message((int)error)); }

			bool RootExists() const { return GetFileAttributesW(WideDir.c_str()) != INVALID_FILE_ATTRIBUTES; }

			bool IsDirectory(const wchar_t* name, size_t length) const
			{
				const std::wstring full = WideDir + L"\\" + std::wstring(name, length);
				const DWORD attributes = GetFileAttributesW(full.c_str());
				return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
			}

			// The system's records into the batch; a rename's old and new
			// names come one after the other.
			void Parse(const char* data, DWORD size, std::vector<Change>& batch) const
			{
				std::optional<std::string> oldName;
				auto unpaired = [&] {
					if (oldName)
						batch.push_back(Change { Kind::Removed, *oldName, {}, false });
					oldName.reset();
				};
				constexpr size_t kHead = offsetof(FILE_NOTIFY_INFORMATION, FileName);
				for (size_t offset = 0; offset + kHead <= size;)
				{
					const FILE_NOTIFY_INFORMATION* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(data + offset);
					if (offset + kHead + info->FileNameLength > size)
						break;
					const size_t length = info->FileNameLength / sizeof(WCHAR);
					std::string rel = Utf8(info->FileName, length);
					for (char& c : rel)
						if (c == '\\')
							c = '/';
					switch (info->Action)
					{
					case FILE_ACTION_ADDED:
						unpaired();
						batch.push_back(Change { Kind::Created, rel, {}, false });
						break;
					case FILE_ACTION_REMOVED:
						unpaired();
						batch.push_back(Change { Kind::Removed, rel, {}, false });
						break;
					case FILE_ACTION_MODIFIED:
						// A directory's own times change with what it holds:
						// only files are reported as modified.
						unpaired();
						if (!IsDirectory(info->FileName, length))
							batch.push_back(Change { Kind::Modified, rel, {}, false });
						break;
					case FILE_ACTION_RENAMED_OLD_NAME:
						unpaired();
						oldName = rel;
						break;
					case FILE_ACTION_RENAMED_NEW_NAME:
						if (oldName)
						{
							batch.push_back(Change { Kind::Renamed, rel, *oldName, false });
							oldName.reset();
						}
						else
							batch.push_back(Change { Kind::Created, rel, {}, false });
						break;
					default: break;
					}
					if (info->NextEntryOffset == 0)
						break;
					offset += info->NextEntryOffset;
				}
				unpaired();
			}

			// The wait for changes failed: the directory has gone, or the
			// system gave up on it.
			void ReadFailed(DWORD error)
			{
				if (!RootExists())
				{
					Deliver({ Change { Kind::Removed, {}, {}, true } });
					MarkEnded();
				}
				else
					Fail(SystemMessage(error));
			}

			void Run(std::promise<DWORD>& started)
			{
				// DWORD-aligned, as the system writes it; 64 KiB is the most a
				// directory on a network share takes.
				constexpr DWORD kBytes = 64 * 1024;
				std::vector<DWORD> buffer(kBytes / sizeof(DWORD));
				std::vector<char> copy;
				OVERLAPPED overlapped = {};
				overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if (!overlapped.hEvent)
				{
					started.set_value(GetLastError());
					return;
				}
				const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
									 FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
				auto issue = [&]() -> bool {
					ResetEvent(overlapped.hEvent);
					return ReadDirectoryChangesW(Handle, buffer.data(), kBytes, Recursive ? TRUE : FALSE, filter, nullptr, &overlapped, nullptr) != 0;
				};
				// The system keeps changes for the handle only from its first
				// read on: watch.new returns once that read is under way.
				if (!issue())
				{
					started.set_value(GetLastError());
					CloseHandle(overlapped.hEvent);
					CloseHandle(Handle);
					Handle = INVALID_HANDLE_VALUE;
					return;
				}
				started.set_value(0);
				for (;;)
				{
					HANDLE waits[2] = { overlapped.hEvent, StopEvent };
					// The read need not end when the directory itself is
					// removed (it is only marked for deletion while the handle
					// is open), so the directory is looked for now and then.
					const DWORD which = WaitForMultipleObjects(2, waits, FALSE, 500);
					const bool gone = which == WAIT_TIMEOUT && !RootExists();
					if (which != WAIT_OBJECT_0)
					{
						if (which == WAIT_TIMEOUT && !gone)
							continue;
						DWORD ignored = 0;
						CancelIoEx(Handle, &overlapped);
						GetOverlappedResult(Handle, &overlapped, &ignored, TRUE);
						if (gone)
							ReadFailed(ERROR_FILE_NOT_FOUND);
						break;
					}
					DWORD bytes = 0;
					if (!GetOverlappedResult(Handle, &overlapped, &bytes, FALSE))
					{
						const DWORD error = GetLastError();
						if (error == ERROR_NOTIFY_ENUM_DIR)
						{
							// More changes than the buffer holds.
							Deliver({ Change { Kind::Overflow, {}, {}, false } });
							if (issue())
								continue;
							ReadFailed(GetLastError());
							break;
						}
						ReadFailed(error);
						break;
					}
					if (bytes == 0)
					{
						if (!RootExists())
						{
							ReadFailed(ERROR_FILE_NOT_FOUND);
							break;
						}
						Deliver({ Change { Kind::Overflow, {}, {}, false } });
						if (issue())
							continue;
						ReadFailed(GetLastError());
						break;
					}
					// The next read is made before this one's records are read,
					// so that fewer changes come while none is under way.
					const char* bytesRead = reinterpret_cast<const char*>(buffer.data());
					copy.assign(bytesRead, bytesRead + bytes);
					const bool again = issue();
					const DWORD againError = again ? 0 : GetLastError();
					std::vector<Change> batch;
					Parse(copy.data(), bytes, batch);
					Deliver(batch);
					if (!again)
					{
						ReadFailed(againError);
						break;
					}
				}
				CloseHandle(overlapped.hEvent);
				// Closed now, not at close(): a directory removed is gone only
				// once no handle holds it.
				CloseHandle(Handle);
				Handle = INVALID_HANDLE_VALUE;
			}

			std::string Start()
			{
				WideDir = Dir.wstring();
				Handle = CreateFileW(WideDir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
					OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
				if (Handle == INVALID_HANDLE_VALUE)
					return SystemMessage(GetLastError());
				StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
				if (!StopEvent)
					return SystemMessage(GetLastError());
				std::promise<DWORD> started;
				std::future<DWORD> result = started.get_future();
				Thread = std::thread([this, started = std::move(started)]() mutable { Run(started); });
				const DWORD error = result.get();
				if (error != 0)
				{
					Thread.join();
					return SystemMessage(error);
				}
				return {};
			}

			void Stop()
			{
				std::lock_guard<std::mutex> lock(Stopping);
				if (Stopped)
					return;
				Stopped = true;
				if (Thread.joinable())
				{
					SetEvent(StopEvent);
					Thread.join();
				}
				if (Handle != INVALID_HANDLE_VALUE)
					CloseHandle(Handle);
				if (StopEvent)
					CloseHandle(StopEvent);
				Handle = INVALID_HANDLE_VALUE;
				StopEvent = nullptr;
			}

#else

			std::string Start() { return "this system has no file watching cluau knows"; }
			void Stop() {}

#endif
		};

		// ---- the script's side ----------------------------------------------

		struct WatcherBox
		{
			std::shared_ptr<Watcher>* Shared = nullptr;
		};

		std::shared_ptr<Watcher> Check(lua_State* L, int index)
		{
			WatcherBox* box = static_cast<WatcherBox*>(luaL_checkudata(L, index, kWatcherMeta));
			return *box->Shared;
		}

		void PushEvent(lua_State* L, const Event& event)
		{
			lua_createtable(L, 0, 3);
			lua_pushlstring(L, event.Path.data(), event.Path.size());
			lua_setfield(L, -2, "path");
			lua_pushstring(L, KindName(event.What));
			lua_setfield(L, -2, "kind");
			if (event.What == Kind::Renamed)
			{
				lua_pushlstring(L, event.From.data(), event.From.size());
				lua_setfield(L, -2, "from");
			}
		}

		int WatchNew(lua_State* L)
		{
			size_t length = 0;
			const char* text = luaL_checklstring(L, 1, &length);
			if (std::memchr(text, 0, length))
				luaL_argerror(L, 1, "the path holds a NUL byte");
			bool recursive = true;
			if (!lua_isnoneornil(L, 2))
			{
				luaL_checktype(L, 2, LUA_TTABLE);
				lua_getfield(L, 2, "recursive");
				if (lua_isboolean(L, -1))
					recursive = lua_toboolean(L, -1) != 0;
				else if (!lua_isnil(L, -1))
					luaL_error(L, "watch.new: options.recursive is a boolean");
				lua_pop(L, 1);
			}
			const fs::path given = fs::u8path(text, text + length);
			const auto generic = given.generic_u8string();
			std::string path(generic.begin(), generic.end());
			// No `/` at the end (a root's aside), so that changed paths join
			// to it as @std/path joins them.
			while (path.size() > 1 && path.back() == '/' && !(path.size() == 3 && path[1] == ':'))
				path.pop_back();

			std::error_code ec;
			const fs::file_status status = fs::status(given, ec);
			if (status.type() == fs::file_type::not_found || path.empty())
				luaL_error(L, "watch.new: cannot watch '%s': no such file or directory", path.c_str());
			if (ec)
				luaL_error(L, "watch.new: cannot watch '%s': %s", path.c_str(), Lowered(ec.message()).c_str());
			auto watcher = std::make_shared<Watcher>();
			watcher->Path = path;
			if (fs::is_directory(status))
			{
				watcher->Base = path;
				watcher->Dir = fs::u8path(path);
				watcher->Recursive = recursive;
			}
			else
			{
				const fs::path p = fs::u8path(path);
				const auto name = p.filename().u8string();
				const auto parent = p.parent_path().generic_u8string();
				watcher->Only = std::string(name.begin(), name.end());
				watcher->Base = std::string(parent.begin(), parent.end());
				watcher->Dir = watcher->Base.empty() ? fs::path(".") : fs::u8path(watcher->Base);
				watcher->Recursive = false;
			}
			std::string why;
			try
			{
				why = watcher->Start();
			}
			catch (const std::exception& e)
			{
				why = e.what();
			}
			if (!why.empty())
			{
				watcher->Stop();
				luaL_error(L, "watch.new: cannot watch '%s': %s", path.c_str(), why.c_str());
			}

			WatcherBox* box = static_cast<WatcherBox*>(lua_newuserdatadtor(L, sizeof(WatcherBox), [](lua_State*, void* p) {
				WatcherBox* box = static_cast<WatcherBox*>(p);
				if (box->Shared)
				{
					(*box->Shared)->Close();
					delete box->Shared;
				}
			}));
			box->Shared = nullptr;
			box->Shared = new std::shared_ptr<Watcher>(std::move(watcher));
			luaL_getmetatable(L, kWatcherMeta);
			lua_setmetatable(L, -2);
			return 1;
		}

		// What a wait got: the event, or why it has none.
		struct Waited
		{
			std::mutex Lock;
			std::atomic<bool> Dropped { false }; // its task was cancelled
			bool Done = false;
			std::optional<Event> Got;
		};

		// The next change: one queued now is returned at once; otherwise
		// the wait is a job, of `seconds` at most (HUGE_VAL: no limit).
		int Next(lua_State* L, std::shared_ptr<Watcher> watcher, double seconds, const char* fn)
		{
			{
				std::lock_guard<std::mutex> lock(watcher->Lock);
				if (!watcher->Closed && !watcher->Queue.empty())
				{
					const Event event = watcher->TakeLocked();
					PushEvent(L, event);
					return 1;
				}
				if (watcher->Closed || watcher->Ended)
				{
					if (!watcher->Closed && !watcher->Failure.empty())
						luaL_error(L, "%s: %s", fn, watcher->Failure.c_str());
					lua_pushnil(L);
					return 1;
				}
			}
			if (!(seconds > 0))
			{
				lua_pushnil(L);
				return 1;
			}
			const unsigned interrupts = InterruptCount();
			auto waited = std::make_shared<Waited>();
			auto unclaimed = [watcher, waited] {
				std::optional<Event> event;
				{
					std::lock_guard<std::mutex> lock(waited->Lock);
					waited->Dropped = true;
					if (waited->Done)
						event = std::move(waited->Got);
				}
				if (event)
					watcher->GiveBack(std::move(*event));
				else
					watcher->Changed.notify_all(); // the wait ends now
			};
			auto work = [watcher, waited, seconds, interrupts, fn]() -> JobResult {
				using Clock = std::chrono::steady_clock;
				const bool forever = !(seconds < 1e9);
				const Clock::time_point until =
					forever ? Clock::time_point::max() : Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
				std::optional<Event> event;
				std::string failure;
				bool interrupted = false;
				{
					std::unique_lock<std::mutex> lock(watcher->Lock);
					for (;;)
					{
						if (watcher->Closed)
							break;
						if (!watcher->Queue.empty())
						{
							event = watcher->TakeLocked();
							break;
						}
						if (watcher->Ended)
						{
							failure = watcher->Failure;
							break;
						}
						if (waited->Dropped)
							break;
						if (InterruptPending() || InterruptCount() != interrupts)
						{
							interrupted = true;
							break;
						}
						const Clock::time_point now = Clock::now();
						if (now >= until)
							break;
						// In steps, so that a Ctrl-C is seen.
						Clock::time_point step = now + std::chrono::milliseconds(100);
						if (until < step)
							step = until;
						watcher->Changed.wait_until(lock, step);
					}
				}
				{
					std::lock_guard<std::mutex> lock(waited->Lock);
					if (waited->Dropped)
					{
						if (event)
							watcher->GiveBack(std::move(*event));
						return [](lua_State*) { return 0; }; // never taken
					}
					waited->Done = true;
					waited->Got = event;
				}
				if (interrupted)
					return [](lua_State* L) {
						RaiseIfInterrupted(L);
						lua_pushnil(L);
						return 1;
					};
				if (!failure.empty())
					return [failure, fn](lua_State* L) -> int {
						luaL_error(L, "%s: %s", fn, failure.c_str());
						return 0;
					};
				return [event = std::move(event)](lua_State* L) {
					if (event)
						PushEvent(L, *event);
					else
						lua_pushnil(L);
					return 1;
				};
			};
			return Await(L, std::move(work), std::move(unclaimed));
		}

		int WatcherNext(lua_State* L)
		{
			std::shared_ptr<Watcher> watcher = Check(L, 1);
			double seconds = HUGE_VAL;
			if (!lua_isnoneornil(L, 2))
			{
				seconds = luaL_checknumber(L, 2);
				if (seconds != seconds)
					luaL_error(L, "watcher:next: the time is NaN, not a number of seconds");
			}
			return Next(L, std::move(watcher), seconds, "watcher:next");
		}

		int WatcherEventsNext(lua_State* L) { return Next(L, Check(L, lua_upvalueindex(1)), HUGE_VAL, "watcher:events"); }

		int WatcherEvents(lua_State* L)
		{
			Check(L, 1);
			lua_pushvalue(L, 1);
			lua_pushcclosurek(L, WatcherEventsNext, "events", 1, AwaitContinue);
			return 1;
		}

		int WatcherClose(lua_State* L)
		{
			Check(L, 1)->Close();
			return 0;
		}

		int WatcherIndex(lua_State* L)
		{
			std::shared_ptr<Watcher> watcher = Check(L, 1);
			const char* key = luaL_checkstring(L, 2);
			if (std::strcmp(key, "path") == 0)
				lua_pushlstring(L, watcher->Path.data(), watcher->Path.size());
			else if (std::strcmp(key, "closed") == 0)
			{
				std::lock_guard<std::mutex> lock(watcher->Lock);
				lua_pushboolean(L, watcher->Closed || watcher->Ended);
			}
			else
			{
				lua_getmetatable(L, 1);
				lua_getfield(L, -1, "methods");
				lua_getfield(L, -1, key);
			}
			return 1;
		}

	} // namespace

	void OpenWatch(lua_State* L)
	{
		luaL_newmetatable(L, kWatcherMeta);
		lua_pushstring(L, "Watcher");
		lua_setfield(L, -2, "__type");
		lua_pushcfunction(L, WatcherIndex, "index");
		lua_setfield(L, -2, "__index");
		lua_newtable(L);
		lua_pushcclosurek(L, WatcherNext, "next", 0, AwaitContinue);
		lua_setfield(L, -2, "next");
		lua_pushcfunction(L, WatcherEvents, "events");
		lua_setfield(L, -2, "events");
		lua_pushcfunction(L, WatcherClose, "close");
		lua_setfield(L, -2, "close");
		lua_setfield(L, -2, "methods");
		lua_pop(L, 1);

		lua_newtable(L);
		lua_pushcfunction(L, WatchNew, "new");
		lua_setfield(L, -2, "new");
	}

} // namespace cluau::runtime::detail
