#include "config.h"
#include "db.h"
#include "transfer.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  define TEST_SEP "\\"
#else
#  define TEST_SEP "/"
#endif

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
    store_cleanup_job("artifact-job");
    remove("artifact_test.db");
    remove("artifact_test.db-wal");
    remove("artifact_test.db-shm");
}

static int write_file(const char *path, const char *contents)
{
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    size_t length = strlen(contents);
    int ok = fwrite(contents, 1, length, file) == length ? 0 : -1;
    fclose(file);
    return ok;
}

int main(void)
{
    config_defaults();
    strcpy(g_config.work_dir, "artifact_test_work");
    cleanup();
    if (db_open("artifact_test.db") != 0) return 1;
    if (store_init_job_dirs("artifact-job") != 0) return 1;

    char stdout_path[1024], stderr_path[1024], output_dir[1024], output_path[1024];
    store_stdout_path("artifact-job", stdout_path, sizeof(stdout_path));
    store_stderr_path("artifact-job", stderr_path, sizeof(stderr_path));
    store_output_dir("artifact-job", output_dir, sizeof(output_dir));
    snprintf(output_path, sizeof(output_path), "%s%sresult.bin", output_dir, TEST_SEP);

    if (write_file(stdout_path, "standard output\n") != 0 ||
        write_file(stderr_path, "error output\n") != 0 ||
        write_file(output_path, "123456789") != 0) return 1;

    if (artifact_collect_job("artifact-job") != 3 ||
        artifact_collect_job("artifact-job") != 3) return 1;

    ArtifactRecord items[8];
    int count = db_list_artifacts("artifact-job", items, 8);
    if (count != 3) {
        fprintf(stderr, "expected 3 idempotent artifacts, got %d\n", count);
        return 1;
    }

    int stdout_seen = 0, stderr_seen = 0, output_seen = 0;
    for (int i = 0; i < count; i++) {
        if (items[i].id <= 0 || items[i].created_at <= 0 || !items[i].uri[0]) return 1;
        if (!strcmp(items[i].type, "stdout") && items[i].size_bytes == 16) stdout_seen++;
        if (!strcmp(items[i].type, "stderr") && items[i].size_bytes == 13) stderr_seen++;
        if (!strcmp(items[i].type, "output") && items[i].size_bytes == 9) output_seen++;
    }
    if (stdout_seen != 1 || stderr_seen != 1 || output_seen != 1) return 1;

    db_close();
    cleanup();
    return 0;
}
