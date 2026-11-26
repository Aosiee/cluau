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

#ifdef _WIN32
		// Whether Windows lets a root vouch for servers: its enhanced key
		// usage (the certificate's own and the store's restrictions together)
		// is any use, or includes server authentication.
		bool TrustedForServers(PCCERT_CONTEXT cert)
		{
			DWORD size = 0;
			if (!CertGetEnhancedKeyUsage(cert, 0, nullptr, &size))
				return false;
			std::vector<BYTE> data(size);
			PCERT_ENHKEY_USAGE usage = reinterpret_cast<PCERT_ENHKEY_USAGE>(data.data());
			if (!CertGetEnhancedKeyUsage(cert, 0, usage, &size))
				return false;
			// No usages: good for all when the error is CRYPT_E_NOT_FOUND,
			// else good for none.
			if (usage->cUsageIdentifier == 0)
				return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
			for (DWORD i = 0; i < usage->cUsageIdentifier; i++)
				if (std::strcmp(usage->rgpszUsageIdentifier[i], szOID_PKIX_KP_SERVER_AUTH) == 0)
					return true;
			return false;
		}

		// The machine's and the user's trusted roots, as PEM: only the roots
		// (mbedTLS would take any certificate given as one to trust, an
		// intermediate too), only those trusted for servers, and none that is
		// in the Disallowed store.
		std::string WindowsStorePem()
		{
			std::string pem;
			HCERTSTORE disallowed = CertOpenSystemStoreW(0, L"Disallowed");
			HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
			if (store)
			{
				PCCERT_CONTEXT cert = nullptr;
				while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr)
				{
					if (cert->dwCertEncodingType != X509_ASN_ENCODING || !TrustedForServers(cert))
						continue;
					if (disallowed)
					{
						if (PCCERT_CONTEXT found = CertFindCertificateInStore(disallowed, X509_ASN_ENCODING, 0, CERT_FIND_EXISTING, cert, nullptr))
						{
							CertFreeCertificateContext(found);
							continue;
						}
					}
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
			if (disallowed)
				CertCloseStore(disallowed, 0);
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
			error = !system.File.empty() || !system.Dir.empty()
				? "no trusted certificates were found in " + system.Where
				: "no trusted certificates were found on this system: set SSL_CERT_FILE to a CA bundle";
		return any;
	}

} // namespace cluau::runtime::detail
