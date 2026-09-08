#include <stdio.h>
#include <string.h>

#include "net.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;

    if (net_init() != 0) {
        fprintf(stderr, "net_init failed: %s\n", net_error_string());
        return 1;
    }

    Socket_Handle srv = net_listen(0);
    fails += check("listener bound", srv != -1);
    int port = net_bound_port(srv);
    fails += check("ephemeral port assigned", port > 0);

    Socket_Handle client = net_connect("127.0.0.1", port);
    fails += check("client connected", client != -1);
    Socket_Handle peer = net_accept(srv);
    fails += check("server accepted", peer != -1);

    // server says ping, client must hear it
    if (net_send_all(peer, "ping", 4) != 4) {
        fprintf(stderr, "server send failed: %s\n", net_error_string());
        return 1;
    }
    char buf[16] = {0};
    long n = net_recv(client, buf, sizeof(buf));
    fails += check("server->client message", n == 4 && memcmp(buf, "ping", 4) == 0);

    // client answers pong
    if (net_send_all(client, "pong", 4) != 4) {
        fprintf(stderr, "client send failed: %s\n", net_error_string());
        return 1;
    }
    n = net_recv(peer, buf, sizeof(buf));
    fails += check("client->server message", n == 4 && memcmp(buf, "pong", 4) == 0);

    // closing the peer must surface as a clean EOF on the other side
    net_close(client);
    n = net_recv(peer, buf, sizeof(buf));
    fails += check("close seen as EOF", n == 0);

    // a connected socket with nothing coming in eventually times out instead
    // of blocking the thread forever (slowloris backstop)
    Socket_Handle srv2 = net_listen(0);
    int port2 = net_bound_port(srv2);
    Socket_Handle client2 = net_connect("127.0.0.1", port2);
    Socket_Handle peer2 = net_accept(srv2);
    if (net_set_timeout(peer2, 200) != 0) {
        fprintf(stderr, "net_set_timeout failed: %s\n", net_error_string());
        return 1;
    }
    n = net_recv(peer2, buf, sizeof(buf));
    fails += check("empty socket times out", n == NET_READ_TIMEOUT);

    // and after the timeout the socket is still usable once data shows up
    if (net_send_all(client2, "late", 4) != 4) {
        fprintf(stderr, "late send failed: %s\n", net_error_string());
        return 1;
    }
    n = net_recv(peer2, buf, sizeof(buf));
    fails += check("socket alive after timeout", n == 4 && memcmp(buf, "late", 4) == 0);

    net_close(client2);
    net_close(peer2);
    net_close(srv2);

    net_close(peer);
    net_close(srv);
    net_cleanup();

    if (fails == 0) {
        printf("net ok\n");
    }
    return fails != 0;
}