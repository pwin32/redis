/* Exercise the production BIO and IOCP ownership paths with native sockets. */
#include "Win32_Interop/win32fixes.h"
#include "Win32_Interop/Win32_FDAPI.h"
#include "Win32_Interop/win32_wsiocp.h"
#include "Win32_Interop/Win32_TLS.h"
#include <errno.h>

#ifdef USE_OPENSSL
static int failures;
#define check(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: TLS BIO %s (errno=%d)\n", message, errno); failures++; } \
} while (0)

static SSL *borrowSocket(SSL_CTX *ctx, int fd) {
    SSL *ssl = SSL_new(ctx);
    check(ssl && Win32TLS_SetFD(ssl, fd), "attaches a synthetic descriptor");
    return ssl;
}

static fdapi_write underlyingWrite;
static ssize_t shortWrite(int fd, const void *buffer, size_t length) {
    /* Exercise a short FDAPI result independently of Winsock buffering. */
    return underlyingWrite(fd, buffer, length > 7 ? 7 : length);
}

static void testBIO(SSL_CTX *ctx) {
    int pair[2];
    check(FDAPI_pipe_for_eventloop(pair) == 0, "creates socket pair");
    for (int i = 0; i < 2; i++) fcntl(pair[i], F_SETFL, O_NONBLOCK);
    SSL *ssl = borrowSocket(ctx, pair[0]);
    BIO *bio = SSL_get_rbio(ssl);
    char buffer[65536];
    check(BIO_get_fd(bio, NULL) == pair[0], "reports the synthetic fd without narrowing SOCKET");
    check(BIO_read(bio, buffer, 1) == -1 && BIO_should_read(bio), "empty socket retries reads");
    check(write(pair[1], "abc", 3) == 3, "peer sends a short fragment");
    check(aeWait(pair[0], AE_READABLE, 1000) & AE_READABLE, "fragment arrives");
    check(BIO_read(bio, buffer, sizeof(buffer)) == 3 && !memcmp(buffer, "abc", 3), "partial read preserves bytes");
    check(!BIO_should_retry(bio), "success clears the previous retry flag");
    check(BIO_read(bio, buffer, 0) == 0 && !BIO_eof(bio), "zero-length read is not EOF");

    underlyingWrite = write;
    write = shortWrite;
    int short_result = BIO_write(bio, "partial write", 13);
    write = underlyingWrite;
    check(short_result == 7, "partial writes retain their actual byte count");
    check(aeWait(pair[1], AE_READABLE, 1000) & AE_READABLE, "partial write arrives");
    check(read(pair[1], buffer, sizeof(buffer)) == 7 && !memcmp(buffer, "partial", 7),
          "partial write sends exactly the reported bytes");

    int small = 4096;
    setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, (char *)&small, sizeof(small));
    memset(buffer, 0xa5, sizeof(buffer));
    size_t sent = 0, received = 0;
    int blocked = 0;
    for (int i = 0; i < 1024; i++) {
        int n = BIO_write(bio, buffer, sizeof(buffer));
        if (n < 0) {
            blocked = BIO_should_write(bio);
            break;
        }
        sent += n;
    }
    check(blocked && sent > 0, "backpressure retries writes");
    while (received < sent) {
        check(aeWait(pair[1], AE_READABLE, 1000) & AE_READABLE, "queued bytes drain");
        int n = read(pair[1], buffer, sizeof(buffer));
        if (n <= 0) { check(0, "drain makes progress"); break; }
        received += n;
        for (int i = 0; i < n; i++) {
            if ((unsigned char)buffer[i] != 0xa5) { check(0, "binary data is intact"); break; }
        }
    }
    check(received == sent, "partial writes neither lose nor duplicate bytes");
    FDAPI_shutdown(pair[1], SD_SEND);
    aeWait(pair[0], AE_READABLE, 1000);
    check(BIO_read(bio, buffer, 1) == 0 && BIO_eof(bio) && !BIO_should_retry(bio), "EOF is not a retry");
    BIO_set_close(bio, BIO_CLOSE);
    SSL_free(ssl);
    check(fcntl(pair[0], F_GETFL, 0) >= 0, "BIO always borrows, including BIO_CLOSE requests");
    FDAPI_close(pair[0]);
    FDAPI_close(pair[1]);
}

static void countRead(aeEventLoop *el, int fd, void *data, int mask) {
    (void)el; (void)fd; (void)mask;
    (*(int *)data)++;
}

static void testHandshakeDeadline(SSL_CTX *ctx) {
    int pair[2], ssl_error = 0;
    FDAPI_pipe_for_eventloop(pair);
    SSL *ssl = borrowSocket(ctx, pair[0]);
    long long deadline = Win32TLS_Deadline(25);
    DWORD started = GetTickCount();
    check(Win32TLS_BeginSync(ssl, deadline) == 0, "starts synchronous handshake");
    check(Win32TLS_SyncIO(ssl, WIN32_TLS_CONNECT, NULL, 0, deadline, &ssl_error) < 0 &&
          ssl_error == SSL_ERROR_SYSCALL && errno == ETIMEDOUT,
          "silent TLS peer observes the operation deadline");
    check(GetTickCount() - started < 2000, "handshake deadline is bounded");
    check(Win32TLS_EndSync(ssl) == 0, "restores the socket after a handshake timeout");
    SSL_free(ssl);
    FDAPI_close(pair[0]);
    FDAPI_close(pair[1]);
}

