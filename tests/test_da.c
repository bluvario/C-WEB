#include <stdio.h>
#include <string.h>

#include "da.h"

typedef struct {
    int *items;
    size_t count;
    size_t capacity;
} Int_Array;

typedef struct {
    size_t id;
    const char *name;
} Record;

typedef struct {
    Record *items;
    size_t count;
    size_t capacity;
} Record_Array;

int main(void)
{
    Int_Array ints = {0};
    for (int i = 0; i < 100000; i++) {
        da_append(&ints, i);
    }
    if (ints.count != 100000) {
        fprintf(stderr, "ints.count=%zu, want 100000\n", ints.count);
        return 1;
    }
    for (int i = 0; i < 100000; i++) {
        if (ints.items[i] != i) {
            fprintf(stderr, "ints[%d]=%d\n", i, ints.items[i]);
            return 1;
        }
    }
    da_free(&ints);

    Record_Array recs = {0};
    for (size_t i = 0; i < 1000; i++) {
        Record r = {i, "cweb"};
        da_append(&recs, r);
    }
    if (recs.count != 1000) {
        fprintf(stderr, "recs.count=%zu, want 1000\n", recs.count);
        return 1;
    }
    for (size_t i = 0; i < 1000; i++) {
        if (recs.items[i].id != i || strcmp(recs.items[i].name, "cweb") != 0) {
            fprintf(stderr, "recs[%zu] corrupted\n", i);
            return 1;
        }
    }
    da_free(&recs);

    printf("da ok\n");
    return 0;
}