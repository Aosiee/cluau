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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
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

		struct Transfer
		{
			std::string Body;
			FILE* File = nullptr; // download: the body goes here
			std::map<std::string, std::string> Headers;
			bool WriteFailed = false;
		};

		size_t OnBody(char* data, size_t size, size_t count, void* user)
		{
			Transfer& t = *static_cast<Transfer*>(user);
			const size_t n = size * count;
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
			// A new status line: the response after a redirect starts afresh.
			if (line.rfind("HTTP/", 0) == 0)
			{
				t.Headers.clear();
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
			std::vector<std::string> Headers; // "Name: value"
			std::optional<std::string> Body;
			double Timeout = 0;
			bool FollowRedirects = true;
			std::string CaFile;
		};

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
					const std::string name = lua_tostring(L, -2);
					std::string lower = name;
					std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
					contentType = contentType || lower == "content-type";
					request.Headers.push_back(name + ": " + lua_tostring(L, -1));
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
					request.Headers.push_back("Content-Type: application/json");
			}
			lua_pop(L, 1);
			lua_getfield(L, index, "timeout");
			if (!lua_isnil(L, -1))
			{
				if (!lua_isnumber(L, -1) || lua_tonumber(L, -1) <= 0)
					luaL_error(L, "%s: options.timeout is a number of seconds", fn);
				request.Timeout = lua_tonumber(L, -1);
			}
			lua_pop(L, 1);
			lua_getfield(L, index, "followRedirects");
			if (!lua_isnil(L, -1))
				request.FollowRedirects = lua_toboolean(L, -1);
			lua_pop(L, 1);
		}

		bool HasHeader(const Request& request, const std::string& lowerName)
		{
			for (const std::string& h : request.Headers)
			{
				std::string name = h.substr(0, h.find(':'));
				std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
				if (name == lowerName)
					return true;
			}
			return false;
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

		// Runs a request (on a job's thread: no Lua state). `file` receives
		// the body (a download), else the outcome holds it.
		std::shared_ptr<Outcome> Run(const Request& request, FILE* file)
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
			char errorText[CURL_ERROR_SIZE] = {};
			curl_slist* headers = nullptr;
			for (const std::string& h : request.Headers)
				headers = curl_slist_append(headers, h.c_str());

			const std::string agent = std::string("cluau/") + Version();
			curl_easy_setopt(curl, CURLOPT_URL, request.Url.c_str());
			curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
			curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorText);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnBody);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
			curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, OnHeader);
			curl_easy_setopt(curl, CURLOPT_HEADERDATA, &transfer);
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, request.FollowRedirects ? 1L : 0L);
			curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
			curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
			curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			if (request.Timeout > 0)
				curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(request.Timeout * 1000));

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
			curl_blob pem{};
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

			outcome->Code = curl_easy_perform(curl);
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &outcome->Status);
			char* finalUrl = nullptr;
			curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &finalUrl);
			outcome->Url = finalUrl ? finalUrl : request.Url;
			outcome->ErrorText = errorText;
			curl_slist_free_all(headers);
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
				luaL_error(L, "%s: %s %s: %s", fn, request.Method.c_str(), request.Url.c_str(), why.c_str());
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
			ReadOptions(L, 1, "http.request", request);
			return Perform(L, "http.request", std::move(request));
		}

		int HttpGet(lua_State* L)
		{
			Request request;
			request.Url = luaL_checkstring(L, 1);
			ReadOptions(L, 2, "http.get", request);
			return Perform(L, "http.get", std::move(request));
		}

		// http.post(url, body?, options?): `body` is text (options.json for a value as JSON).
		int HttpPost(lua_State* L)
		{
			Request request;
			request.Method = "POST";
			request.Url = luaL_checkstring(L, 1);
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
			request.Url = luaL_checkstring(L, 1);
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
					if (ec)
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
