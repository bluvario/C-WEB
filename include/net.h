#ifndef CWEB_NET_H
#define CWEB_NET_H

#include <stddef.h>
#include <stdint.h>

// portable socket handle. -1 means invalid.
typedef intptr_t Socket_Handle;

// winsock setup on Windows, SIGPIPE silencing on POSIX. call once at startup.
int net_init(void);
void net_cleanup(void);

// binds 0.0.0.0:port and starts listening. port 0 picks an ephemeral port.
// returns -1 on failure.
Socket_Handle net_listen(int port);
// the port a listening socket actually ended up bound to
int net_bound_port(Socket_Handle listener);

Socket_Handle net_accept(Socket_Handle listener);
// blocking tcp connect to host:port ("127.0.0.1" style, no DNS yet)
Socket_Handle net_connect(const char *host, int port);

// bytes read, 0 on an orderly close, -1 on error
long net_recv(Socket_Handle sock, void *buf, size_t len);
// sends until everything is out or the connection dies. -1 on error.
long net_send_all(Socket_Handle sock, const void *buf, size_t len);

void net_close(Socket_Handle sock);

// human-readable reason for the last failed net_* call, for logging
const char *net_error_string(void);

#endif