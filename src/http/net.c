#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET raw_socket;
#define RAW_INVALID INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int raw_socket;
#define RAW_INVALID (-1)
#endif

// TODO: this scratchpad is shared, will need a real lock when threaded
static char g_error[256] = "no error yet";

static void set_err(const char *why)
{
    const char *detail;
#ifdef _WIN32
    detail = "see WSAGetLastError"; // TODO: FormatMessage the code
#else
    detail = strerror(errno);
#endif
    snprintf(g_error, sizeof(g_error), "%s: %s", why, detail);
}

static int raw_valid(raw_socket fd)
{
#ifdef _WIN32
    return fd != RAW_INVALID;
#else
    return fd >= 0;
#endif
}

static void raw_close(raw_socket fd)
{
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

int net_init(void)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        set_err("WSAStartup failed");
        return -1;
    }
#else
    // a peer closing mid-write must not kill the whole server with SIGPIPE
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        set_err("could not ignore SIGPIPE");
        return -1;
    }
#endif
    return 0;
}

void net_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

Socket_Handle net_listen(int port)
{
    raw_socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!raw_valid(fd)) {
        set_err("socket() failed");
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        set_err("bind() failed");
        raw_close(fd);
        return -1;
    }
    if (listen(fd, 128) != 0) {
        set_err("listen() failed");
        raw_close(fd);
        return -1;
    }
    return (Socket_Handle)fd;
}

int net_bound_port(Socket_Handle listener)
{
    struct sockaddr_in addr;
#ifdef _WIN32
    int len = sizeof addr;
#else
    socklen_t len = sizeof addr;
#endif
    if (getsockname((raw_socket)listener, (struct sockaddr *)&addr, &len) != 0) {
        set_err("getsockname() failed");
        return -1;
    }
    return ntohs(addr.sin_port);
}

Socket_Handle net_accept(Socket_Handle listener)
{
    raw_socket c = accept((raw_socket)listener, NULL, NULL);
    if (!raw_valid(c)) {
        set_err("accept() failed");
        return -1;
    }
    return (Socket_Handle)c;
}

Socket_Handle net_connect(const char *host, int port)
{
    raw_socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!raw_valid(fd)) {
        set_err("socket() failed");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        set_err("inet_pton() failed");
        raw_close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        set_err("connect() failed");
        raw_close(fd);
        return -1;
    }
    return (Socket_Handle)fd;
}

long net_recv(Socket_Handle sock, void *buf, size_t len)
{
    long n;
#ifdef _WIN32
    n = recv((SOCKET)sock, (char *)buf, (int)len, 0);
#else
    n = recv((int)sock, buf, len, 0);
#endif
    if (n < 0) {
#ifdef _WIN32
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) {
            // SO_RCVTIMEO expired, no data was waiting
            return NET_READ_TIMEOUT;
        }
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return NET_READ_TIMEOUT;
        }
#endif
        set_err("recv() failed");
        return -1;
    }
    return n;
}

int net_set_timeout(Socket_Handle sock, unsigned long ms)
{
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    if (setsockopt((SOCKET)sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t) != 0) {
        set_err("setsockopt(SO_RCVTIMEO) failed");
        return -1;
    }
#else
    struct timeval tv;
    tv.tv_sec = (long)(ms / 1000);
    tv.tv_usec = (long)(ms % 1000) * 1000;
    if (setsockopt((int)sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
        set_err("setsockopt(SO_RCVTIMEO) failed");
        return -1;
    }
#endif
    return 0;
}

long net_send_all(Socket_Handle sock, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t left = len;
    while (left > 0) {
        long sent;
#ifdef _WIN32
        sent = send((SOCKET)sock, (const char *)p, (int)left, 0);
#else
        sent = send((int)sock, p, left, 0);
#endif
        if (sent <= 0) {
            if (sent < 0)
                set_err("send() failed");
            else
                snprintf(g_error, sizeof(g_error), "send() truncated to zero");
            return -1;
        }
        p += (size_t)sent;
        left -= (size_t)sent;
    }
    return (long)len;
}

void net_close(Socket_Handle sock)
{
    if (sock == -1) {
        return;
    }
    raw_close((raw_socket)sock);
}

const char *net_error_string(void)
{
    return g_error;
}