// @std/http: HTTP and HTTPS requests through libcurl (over mbedTLS).
//
// mbedTLS has no access to a system's certificate store, so the trusted
// certificates come from here: SSL_CERT_FILE / SSL_CERT_DIR when set, else
// Windows' own root store (exported for curl), macOS's /etc/ssl/cert.pem, or
// the first of the usual bundles a Linux distribution keeps.

#include "Std.h"
#include "Jobs.h"
#include "Tls.h"

#include "cluau/cluau.h"
#include "lua.h"
#include "lualib.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace fs = std::filesystem;

namespace cluau::runtime::detail {

	namespace {

		// ---- a request ------------------------------------------------------

		void InitCurl()
		{
			StartMbedtls();
			static const bool done = [] {
				curl_global_init(CURL_GLOBAL_DEFAULT);
				return true;
			}();
			(void)done;
		}

		// The statuses a redirect is followed for.
		bool IsRedirect(long status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

		struct Transfer
		{
			std::string Body;
			FILE* File = nullptr; // download: the body goes here
			std::map<std::string, std::string> Headers;
			bool WriteFailed = false;
			bool Follow = false; // redirects are followed
			long Status = 0;     // the status line's, as it arrives
			bool Skip = false;   // the body of a redirect that is followed
		};

		size_t OnBody(char* data, size_t size, size_t count, void* user)
		{
			Transfer& t = *static_cast<Transfer*>(user);
			const size_t n = size * count;
			if (t.Skip)
				return n;
			if (t.File)
			{
				if (std::fwrite(data, 1, n, t.File) != n)
				{
					t.WriteFailed = true;
					return 0;
				}
				return n;
			}
			t.Body.append(data, n);
			return n;
		}

		std::string Trim(std::string s)
		{
			while (!s.empty() && std::isspace((unsigned char)s.back()))
				s.pop_back();
			size_t start = 0;
			while (start < s.size() && std::isspace((unsigned char)s[start]))
				start++;
			return s.substr(start);
		}

		size_t OnHeader(char* data, size_t size, size_t count, void* user)
		{
			Transfer& t = *static_cast<Transfer*>(user);
			const size_t n = size * count;
			const std::string line(data, n);
			// A new status line: the response after a 1xx starts afresh.
			if (line.rfind("HTTP/", 0) == 0)
			{
				t.Headers.clear();
				const size_t space = line.find(' ');
				t.Status = space == std::string::npos ? 0 : std::strtol(line.c_str() + space + 1, nullptr, 10);
				return n;
			}
			// The end of the head: a redirect to follow has a body no one reads.
			if (line == "\r\n" || line == "\n")
			{
				t.Skip = t.Follow && IsRedirect(t.Status) && t.Headers.count("location") > 0;
				return n;
			}
			const size_t colon = line.find(':');
			if (colon == std::string::npos)
				return n;
			std::string name = line.substr(0, colon);
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			const std::string value = Trim(line.substr(colon + 1));
			std::string& slot = t.Headers[name];
			slot = slot.empty() ? value : slot + ", " + value;
			return n;
		}

		// Ctrl-C stops a transfer (the script then raises `interrupted`).
		int OnProgress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return InterruptPending() ? 1 : 0; }

		struct Request
		{
			std::string Method = "GET";
			std::string Url;
			std::vector<std::pair<std::string, std::string>> Headers; // the script's, and Content-Type for json
			std::optional<std::string> Body;
			double Timeout = 0;
			bool FollowRedirects = true;
			std::string CaFile;
		};

		// A header name or a method: an HTTP token (RFC 9110 5.6.2).
		bool IsToken(const std::string& text)
		{
			if (text.empty())
				return false;
			for (unsigned char c : text)
				if (!std::isalnum(c) && !std::strchr("!#$%&'*+-.^_`|~", c))
					return false;
			return true;
		}

		// The URL as an error message shows it: without a user name and
		// password.
		std::string Shown(const std::string& url)
		{
			const size_t scheme = url.find("://");
			if (scheme == std::string::npos)
				return url;
			const size_t start = scheme + 3;
			const size_t end = (std::min)(url.find_first_of("/?#", start), url.size());
			const size_t at = url.rfind('@', end - 1);
			if (at == std::string::npos || at < start || end == start)
				return url;
			return url.substr(0, start) + url.substr(at + 1);
		}

		// A URL is text with no NUL in it (curl would read it only up to one).
		void CheckUrl(lua_State* L, const char* url, size_t length, const char* fn)
		{
			if (std::strlen(url) != length)
				luaL_error(L, "%s: the address has a NUL byte in it", fn);
		}

