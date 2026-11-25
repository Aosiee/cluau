// mbedTLS's mutex on Windows (MBEDTLS_THREADING_ALT): a slim reader/writer
// lock, kept as the pointer an SRWLOCK is, so this header needs no
// <windows.h>. Tls.cpp gives mbedTLS the functions that use it.

#pragma once

typedef struct mbedtls_threading_mutex_t
{
	void* lock;
	char valid;
} mbedtls_threading_mutex_t;
