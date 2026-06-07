#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "copy_sync.h"
#include "util.h"

#define META_NAME ".backup_meta"

volatile sig_atomic_t g_copy_stop = 0;

static void on_term(int sig)
{
    (void)sig;
    g_copy_stop = 1;
}

static int ensure_parent_dir(const char *path)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return 0;
    *slash = '\0';
    return safe_mkdir_p(tmp, 0755);
}

static int copy_file(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0)
    {
        close(in);
        return -1;
    }

    char buf[64 * 1024];
    while (!g_copy_stop)
    {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            close(in);
            close(out);
            return -1;
        }
        if (r == 0)
            break;

        if (safe_write_all(out, buf, (size_t)r) < 0)
        {
            close(in);
            close(out);
            return -1;
        }
    }

    close(in);
    close(out);

    if (g_copy_stop)
    {
        errno = EINTR;
        return -1;
    }

    return 0;
}

static int copy_symlink_adjust(const char *src_root, const char *dst_root_for_links, const char *src_link_path,
                               const char *dst_link_path)
{
    char linkbuf[PATH_MAX];
    ssize_t n = readlink(src_link_path, linkbuf, sizeof(linkbuf) - 1);
    if (n < 0)
        return -1;
    linkbuf[n] = '\0';

    if (linkbuf[0] == '/' && path_is_prefix_dir(src_root, linkbuf))
    {
        const char *rest = linkbuf + strlen(src_root);
        char newtarget[PATH_MAX];
        if (snprintf(newtarget, sizeof(newtarget), "%s%s", dst_root_for_links, rest) >= (int)sizeof(newtarget))
        {
            errno = ENAMETOOLONG;
            return -1;
        }
        (void)unlink(dst_link_path);
        return symlink(newtarget, dst_link_path);
    }

    (void)unlink(dst_link_path);
    return symlink(linkbuf, dst_link_path);
}

static int copy_tree_rec(const char *src_root, const char *dst_root_for_links, const char *src_path,
                         const char *dst_path)
{
    if (g_copy_stop)
    {
        errno = EINTR;
        return -1;
    }

    struct stat st;
    if (lstat(src_path, &st) < 0)
        return -1;

    if (S_ISDIR(st.st_mode))
    {
        if (mkdir(dst_path, st.st_mode & 0777) < 0 && errno != EEXIST)
            return -1;

        DIR *d = opendir(src_path);
        if (!d)
            return -1;

        struct dirent *e;
        while (!g_copy_stop && (e = readdir(d)) != NULL)
        {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;

            char s_child[PATH_MAX], d_child[PATH_MAX];
            if (snprintf(s_child, sizeof(s_child), "%s/%s", src_path, e->d_name) >= (int)sizeof(s_child) ||
                snprintf(d_child, sizeof(d_child), "%s/%s", dst_path, e->d_name) >= (int)sizeof(d_child))
            {
                closedir(d);
                errno = ENAMETOOLONG;
                return -1;
            }

            if (copy_tree_rec(src_root, dst_root_for_links, s_child, d_child) < 0)
            {
                closedir(d);
                return -1;
            }
        }

        closedir(d);
        if (g_copy_stop)
        {
            errno = EINTR;
            return -1;
        }
        return 0;
    }

    if (S_ISREG(st.st_mode))
    {
        if (ensure_parent_dir(dst_path) < 0)
            return -1;
        return copy_file(src_path, dst_path, st.st_mode & 0777);
    }

    if (S_ISLNK(st.st_mode))
    {
        if (ensure_parent_dir(dst_path) < 0)
            return -1;
        return copy_symlink_adjust(src_root, dst_root_for_links, src_path, dst_path);
    }

    return 0;
}

