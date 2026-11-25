// cluau's additions to mbedTLS's configuration (MBEDTLS_USER_CONFIG_FILE):
// its threading, so that transfers and connections on several threads at
// once are safe (TLS 1.3's keys live in a store all of them share). On
// Windows the mutexes are the runtime's (threading_alt.h, Tls.cpp).

#define MBEDTLS_THREADING_C
#if defined(_WIN32)
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif

// The self-tests are mbedTLS's own; their counters would be shared by every
// thread.
#undef MBEDTLS_SELF_TEST
