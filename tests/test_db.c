#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "sv.h"
#include "thread.h"

static int fails = 0;

static void check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

typedef struct {
    Cweb_Db *db;
    long id;
} WriterArg;

static void writer(void *arg)
{
    // each thread owns an id and stamps a run of distinct keys; the db
    // serializes writes so every value lands
    WriterArg *wa = arg;
    for (int i = 0; i < 200; i++) {
        char key[32], val[32];
        snprintf(key, sizeof key, "t%ld-%d", wa->id, i);
        snprintf(val, sizeof val, "v%ld-%d", wa->id, i);
        cweb_db_put(wa->db, sv_from_cstr(key), sv_from_cstr(val));
    }
}

int main(void)
{
    char dir[64];
    strcpy(dir, "/tmp/cweb_db_test_XXXXXX");
    if (mkdtemp(dir) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/store.db", dir);

    Cweb_Db db;

    // a missing file opens as a fresh empty store
    check("missing file opens fresh", cweb_db_open(&db, path) == 0);
    check("fresh db has nothing", cweb_db_get(&db, sv_from_cstr("name")).data == NULL);
    check("delete from empty db fails", cweb_db_delete(&db, sv_from_cstr("name")) == -1);

    // put / get round-trip
    check("put stores a value",
          cweb_db_put(&db, sv_from_cstr("name"), sv_from_cstr("cweb")) == 0);
    check("get finds the value",
          sv_equal(cweb_db_get(&db, sv_from_cstr("name")), sv_from_cstr("cweb")));
    check("absent key still absent",
          cweb_db_get(&db, sv_from_cstr("missing")).data == NULL);

    // putting the same key replaces the old value
    cweb_db_put(&db, sv_from_cstr("name"), sv_from_cstr("cweb2"));
    check("put replaces a value",
          sv_equal(cweb_db_get(&db, sv_from_cstr("name")), sv_from_cstr("cweb2")));
    check("store grew to one entry", db.count == 1);

    // keys and values may hold arbitrary bytes, including embedded NULs
    String_View bkey = {sv_from_cstr("bi\0n").data, 4};
    String_View bval = {sv_from_cstr("va\0l").data, 4};
    check("binary value stored", cweb_db_put(&db, bkey, bval) == 0);
    String_View got = cweb_db_get(&db, bkey);
    check("binary value round-trips",
          got.count == 4 && memcmp(got.data, "va\0l", 4) == 0);

    // sync persists; a fresh open reads the same state back
    check("sync writes the file", cweb_db_sync(&db) == 0);
    check("sync clears the dirty flag", db.dirty == 0);
    Cweb_Db db2;
    check("reopen reads what sync wrote", cweb_db_open(&db2, path) == 0);
    check("persisted key survives open",
          sv_equal(cweb_db_get(&db2, sv_from_cstr("name")), sv_from_cstr("cweb2")));
    got = cweb_db_get(&db2, bkey);
    check("persisted binary value survives open",
          got.count == 4 && memcmp(got.data, "va\0l", 4) == 0);
    cweb_db_close(&db2);

    // delete behaves and the deletion also persists
    check("delete removes a key", cweb_db_delete(&db, bkey) == 0);
    check("deleted key is gone", cweb_db_get(&db, bkey).data == NULL);
    check("double delete fails", cweb_db_delete(&db, bkey) == -1);
    check("resolution honors dirty state",
          cweb_db_sync(&db) == 0 && !cweb_db_get(&db, bkey).data);

    // wiping the store removes the file, so the next open is fresh again
    check("delete the last key", cweb_db_delete(&db, sv_from_cstr("name")) == 0);
    check("empty db sync removes the file", cweb_db_sync(&db) == 0);
    check("file is gone", access(path, F_OK) != 0);
    cweb_db_close(&db);
    check("closed handle is reusable", cweb_db_open(&db, path) == 0);
    check("wiped store opens empty", cweb_db_get(&db, sv_from_cstr("name")).data == NULL);
    cweb_db_close(&db);

    // a file that is not ours is refused, and *db is left untouched
    snprintf(path, sizeof path, "%s/junk.db", dir);
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        fwrite("this is not a cweb store", 1, 24, f);
        fclose(f);
    }
    Cweb_Db junk;
    memset(&junk, 0, sizeof junk);
    check("corrupt file refused", cweb_db_open(&junk, path) == -1);
    check("failed open leaves handle untouched", junk.path == NULL);

    // concurrent writers: every key must land and read back correctly
    snprintf(path, sizeof path, "%s/sync.db", dir);
    Cweb_Db shared;
    check("concurrent db opens", cweb_db_open(&shared, path) == 0);
    Thread threads[4];
    WriterArg wa[4];
    for (long t = 0; t < 4; t++) {
        wa[t].db = &shared;
        wa[t].id = t;
        check("thread spawns", thread_init(&threads[t], writer, &wa[t]) == 0);
    }
    for (int t = 0; t < 4; t++) {
        check("thread joins", thread_join(&threads[t]) == 0);
    }
    check("all written keys present", shared.count == 800);
    char key[32];
    snprintf(key, sizeof key, "t3-199");
    check("concurrent value reads back",
          sv_equal(cweb_db_get(&shared, sv_from_cstr(key)), sv_from_cstr("v3-199")));
    check("concurrent store syncs", cweb_db_sync(&shared) == 0);
    cweb_db_close(&shared);

    unlink(path);
    char junk2[512];
    snprintf(junk2, sizeof junk2, "%s/store.db", dir);
    unlink(junk2);
    rmdir(dir);

    if (fails == 0) {
        printf("db ok\n");
        return 0;
    }
    fprintf(stderr, "%d FAILURES\n", fails);
    return 1;
}