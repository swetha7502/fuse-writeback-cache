#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

static const char *backing_path = "/app/backing_store";

#define MAX_CACHE_ENTRIES 16

struct cache_entry {
    char path[PATH_MAX];
    char *data;
    size_t size;
    int dirty;
    int valid;
};

static struct cache_entry cache[MAX_CACHE_ENTRIES];

static void full_path(char fpath[PATH_MAX], const char *path) {
    snprintf(fpath, PATH_MAX, "%s%s", backing_path, path);
}

static struct cache_entry *find_cache_entry(const char *path) {
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (cache[i].valid && strcmp(cache[i].path, path) == 0) {
            return &cache[i];
        }
    }
    return NULL;
}

static struct cache_entry *create_cache_entry(const char *path) {
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (!cache[i].valid) {
            cache[i].valid = 1;
            cache[i].dirty = 0;
            cache[i].size = 0;
            cache[i].data = NULL;
            strncpy(cache[i].path, path, PATH_MAX - 1);
            cache[i].path[PATH_MAX - 1] = '\0';
            return &cache[i];
        }
    }
    return NULL;
}

static int load_file_into_cache(struct cache_entry *entry) {
    char fpath[PATH_MAX];
    struct stat st;
    full_path(fpath, entry->path);

    if (stat(fpath, &st) == -1) {
        if (errno == ENOENT) {
            entry->data = NULL;
            entry->size = 0;
            return 0;
        }
        return -errno;
    }

    entry->size = st.st_size;
    if (entry->size == 0) {
        entry->data = NULL;
        return 0;
    }

    entry->data = malloc(entry->size);
    if (!entry->data) {
        return -ENOMEM;
    }

    int fd = open(fpath, O_RDONLY);
    if (fd == -1) {
        free(entry->data);
        entry->data = NULL;
        entry->size = 0;
        return -errno;
    }

    ssize_t res = pread(fd, entry->data, entry->size, 0);
    close(fd);

    if (res == -1) {
        free(entry->data);
        entry->data = NULL;
        entry->size = 0;
        return -errno;
    }

    return 0;
}

static int flush_cache_entry(struct cache_entry *entry) {
    if (!entry || !entry->valid || !entry->dirty) {
        return 0;
    }

    char fpath[PATH_MAX];
    full_path(fpath, entry->path);

    int fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        return -errno;
    }

    if (entry->size > 0) {
        ssize_t res = pwrite(fd, entry->data, entry->size, 0);
        if (res == -1) {
            close(fd);
            return -errno;
        }
    }

    close(fd);
    entry->dirty = 0;
    return 0;
}

static int xmp_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void) fi;
    int res;
    char fpath[PATH_MAX];
    full_path(fpath, path);

    res = lstat(fpath, stbuf);
    if (res == -1)
        return -errno;

    return 0;
}

static int xmp_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags) {
    (void) offset;
    (void) fi;
    (void) flags;

    DIR *dp;
    struct dirent *de;
    char fpath[PATH_MAX];
    full_path(fpath, path);

    dp = opendir(fpath);
    if (dp == NULL)
        return -errno;

    while ((de = readdir(dp)) != NULL) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;

        if (filler(buf, de->d_name, &st, 0, 0))
            break;
    }

    closedir(dp);
    return 0;
}

static int xmp_open(const char *path, struct fuse_file_info *fi) {
    int fd;
    char fpath[PATH_MAX];
    full_path(fpath, path);

    fd = open(fpath, fi->flags);
    if (fd == -1)
        return -errno;

    fi->fh = fd;
    return 0;
}

static int xmp_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    int fd;
    char fpath[PATH_MAX];
    full_path(fpath, path);

    fd = open(fpath, fi->flags | O_CREAT, mode);
    if (fd == -1)
        return -errno;

    fi->fh = fd;
    return 0;
}

static int xmp_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi) {
    (void) fi;

    struct cache_entry *entry = find_cache_entry(path);
    if (entry && entry->valid) {
        if ((size_t)offset >= entry->size) {
            return 0;
        }

        if (offset + size > entry->size) {
            size = entry->size - offset;
        }

        memcpy(buf, entry->data + offset, size);
        return size;
    }

    char fpath[PATH_MAX];
    full_path(fpath, path);

    int fd = open(fpath, O_RDONLY);
    if (fd == -1)
        return -errno;

    int res = pread(fd, buf, size, offset);
    if (res == -1)
        res = -errno;

    close(fd);
    return res;
}

static int xmp_write(const char *path, const char *buf, size_t size, off_t offset,
                     struct fuse_file_info *fi) {
    (void) fi;

    struct cache_entry *entry = find_cache_entry(path);
    if (!entry) {
        entry = create_cache_entry(path);
        if (!entry) {
            return -ENOSPC;
        }

        int res = load_file_into_cache(entry);
        if (res < 0) {
            return res;
        }
    }

    size_t new_size = offset + size;
    if (new_size > entry->size) {
        char *new_data = realloc(entry->data, new_size);
        if (!new_data) {
            return -ENOMEM;
        }

        if (new_size > entry->size) {
            memset(new_data + entry->size, 0, new_size - entry->size);
        }

        entry->data = new_data;
        entry->size = new_size;
    }

    memcpy(entry->data + offset, buf, size);
    entry->dirty = 1;

    return size;
}

static int xmp_release(const char *path, struct fuse_file_info *fi) {
    struct cache_entry *entry = find_cache_entry(path);
    if (entry && entry->dirty) {
        int res = flush_cache_entry(entry);
        if (res < 0) {
            close(fi->fh);
            return res;
        }
    }

    close(fi->fh);
    return 0;
}

static int xmp_unlink(const char *path) {
    char fpath[PATH_MAX];
    full_path(fpath, path);

    struct cache_entry *entry = find_cache_entry(path);
    if (entry) {
        free(entry->data);
        entry->data = NULL;
        entry->size = 0;
        entry->dirty = 0;
        entry->valid = 0;
    }

    if (unlink(fpath) == -1)
        return -errno;

    return 0;
}

static int xmp_mkdir(const char *path, mode_t mode) {
    char fpath[PATH_MAX];
    full_path(fpath, path);

    if (mkdir(fpath, mode) == -1)
        return -errno;

    return 0;
}

static int xmp_rmdir(const char *path) {
    char fpath[PATH_MAX];
    full_path(fpath, path);

    if (rmdir(fpath) == -1)
        return -errno;

    return 0;
}

static int xmp_utimens(const char *path, const struct timespec ts[2],
                       struct fuse_file_info *fi) {
    (void) fi;
    char fpath[PATH_MAX];
    full_path(fpath, path);

    if (utimensat(0, fpath, ts, AT_SYMLINK_NOFOLLOW) == -1)
        return -errno;

    return 0;
}

static const struct fuse_operations xmp_oper = {
    .getattr = xmp_getattr,
    .readdir = xmp_readdir,
    .open = xmp_open,
    .create = xmp_create,
    .read = xmp_read,
    .write = xmp_write,
    .release = xmp_release,
    .unlink = xmp_unlink,
    .mkdir = xmp_mkdir,
    .rmdir = xmp_rmdir,
    .utimens = xmp_utimens,
};

int main(int argc, char *argv[]) {
    return fuse_main(argc, argv, &xmp_oper, NULL);
}