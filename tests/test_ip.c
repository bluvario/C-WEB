#include <stdio.h>
#include <string.h>

#include "ip.h"
#include "sv.h"

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

    Http_Cidr c;
    fails += check("parse bare v4", http_cidr_parse(&c, "1.2.3.4") == 0 &&
                                       c.v6 == 0 && c.prefix == 32);
    fails += check("parse v4 /24",
        http_cidr_parse(&c, "1.2.3.0/24") == 0 && c.prefix == 24);
    fails += check("v4 /24 contains", http_cidr_contains(&c, sv_from_cstr("1.2.3.99")) == 1);
    fails += check("v4 /24 excludes", http_cidr_contains(&c, sv_from_cstr("1.2.4.1")) == 0);
    fails += check("v4 /24 excludes broadcast side",
        http_cidr_contains(&c, sv_from_cstr("1.3.0.1")) == 0);

    fails += check("parse v4 /8",
        http_cidr_parse(&c, "10.0.0.0/8") == 0);
    fails += check("v4 /8 contains top", http_cidr_contains(&c, sv_from_cstr("10.255.255.255")) == 1);
    fails += check("v4 /8 excludes 11/8", http_cidr_contains(&c, sv_from_cstr("11.0.0.1")) == 0);

    // a partial-byte prefix boundary: 10.0.0.0/9 is 10.0.0.0 - 10.127.255.255
    fails += check("parse v4 /9",
        http_cidr_parse(&c, "10.0.0.0/9") == 0);
    fails += check("v4 /9 contains 10.127.0.1",
        http_cidr_contains(&c, sv_from_cstr("10.127.0.1")) == 1);
    fails += check("v4 /9 excludes 10.128.0.1",
        http_cidr_contains(&c, sv_from_cstr("10.128.0.1")) == 0);

    fails += check("parse v4 /0",
        http_cidr_parse(&c, "0.0.0.0/0") == 0);
    fails += check("v4 /0 contains anything", http_cidr_contains(&c, sv_from_cstr("203.0.113.9")) == 1);

    fails += check("parse bare v6",
        http_cidr_parse(&c, "::1") == 0 && c.v6 == 1 && c.prefix == 128);
    fails += check("v6 host route contains", http_cidr_contains(&c, sv_from_cstr("::1")) == 1);
    fails += check("v6 host route excludes", http_cidr_contains(&c, sv_from_cstr("::2")) == 0);

    fails += check("parse v6 /10",
        http_cidr_parse(&c, "fe80::/10") == 0);
    fails += check("v6 /10 contains fe80::1",
        http_cidr_contains(&c, sv_from_cstr("fe80::1")) == 1);
    fails += check("v6 /10 excludes 2001:db8::1",
        http_cidr_contains(&c, sv_from_cstr("2001:db8::1")) == 0);

    fails += check("parse v6 /0",
        http_cidr_parse(&c, "::/0") == 0);
    fails += check("v6 /0 contains", http_cidr_contains(&c, sv_from_cstr("2001:db8::1")) == 1);

    // families never mix
    fails += check("v4 address in v6 network is outside",
        http_cidr_contains(&c, sv_from_cstr("192.168.1.1")) == 0);
    fails += check("v6 address in v4 network is outside",
        http_cidr_contains(&(Http_Cidr){.addr = {0},
                                        .prefix = 0, .v6 = 0},
                           sv_from_cstr("::1")) == 0);

    // malformed networks and addresses
    static const char *bad_nets[] = {
        "", "1.2.3", "1.2.3.4/33", "::1/129", "1.2.3.4/x", "1.2.3.4/-1",
        "1.2.3.4/24/8", "/8", "300.1.2.3", "fe80:z::1/8",
    };
    for (size_t i = 0; i < sizeof bad_nets / sizeof bad_nets[0]; i++) {
        fails += check("malformed network rejected",
            http_cidr_parse(&c, bad_nets[i]) == -1);
    }

    // unparsable address text is distinguishable from a plain miss
    fails += check("garbage ip is not a miss",
        http_cidr_contains(&(Http_Cidr){.addr = {0}, .prefix = 0, .v6 = 0},
                           sv_from_cstr("not-an-ip")) == -1);

    if (fails == 0) {
        printf("ip ok\n");
    }
    return fails != 0;
}