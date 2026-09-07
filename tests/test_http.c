#include <stdio.h>
#include <string.h>

#include "http.h"
#include "sv.h"

int main(void)
{
    if (http_status_reason(HTTP_200_OK) == NULL ||
        strcmp(http_status_reason(HTTP_200_OK), "OK") != 0) {
        fprintf(stderr, "bad reason for 200\n");
        return 1;
    }
    if (http_status_reason(HTTP_404_NOT_FOUND) == NULL ||
        strcmp(http_status_reason(HTTP_404_NOT_FOUND), "Not Found") != 0) {
        fprintf(stderr, "bad reason for 404\n");
        return 1;
    }
    if (strcmp(http_status_reason(HTTP_500_INTERNAL_SERVER_ERROR), "Internal Server Error") != 0) {
        fprintf(stderr, "bad reason for 500\n");
        return 1;
    }
    if (strcmp(http_status_reason(599), "Unknown") != 0) {
        fprintf(stderr, "unexpected reason for unknown status\n");
        return 1;
    }

    if (http_method_from_sv(sv_from_cstr("GET")) != HTTP_GET) {
        fprintf(stderr, "GET did not parse\n");
        return 1;
    }
    if (http_method_from_sv(sv_from_cstr("DELETE")) != HTTP_DELETE) {
        fprintf(stderr, "DELETE did not parse\n");
        return 1;
    }
    if (http_method_from_sv(sv_from_cstr("get")) != HTTP_UNKNOWN_METHOD) {
        fprintf(stderr, "lowercase get must not match, methods are case-sensitive\n");
        return 1;
    }
    if (http_method_from_sv(sv_from_cstr("BANANA")) != HTTP_UNKNOWN_METHOD) {
        fprintf(stderr, "BANANA unexpectedly parsed\n");
        return 1;
    }
    if (strcmp(http_method_name(HTTP_GET), "GET") != 0 ||
        http_method_name(HTTP_UNKNOWN_METHOD) != NULL) {
        fprintf(stderr, "method name lookup broken\n");
        return 1;
    }

    printf("http ok\n");
    return 0;
}