int full_copy_tree(const char *src_root, const char *final_dst_root)
{
    char parent[PATH_MAX];
    char tmpdir[PATH_MAX];

    struct sigaction old_term, sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    (void)sigaction(SIGTERM, &sa, &old_term);

    g_copy_stop = 0;

    if (snprintf(parent, sizeof(parent), "%s", final_dst_root) >= (int)sizeof(parent))
    {
        errno = ENAMETOOLONG;
        (void)sigaction(SIGTERM, &old_term, NULL);
        return -1;
    }

    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent)
    {
        errno = EINVAL;
        (void)sigaction(SIGTERM, &old_term, NULL);
        return -1;
    }
    *slash = '\0';

    if (safe_mkdir_p(parent, 0755) < 0)
    {
        (void)sigaction(SIGTERM, &old_term, NULL);
        return -1;
    }

    if (snprintf(tmpdir, sizeof(tmpdir), "%s/.sop-backup-tmp.%ld.%ld", parent, (long)getpid(), (long)time(NULL)) >=
        (int)sizeof(tmpdir))
    {
        errno = ENAMETOOLONG;
        (void)sigaction(SIGTERM, &old_term, NULL);
        return -1;
    }

    if (mkdir(tmpdir, 0755) < 0)
    {
        (void)sigaction(SIGTERM, &old_term, NULL);
        return -1;
    }

    if (copy_tree_rec(src_root, final_dst_root, src_root, tmpdir) < 0)
    {
        (void)remove_tree(tmpdir);
        (void)sigaction(SIGTERM, &old_term, NULL);
        return -1;
    }

    if (rename(tmpdir, final_dst_root) < 0)
    {
        int saved = errno;

        if (saved == EEXIST || saved == ENOTEMPTY || saved == EISDIR)
        {
            (void)rmdir(final_dst_root);
            if (rename(tmpdir, final_dst_root) < 0)
            {
                (void)remove_tree(tmpdir);
                (void)sigaction(SIGTERM, &old_term, NULL);
                return -1;
            }
        }
        else
        {
            (void)remove_tree(tmpdir);
            errno = saved;
            (void)sigaction(SIGTERM, &old_term, NULL);
            return -1;
        }
    }

    (void)sigaction(SIGTERM, &old_term, NULL);
    return 0;
}

