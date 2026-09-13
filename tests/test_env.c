#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "env.h"

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
    unsetenv("CWEB_TEST_VAR_ONE");
    unsetenv("CWEB_TEST_VAR_TWO");
    unsetenv("CWEB_TEST_EMPTY");
    unsetenv("CWEB_TEST_QUOTED");
    unsetenv("CWEB_TEST_JUNK");
    unsetenv("CWEB_TEST_EXPORTED");

    // missing file is an error, not silence
    fails += check("missing file", cweb_env_file("/nonexistent/cweb.env") == -1);

    // a well-formed file loads every entry
    fails += check("load good", cweb_env_file("tests/env_good.env") == 0);
    fails += check("plain", strcmp(getenv("CWEB_TEST_VAR_ONE"), "hello") == 0);
    fails += check("spaced", strcmp(getenv("CWEB_TEST_VAR_TWO"), "with spaces") == 0);
    fails += check("quoted", strcmp(getenv("CWEB_TEST_QUOTED"), "a value with spaces") == 0);
    fails += check("empty value", getenv("CWEB_TEST_EMPTY") != NULL &&
                                  strcmp(getenv("CWEB_TEST_EMPTY"), "") == 0);

    // an existing environment entry wins over the file
    setenv("CWEB_TEST_EXISTING", "from-shell", 1);
    fails += check("load again", cweb_env_file("tests/env_good.env") == 0);
    fails += check("shell wins", strcmp(getenv("CWEB_TEST_EXISTING"), "from-shell") == 0);
    unsetenv("CWEB_TEST_EXISTING");

    // junk lines are skipped without failing the file
    fails += check("load ragged", cweb_env_file("tests/env_ragged.env") == 0);
    fails += check("export prefix", strcmp(getenv("CWEB_TEST_EXPORTED"), "glob-trotter") == 0);
    fails += check("junk skipped", getenv("CWEB_TEST_JUNK") == NULL);

    if (fails == 0) {
        printf("env ok\n");
    }
    return fails != 0;
}