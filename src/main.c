#define _XOPEN_SOURCE 700
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "backup_child.h"
#include "copy_sync.h"
#include "parser.h"
#include "util.h"

typedef struct
{
    char src[PATH_MAX];
    char dst[PATH_MAX];
    pid_t pid;
    int active;
    time_t created_at;
} backup_pair_t;

static backup_pair_t g_pairs[512];
static size_t g_pairs_n = 0;
static volatile sig_atomic_t g_exit_requested = 0;

static void print_help(void)
{
    printf("Commands:\n");
    printf("  add \"<source>\" \"<target1>\" \"<target2>\" ...\n");
    printf("  end \"<source>\" \"<target1>\" \"<target2>\" ...\n");
    printf("  list\n");
    printf("  restore \"<source>\" \"<target>\"   (blocking)\n");
    printf("  exit\n");
}

static void on_sig_exit(int sig)
{
    (void)sig;
    g_exit_requested = 1;
}

static void ignore_some_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;

    (void)sigaction(SIGHUP, &sa, NULL);
    (void)sigaction(SIGQUIT, &sa, NULL);
    (void)sigaction(SIGPIPE, &sa, NULL);
    (void)sigaction(SIGALRM, &sa, NULL);
    (void)sigaction(SIGUSR1, &sa, NULL);
    (void)sigaction(SIGUSR2, &sa, NULL);
    (void)sigaction(SIGTSTP, &sa, NULL);
    (void)sigaction(SIGTTIN, &sa, NULL);
    (void)sigaction(SIGTTOU, &sa, NULL);
    (void)sigaction(SIGWINCH, &sa, NULL);
}

static void install_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig_exit;

    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    ignore_some_signals();
}

static void reap_children_nonblock(void)
{
    int st;
    pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0)
    {
        for (size_t i = 0; i < g_pairs_n; i++)
        {
            if (g_pairs[i].active && g_pairs[i].pid == p)
            {
                g_pairs[i].active = 0;
                break;
            }
        }
    }
}

static void kill_all_children(void)
{
    for (size_t i = 0; i < g_pairs_n; i++)
    {
        if (g_pairs[i].active)
            kill(g_pairs[i].pid, SIGTERM);
    }
    while (waitpid(-1, NULL, 0) > 0)
    {
    }
}

static int pair_exists_active(const char *src_real, const char *dst_real)
{
    for (size_t i = 0; i < g_pairs_n; i++)
    {
        if (g_pairs[i].active && strcmp(g_pairs[i].src, src_real) == 0 && strcmp(g_pairs[i].dst, dst_real) == 0)
            return 1;
    }
    return 0;
}

static int find_pair_active(const char *src_real, const char *dst_real)
{
    for (size_t i = 0; i < g_pairs_n; i++)
    {
        if (g_pairs[i].active && strcmp(g_pairs[i].src, src_real) == 0 && strcmp(g_pairs[i].dst, dst_real) == 0)
            return (int)i;
    }
    return -1;
}

static int target_ok_or_make_empty(const char *dst_real)
{
    struct stat st;
    if (lstat(dst_real, &st) == 0)
    {
        if (!S_ISDIR(st.st_mode))
        {
            errno = ENOTDIR;
            return -1;
        }
        int emp = dir_is_empty(dst_real);
        if (emp < 0)
            return -1;
        if (emp == 0)
        {
            errno = ENOTEMPTY;
            return -1;
        }
        return 0;
    }
    if (errno != ENOENT)
        return -1;
    return ensure_dir_exists(dst_real, 0755);
}

static int resolve_target_real(const char *target_arg, char *dst_real, size_t dst_real_sz)
{
    if (ensure_dir_exists(target_arg, 0755) < 0)
        return -1;
    return path_real(target_arg, dst_real, dst_real_sz);
}

static int pid_alive(pid_t pid)
{
    if (pid <= 0)
        return 0;
    if (kill(pid, 0) == 0)
        return 1;
    if (errno == EPERM)
        return 1;
    return 0;
}

static void refresh_pairs_alive_state(void)
{
    for (size_t i = 0; i < g_pairs_n; i++)
    {
        if (!g_pairs[i].active)
            continue;

        if (!pid_alive(g_pairs[i].pid))
        {
            g_pairs[i].active = 0;

            (void)waitpid(g_pairs[i].pid, NULL, WNOHANG);
        }
    }
}

static int cmd_add(parsed_line_t *p)
{
    if (p->argc < 3)
    {
        fprintf(stderr, "Error: usage: add \"<source>\" \"<target1>\" ...\n");
        return -1;
    }

    char src_real[PATH_MAX];
    if (path_real(p->argv[1], src_real, sizeof(src_real)) < 0)
    {
        perror("Error: realpath(source)");
        return -1;
    }

    time_t created_at = time(NULL);

    for (int i = 2; i < p->argc; i++)
    {
        char dst_real[PATH_MAX];
        if (resolve_target_real(p->argv[i], dst_real, sizeof(dst_real)) < 0)
        {
            perror("Error: resolve target");
            continue;
        }

        if (path_is_prefix_dir(src_real, dst_real) && strcmp(src_real, dst_real) != 0)
        {
            fprintf(stderr, "Error: target is inside source, skipped: \"%s\"\n", dst_real);
            continue;
        }

        if (path_is_prefix_dir(dst_real, src_real) && strcmp(src_real, dst_real) != 0)
        {
            fprintf(stderr, "Error: source is inside target, skipped: \"%s\"\n", dst_real);
            continue;
        }

        if (pair_exists_active(src_real, dst_real))
        {
            fprintf(stderr, "Error: backup already exists, skipped: \"%s\" -> \"%s\"\n", src_real, dst_real);
            continue;
        }

        if (target_ok_or_make_empty(dst_real) < 0)
        {
            if (errno == ENOTEMPTY)
                fprintf(stderr, "Error: target must be empty, skipped: \"%s\"\n", dst_real);
            else
                perror("Error: target check");
            continue;
        }

        if (g_pairs_n >= 512)
        {
            fprintf(stderr, "Error: too many backups.\n");
            return -1;
        }

        pid_t pid = fork();
        if (pid < 0)
        {
            perror("Error: fork");
            continue;
        }

        if (pid == 0)
        {
            int rc = run_backup_child(src_real, dst_real, created_at);
            _exit(rc);
        }

        backup_pair_t bp;
        memset(&bp, 0, sizeof(bp));
        strncpy(bp.src, src_real, sizeof(bp.src) - 1);
        strncpy(bp.dst, dst_real, sizeof(bp.dst) - 1);
        bp.pid = pid;
        bp.active = 1;
        bp.created_at = created_at;

        g_pairs[g_pairs_n++] = bp;
        printf("Backup started: \"%s\" -> \"%s\" (pid=%d)\n", bp.src, bp.dst, (int)bp.pid);
    }

    return 0;
}

static int cmd_end(parsed_line_t *p)
{
    if (p->argc < 3)
    {
        fprintf(stderr, "Error: usage: end \"<source>\" \"<target1>\" ...\n");
        return -1;
    }

    char src_real[PATH_MAX];
    if (path_real(p->argv[1], src_real, sizeof(src_real)) < 0)
    {
        perror("Error: realpath(source)");
        return -1;
    }

    int ended_any = 0;
    for (int i = 2; i < p->argc; i++)
    {
        char dst_real[PATH_MAX];
        if (path_real(p->argv[i], dst_real, sizeof(dst_real)) < 0)
        {
            fprintf(stderr, "Error: target not found: \"%s\"\n", p->argv[i]);
            continue;
        }

        int idx = find_pair_active(src_real, dst_real);
        if (idx < 0)
        {
            fprintf(stderr, "Error: backup not found/active: \"%s\" -> \"%s\"\n", src_real, dst_real);
            continue;
        }

        kill(g_pairs[idx].pid, SIGTERM);
        (void)waitpid(g_pairs[idx].pid, NULL, 0);
        g_pairs[idx].active = 0;
        ended_any = 1;
        printf("Backup ended: \"%s\" -> \"%s\"\n", src_real, dst_real);
    }

    return ended_any ? 0 : -1;
}

static int cmd_list(void)
{
    reap_children_nonblock();
    refresh_pairs_alive_state();
    int any = 0;

    for (size_t i = 0; i < g_pairs_n; i++)
    {
        if (!g_pairs[i].active)
            continue;

        int already = 0;
        for (size_t j = 0; j < i; j++)
        {
            if (g_pairs[j].active && strcmp(g_pairs[j].src, g_pairs[i].src) == 0)
            {
                already = 1;
                break;
            }
        }
        if (already)
            continue;

        any = 1;
        printf("Source: \"%s\"\n", g_pairs[i].src);
        for (size_t k = 0; k < g_pairs_n; k++)
        {
            if (g_pairs[k].active && strcmp(g_pairs[k].src, g_pairs[i].src) == 0)
            {
                printf("  - Target: \"%s\" (pid=%d)\n", g_pairs[k].dst, (int)g_pairs[k].pid);
            }
        }
    }

    if (!any)
        printf("(no active backups)\n");
    return 0;
}

static int cmd_restore(parsed_line_t *p)
{
    if (p->argc != 3)
    {
        fprintf(stderr, "Error: usage: restore \"<source>\" \"<target>\"\n");
        return -1;
    }

    char src_real[PATH_MAX], dst_real[PATH_MAX];
    if (path_real(p->argv[1], src_real, sizeof(src_real)) < 0)
    {
        perror("Error: realpath(source)");
        return -1;
    }
    if (path_real(p->argv[2], dst_real, sizeof(dst_real)) < 0)
    {
        perror("Error: realpath(target)");
        return -1;
    }

    for (size_t i = 0; i < g_pairs_n; i++)
    {
        if (g_pairs[i].active && strcmp(g_pairs[i].src, src_real) == 0)
        {
            kill(g_pairs[i].pid, SIGTERM);
            (void)waitpid(g_pairs[i].pid, NULL, 0);
            g_pairs[i].active = 0;
        }
    }

    char meta_src[PATH_MAX];
    time_t created_at = 0;
    if (read_backup_meta(dst_real, meta_src, sizeof(meta_src), &created_at) < 0)
    {
        fprintf(stderr, "Error: missing/invalid .backup_meta in target. Cannot do optimized restore.\n");
        fprintf(stderr, "Note: run add once to create proper backup metadata.\n");
        return -1;
    }
    if (strcmp(meta_src, src_real) != 0)
    {
        fprintf(stderr, "Error: target backup was created from different source.\n");
        fprintf(stderr, "  meta source: \"%s\"\n", meta_src);
        fprintf(stderr, "  requested:   \"%s\"\n", src_real);
        return -1;
    }

    printf("Restoring from \"%s\" -> \"%s\" ...\n", dst_real, src_real);

    if (restore_from_backup(src_real, dst_real, created_at) < 0)
    {
        perror("Error: restore");
        return -1;
    }

    printf("Restore done.\n");
    return 0;
}

int main(void)
{
    install_handlers();
    print_help();

    char line[4096];

    while (!g_exit_requested)
    {
        reap_children_nonblock();
        refresh_pairs_alive_state();

        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin))
            break;

        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n')
            line[n - 1] = '\0';

        parsed_line_t p;
        if (parse_line(line, &p) < 0)
        {
            fprintf(stderr, "Error: invalid input (check quotes).\n");
            free_parsed(&p);
            continue;
        }

        if (p.argc == 0)
        {
            free_parsed(&p);
            continue;
        }

        if (strcmp(p.argv[0], "add") == 0)
            (void)cmd_add(&p);
        else if (strcmp(p.argv[0], "end") == 0)
            (void)cmd_end(&p);
        else if (strcmp(p.argv[0], "list") == 0)
            (void)cmd_list();
        else if (strcmp(p.argv[0], "restore") == 0)
            (void)cmd_restore(&p);
        else if (strcmp(p.argv[0], "exit") == 0)
        {
            free_parsed(&p);
            break;
        }
        else
        {
            fprintf(stderr, "Error: unknown command.\n");
            print_help();
        }

        free_parsed(&p);
    }

    printf("\nExiting...\n");
    kill_all_children();
    return 0;
}
