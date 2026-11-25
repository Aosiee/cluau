#include "Tls.h"

#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/threading.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>

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

#ifdef _WIN32
		// mbedTLS's mutexes on Windows: slim reader/writer locks.
		void MutexInit(mbedtls_threading_mutex_t* m)
		{
			InitializeSRWLock(reinterpret_cast<PSRWLOCK>(&m->lock));
			m->valid = 1;
		}
		void MutexFree(mbedtls_threading_mutex_t* m) { m->valid = 0; }
		int MutexLock(mbedtls_threading_mutex_t* m)
		{
			if (!m->valid)
				return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
			AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m->lock));
			return 0;
		}
		int MutexUnlock(mbedtls_threading_mutex_t* m)
		{
			if (!m->valid)
				return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
			ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(&m->lock));
			return 0;
		}
#endif

	} // namespace

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

	void StartMbedtls()
	{
		static std::once_flag once;
		std::call_once(once, [] {
#ifdef _WIN32
			mbedtls_threading_set_alt(MutexInit, MutexFree, MutexLock, MutexUnlock);
#endif
			psa_crypto_init();
			// mbedTLS makes its list of cipher suites on the first call: here,
			// before any connection's thread could make it at the same time.
			mbedtls_ssl_list_ciphersuites();
		});
	}

	bool LoadTrusted(mbedtls_x509_crt& chain, const std::string& caFile, std::string& error)
	{
		auto failed = [&](int rc, const std::string& what) {
			char text[200] = {};
			mbedtls_strerror(rc, text, sizeof(text));
			error = "cannot read " + what + ": " + text;
			return false;
		};
		if (!caFile.empty())
		{
			const int rc = mbedtls_x509_crt_parse_file(&chain, caFile.c_str());
			return rc < 0 ? failed(rc, "the certificates in '" + caFile + "'") : true;
		}
		const Certificates& system = SystemCertificates();
		bool any = false;
		if (!system.File.empty() && mbedtls_x509_crt_parse_file(&chain, system.File.c_str()) >= 0)
			any = true;
		if (!system.Dir.empty() && mbedtls_x509_crt_parse_path(&chain, system.Dir.c_str()) >= 0)
			any = true;
		if (!system.Pem.empty() &&
			mbedtls_x509_crt_parse(&chain, reinterpret_cast<const unsigned char*>(system.Pem.c_str()), system.Pem.size() + 1) >= 0)
			any = true;
		if (!any)
			error = "no trusted certificates were found on this system: set SSL_CERT_FILE to a CA bundle";
		return any;
	}

} // namespace cluau::runtime::detail
