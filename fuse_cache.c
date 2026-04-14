#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <time.h>
#include <stddef.h>

/* ── Configuration ──────────────────────────────────────────────── */
#define MAX_PATH       4096
#define DEFAULT_CACHE_SIZE_MB 64
#define MAX_CACHE_ENTRIES 1024

static char  underlying_dir[MAX_PATH];
static int   writeback_mode = 1;          /* 1 = write-back, 0 = write-through */
static size_t max_cache_bytes;

/* ── Cache entry ────────────────────────────────────────────────── */
typedef struct cache_entry {
    char   path[MAX_PATH];
    char  *data;
    size_t size;
    int    dirty;
    time_t last_access;
    struct cache_entry *prev, *next;
} cache_entry_t;

static cache_entry_t *cache_head = NULL;   /* MRU end */
static cache_entry_t *cache_tail = NULL;   /* LRU end */
static int    cache_count = 0;
static size_t cache_used_bytes = 0;

/* ── Stats ──────────────────────────────────────────────────────── */
static long hits = 0, misses = 0, evictions = 0;

/* ── Helpers ────────────────────────────────────────────────────── */
static void full_path(char *out, const char *fuse_path) {
    snprintf(out, MAX_PATH, "%s%s", underlying_dir, fuse_path);
}

/* Move entry to MRU (head) */
static void lru_touch(cache_entry_t *e) {
    if (e == cache_head) return;
    if (e->prev) e->prev->next = e->next;
    if (e->next) e->next->prev = e->prev;
    if (e == cache_tail) cache_tail = e->prev;
    e->prev = NULL;
    e->next = cache_head;
    if (cache_head) cache_head->prev = e;
    cache_head = e;
    if (!cache_tail) cache_tail = e;
    e->last_access = time(NULL);
}

/* Flush a dirty entry to disk */
static int flush_entry(cache_entry_t *e) {
    if (!e->dirty) return 0;
    char fp[MAX_PATH];
    full_path(fp, e->path);
    int fd = open(fp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    ssize_t written = write(fd, e->data, e->size);
    close(fd);
    if (written < 0) return -errno;
    e->dirty = 0;
    return 0;
}

/* Evict the LRU entry (flush first if dirty) */
static void evict_lru(void) {
    if (!cache_tail) return;
    cache_entry_t *victim = cache_tail;
    flush_entry(victim);
    if (victim->prev) victim->prev->next = NULL;
    cache_tail = victim->prev;
    if (victim == cache_head) cache_head = NULL;
    cache_used_bytes -= victim->size;
    free(victim->data);
    free(victim);
    cache_count--;
    evictions++;
}

/* Find entry by path */
static cache_entry_t *cache_find(const char *path) {
    for (cache_entry_t *e = cache_head; e; e = e->next)
        if (strcmp(e->path, path) == 0) return e;
    return NULL;
}

/* Insert or update a cache entry */
static cache_entry_t *cache_put(const char *path, const char *data, size_t size, int dirty) {
    cache_entry_t *e = cache_find(path);
    if (e) {
        cache_used_bytes -= e->size;
        free(e->data);
    } else {
        /* Evict until we have room */
        while (cache_count >= MAX_CACHE_ENTRIES ||
               cache_used_bytes + size > max_cache_bytes)
        {
            if (!cache_tail) break;
            evict_lru();
        }
        e = calloc(1, sizeof(cache_entry_t));
        if (!e) return NULL;
        strncpy(e->path, path, MAX_PATH - 1);
        e->next = e->prev = NULL;
        cache_count++;
    }
    e->data = malloc(size + 1);
    if (!e->data) { free(e); return NULL; }
    memcpy(e->data, data, size);
    e->size = size;
    e->dirty = dirty;
    cache_used_bytes += size;
    lru_touch(e);
    return e;
}

/* Remove an entry from the cache */
static void cache_remove(const char *path) {
    cache_entry_t *e = cache_find(path);
    if (!e) return;
    if (e->prev) e->prev->next = e->next;
    if (e->next) e->next->prev = e->prev;
    if (e == cache_head) cache_head = e->next;
    if (e == cache_tail) cache_tail = e->prev;
    cache_used_bytes -= e->size;
    free(e->data);
    free(e);
    cache_count--;
}

/* ── FUSE operations ────────────────────────────────────────────── */

static int fc_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    char fp[MAX_PATH]; full_path(fp, path);
    if (lstat(fp, st) < 0) return -errno;
    return 0;
}

static int fc_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
    (void)offset; (void)fi; (void)flags;
    char fp[MAX_PATH]; full_path(fp, path);
    DIR *dp = opendir(fp);
    if (!dp) return -errno;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL)
        if (filler(buf, de->d_name, NULL, 0, 0)) break;
    closedir(dp);
    return 0;
}

static int fc_open(const char *path, struct fuse_file_info *fi) {
    char fp[MAX_PATH]; full_path(fp, path);
    int fd = open(fp, fi->flags);
    if (fd < 0) return -errno;
    close(fd);
    return 0;
}

static int fc_read(const char *path, char *buf, size_t size,
                   off_t offset, struct fuse_file_info *fi) {
    (void)fi;
    /* Check cache first */
    cache_entry_t *e = cache_find(path);
    if (e) {
        hits++;
        lru_touch(e);
        if ((size_t)offset >= e->size) return 0;
        size_t avail = e->size - offset;
        size_t to_copy = avail < size ? avail : size;
        memcpy(buf, e->data + offset, to_copy);
        return (int)to_copy;
    }
    misses++;
    /* Cache miss — read from disk, populate cache */
    char fp[MAX_PATH]; full_path(fp, path);
    int fd = open(fp, O_RDONLY);
    if (fd < 0) return -errno;
    struct stat st;
    fstat(fd, &st);
    size_t file_size = (size_t)st.st_size;
    char *tmp = malloc(file_size);
    if (tmp) {
        ssize_t n = pread(fd, tmp, file_size, 0);
        if (n > 0) cache_put(path, tmp, (size_t)n, 0);
        free(tmp);
    }
    ssize_t res = pread(fd, buf, size, offset);
    close(fd);
    return res < 0 ? -errno : (int)res;
}

static int fc_write(const char *path, const char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi) {
    (void)fi;

    if (!writeback_mode) {
        /* ── Write-through: write directly to disk ── */
        char fp[MAX_PATH]; full_path(fp, path);
        int fd = open(fp, O_WRONLY | O_CREAT, 0644);
        if (fd < 0) return -errno;
        ssize_t res = pwrite(fd, buf, size, offset);
        close(fd);
        return res < 0 ? -errno : (int)res;
    }

    /* ── Write-back: buffer in cache, mark dirty ── */
    cache_entry_t *e = cache_find(path);
    size_t needed = offset + size;
    if (e) {
        if (needed > e->size) {
            char *new_data = realloc(e->data, needed);
            if (!new_data) return -ENOMEM;
            memset(new_data + e->size, 0, needed - e->size);
            cache_used_bytes -= e->size;
            e->data = new_data;
            e->size = needed;
            cache_used_bytes += needed;
        }
        memcpy(e->data + offset, buf, size);
        e->dirty = 1;
        lru_touch(e);
    } else {
        /* Load from disk first, then overlay the write */
        char fp[MAX_PATH]; full_path(fp, path);
        struct stat st;
        size_t file_size = 0;
        char *base = NULL;
        if (stat(fp, &st) == 0) {
            file_size = (size_t)st.st_size;
            base = calloc(1, file_size > needed ? file_size : needed);
            if (!base) return -ENOMEM;
            int fd = open(fp, O_RDONLY);
            if (fd >= 0) { read(fd, base, file_size); close(fd); }
        } else {
            base = calloc(1, needed);
            if (!base) return -ENOMEM;
        }
        size_t final_size = needed > file_size ? needed : file_size;
        memcpy(base + offset, buf, size);
        cache_put(path, base, final_size, 1);
        free(base);
    }
    return (int)size;
}

static int fc_release(const char *path, struct fuse_file_info *fi) {
    (void)fi;
    /* Flush dirty cache entry on close */
    cache_entry_t *e = cache_find(path);
    if (e && e->dirty) flush_entry(e);
    return 0;
}

static int fc_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    char fp[MAX_PATH]; full_path(fp, path);
    int fd = creat(fp, mode);
    if (fd < 0) return -errno;
    close(fd);
    return 0;
}

static int fc_unlink(const char *path) {
    cache_remove(path);
    char fp[MAX_PATH]; full_path(fp, path);
    if (unlink(fp) < 0) return -errno;
    return 0;
}

static int fc_mkdir(const char *path, mode_t mode) {
    char fp[MAX_PATH]; full_path(fp, path);
    if (mkdir(fp, mode) < 0) return -errno;
    return 0;
}

static int fc_rmdir(const char *path) {
    char fp[MAX_PATH]; full_path(fp, path);
    if (rmdir(fp) < 0) return -errno;
    return 0;
}

static int fc_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    (void)fi;
    cache_entry_t *e = cache_find(path);
    if (e) {
        if ((size_t)size != e->size) {
            char *nd = realloc(e->data, size > 0 ? (size_t)size : 1);
            if (!nd) return -ENOMEM;
            if ((size_t)size > e->size)
                memset(nd + e->size, 0, (size_t)size - e->size);
            cache_used_bytes = cache_used_bytes - e->size + (size_t)size;
            e->data = nd;
            e->size = (size_t)size;
            e->dirty = 1;
        }
        return 0;
    }
    char fp[MAX_PATH]; full_path(fp, path);
    if (truncate(fp, size) < 0) return -errno;
    return 0;
}

static int fc_utimens(const char *path, const struct timespec tv[2],
                      struct fuse_file_info *fi) {
    (void)fi;
    char fp[MAX_PATH]; full_path(fp, path);
    if (utimensat(AT_FDCWD, fp, tv, AT_SYMLINK_NOFOLLOW) < 0) return -errno;
    return 0;
}

static void fc_destroy(void *private_data) {
    (void)private_data;
    /* Flush all dirty entries on unmount */
    printf("\n[fuse-cache] Unmounting — flushing dirty entries...\n");
    for (cache_entry_t *e = cache_head; e; e = e->next)
        if (e->dirty) flush_entry(e);
    printf("[fuse-cache] Stats: hits=%ld  misses=%ld  evictions=%ld\n",
           hits, misses, evictions);
    if (hits + misses > 0)
        printf("[fuse-cache] Hit rate: %.1f%%\n",
               100.0 * hits / (hits + misses));
}

/* ── Operation table ────────────────────────────────────────────── */
static const struct fuse_operations fc_ops = {
    .getattr  = fc_getattr,
    .readdir  = fc_readdir,
    .open     = fc_open,
    .read     = fc_read,
    .write    = fc_write,
    .release  = fc_release,
    .create   = fc_create,
    .unlink   = fc_unlink,
    .mkdir    = fc_mkdir,
    .rmdir    = fc_rmdir,
    .truncate = fc_truncate,
    .utimens  = fc_utimens,
    .destroy  = fc_destroy,
};

/* ── main ───────────────────────────────────────────────────────── */
struct options {
    char *underlying;
    int   writethrough;
    int   cache_mb;
};
static struct options options = { .underlying = NULL, .writethrough = 0, .cache_mb = DEFAULT_CACHE_SIZE_MB };

#define OPTION(t, p) { t, offsetof(struct options, p), 1 }
static const struct fuse_opt option_spec[] = {
    OPTION("--underlying=%s", underlying),
    OPTION("-u %s",           underlying),
    OPTION("--writethrough",  writethrough),
    OPTION("--cache-mb=%d",   cache_mb),
    FUSE_OPT_END
};

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    if (fuse_opt_parse(&args, &options, option_spec, NULL) != 0) return 1;
    if (!options.underlying) {
        fprintf(stderr, "Usage: %s <mountpoint> --underlying=<dir> [--writethrough] [--cache-mb=N]\n", argv[0]);
        return 1;
    }
    realpath(options.underlying, underlying_dir);
    writeback_mode = !options.writethrough;
    max_cache_bytes = (size_t)options.cache_mb * 1024 * 1024;
    printf("[fuse-cache] Mode: %s | Cache: %d MB | Underlying: %s\n",
           writeback_mode ? "write-back" : "write-through",
           options.cache_mb, underlying_dir);
    return fuse_main(args.argc, args.argv, &fc_ops, NULL);
}