static void testCancellation(SSL_CTX *ctx, int completed, int timeout) {
    int pair[2], other[2], callbacks = 0, other_callbacks = 0;
    char byte;
    aeEventLoop *el = aeCreateEventLoop(65536);
    check(el != NULL, "creates a real event loop");
    check(FDAPI_pipe_for_eventloop(pair) == 0, "creates cancellation pair");
    check(FDAPI_pipe_for_eventloop(other) == 0, "creates unrelated pair");
    SSL *ssl = borrowSocket(ctx, pair[0]);
    BIO *bio = SSL_get_rbio(ssl);
    check(aeCreateFileEvent(el, pair[0], AE_READABLE, countRead, &callbacks) == AE_OK, "queues readiness receive");
    check(aeCreateFileEvent(el, other[0], AE_READABLE, countRead, &other_callbacks) == AE_OK, "queues unrelated receive");
    iocpSockState *state = WSIOCP_GetExistingSocketState(pair[0]);
    iocpSockState *other_state = WSIOCP_GetExistingSocketState(other[0]);
    write(other[1], "o", 1);
    check(WaitForSingleObject(other_state->read_event, 1000) == WAIT_OBJECT_0, "unrelated completion is queued first");
    if (completed) {
        write(pair[1], "x", 1);
        check(WaitForSingleObject(state->read_event, 1000) == WAIT_OBJECT_0, "read can complete before cancellation");
    }
    check(BIO_read(bio, &byte, 1) < 0 && BIO_should_read(bio), "async BIO waits for matching dequeue");
    HANDLE original_event = state->read_event;
    if (timeout) {
        /* Simulate a delayed cancellation acknowledgement, without touching
         * the real pending OVERLAPPED or its event. */
        state->read_event = CreateEventW(NULL, TRUE, FALSE, NULL);
        check(Win32TLS_BeginSync(ssl, Win32TLS_Deadline(5)) < 0 && errno == ETIMEDOUT,
              "cancellation acknowledgement has a deadline");
        CloseHandle(state->read_event);
        state->read_event = original_event;
        check((state->masks & READ_QUEUED) && !state->read_suspended,
              "timeout preserves pending ownership and resumes async mode");
    }
    check(Win32TLS_BeginSync(ssl, Win32TLS_Deadline(1000)) == 0, "acknowledges only its own cancellation");
    check((state->masks & READ_QUEUED) && (other_state->masks & READ_QUEUED), "sync transition consumes no IOCP packets");
    if (!completed) write(pair[1], "x", 1);
    aeWait(pair[0], AE_READABLE, 1000);
    check(BIO_read(bio, &byte, 1) == 1 && byte == 'x', "sync read bypasses acknowledged readiness");
    check(Win32TLS_EndSync(ssl) == 0, "resumes readiness without reusing OVERLAPPED");
    check(state->ov_read.hEvent == original_event && (state->masks & READ_QUEUED), "old OVERLAPPED survives until ordinary dequeue");

    int old_fd = pair[0];
    aeDeleteFileEvent(el, old_fd, AE_READABLE);
    FDAPI_close(old_fd);
    check(BIO_read(bio, &byte, 1) < 0 && errno == EBADF && !BIO_should_retry(bio), "closed descriptor is a permanent error");
    int fresh[2];
    FDAPI_pipe_for_eventloop(fresh);
    check(fresh[0] != old_fd && fresh[1] != old_fd, "closed descriptor is retained through SSL and late completion");
    SSL_free(ssl);
    for (int i = 0; i < 100 && (WSIOCP_GetExistingSocketState(old_fd) || !other_callbacks); i++) {
        aeProcessEvents(el, AE_FILE_EVENTS | AE_DONT_WAIT);
        Sleep(1);
    }
    check(WSIOCP_GetExistingSocketState(old_fd) == NULL, "late completion releases closed descriptor");
    check(callbacks == 0 && other_callbacks == 1, "unrelated completion dispatches normally, closed socket never dispatches");
    aeDeleteFileEvent(el, other[0], AE_READABLE);
    FDAPI_close(pair[1]);
    FDAPI_close(other[0]);
    FDAPI_close(other[1]);
    FDAPI_close(fresh[0]);
    FDAPI_close(fresh[1]);
    aeDeleteEventLoop(el);
}
#endif

int win32_tls_interop_test(void) {
#ifdef USE_OPENSSL
    SSL_CTX *ctx = SSL_CTX_new(TLS_method());
    if (!ctx) return 1;
    testBIO(ctx);
    testHandshakeDeadline(ctx);
    for (int i = 0; i < 8; i++) {
        testCancellation(ctx, i & 1, i & 2);
    }
    SSL_CTX_free(ctx);
    return failures;
#else
    return 0;
#endif
}
