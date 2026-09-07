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

    net_close(peer);
    net_close(srv);
    net_cleanup();

    if (fails == 0) {
        printf("net ok\n");
    }
    return fails != 0;
}