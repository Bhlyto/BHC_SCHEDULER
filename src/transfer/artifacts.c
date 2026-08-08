#include "transfer.h"
#include "db.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dirent.h>
#endif

int artifact_register_file(const char *job_id, const char *type,
                           const char *path)
{
    if (!job_id || !job_id[0] || !type || !type[0] || !path || !path[0])
        return -1;

    struct stat st;
    if (stat(path, &st) != 0) return 0;
#ifdef _WIN32
    if ((st.st_mode & _S_IFREG) == 0) return 0;
#else
    if (!S_ISREG(st.st_mode)) return 0;
#endif

    ArtifactRecord artifact;
    memset(&artifact, 0, sizeof(artifact));
    strncpy(artifact.job_id, job_id, sizeof(artifact.job_id) - 1);
    strncpy(artifact.type, type, sizeof(artifact.type) - 1);
    strncpy(artifact.uri, path, sizeof(artifact.uri) - 1);
    artifact.size_bytes = (long long)st.st_size;
    artifact.created_at = time(NULL);

    if (db_upsert_artifact(&artifact) != 0) {
        log_error("artifacts", "Failed to register %s for job %s", path, job_id);
        return -1;
    }
    return 1;
}

#ifdef _WIN32
static int collect_output_tree(const char *job_id, const char *dir, int depth)
{
    if (depth > 32) return -1;
    char pattern[1100];
    if (snprintf(pattern, sizeof(pattern), "%s\\*", dir) >= (int)sizeof(pattern))
        return -1;

    WIN32_FIND_DATAA fd;
    HANDLE handle = FindFirstFileA(pattern, &fd);
    if (handle == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char path[1100];
        if (snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName) >=
            (int)sizeof(path)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            int nested = collect_output_tree(job_id, path, depth + 1);
            if (nested < 0) { FindClose(handle); return -1; }
            count += nested;
        } else {
            int rc = artifact_register_file(job_id, "output", path);
            if (rc < 0) { FindClose(handle); return -1; }
            count += rc;
        }
    } while (FindNextFileA(handle, &fd));
    FindClose(handle);
    return count;
}
#else
static int collect_output_tree(const char *job_id, const char *dir, int depth)
{
    if (depth > 32) return -1;
    DIR *handle = opendir(dir);
    if (!handle) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char path[1100];
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >=
            (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            int nested = collect_output_tree(job_id, path, depth + 1);
            if (nested < 0) { closedir(handle); return -1; }
            count += nested;
        } else if (S_ISREG(st.st_mode)) {
            int rc = artifact_register_file(job_id, "output", path);
            if (rc < 0) { closedir(handle); return -1; }
            count += rc;
        }
    }
    closedir(handle);
    return count;
}
#endif

int artifact_collect_job(const char *job_id)
{
    char path[1024];
    int count = 0;

    store_stdout_path(job_id, path, sizeof(path));
    int rc = artifact_register_file(job_id, "stdout", path);
    if (rc < 0) return -1;
    count += rc;

    store_stderr_path(job_id, path, sizeof(path));
    rc = artifact_register_file(job_id, "stderr", path);
    if (rc < 0) return -1;
    count += rc;

    store_output_dir(job_id, path, sizeof(path));
    rc = collect_output_tree(job_id, path, 0);
    if (rc < 0) return -1;
    count += rc;
    log_info("artifacts", "Registered %d artifact(s) for job %s", count, job_id);
    return count;
}
