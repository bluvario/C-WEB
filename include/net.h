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

// binds the unix stream socket at path and starts listening. a leftover file
// at path from an earlier run is removed first, so a crash never wedges a
// restart. the created socket file must be deleted once the server exits,
// otherwise nothing can rebind the path; net_unix_unlink() does that.
// returns -1 on failure (and on OSes without unix sockets).
Socket_Handle net_listen_unix(const char *path);
// blocking connect to the unix socket at path; -1 on failure
Socket_Handle net_connect_unix(const char *path);
// removes the socket file a unix listener left behind; no-op on Windows
void net_unix_unlink(const char *path);

Socket_Handle net_accept(Socket_Handle listener);
// blocking tcp connect to host:port ("127.0.0.1" style, no DNS yet)
Socket_Handle net_connect(const char *host, int port);
// blocking tcp connect that resolves names through getaddrinfo, trying each
// address the resolution hands back. -1 when none of them connect, in which
// case net_error_string explains the last one.
Socket_Handle net_connect_host(const char *host, int port);

// bytes read, 0 on an orderly close, -1 on error. after net_set_timeout a
// read that sits empty for the budget comes back as NET_READ_TIMEOUT.
long net_recv(Socket_Handle sock, void *buf, size_t len);
#define NET_READ_TIMEOUT (-2)
// sends until everything is out or the connection dies. -1 on error.
long net_send_all(Socket_Handle sock, const void *buf, size_t len);

// bounds future net_recv calls on sock to ms milliseconds. 0 disables the
// bound again. returns 0 or -1.
int net_set_timeout(Socket_Handle sock, unsigned long ms);

void net_close(Socket_Handle sock);

// makes a blocked net_accept() on sock return. only one thread handles a
// process-directed signal, so a peer accept loop may stay asleep otherwise;
// shutdown() wakes it, where close() from another thread is not guaranteed to
// on Linux. no-op on an already-partial fd.
void net_shutdown(Socket_Handle sock);

// human-readable reason for the last failed net_* call, for logging. the
// string lives in a per-thread buffer: it is only a promise for the calling
// thread, so snapshot it (format it into your own buffer) before any further
// net_* call on the same thread
const char *net_error_string(void);

// true when the last failed net_* call on this thread ran the process out of
// file descriptors (EMFILE / WSAEMFILE). the accept loop uses this to back
// off: on fd exhaustion the kernel drops the pending connection, so the only
// way out is to pause long enough for a worker to close a socket.
int net_exhausted_fds(void);
// pauses the calling thread for at least ms milliseconds, retrying when the
// sleep is interrupted. used as the accept loop's fd-exhaustion back-off.
void net_pause_ms(unsigned long ms);

#endif