#include <stddef.h>
#include <sys/stat.h>

int path_real(const char *in, char *out, size_t out_sz);
int path_is_prefix_dir(const char *parent, const char *child);

int ensure_dir_exists(const char *path, mode_t mode);
int dir_is_empty(const char *path);
int remove_tree(const char *path);

int safe_mkdir_p(const char *path, mode_t mode);
int safe_write_all(int fd, const void *buf, size_t n);
