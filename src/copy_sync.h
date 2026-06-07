#include <signal.h>
#include <stddef.h>
#include <time.h>

extern volatile sig_atomic_t g_copy_stop;

int full_copy_tree(const char *src_root, const char *final_dst_root);
int mirror_event_loop(const char *src_root, const char *dst_root);

int write_backup_meta(const char *backup_root, const char *src_real, time_t created_at);
int read_backup_meta(const char *backup_root, char *src_out, size_t src_out_sz, time_t *created_at_out);

int restore_from_backup(const char *src_root, const char *backup_root, time_t backup_created_at);
