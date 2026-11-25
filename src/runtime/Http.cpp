// @std/http: HTTP and HTTPS requests through libcurl (over mbedTLS).
//
// mbedTLS has no access to a system's certificate store, so the trusted
// certificates come from here: SSL_CERT_FILE / SSL_CERT_DIR when set, else
// Windows' own root store (exported for curl), macOS's /etc/ssl/cert.pem, or
// the first of the usual bundles a Linux distribution keeps.

#include "Std.h"

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

		// ---- certificates ------------------------------------------------

		struct Certificates
		{
			std::string File;  // a bundle file
			std::string Dir;   // a directory of certificates
			std::string Pem;   // certificates in memory (Windows' store)
			std::string Where; // what was found, for an error message
		};

#ifdef _WIN32
		// The machine's and the user's trusted roots (and intermediates), as PEM.
		std::string WindowsStorePem()
		{
			std::string pem;
			for (const wchar_t* name : { L"ROOT", L"CA" })
			{
				HCERTSTORE store = CertOpenSystemStoreW(0, name);
				if (!store)
					continue;
				PCCERT_CONTEXT cert = nullptr;
				while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr)
				{
					if (cert->dwCertEncodingType != X509_ASN_ENCODING)
						continue;
					DWORD length = 0;
					if (!CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded, CRYPT_STRING_BASE64HEADER | CRYPT_STRING_NOCR, nullptr,
							&length))
						continue;
					std::string text(length, '\0');
					if (CryptBinaryToStringA(cert->pbCertEncoded, cert->cbCertEncoded, CRYPT_STRING_BASE64HEADER | CRYPT_STRING_NOCR, text.data(),
							&length))
					{
						text.resize(std::strlen(text.c_str()));
						pem += text;
					}
				}
				CertCloseStore(store, 0);
			}
			return pem;
		}
