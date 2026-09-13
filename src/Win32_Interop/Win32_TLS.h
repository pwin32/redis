/* Internal TLS transport for Redis's synthetic Windows descriptors. */
#ifndef REDIS_WIN32_TLS_H
#define REDIS_WIN32_TLS_H

#if defined(_WIN32) && defined(USE_OPENSSL)
#include <openssl/ssl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SSL owns the BIO, which borrows the descriptor and retains its IOCP state. */
int Win32TLS_SetFD(SSL *ssl, int fd);

enum { WIN32_TLS_CONNECT, WIN32_TLS_READ, WIN32_TLS_WRITE };
/* Negative timeout/deadline means unlimited; all other deadlines are monotonic. */
long long Win32TLS_Deadline(long long timeout_ms);
int Win32TLS_Remaining(long long deadline);
int Win32TLS_BeginSync(SSL *ssl, long long deadline);
int Win32TLS_SyncIO(SSL *ssl, int operation, void *buffer, int length,
                    long long deadline, int *ssl_error);
int Win32TLS_EndSync(SSL *ssl);

/* Internal hooks for the benchmark's one-shot hiredis event adapter. */
struct redisContext;
int Win32TLS_HiredisWant(struct redisContext *context, int writing);
int Win32TLS_HiredisPending(struct redisContext *context);

#ifdef __cplusplus
}
#endif
#endif
#endif