int write_backup_meta(const char *backup_root, const char *src_real, time_t created_at)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", backup_root, META_NAME) >= (int)sizeof(path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;

    char buf[PATH_MAX + 128];
    int n = snprintf(buf, sizeof(buf), "SOURCE=%s\nCREATED_AT=%ld\n", src_real, (long)created_at);
    if (n < 0 || n >= (int)sizeof(buf))
    {
        close(fd);
        errno = EOVERFLOW;
        return -1;
    }

    if (safe_write_all(fd, buf, (size_t)n) < 0)
    {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int read_backup_meta(const char *backup_root, char *src_out, size_t src_out_sz, time_t *created_at_out)
{
    if (!src_out || src_out_sz == 0 || !created_at_out)
    {
        errno = EINVAL;
        return -1;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", backup_root, META_NAME) >= (int)sizeof(path))
    {
        errno = ENAMETOOLONG;
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[PATH_MAX + 256];
    src_out[0] = '\0';
    *created_at_out = 0;

    while (fgets(line, sizeof(line), f))
    {
        if (strncmp(line, "SOURCE=", 7) == 0)
        {
            char *v = line + 7;
            v[strcspn(v, "\r\n")] = '\0';
            snprintf(src_out, src_out_sz, "%s", v);
        }
        else if (strncmp(line, "CREATED_AT=", 11) == 0)
        {
            char *v = line + 11;
            *created_at_out = (time_t)atol(v);
        }
    }

    fclose(f);

    if (src_out[0] == '\0' || *created_at_out == 0)
    {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

typedef struct
{
    int wd;
    char path[PATH_MAX];
} watch_item_t;
typedef struct
{
    watch_item_t *items;
    size_t n, cap;
} watch_map_t;

static int add_watch_recursive(int inofd, watch_map_t *map, const char *dirpath);

static int watchmap_add(watch_map_t *m, int wd, const char *path)
{
    if (m->n == m->cap)
    {
        size_t nc = m->cap ? m->cap * 2 : 64;
        watch_item_t *ni = (watch_item_t *)realloc(m->items, nc * sizeof(*ni));
        if (!ni)
            return -1;
        m->items = ni;
        m->cap = nc;
    }
    m->items[m->n].wd = wd;
    snprintf(m->items[m->n].path, sizeof(m->items[m->n].path), "%s", path);
    m->n++;
    return 0;
}

static const char *watchmap_get(watch_map_t *m, int wd)
{
    for (size_t i = 0; i < m->n; i++)
    {
        if (m->items[i].wd == wd)
            return m->items[i].path;
    }
    return NULL;
}

static void watchmap_remove(watch_map_t *m, int wd)
{
    for (size_t i = 0; i < m->n; i++)
    {
        if (m->items[i].wd == wd)
        {
            m->items[i] = m->items[m->n - 1];
            m->n--;
            return;
        }
    }
}

static void watchmap_rebuild(int inofd, watch_map_t *map, const char *src_root)
{
    for (size_t i = 0; i < map->n; i++)
    {
        (void)inotify_rm_watch(inofd, map->items[i].wd);
    }
    map->n = 0;
    (void)add_watch_recursive(inofd, map, src_root);
}

static int add_watch_recursive(int inofd, watch_map_t *map, const char *dirpath)
{
    int wd = inotify_add_watch(inofd, dirpath,
                               IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO |
                                   IN_DELETE_SELF | IN_MOVE_SELF | IN_Q_OVERFLOW);
    if (wd < 0)
        return -1;
    if (watchmap_add(map, wd, dirpath) < 0)
        return -1;

    DIR *d = opendir(dirpath);
    if (!d)
        return -1;

    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;

        char child[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", dirpath, e->d_name) >= (int)sizeof(child))
        {
            closedir(d);
            errno = ENAMETOOLONG;
            return -1;
        }

        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
        {
            (void)add_watch_recursive(inofd, map, child);
        }
    }

    closedir(d);
    return 0;
}

static int build_dst_path(const char *src_root, const char *dst_root, const char *src_abs, char *dst_abs, size_t dst_sz)
{
    if (!path_is_prefix_dir(src_root, src_abs))
    {
        errno = EINVAL;
        return -1;
    }
    const char *rest = src_abs + strlen(src_root);
    if (snprintf(dst_abs, dst_sz, "%s%s", dst_root, rest) >= (int)dst_sz)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

typedef struct
{
    uint32_t cookie;
    char old_src_abs[PATH_MAX];
} move_item_t;
typedef struct
{
    move_item_t items[256];
    size_t n;
} move_map_t;

static void movemap_put(move_map_t *m, uint32_t cookie, const char *old_src_abs)
{
    if (cookie == 0)
        return;
    if (m->n < 256)
    {
        m->items[m->n].cookie = cookie;
        snprintf(m->items[m->n].old_src_abs, sizeof(m->items[m->n].old_src_abs), "%s", old_src_abs);
        m->n++;
    }
}

static int movemap_take(move_map_t *m, uint32_t cookie, char *out, size_t out_sz)
{
    for (size_t i = 0; i < m->n; i++)
    {
        if (m->items[i].cookie == cookie)
        {
            snprintf(out, out_sz, "%s", m->items[i].old_src_abs);
            m->items[i] = m->items[m->n - 1];
            m->n--;
            return 0;
        }
    }
    return -1;
}

static int handle_create_or_modify(const char *src_root, const char *dst_root, const char *src_abs)
{
    struct stat st;
    if (lstat(src_abs, &st) < 0)
        return -1;

    char dst_abs[PATH_MAX];
    if (build_dst_path(src_root, dst_root, src_abs, dst_abs, sizeof(dst_abs)) < 0)
        return -1;

    if (S_ISDIR(st.st_mode))
    {
        if (mkdir(dst_abs, st.st_mode & 0777) < 0 && errno != EEXIST)
            return -1;
        return 0;
    }
    if (S_ISREG(st.st_mode))
    {
        if (ensure_parent_dir(dst_abs) < 0)
            return -1;
        return copy_file(src_abs, dst_abs, st.st_mode & 0777);
    }
    if (S_ISLNK(st.st_mode))
    {
        if (ensure_parent_dir(dst_abs) < 0)
            return -1;
        return copy_symlink_adjust(src_root, dst_root, src_abs, dst_abs);
    }
    return 0;
}

static int handle_delete(const char *src_root, const char *dst_root, const char *src_abs, int is_dir_hint)
{
    char dst_abs[PATH_MAX];
    if (build_dst_path(src_root, dst_root, src_abs, dst_abs, sizeof(dst_abs)) < 0)
        return -1;

    if (is_dir_hint)
    {
        if (rmdir(dst_abs) < 0)
        {
            if (remove_tree(dst_abs) < 0)
                return -1;
        }
    }
    else
    {
        if (unlink(dst_abs) < 0)
        {
            if (rmdir(dst_abs) < 0)
                return -1;
        }
    }
    return 0;
}

static int handle_move(const char *src_root, const char *dst_root, const char *old_src_abs, const char *new_src_abs)
{
    char old_dst[PATH_MAX], new_dst[PATH_MAX];
    if (build_dst_path(src_root, dst_root, old_src_abs, old_dst, sizeof(old_dst)) < 0)
        return -1;
    if (build_dst_path(src_root, dst_root, new_src_abs, new_dst, sizeof(new_dst)) < 0)
        return -1;

    (void)ensure_parent_dir(new_dst);
    if (rename(old_dst, new_dst) < 0)
    {
        (void)handle_create_or_modify(src_root, dst_root, new_src_abs);
        (void)handle_delete(src_root, dst_root, old_src_abs, 0);
    }
    return 0;
}

static void mirror_catch_up_dir(const char *src_root, const char *dst_root, const char *dir_abs)
{
    char dst_abs[PATH_MAX];
    if (build_dst_path(src_root, dst_root, dir_abs, dst_abs, sizeof(dst_abs)) < 0)
        return;
    (void)copy_tree_rec(src_root, dst_root, dir_abs, dst_abs);
}

int mirror_event_loop(const char *src_root, const char *dst_root)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    (void)sigaction(SIGTERM, &sa, NULL);

    int inofd = inotify_init1(IN_NONBLOCK);
    if (inofd < 0)
        return -1;

    watch_map_t map;
    memset(&map, 0, sizeof(map));

    if (add_watch_recursive(inofd, &map, src_root) < 0)
    {
        close(inofd);
        free(map.items);
        return -1;
    }

    move_map_t moves;
    memset(&moves, 0, sizeof(moves));

    char buf[64 * 1024];
    while (!g_copy_stop)
    {
        ssize_t r = read(inofd, buf, sizeof(buf));
        if (r < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
            {
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = 50L * 1000L * 1000L;
                (void)nanosleep(&ts, NULL);
                continue;
            }
            break;
        }
        if (r == 0)
            continue;

        size_t off = 0;
        while (off < (size_t)r)
        {
            struct inotify_event *ev = (struct inotify_event *)(buf + off);
            off += sizeof(*ev) + ev->len;

            if (ev->mask & IN_Q_OVERFLOW)
            {
                (void)full_copy_tree(src_root, dst_root);
                continue;
            }

            const char *base = watchmap_get(&map, ev->wd);
            if (!base)
                continue;

            int is_root_watch = (strcmp(base, src_root) == 0);

            char src_abs[PATH_MAX];
            if (ev->len > 0 && ev->name[0])
            {
                (void)snprintf(src_abs, sizeof(src_abs), "%s/%s", base, ev->name);
            }
            else
            {
                (void)snprintf(src_abs, sizeof(src_abs), "%s", base);
            }

            if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
            {
                if (is_root_watch)
                {
                    g_copy_stop = 1;
                    break;
                }

                (void)inotify_rm_watch(inofd, ev->wd);
                watchmap_remove(&map, ev->wd);
                continue;
            }

            int is_dir = (ev->mask & IN_ISDIR) != 0;

            if (ev->mask & IN_CREATE)
            {
                if (handle_create_or_modify(src_root, dst_root, src_abs) == 0)
                {
                    if (is_dir)
                    {
                        (void)add_watch_recursive(inofd, &map, src_abs);
                        mirror_catch_up_dir(src_root, dst_root, src_abs);
                    }
                }
            }
            else if (ev->mask & (IN_MODIFY | IN_ATTRIB))
            {
                (void)handle_create_or_modify(src_root, dst_root, src_abs);
            }
            else if (ev->mask & IN_DELETE)
            {
                (void)handle_delete(src_root, dst_root, src_abs, is_dir);
            }
            else if (ev->mask & IN_MOVED_FROM)
            {
                movemap_put(&moves, ev->cookie, src_abs);
            }
            else if (ev->mask & IN_MOVED_TO)
            {
                char old_src[PATH_MAX];

                if (movemap_take(&moves, ev->cookie, old_src, sizeof(old_src)) == 0)
                {
                    (void)handle_move(src_root, dst_root, old_src, src_abs);

                    if (is_dir)
                    {
                        watchmap_rebuild(inofd, &map, src_root);
                    }
                }
                else
                {
                    if (handle_create_or_modify(src_root, dst_root, src_abs) == 0)
                    {
                        if (is_dir)
                        {
                            (void)add_watch_recursive(inofd, &map, src_abs);
                            mirror_catch_up_dir(src_root, dst_root, src_abs);
                        }
                    }
                }
            }
        }
    }

    close(inofd);
    free(map.items);
    return 0;
}

static int restore_symlink_adjust(const char *src_root, const char *backup_root, const char *backup_link_path,
                                  const char *src_link_path)
{
    char linkbuf[PATH_MAX];
    ssize_t n = readlink(backup_link_path, linkbuf, sizeof(linkbuf) - 1);
    if (n < 0)
        return -1;
    linkbuf[n] = '\0';

    if (linkbuf[0] == '/' && path_is_prefix_dir(backup_root, linkbuf))
    {
        const char *rest = linkbuf + strlen(backup_root);
        char newtarget[PATH_MAX];
        if (snprintf(newtarget, sizeof(newtarget), "%s%s", src_root, rest) >= (int)sizeof(newtarget))
        {
            errno = ENAMETOOLONG;
            return -1;
        }
        (void)unlink(src_link_path);
        return symlink(newtarget, src_link_path);
    }

    (void)unlink(src_link_path);
    return symlink(linkbuf, src_link_path);
}

static int path_join(char *out, size_t out_sz, const char *a, const char *b)
{
    if (snprintf(out, out_sz, "%s/%s", a, b) >= (int)out_sz)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int remove_any(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return -1;
    if (S_ISDIR(st.st_mode))
        return remove_tree(path);
    return unlink(path);
}

static int restore_copy_pass(const char *src_root, const char *backup_root, const char *backup_path,
                             const char *src_path, time_t created_at)
{
    struct stat bst;
    if (lstat(backup_path, &bst) < 0)
        return -1;

    if (S_ISDIR(bst.st_mode))
    {
        struct stat sst;
        if (lstat(src_path, &sst) == 0 && !S_ISDIR(sst.st_mode))
        {
            (void)remove_any(src_path);
        }
        if (mkdir(src_path, bst.st_mode & 0777) < 0 && errno != EEXIST)
            return -1;

        DIR *d = opendir(backup_path);
        if (!d)
            return -1;

        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            if (strcmp(e->d_name, META_NAME) == 0)
                continue;

            char bchild[PATH_MAX], schild[PATH_MAX];
            if (path_join(bchild, sizeof(bchild), backup_path, e->d_name) < 0)
            {
                closedir(d);
                return -1;
            }
            if (path_join(schild, sizeof(schild), src_path, e->d_name) < 0)
            {
                closedir(d);
                return -1;
            }

            if (restore_copy_pass(src_root, backup_root, bchild, schild, created_at) < 0)
            {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
        return 0;
    }

    if (S_ISREG(bst.st_mode))
    {
        int need_copy = 0;
        struct stat sst;

        if (lstat(src_path, &sst) < 0)
        {
            if (errno == ENOENT)
                need_copy = 1;
            else
                return -1;
        }
        else
        {
            if (!S_ISREG(sst.st_mode))
            {
                (void)remove_any(src_path);
                need_copy = 1;
            }
            else
            {
                if (sst.st_size != bst.st_size || sst.st_mtime > created_at)
                {
                    need_copy = 1;
                }
            }
        }

        if (!need_copy)
            return 0;
        if (ensure_parent_dir(src_path) < 0)
            return -1;
        return copy_file(backup_path, src_path, bst.st_mode & 0777);
    }

    if (S_ISLNK(bst.st_mode))
    {
        struct stat sst;
        if (lstat(src_path, &sst) == 0 && !S_ISLNK(sst.st_mode))
        {
            (void)remove_any(src_path);
        }
        if (ensure_parent_dir(src_path) < 0)
            return -1;
        return restore_symlink_adjust(src_root, backup_root, backup_path, src_path);
    }

    return 0;
}

static int restore_delete_pass(const char *backup_path, const char *src_path)
{
    struct stat sst;
    if (lstat(src_path, &sst) < 0)
        return -1;

    struct stat bst;
    if (lstat(backup_path, &bst) < 0)
    {
        if (errno == ENOENT)
            return remove_any(src_path);
        return -1;
    }

    if (S_ISDIR(sst.st_mode))
    {
        if (!S_ISDIR(bst.st_mode))
            return remove_any(src_path);

        DIR *d = opendir(src_path);
        if (!d)
            return -1;

        struct dirent *e;
        while ((e = readdir(d)) != NULL)
        {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;

            if (strcmp(e->d_name, META_NAME) == 0)
            {
                char schild[PATH_MAX];
                if (snprintf(schild, sizeof(schild), "%s/%s", src_path, e->d_name) < (int)sizeof(schild))
                {
                    (void)remove_any(schild);
                }
                continue;
            }

            char schild[PATH_MAX], bchild[PATH_MAX];
            if (path_join(schild, sizeof(schild), src_path, e->d_name) < 0)
            {
                closedir(d);
                return -1;
            }
            if (path_join(bchild, sizeof(bchild), backup_path, e->d_name) < 0)
            {
                closedir(d);
                return -1;
            }

            if (restore_delete_pass(bchild, schild) < 0)
            {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
    }

    return 0;
}

int restore_from_backup(const char *src_root, const char *backup_root, time_t backup_created_at)
{
    if (ensure_dir_exists(src_root, 0755) < 0)
        return -1;

    if (restore_delete_pass(backup_root, src_root) < 0)
        return -1;

    if (restore_copy_pass(src_root, backup_root, backup_root, src_root, backup_created_at) < 0)
        return -1;

    return 0;
}
