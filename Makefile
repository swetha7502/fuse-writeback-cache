CC = gcc
CFLAGS = -Wall -Wextra -g $(shell pkg-config fuse3 --cflags)
LIBS = $(shell pkg-config fuse3 --libs)

all: myfs_baseline myfs_cache

myfs_baseline: src/fuse_baseline.c
	$(CC) $(CFLAGS) -o myfs_baseline src/fuse_baseline.c $(LIBS)

myfs_cache: src/fuse_cache.c
	$(CC) $(CFLAGS) -o myfs_cache src/fuse_cache.c $(LIBS)

clean:
	rm -f myfs_baseline myfs_cache