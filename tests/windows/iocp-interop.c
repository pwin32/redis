/* Native IOCP listener readiness, including deferred accept callbacks. */
#include "Win32_Interop/win32fixes.h"
#include "Win32_Interop/Win32_FDAPI.h"
#include "Win32_Interop/win32_wsiocp.h"
#include <errno.h>

static int failures;
static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: IOCP %s (errno=%d)\n", message, errno);
        failures++;
    }
}

struct deferred_accept_context {
    int callbacks;
    int accepted;
};

static void deferred_accept(aeEventLoop *loop, int fd, void *data, int mask) {
    (void)loop; (void)mask;
    struct deferred_accept_context *context =
        (struct deferred_accept_context *)data;
    /* Cluster listeners can leave a completed accept queued while loading. */
    if (++context->callbacks == 1) return;
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    int peer = WSIOCP_Accept(fd, (struct sockaddr *)&address,
                            &length);
    check(peer != -1, "a deferred listener callback should retain its accept");
    if (peer != -1) {
        context->accepted++;
        FDAPI_close(peer);
    }
}

static void test_deferred_accept(void) {
    aeEventLoop *loop = aeCreateEventLoop(65536);
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int peers[2] = {-1, -1};
    struct sockaddr_in address = {0};
    socklen_t length = sizeof(address);
    struct deferred_accept_context context = {0};
    DWORD deadline;
    check(loop != NULL && listener != -1,
          "deferred accept test should create its event loop and listener");
    if (loop == NULL || listener == -1) goto cleanup;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, length) != 0 ||
        getsockname(listener, (struct sockaddr *)&address, &length) != 0 ||
        WSIOCP_Listen(listener, 8) != 0 ||
        aeCreateFileEvent(loop, listener, AE_READABLE, deferred_accept, &context) != AE_OK) {
        check(0, "deferred accept listener should start");
        goto cleanup;
    }

    /* Prime an empty scan, then verify that a fresh completion still fires
     * and that leaving it unconsumed produces a later readiness callback. */
    aeProcessEvents(loop, AE_FILE_EVENTS | AE_DONT_WAIT);
    for (int index = 0; index < 2; index++) {
        peers[index] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (peers[index] == -1 ||
            connect(peers[index], (struct sockaddr *)&address, length) != 0) {
            check(0, "deferred accept peer should connect");
            goto cleanup;
        }
        deadline = GetTickCount() + 2000;
        while (context.accepted <= index && (LONG)(deadline - GetTickCount()) > 0) {
            aeProcessEvents(loop, AE_FILE_EVENTS | AE_DONT_WAIT);
            Sleep(1);
        }
        check(context.accepted == index + 1,
              "fresh and deferred accepts should both reach the listener");
        check(context.callbacks == index + 2,
              "each consumed accept should stop repeated listener callbacks");
    }

cleanup:
    if (loop != NULL && listener != -1)
        aeDeleteFileEvent(loop, listener, AE_READABLE);
    if (listener != -1) FDAPI_close(listener);
    for (int index = 0; index < 2; index++)
        if (peers[index] != -1) FDAPI_close(peers[index]);
    if (loop != NULL) aeDeleteEventLoop(loop);
}

int win32_iocp_interop_test(void) {
    test_deferred_accept();
    return failures;
}