#endif

		const Certificates& SystemCertificates()
		{
			static const Certificates found = [] {
				Certificates c;
				std::error_code ec;
				if (const char* file = std::getenv("SSL_CERT_FILE"); file && *file)
				{
					c.File = file;
					c.Where = std::string("SSL_CERT_FILE (") + file + ")";
				}
				if (const char* dir = std::getenv("SSL_CERT_DIR"); dir && *dir)
				{
					c.Dir = dir;
					c.Where += (c.Where.empty() ? "" : ", ") + std::string("SSL_CERT_DIR (") + dir + ")";
				}
				if (!c.File.empty() || !c.Dir.empty())
					return c;
#if defined(_WIN32)
				c.Pem = WindowsStorePem();
				c.Where = "the Windows certificate store";
#else
				static const char* kBundles[] = {
#if defined(__APPLE__)
					"/etc/ssl/cert.pem",
#endif
					"/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, Gentoo
					"/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // Fedora, RHEL
					"/etc/pki/tls/certs/ca-bundle.crt",                  // older Fedora, RHEL
					"/etc/ssl/ca-bundle.pem",                            // openSUSE
					"/etc/ssl/cert.pem",                                 // Alpine, the BSDs
					"/usr/local/share/certs/ca-root-nss.crt",            // FreeBSD
				};
				for (const char* bundle : kBundles)
					if (fs::is_regular_file(bundle, ec))
					{
						c.File = bundle;
						c.Where = bundle;
						break;
					}
#endif
				return c;
			}();
			return found;
		}

		// ---- a request ------------------------------------------------------

		void InitCurl()
		{
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

		// Runs a request; pushes the response table. `file` receives the body
		// (a download), else the response holds it.
		void Perform(lua_State* L, const char* fn, const Request& request, FILE* file)
		{
			InitCurl();
			CURL* curl = curl_easy_init();
			if (!curl)
				luaL_error(L, "%s: cannot start a transfer", fn);
			Transfer transfer;
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

			const CURLcode code = curl_easy_perform(curl);
			long status = 0;
			curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
			char* finalUrl = nullptr;
			curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &finalUrl);
			const std::string url = finalUrl ? finalUrl : request.Url;
			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);

			if (code == CURLE_ABORTED_BY_CALLBACK)
				RaiseIfInterrupted(L);
			if (code != CURLE_OK)
			{
				std::string why = errorText[0] ? errorText : curl_easy_strerror(code);
				while (!why.empty() && (why.back() == '\n' || why.back() == '.'))
					why.pop_back();
				if (code == CURLE_OPERATION_TIMEDOUT)
					why = "no answer within " + std::to_string(request.Timeout).substr(0, 6) + " seconds (" + why + ")";
				else if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CACERT_BADFILE)
					why += system.Where.empty() && request.CaFile.empty()
						? " (no trusted certificates were found on this system: set SSL_CERT_FILE to a CA bundle)"
						: " (trusted certificates: " + (request.CaFile.empty() ? system.Where : request.CaFile) + ")";
				else if (transfer.WriteFailed)
					why = "cannot write the download";
				luaL_error(L, "%s: %s %s: %s", fn, request.Method.c_str(), request.Url.c_str(), why.c_str());
			}

			lua_createtable(L, 0, 5);
			lua_pushinteger(L, (int)status);
			lua_setfield(L, -2, "status");
			lua_pushboolean(L, status >= 200 && status < 300);
			lua_setfield(L, -2, "ok");
			lua_pushlstring(L, transfer.Body.data(), transfer.Body.size());
			lua_setfield(L, -2, "body");
			lua_pushstring(L, url.c_str());
			lua_setfield(L, -2, "url");
			lua_createtable(L, 0, (int)transfer.Headers.size());
			for (const auto& [name, value] : transfer.Headers)
			{
				lua_pushlstring(L, value.data(), value.size());
				lua_setfield(L, -2, name.c_str());
			}
			lua_setfield(L, -2, "headers");
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
			Perform(L, "http.request", request, nullptr);
			return 1;
		}

		int HttpGet(lua_State* L)
		{
			Request request;
			request.Url = luaL_checkstring(L, 1);
			ReadOptions(L, 2, "http.get", request);
			Perform(L, "http.get", request, nullptr);
			return 1;
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
			Perform(L, "http.post", request, nullptr);
			return 1;
		}

		// http.download(url, path, options?): the body into a file (it is
		// replaced only when the download succeeds with a 2xx status).
		int HttpDownload(lua_State* L)
		{
			Request request;
			request.Url = luaL_checkstring(L, 1);
			const fs::path path = fs::u8path(luaL_checkstring(L, 2));
			ReadOptions(L, 3, "http.download", request);
			fs::path partial = path;
			partial += ".download";
#ifdef _WIN32
			FILE* file = _wfopen(partial.c_str(), L"wb");
#else
			FILE* file = std::fopen(partial.c_str(), "wb");
#endif
			if (!file)
				luaL_error(L, "http.download: cannot write '%s'", luaL_checkstring(L, 2));
			struct Closer
			{
				FILE*& File;
				fs::path Partial;
				~Closer()
				{
					if (File)
						std::fclose(File);
					std::error_code ec;
					fs::remove(Partial, ec);
				}
			} closer{ file, partial };
			Perform(L, "http.download", request, file);
			std::fclose(file);
			file = nullptr;
			lua_getfield(L, -1, "ok");
			const bool ok = lua_toboolean(L, -1);
			lua_pop(L, 1);
			if (ok)
			{
				std::error_code ec;
				fs::rename(partial, path, ec);
				if (ec)
				{
					fs::remove(path, ec);
					fs::rename(partial, path, ec);
				}
				if (ec)
					luaL_error(L, "http.download: cannot write '%s': %s", luaL_checkstring(L, 2), ec.message().c_str());
			}
			return 1;
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
			lua_pushcfunction(L, f->func, f->name);
			lua_setfield(L, -2, f->name);
		}
	}

} // namespace cluau::runtime::detail
