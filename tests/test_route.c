#include <stdio.h>
#include <string.h>

#include "route.h"
#include "strmap.h"
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
    Str_Map params;
    strmap_init(&params);

    Route_Def get_user = {HTTP_GET, "/users/<id>"};

    fails += check("parameterized match", route_match(get_user, HTTP_GET, sv_from_cstr("/users/42"), &params));
    fails += check("id captured", strcmp(strmap_get_cstr(&params, "id"), "42") == 0);

    strmap_free(&params);
    strmap_init(&params);

    Route_Def nested = {HTTP_POST, "/posts/<post_id>/comments"};
    fails += check("nested match", route_match(nested, HTTP_POST, sv_from_cstr("/posts/7/comments"), &params));
    fails += check("post_id captured", strcmp(strmap_get_cstr(&params, "post_id"), "7") == 0);
    strmap_free(&params);

    fails += check("wrong method rejected", !route_match(get_user, HTTP_POST, sv_from_cstr("/users/42"), NULL));
    fails += check("wrong path rejected", !route_match(get_user, HTTP_GET, sv_from_cstr("/usersx/42"), NULL));
    fails += check("extra segment rejected", !route_match(get_user, HTTP_GET, sv_from_cstr("/users/42/x"), NULL));
    fails += check("missing segment rejected", !route_match(get_user, HTTP_GET, sv_from_cstr("/users"), NULL));

    Route_Def about = {HTTP_GET, "/about"};
    fails += check("static match", route_match(about, HTTP_GET, sv_from_cstr("/about"), NULL));
    fails += check("trailing slash tolerated", route_match(about, HTTP_GET, sv_from_cstr("/about/"), NULL));
    fails += check("root matches", route_match((Route_Def){HTTP_GET, "/"}, HTTP_GET, sv_from_cstr("/"), NULL));

    fails += check("empty placeholder rejected",
        !route_match((Route_Def){HTTP_GET, "/a/<>/b"}, HTTP_GET, sv_from_cstr("/a/x/b"), NULL));

    if (fails == 0) {
        printf("route ok\n");
    }
    return fails != 0;
}