FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    gcc make pkg-config \
    libfuse-dev fuse3 libfuse3-dev \
    fio \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN make

RUN mkdir -p /mnt/underlying /mnt/fuse_mount

CMD ["bash"]