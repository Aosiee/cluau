#pragma once

// What @std/http, @std/crypto and @std/net's TLS share over mbedTLS: its
// start (once, on the script's thread, before any other use) and the
// system's trusted certificates.
//
// mbedTLS has no access to a system's certificate store, so the trusted
// certificates come from here: SSL_CERT_FILE / SSL_CERT_DIR when set, else
// Windows' own root store (exported as PEM), macOS's /etc/ssl/cert.pem, or
// the first of the usual bundles a Linux distribution keeps.

#include <string>

struct mbedtls_x509_crt;

namespace cluau::runtime::detail {

	struct Certificates
	{
		std::string File;  // a bundle file
		std::string Dir;   // a directory of certificates
		std::string Pem;   // certificates in memory (Windows' store)
		std::string Where; // what was found, for an error message
	};

	const Certificates& SystemCertificates();

	// mbedTLS's threading (Windows' mutexes) and its PSA crypto: once.
	void StartMbedtls();

	// The certificates a peer's must chain to: `caFile`'s, else the
	// system's. False with the reason when there are none.
	bool LoadTrusted(mbedtls_x509_crt& chain, const std::string& caFile, std::string& error);

} // namespace cluau::runtime::detail
