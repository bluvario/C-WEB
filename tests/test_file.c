#include <stdio.h>
#include <string.h>

#include "file.h"
#include "xmem.h"

int main(void)
{
    char *buf;
    size_t len;

    if (file_read_all("README.md", &buf, &len) != 0) {
        fprintf(stderr, "could not read README.md\n");
        return 1;
    }
    if (strncmp(buf, "# C-WEB", 7) != 0) {
        fprintf(stderr, "README.md starts with wrong content\n");
        return 1;
    }
    if (buf[len] != '\0') {
        fprintf(stderr, "buffer not NUL terminated\n");
        return 1;
    }
    xfree(buf);

    if (file_read_all("no/such/file.txt", &buf, &len) == 0) {
        fprintf(stderr, "reading a missing file should fail\n");
        return 1;
    }

    printf("file ok\n");
    return 0;
}