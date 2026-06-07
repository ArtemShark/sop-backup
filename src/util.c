#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

int path_real(const char *in, char *out, size_t out_sz)
{
    char tmp[PATH_MAX];

    if (!realpath(in, tmp))
        return -1;
    if (strlen(tmp) + 1 > out_sz)
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(out, tmp);
    return 0;
}

int path_is_prefix_dir(const char *parent, const char *child)
{
    size_t lp = strlen(parent);

    if (lp == 0)
        return 0;
    if (strncmp(parent, child, lp) != 0)
        return 0;

    if (child[lp] == '\0')
        return 1;
    if (parent[lp - 1] == '/')
        return 1;
    return child[lp] == '/';
}

int safe_mkdir_p(const char *path, mode_t mode)
{
    char buf[PATH_MAX];

    if (strlen(path) >= sizeof(buf))
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(buf, path);

    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/')
    {
        buf[n - 1] = '\0';
        n--;
    }

    for (char *p = buf + 1; *p; p++)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (mkdir(buf, mode) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }

    if (mkdir(buf, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

int ensure_dir_exists(const char *path, mode_t mode)
{
    struct stat st;

    if (stat(path, &st) == 0)
    {
        if (!S_ISDIR(st.st_mode))
        {
            errno = ENOTDIR;
            return -1;
        }
        return 0;
    }

    if (errno != ENOENT)
        return -1;
    return safe_mkdir_p(path, mode);
}

int dir_is_empty(const char *path)
{
    DIR *d = opendir(path);
    if (!d)
        return -1;

    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
        {
            closedir(d);
            return 0;
        }
    }

    closedir(d);
    return 1;
}

int safe_write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    size_t left = n;

    while (left > 0)
    {
        ssize_t w = write(fd, p, left);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += (size_t)w;
        left -= (size_t)w;
    }

    return 0;
}

static int remove_tree_rec(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return -1;

    if (S_ISDIR(st.st_mode))
    {
        DIR *d = opendir(path);
        if (!d)
            return -1;

        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;

            char child[PATH_MAX];
            if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child))
            {
                closedir(d);
                errno = ENAMETOOLONG;
                return -1;
            }

            if (remove_tree_rec(child) < 0)
            {
                closedir(d);
                return -1;
            }
        }

        closedir(d);
        return rmdir(path);
    }

    return unlink(path);
}

int remove_tree(const char *path) { return remove_tree_rec(path); }
