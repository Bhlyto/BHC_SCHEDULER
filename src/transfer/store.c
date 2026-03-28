#include "transfer.h"
#include "config.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#  define SEP "\\"
#else
#  include <unistd.h>
#  define MKDIR(p) mkdir(p, 0755)
#  define SEP "/"
#endif

static void make_dir(const char *path)
{
    /* Create each component of path if missing */
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp)-1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            MKDIR(tmp);
            *p = SEP[0];
        }
    }
    MKDIR(tmp);
}

void store_input_dir(const char *job_id, char *buf, int len)
{
    snprintf(buf, len, "%s%s%s%sinput", g_config.work_dir, SEP, job_id, SEP);
}

void store_output_dir(const char *job_id, char *buf, int len)
{
    snprintf(buf, len, "%s%s%s%soutput", g_config.work_dir, SEP, job_id, SEP);
}

void store_stdout_path(const char *job_id, char *buf, int len)
{
    snprintf(buf, len, "%s%s%s%sstdout.log", g_config.work_dir, SEP, job_id, SEP);
}

void store_stderr_path(const char *job_id, char *buf, int len)
{
    snprintf(buf, len, "%s%s%s%sstderr.log", g_config.work_dir, SEP, job_id, SEP);
}

int store_init_job_dirs(const char *job_id)
{
    char path[512];
    store_input_dir(job_id, path, sizeof(path));
    make_dir(path);
    store_output_dir(job_id, path, sizeof(path));
    make_dir(path);
    log_debug("store", "Created dirs for job %s", job_id);
    return 0;
}

/* Copy a single file from src to dst (binary-safe). */
static int copy_file(const char *src, const char *dst)
{
    FILE *fin = fopen(src, "rb");
    if (!fin) return -1;
    FILE *fout = fopen(dst, "wb");
    if (!fout) { fclose(fin); return -1; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0)
        fwrite(buf, 1, n, fout);
    fclose(fin);
    fclose(fout);
    return 0;
}

int store_forward_outputs(const char *parent_job_id, const char *child_job_id)
{
    char src_dir[512], dst_dir[512];
    store_output_dir(parent_job_id, src_dir, sizeof(src_dir));
    store_input_dir(child_job_id,   dst_dir, sizeof(dst_dir));
    make_dir(dst_dir);

    int copied = 0;
#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*", src_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char src_path[512], dst_path[512];
        snprintf(src_path, sizeof(src_path), "%s\\%s", src_dir, fd.cFileName);
        snprintf(dst_path, sizeof(dst_path), "%s\\%s", dst_dir, fd.cFileName);
        if (copy_file(src_path, dst_path) == 0) copied++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(src_dir);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char src_path[512], dst_path[512];
        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, e->d_name);
        struct stat st;
        if (stat(src_path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, e->d_name);
        if (copy_file(src_path, dst_path) == 0) copied++;
    }
    closedir(d);
#endif
    if (copied > 0)
        log_info("store", "Forwarded %d file(s) from job %s output to job %s input",
                 copied, parent_job_id, child_job_id);
    return copied;
}

#ifdef _WIN32
static void rmdir_r(const char *path)
{
    char pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
            char sub[512];
            snprintf(sub, sizeof(sub), "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                rmdir_r(sub);
            else
                DeleteFileA(sub);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    RemoveDirectoryA(path);
}
#else
#include <dirent.h>
static void rmdir_r(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    char sub[512];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode))
            rmdir_r(sub);
        else
            remove(sub);
    }
    closedir(d);
    rmdir(path);
}
#endif

int store_cleanup_job(const char *job_id)
{
    char path[512];
    snprintf(path, sizeof(path), "%s%s%s", g_config.work_dir, SEP, job_id);
    rmdir_r(path);
    log_info("store", "Cleaned up job dir for %s", job_id);
    return 0;
}
