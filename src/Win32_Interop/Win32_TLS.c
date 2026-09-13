/* OpenSSL must never interpret an FDAPI descriptor as a native SOCKET.
 * Both the server and hiredis use this borrowing, FDAPI-backed BIO. */
#if defined(_WIN32) && defined(USE_OPENSSL)
#include "win32fixes.h"
#include "Win32_FDAPI.h"
#include "win32_wsiocp.h"
#include "Win32_TLS.h"
#include <openssl/err.h>
#include <errno.h>
#include <limits.h>

#if OPENSSL_VERSION_NUMBER < 0x30000000L
#error "The Windows TLS build requires OpenSSL 3 or newer"
#endif

typedef struct win32_tls_bio {
    iocpSockState *state;
    int sync;
    int saved_flags;
    int eof;
} win32_tls_bio;

static BIO_METHOD *fdapi_method;
static CRYPTO_ONCE fdapi_method_once = CRYPTO_ONCE_STATIC_INIT;

static int fdapiBIOCreate(BIO *bio) {
    BIO_set_init(bio, 0);
    BIO_set_shutdown(bio, BIO_NOCLOSE);
    return 1;
}

static int fdapiBIODestroy(BIO *bio) {
    win32_tls_bio *transport = BIO_get_data(bio);
    if (transport) {
        /* A caller must finish a synchronous operation before freeing SSL. */
        WSIOCP_ReleaseSocketState(transport->state);
        OPENSSL_free(transport);
    }
    BIO_set_data(bio, NULL);
    BIO_set_init(bio, 0);
    return 1;
}

static int fdapiBIOTransfer(BIO *bio, char *buffer, int length, int writing) {
    win32_tls_bio *transport = BIO_get_data(bio);
    int result;
    BIO_clear_retry_flags(bio);
    if (!transport || (transport->state->masks & CLOSE_PENDING)) {
        errno = EBADF;
        return -1;
    }
    if (length < 0 || (!buffer && length)) {
        errno = EINVAL;
        return -1;
    }
    if (!length) return 0;

    /* The zero-byte receive owns ov_read until its normal IOCP dequeue.
     * A synchronous operation may bypass it only after kernel cancellation
     * was acknowledged through its event. Never drain the completion port. */
    if (!writing && !transport->sync &&
        (transport->state->masks & (READ_QUEUED | CONNECT_PENDING))) {
        errno = EAGAIN;
        BIO_set_retry_read(bio);
        return -1;
    }
    errno = 0;
    result = writing ? write(transport->state->fd, buffer, length) :
                       read(transport->state->fd, buffer, length);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        if (writing) BIO_set_retry_write(bio);
        else BIO_set_retry_read(bio);
    }
    if (!writing && result == 0) transport->eof = 1;
    return result;
}

static int fdapiBIORead(BIO *bio, char *buffer, int length) {
    return fdapiBIOTransfer(bio, buffer, length, 0);
}

static int fdapiBIOWrite(BIO *bio, const char *buffer, int length) {
    return fdapiBIOTransfer(bio, (char *)buffer, length, 1);
}

static long fdapiBIOCtrl(BIO *bio, int command, long argument, void *pointer) {
    win32_tls_bio *transport = BIO_get_data(bio);
    (void)argument;
    switch (command) {
    case BIO_C_GET_FD:
        if (!transport) return -1;
        if (pointer) *(int *)pointer = transport->state->fd;
        return transport->state->fd;
    case BIO_CTRL_GET_CLOSE:
        return BIO_NOCLOSE;
    case BIO_CTRL_SET_CLOSE:
    case BIO_CTRL_FLUSH:
        return 1;
    case BIO_CTRL_EOF:
        return transport && transport->eof;
    case BIO_CTRL_DUP:
        /* Duplicating a transport would also duplicate its sync ownership. */
        return 0;
    default:
        return 0;
    }
}

static void fdapiBIOInit(void) {
    BIO_METHOD *method = BIO_meth_new(BIO_get_new_index() |
        BIO_TYPE_SOURCE_SINK | BIO_TYPE_DESCRIPTOR, "Redis FDAPI socket");
    if (!method) return;
    if (!BIO_meth_set_create(method, fdapiBIOCreate) ||
        !BIO_meth_set_destroy(method, fdapiBIODestroy) ||
        !BIO_meth_set_read(method, fdapiBIORead) ||
        !BIO_meth_set_write(method, fdapiBIOWrite) ||
        !BIO_meth_set_ctrl(method, fdapiBIOCtrl)) {
        BIO_meth_free(method);
        return;
    }
    /* Process lifetime, shared with hiredis; no teardown race with SSL_free. */
    fdapi_method = method;
}

