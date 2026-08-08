#include "config.h"
#include "db.h"
#include "events.h"
#include "resources.h"
#include "sqlite3.h"

#include <stdio.h>
#include <string.h>

void platform_service_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
}

void platform_request_stop(void)
{
}

int platform_stop_requested(void)
{
    return 1;
}

static void cleanup(void)
{
    remove("allocator_test.db");
    remove("allocator_test.db-wal");
    remove("allocator_test.db-shm");
    remove("allocator_machines.json");
}

static int allocation_row_count(const char *job_id)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    int count = -1;
    if (sqlite3_open("allocator_test.db", &db) != SQLITE_OK) return -1;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM allocations WHERE job_id=?;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, job_id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return count;
}

int main(void)
{
    cleanup();
    FILE *f = fopen("allocator_machines.json", "wb");
    if (!f) return 1;
    fputs("{\"machines\":["
          "{\"id\":\"node-a\",\"hostname\":\"localhost\",\"enabled\":true,"
          "\"cores\":4,\"cores_min\":1,\"ram_mb\":1024,\"disk_mb\":1024},"
          "{\"id\":\"node-b\",\"hostname\":\"localhost\",\"enabled\":true,"
          "\"cores\":4,\"cores_min\":1,\"ram_mb\":1024,\"disk_mb\":1024}"
          "]}", f);
    fclose(f);

    /* Seed the pre-v1 schema to verify the composite allocation-key migration. */
    sqlite3 *legacy = NULL;
    if (sqlite3_open("allocator_test.db", &legacy) != SQLITE_OK) {
        cleanup();
        return 1;
    }
    const char *legacy_sql =
        "CREATE TABLE allocations ("
        "job_id TEXT PRIMARY KEY,machine_id TEXT NOT NULL,"
        "cores INTEGER,gpu INTEGER,ram_mb INTEGER,disk_mb INTEGER,"
        "allocated_at INTEGER,released_at INTEGER);";
    if (sqlite3_exec(legacy, legacy_sql, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(legacy);
        cleanup();
        return 1;
    }
    sqlite3_close(legacy);

    config_defaults();
    events_init();
    if (db_open("allocator_test.db") != 0 ||
        registry_load("allocator_machines.json") != 2) {
        cleanup();
        return 1;
    }

    for (int i = 0; i < 1500; i++) {
        char job_id[37];
        char machine_id[64];
        snprintf(job_id, sizeof(job_id), "allocator-job-%d", i);
        if (alloc_reserve(job_id, 1, 0, 16, 16, machine_id) != 0 ||
            alloc_release(job_id) != 0) {
            fprintf(stderr, "allocation cycle %d failed\n", i);
            db_close();
            cleanup();
            return 1;
        }
    }

    int machine_count = 0;
    Machine *machines = registry_all(&machine_count);
    for (int i = 0; i < machine_count; i++) {
        if (machines[i].cores_reserved != 0 || machines[i].ram_mb_reserved != 0) {
            fprintf(stderr, "resources leaked after repeated allocations\n");
            db_close();
            cleanup();
            return 1;
        }
    }

    if (alloc_can_fit(4, 0, 0, 0) != 0) {
        fprintf(stderr, "cores_min was not respected\n");
        db_close();
        cleanup();
        return 1;
    }

    char machine_ids[1024];
    int n_machines = 0;
    if (alloc_reserve_multi("multi-job", 6, 0, 32, 32,
                            machine_ids, &n_machines) != 0 || n_machines != 2) {
        fprintf(stderr, "multi-machine reservation failed\n");
        db_close();
        cleanup();
        return 1;
    }
    if (allocation_row_count("multi-job") != 2) {
        fprintf(stderr, "multi-machine allocations were not persisted separately\n");
        db_close();
        cleanup();
        return 1;
    }
    if (alloc_release("multi-job") != 0) {
        fprintf(stderr, "multi-machine release failed\n");
        db_close();
        cleanup();
        return 1;
    }

    db_close();
    cleanup();
    return 0;
}