		std::string StringField(lua_State* L, int table, const char* name, const char* fn)
		{
			lua_getfield(L, table, name);
			std::string out;
			if (!lua_isnil(L, -1))
			{
				if (!lua_isstring(L, -1))
					luaL_error(L, "%s: options.%s is a string", fn, name);
				size_t len = 0;
				const char* s = lua_tolstring(L, -1, &len);
				out.assign(s, len);
			}
			lua_pop(L, 1);
			return out;
		}

		// The options table at `index` into `request`.
		void ReadOptions(lua_State* L, int index, const char* fn, Request& request)
		{
			if (lua_isnoneornil(L, index))
				return;
			luaL_checktype(L, index, LUA_TTABLE);
			if (std::string method = StringField(L, index, "method", fn); !method.empty())
			{
				if (!IsToken(method))
					luaL_error(L, "%s: options.method '%s' is not a method name", fn, method.c_str());
				std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) { return (char)std::toupper(c); });
				request.Method = method;
			}
			request.CaFile = StringField(L, index, "caFile", fn);
			bool contentType = false;
			lua_getfield(L, index, "headers");
			if (lua_istable(L, -1))
			{
				lua_pushnil(L);
				while (lua_next(L, -2))
				{
					if (lua_type(L, -2) != LUA_TSTRING || !lua_isstring(L, -1))
						luaL_error(L, "%s: options.headers maps names to text", fn);
					size_t nameLength = 0, valueLength = 0;
					const char* nameText = lua_tolstring(L, -2, &nameLength);
					const std::string name(nameText, nameLength);
					if (!IsToken(name))
						luaL_error(L, "%s: options.headers: '%s' is not a header name", fn, name.c_str());
					lua_pushvalue(L, -1); // lua_tolstring would turn a number into text in place
					const char* valueText = lua_tolstring(L, -1, &valueLength);
					const std::string value(valueText, valueLength);
					lua_pop(L, 1);
					if (value.find_first_of(std::string("\r\n\0", 3)) != std::string::npos)
						luaL_error(L, "%s: options.headers: the %s header's value has a line break or a NUL in it", fn, name.c_str());
					std::string lower = name;
					std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
					contentType = contentType || lower == "content-type";
					request.Headers.emplace_back(name, value);
					lua_pop(L, 1);
				}
			}
			else if (!lua_isnil(L, -1))
				luaL_error(L, "%s: options.headers is a table ({ [\"Name\"] = \"value\" })", fn);
			lua_pop(L, 1);
			lua_getfield(L, index, "body");
			if (!lua_isnil(L, -1))
			{
				size_t len = 0;
				const char* body = lua_tolstring(L, -1, &len);
				if (!body)
					luaL_error(L, "%s: options.body is text", fn);
				request.Body = std::string(body, len);
			}
			lua_pop(L, 1);
			lua_getfield(L, index, "json");
			if (!lua_isnil(L, -1))
			{
				if (request.Body)
					luaL_error(L, "%s: options.body and options.json are each a body: give one", fn);
				request.Body = JsonEncodeValue(L, lua_gettop(L));
				if (!contentType)
					request.Headers.emplace_back("Content-Type", "application/json");
			}
			lua_pop(L, 1);
			lua_getfield(L, index, "timeout");
			if (!lua_isnil(L, -1))
			{
				if (!lua_isnumber(L, -1) || !(lua_tonumber(L, -1) > 0))
					luaL_error(L, "%s: options.timeout is a number of seconds", fn);
				request.Timeout = lua_tonumber(L, -1);
			}
			lua_pop(L, 1);
			lua_getfield(L, index, "followRedirects");
			if (!lua_isnil(L, -1))
				request.FollowRedirects = lua_toboolean(L, -1);
			lua_pop(L, 1);
		}

		std::string Lower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
			return text;
		}

		bool HasHeader(const Request& request, const std::string& lowerName)
		{
			for (const auto& [name, value] : request.Headers)
				if (Lower(name) == lowerName)
					return true;
			return false;
		}

		// The scheme, host and port of a URL ("" when it does not parse).
		std::string Origin(const std::string& url)
		{
			std::string origin;
			CURLU* parsed = curl_url();
			if (parsed && curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK)
			{
				char *scheme = nullptr, *host = nullptr, *port = nullptr;
				curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0);
				curl_url_get(parsed, CURLUPART_HOST, &host, 0);
				curl_url_get(parsed, CURLUPART_PORT, &port, CURLU_DEFAULT_PORT);
				if (scheme && host)
					origin = Lower(scheme) + "://" + Lower(host) + ":" + (port ? port : "");
				curl_free(scheme);
				curl_free(host);
				curl_free(port);
			}
			curl_url_cleanup(parsed);
			return origin;
		}

		// A URL without its user name and password.
		std::string WithoutCredentials(const std::string& url)
		{
			std::string out = url;
			CURLU* parsed = curl_url();
			if (parsed && curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK)
			{
				curl_url_set(parsed, CURLUPART_USER, nullptr, 0);
				curl_url_set(parsed, CURLUPART_PASSWORD, nullptr, 0);
				char* text = nullptr;
				if (curl_url_get(parsed, CURLUPART_URL, &text, 0) == CURLUE_OK && text)
					out = text;
				curl_free(text);
			}
			curl_url_cleanup(parsed);
			return out;
		}

		// The request a redirect (`status`, to `next`) makes of `request`:
		// a 303 is followed with GET (but HEAD), a 301 or 302 after a POST
		// too, without the body; 307 and 308 keep the method and the body.
		// Another host (or scheme, or port) gets none of the script's own
		// headers but those that say what is sent and wanted, and no user
		// name or password: they may be credentials meant for the first.
		void Redirect(Request& request, long status, const std::string& next)
		{
			if ((status == 303 && request.Method != "HEAD") || ((status == 301 || status == 302) && request.Method == "POST"))
			{
				request.Method = "GET";
				request.Body.reset();
				request.Headers.erase(std::remove_if(request.Headers.begin(), request.Headers.end(),
										  [](const auto& h) {
											  const std::string name = Lower(h.first);
											  return name == "content-type" || name == "content-length";
										  }),
					request.Headers.end());
			}
			if (Origin(next) != Origin(request.Url))
			{
				static const char* kKept[] = { "accept", "accept-language", "content-type", "user-agent" };
				request.Headers.erase(std::remove_if(request.Headers.begin(), request.Headers.end(),
										  [](const auto& h) {
											  const std::string name = Lower(h.first);
											  return std::none_of(std::begin(kKept), std::end(kKept), [&](const char* kept) { return name == kept; });
										  }),
					request.Headers.end());
				request.Url = WithoutCredentials(next);
			}
			else
				request.Url = next;
		}

		// A finished transfer: what the script's thread turns into the
		// response table, or the error.
		struct Outcome
		{
			CURLcode Code = CURLE_OK;
			long Status = 0;
			std::string Url;
			std::string ErrorText;
			struct Transfer Transfer;
			std::string Failure; // set when the request could not even start
		};

		// Sets `curl` up for one request (one step of a redirect chain).
		curl_slist* Configure(CURL* curl, const Request& request, Transfer& transfer, char* errorText, curl_blob& pem)
		{
			curl_easy_reset(curl);
			curl_slist* headers = nullptr;
			for (const auto& [name, value] : request.Headers)
				// "Name;" is curl's way to send a header with an empty value.
				headers = curl_slist_append(headers, (value.empty() ? name + ";" : name + ": " + value).c_str());

			static const std::string agent = std::string("cluau/") + Version();
			curl_easy_setopt(curl, CURLOPT_URL, request.Url.c_str());
			curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
			curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorText);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnBody);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
			curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, OnHeader);
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
			// Redirects are followed here (Run), not by curl, which would keep
			// a custom method after a 303 and the script's headers on another host.
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
			curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

			// A body only when there is one (POST always has one, maybe empty),
			// with the content type the script gives (curl's form default off).
			if (request.Method == "HEAD")
				curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
			else if (request.Body || request.Method == "POST")
			{
				const std::string& body = request.Body ? *request.Body : std::string();
				curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
				curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, body.c_str());
				if (request.Method != "POST")
					curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.Method.c_str());
				if (!HasHeader(request, "content-type"))
					headers = curl_slist_append(headers, "Content-Type:");
			}
			else if (request.Method != "GET")
				curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.Method.c_str());
			if (headers)
				curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

			// The certificates the server's must chain to.
			const Certificates& system = SystemCertificates();
			if (!request.CaFile.empty())
				curl_easy_setopt(curl, CURLOPT_CAINFO, request.CaFile.c_str());
			else
			{
				if (!system.File.empty())
					curl_easy_setopt(curl, CURLOPT_CAINFO, system.File.c_str());
				if (!system.Dir.empty())
					curl_easy_setopt(curl, CURLOPT_CAPATH, system.Dir.c_str());
				if (!system.Pem.empty())
				{
					pem.data = const_cast<char*>(system.Pem.data());
					pem.len = system.Pem.size();
					pem.flags = CURL_BLOB_NOCOPY;
					curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &pem);
				}
			}
			return headers;
		}

		// Runs a request (on a job's thread: no Lua state), following its
		// redirects. `file` receives the body (a download), else the outcome
		// holds it.
		std::shared_ptr<Outcome> Run(const Request& first, FILE* file)
		{
			auto outcome = std::make_shared<Outcome>();
			CURL* curl = curl_easy_init();
			if (!curl)
			{
				outcome->Failure = "cannot start a transfer";
				return outcome;
			}
			Transfer& transfer = outcome->Transfer;
			transfer.File = file;
			transfer.Follow = first.FollowRedirects;
			char errorText[CURL_ERROR_SIZE] = {};
			curl_blob pem{};
			Request request = first;
			// The time limit is for the whole request, its redirects too (at
			// least a millisecond: curl reads 0 as none).
			const auto started = std::chrono::steady_clock::now();
			for (int redirects = 0;; redirects++)
			{
				transfer.Headers.clear();
				transfer.Body.clear();
				transfer.Status = 0;
				transfer.Skip = false;
				errorText[0] = 0;
				curl_slist* headers = Configure(curl, request, transfer, errorText, pem);
				if (first.Timeout > 0)
				{
					const double left = first.Timeout - std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
					const double ms = (std::min)(std::ceil((std::max)(left, 0.0) * 1000), (double)INT_MAX);
					curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (std::max)(1L, (long)ms));
				}
				outcome->Code = curl_easy_perform(curl);
				curl_slist_free_all(headers);
				outcome->Status = 0;
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &outcome->Status);
				char* finalUrl = nullptr;
				curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &finalUrl);
				outcome->Url = finalUrl ? finalUrl : request.Url;
				outcome->ErrorText = errorText;
				char* next = nullptr;
				curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &next);
				if (outcome->Code != CURLE_OK || !transfer.Skip || !next)
					break;
				if (redirects == 10)
				{
					outcome->Code = CURLE_TOO_MANY_REDIRECTS;
					outcome->ErrorText = "more than 10 redirects";
					break;
				}
				Redirect(request, outcome->Status, next);
			}
			curl_easy_cleanup(curl);
			transfer.File = nullptr;
			return outcome;
		}

		// The outcome as the response table, or the error raised.
		int Push(lua_State* L, const char* fn, const Request& request, const Outcome& outcome)
		{
			if (!outcome.Failure.empty())
				luaL_error(L, "%s: %s", fn, outcome.Failure.c_str());
			const CURLcode code = outcome.Code;
			if (code == CURLE_ABORTED_BY_CALLBACK)
				RaiseIfInterrupted(L);
			if (code != CURLE_OK)
			{
				const Certificates& system = SystemCertificates();
				std::string why = !outcome.ErrorText.empty() ? outcome.ErrorText : curl_easy_strerror(code);
				while (!why.empty() && (why.back() == '\n' || why.back() == '.'))
					why.pop_back();
				if (code == CURLE_OPERATION_TIMEDOUT)
					why = "no answer within " + std::to_string(request.Timeout).substr(0, 6) + " seconds (" + why + ")";
				else if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE)
					why += system.Where.empty() && request.CaFile.empty()
						? " (no trusted certificates were found on this system: set SSL_CERT_FILE to a CA bundle)"
						: " (trusted certificates: " + (request.CaFile.empty() ? system.Where : request.CaFile) + ")";
				else if (outcome.Transfer.WriteFailed)
					why = "cannot write the download";
				luaL_error(L, "%s: %s %s: %s", fn, request.Method.c_str(), Shown(request.Url).c_str(), why.c_str());
			}

			const Transfer& transfer = outcome.Transfer;
			lua_createtable(L, 0, 5);
			lua_pushinteger(L, (int)outcome.Status);
			lua_setfield(L, -2, "status");
			lua_pushboolean(L, outcome.Status >= 200 && outcome.Status < 300);
			lua_setfield(L, -2, "ok");
			lua_pushlstring(L, transfer.Body.data(), transfer.Body.size());
			lua_setfield(L, -2, "body");
			lua_pushstring(L, outcome.Url.c_str());
			lua_setfield(L, -2, "url");
			lua_createtable(L, 0, (int)transfer.Headers.size());
			for (const auto& [name, value] : transfer.Headers)
			{
				lua_pushlstring(L, value.data(), value.size());
				lua_setfield(L, -2, name.c_str());
			}
			lua_setfield(L, -2, "headers");
			return 1;
		}

		// A request that waits on its own in a task (Jobs.h).
		int Perform(lua_State* L, const char* fn, Request request)
		{
			// Set up once on the script's thread, before any job uses them.
			InitCurl();
			SystemCertificates();
			return Await(L, [fn, request = std::move(request)]() -> JobResult {
				std::shared_ptr<Outcome> outcome = Run(request, nullptr);
				return [fn, request, outcome](lua_State* L) { return Push(L, fn, request, *outcome); };
			});
		}

		// http.request(options): options.url and .method (GET by default).
		int HttpRequest(lua_State* L)
		{
			luaL_checktype(L, 1, LUA_TTABLE);
			Request request;
			request.Url = StringField(L, 1, "url", "http.request");
			if (request.Url.empty())
				luaL_error(L, "http.request: options.url is the address");
			CheckUrl(L, request.Url.c_str(), request.Url.size(), "http.request");
			ReadOptions(L, 1, "http.request", request);
			return Perform(L, "http.request", std::move(request));
		}

		int HttpGet(lua_State* L)
		{
			Request request;
			size_t length = 0;
			const char* url = luaL_checklstring(L, 1, &length);
			CheckUrl(L, url, length, "http.get");
			request.Url = url;
			ReadOptions(L, 2, "http.get", request);
			return Perform(L, "http.get", std::move(request));
		}

		// http.post(url, body?, options?): `body` is text (options.json for a value as JSON).
		int HttpPost(lua_State* L)
		{
			Request request;
			request.Method = "POST";
			size_t length = 0;
			const char* url = luaL_checklstring(L, 1, &length);
			CheckUrl(L, url, length, "http.post");
			request.Url = url;
			if (!lua_isnoneornil(L, 2))
			{
				size_t len = 0;
				const char* body = luaL_checklstring(L, 2, &len);
				request.Body = std::string(body, len);
			}
			ReadOptions(L, 3, "http.post", request);
			if (request.Method != "POST")
				luaL_error(L, "http.post: the method is POST (http.request takes any)");
			return Perform(L, "http.post", std::move(request));
		}

		// http.download(url, path, options?): the body into a file (it is
		// replaced only when the download succeeds with a 2xx status).
		int HttpDownload(lua_State* L)
		{
			Request request;
			size_t length = 0;
			const char* url = luaL_checklstring(L, 1, &length);
			CheckUrl(L, url, length, "http.download");
			request.Url = url;
			const std::string shown = luaL_checkstring(L, 2);
			const fs::path path = fs::u8path(shown);
			ReadOptions(L, 3, "http.download", request);
			InitCurl();
			SystemCertificates();
			return Await(L, [request = std::move(request), path, shown]() -> JobResult {
				fs::path partial = path;
				partial += ".download";
#ifdef _WIN32
				FILE* file = _wfopen(partial.c_str(), L"wb");
#else
				FILE* file = std::fopen(partial.c_str(), "wb");
#endif
				if (!file)
					return [shown](lua_State* L) -> int {
						luaL_error(L, "http.download: cannot write '%s'", shown.c_str());
						return 0;
					};
				std::shared_ptr<Outcome> outcome = Run(request, file);
				std::fclose(file);
				std::string renameError;
				std::error_code ec;
				const bool ok = outcome->Failure.empty() && outcome->Code == CURLE_OK && outcome->Status >= 200 && outcome->Status < 300;
				if (ok)
				{
					fs::rename(partial, path, ec);
					// What is in the way is replaced only when it is a file:
					// a directory (even an empty one) is an error.
					if (std::error_code isDir; ec && !fs::is_directory(fs::symlink_status(path, isDir)))
					{
						fs::remove(path, ec);
						fs::rename(partial, path, ec);
					}
					if (ec)
						renameError = ec.message();
				}
				fs::remove(partial, ec);
				return [request, outcome, shown, renameError](lua_State* L) -> int {
					if (!renameError.empty())
						luaL_error(L, "http.download: cannot write '%s': %s", shown.c_str(), renameError.c_str());
					return Push(L, "http.download", request, *outcome);
				};
			});
		}

		const luaL_Reg kHttp[] = {
			{ "request", HttpRequest }, { "get", HttpGet }, { "post", HttpPost }, { "download", HttpDownload }, { nullptr, nullptr },
		};

	} // namespace

	void OpenHttp(lua_State* L)
	{
		lua_newtable(L);
		for (const luaL_Reg* f = kHttp; f->name; f++)
		{
			lua_pushcclosurek(L, f->func, f->name, 0, AwaitContinue);
			lua_setfield(L, -2, f->name);
		}
	}

} // namespace cluau::runtime::detail