int Win32TLS_SetFD(SSL *ssl, int fd) {
    BIO *bio;
    win32_tls_bio *transport;
    iocpSockState *state;
    if (!CRYPTO_THREAD_run_once(&fdapi_method_once, fdapiBIOInit) ||
        !fdapi_method) return 0;
    state = WSIOCP_GetSocketState(fd);
    if (!state || (state->masks & CLOSE_PENDING)) {
        ERR_raise(ERR_LIB_SYS, EBADF);
        return 0;
    }
    bio = BIO_new(fdapi_method);
    if (!bio) return 0;
    transport = OPENSSL_zalloc(sizeof(*transport));
    if (!transport) {
        BIO_free(bio);
        return 0;
    }
    transport->state = state;
    WSIOCP_RetainSocketState(state);
    BIO_set_data(bio, transport);
    BIO_set_init(bio, 1);
    SSL_set_bio(ssl, bio, bio);
    return 1;
}

static long long monotonicMilliseconds(void) {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return counter.QuadPart / frequency.QuadPart * 1000 +
           counter.QuadPart % frequency.QuadPart * 1000 / frequency.QuadPart;
}

long long Win32TLS_Deadline(long long timeout_ms) {
    long long now = monotonicMilliseconds();
    if (timeout_ms < 0) return -1;
    return timeout_ms > LLONG_MAX - now ? LLONG_MAX : now + timeout_ms;
}

int Win32TLS_Remaining(long long deadline) {
    long long remaining;
    if (deadline < 0) return -1;
    remaining = deadline - monotonicMilliseconds();
    if (remaining <= 0) return 0;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

int Win32TLS_BeginSync(SSL *ssl, long long deadline) {
    win32_tls_bio *transport = BIO_get_data(SSL_get_rbio(ssl));
    if (!transport || transport->sync) {
        errno = EINVAL;
        return -1;
    }
    transport->saved_flags = fcntl(transport->state->fd, F_GETFL, 0);
    if (transport->saved_flags < 0) return -1;
    if (fcntl(transport->state->fd, F_SETFL,
              transport->saved_flags | O_NONBLOCK) < 0) return -1;
    if (WSIOCP_SuspendRead(transport->state, Win32TLS_Remaining(deadline)) < 0) {
        int saved_errno = errno;
        fcntl(transport->state->fd, F_SETFL, transport->saved_flags);
        errno = saved_errno;
        return -1;
    }
    transport->sync = 1;
    return 0;
}

/* Call between BeginSync/EndSync. A single deadline covers cancellation,
 * handshake retries and all fragments of a read-line or full write. */
int Win32TLS_SyncIO(SSL *ssl, int operation, void *buffer, int length,
                    long long deadline, int *ssl_error) {
    win32_tls_bio *transport = BIO_get_data(SSL_get_rbio(ssl));
    int result;
    if (!transport || !transport->sync) {
        errno = EINVAL;
        *ssl_error = SSL_ERROR_SYSCALL;
        return -1;
    }
    for (;;) {
        ERR_clear_error();
        errno = 0;
        if (operation == WIN32_TLS_CONNECT) result = SSL_connect(ssl);
        else if (operation == WIN32_TLS_READ) result = SSL_read(ssl, buffer, length);
        else result = SSL_write(ssl, buffer, length);
        *ssl_error = result > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl, result);
        if (*ssl_error != SSL_ERROR_WANT_READ && *ssl_error != SSL_ERROR_WANT_WRITE)
            return result;

        struct pollfd event;
        event.fd = transport->state->fd;
        event.events = *ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
        event.revents = 0;
        int remaining = Win32TLS_Remaining(deadline);
        if (remaining == 0) {
            errno = ETIMEDOUT;
            break;
        }
        result = poll(&event, 1, remaining);
        if (result == 0) {
            errno = ETIMEDOUT;
            break;
        }
        if (result < 0 && errno != EINTR) break;
    }
    *ssl_error = SSL_ERROR_SYSCALL;
    return -1;
}

int Win32TLS_EndSync(SSL *ssl) {
    win32_tls_bio *transport = BIO_get_data(SSL_get_rbio(ssl));
    int result, saved_errno = errno;
    if (!transport || !transport->sync) {
        errno = EINVAL;
        return -1;
    }
    transport->sync = 0;
    result = fcntl(transport->state->fd, F_SETFL, transport->saved_flags);
    if (WSIOCP_ResumeRead(transport->state) < 0) result = -1;
    if (result == 0) errno = saved_errno;
    return result;
}
#endif
