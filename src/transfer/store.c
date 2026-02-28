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

/*
 * store.c
 * Manage the filesystem workspace for each job:
 *   <work_dir>/<job_id>/input/
 *   <work_dir>/<job_id>/output/
 */

static void make_dir(const char *path)
{
    /* Create each component of path if missing */
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp)-1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            MKDIR(tmp);  /* ignore errors — dir may already exist */
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

/* Recursively delete a directory tree (portable simple version) */
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
