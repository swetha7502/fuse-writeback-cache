CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags fuse3)
LDFLAGS = $(shell pkg-config --libs fuse3)

TARGET = fuse_cache

all: $(TARGET)

$(TARGET): fuse_cache.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean