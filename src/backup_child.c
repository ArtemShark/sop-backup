#define _XOPEN_SOURCE 700
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "backup_child.h"
#include "copy_sync.h"

static void on_term(int sig)
{
    (void)sig;
    g_copy_stop = 1;
}

int run_backup_child(const char *src_real, const char *dst_real, time_t created_at)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = SIG_IGN;
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGPIPE, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    (void)sigaction(SIGTERM, &sa, NULL);

    if (full_copy_tree(src_real, dst_real) < 0)
    {
        perror("child: full_copy_tree");
        return 1;
    }

    if (write_backup_meta(dst_real, src_real, created_at) < 0)
    {
        perror("child: write_backup_meta");
    }

    if (mirror_event_loop(src_real, dst_real) < 0)
    {
        perror("child: mirror_event_loop");
        return 1;
    }

    return 0;
}
