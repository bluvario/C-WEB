#define _POSIX_C_SOURCE 200809L

#include "env.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// trims blanks from both ends of a span [b, e); returns the trimmed span
static void trim(const char **b, const char **e)
{
    while (*b < *e && isspace((unsigned char)**b)) {
        (*b)++;
    }
    while (*e > *b && isspace((unsigned char)(*(*e - 1)))) {
        (*e)--;
    }
}

// 1 when the span is exactly [A-Za-z_][A-Za-z0-9_]*
static int is_identifier(const char *b, const char *e)
{
    if (b == e || !(isalpha((unsigned char)*b) || *b == '_')) {
        return 0;
    }
    for (const char *p = b + 1; p < e; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) {
            return 0;
        }
    }
    return 1;
}

int cweb_env_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }

    char line[65536];
    while (fgets(line, sizeof line, f) != NULL) {
        // trim the trailing newline so it never lands in a value
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }

        const char *b = line;
        const char *e = line + n;
        trim(&b, &e);

        if (b == e || *b == '#') {
            continue; // blank or comment
        }

        // tolerate "export KEY=VALUE" so shell-style files load as-is
        if (e - b >= 7 && strncmp(b, "export ", 7) == 0) {
            b += 7;
            trim(&b, &e);
        }

        const char *eq = memchr(b, '=', (size_t)(e - b));
        if (eq == NULL) {
            continue; // no separator: not a KEY=VALUE line
        }

        const char *key_b = b;
        const char *key_e = eq;
        trim(&key_b, &key_e);
        if (!is_identifier(key_b, key_e)) {
            continue;
        }

        const char *val_b = eq + 1;
        const char *val_e = e;
        trim(&val_b, &val_e);

        // strip one layer of surrounding quotes; nothing inside is escaped
        if (val_e - val_b >= 2 &&
            ((*val_b == '"' && *(val_e - 1) == '"') ||
             (*val_b == '\'' && *(val_e - 1) == '\''))) {
            val_b++;
            val_e--;
        }

        size_t key_len = (size_t)(key_e - key_b);
        size_t val_len = (size_t)(val_e - val_b);
        char *key = malloc(key_len + 1);
        char *val = malloc(val_len + 1);
        if (key == NULL || val == NULL) {
            free(key);
            free(val);
            fclose(f);
            return -1;
        }
        memcpy(key, key_b, key_len);
        key[key_len] = '\0';
        memcpy(val, val_b, val_len);
        val[val_len] = '\0';

        // existing environment wins: the file is a default, not an override
        setenv(key, val, 0);
        free(key);
        free(val);
    }

    fclose(f);
    return 0;
}