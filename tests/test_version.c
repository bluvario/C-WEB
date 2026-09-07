#include <stdio.h>
#include <string.h>

#include "cweb.h"

int main(void)
{
    if (strcmp(cweb_version(), CWEB_VERSION) != 0) {
        fprintf(stderr, "version mismatch, got %s\n", cweb_version());
        return 1;
    }
    printf("cweb %s ok\n", cweb_version());
    return 0;
}