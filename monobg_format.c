#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "monobg_format.h"

static const uint8_t monobg_magic[8] = {
    'M', 'O', 'N', 'O', 'B', 'G', '\r', '\n'
};

static void
put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void
put_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static uint16_t
get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t
get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
      ((uint32_t)p[1] << 16) |
      ((uint32_t)p[2] << 8) |
      (uint32_t)p[3];
}

static int
read_full(int fd, void *buffer, size_t size)
{
    uint8_t *p = buffer;

    while (size != 0) {
        ssize_t n = read(fd, p, size);

        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EINVAL;
            return -1;
        }
        p += (size_t)n;
        size -= (size_t)n;
    }
    return 0;
}

static int
write_full(int fd, const void *buffer, size_t size)
{
    const uint8_t *p = buffer;

    while (size != 0) {
        ssize_t n = write(fd, p, size);

        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        p += (size_t)n;
        size -= (size_t)n;
    }
    return 0;
}

static int
monobg_info_validate(const MonoBgInfo *info)
{
    MonoBgInfo expected;

    if (info == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (monobg_info_init(&expected, info->width, info->height) == -1)
        return -1;
    if (info->depth != expected.depth ||
      info->pixel_format != expected.pixel_format ||
      info->line_bytes != expected.line_bytes ||
      info->payload_size != expected.payload_size) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void
monobg_image_init(MonoBgImage *image)
{
    memset(image, 0, sizeof(*image));
}

void
monobg_image_destroy(MonoBgImage *image)
{
    free(image->pixels);
    image->pixels = NULL;
    memset(&image->info, 0, sizeof(image->info));
}

void
monobg_reader_init(MonoBgReader *reader)
{
    memset(reader, 0, sizeof(*reader));
    reader->fd = -1;
}

void
monobg_reader_close(MonoBgReader *reader)
{
    if (reader->fd != -1) {
        (void)close(reader->fd);
        reader->fd = -1;
    }
    memset(&reader->info, 0, sizeof(reader->info));
    reader->next_row = 0;
}

int
monobg_info_init(MonoBgInfo *info,
  unsigned int width, unsigned int height)
{
    size_t line_bytes;
    size_t payload_size;

    if (info == NULL || width == 0 || height == 0 ||
      width > UINT16_MAX || height > UINT16_MAX) {
        errno = EINVAL;
        return -1;
    }

    line_bytes = ((size_t)width + 7U) / 8U;
    if (line_bytes > UINT32_MAX ||
      height > UINT32_MAX / line_bytes) {
        errno = EOVERFLOW;
        return -1;
    }
    payload_size = line_bytes * height;

    info->width = (uint16_t)width;
    info->height = (uint16_t)height;
    info->depth = 1;
    info->pixel_format = MONOBG_PIXEL_MSB_WHITE_ONE;
    info->line_bytes = (uint32_t)line_bytes;
    info->payload_size = (uint32_t)payload_size;
    return 0;
}

int
monobg_header_encode(uint8_t header[MONOBG_HEADER_SIZE],
  const MonoBgInfo *info)
{
    if (header == NULL || monobg_info_validate(info) == -1)
        return -1;

    memset(header, 0, MONOBG_HEADER_SIZE);
    memcpy(header, monobg_magic, sizeof(monobg_magic));
    put_be16(header + 8, MONOBG_VERSION);
    put_be16(header + 10, MONOBG_HEADER_SIZE);
    put_be16(header + 12, info->width);
    put_be16(header + 14, info->height);
    put_be16(header + 16, info->depth);
    put_be16(header + 18, info->pixel_format);
    put_be32(header + 20, info->line_bytes);
    put_be32(header + 24, info->payload_size);
    put_be32(header + 28, 0);
    return 0;
}

int
monobg_header_decode(const uint8_t header[MONOBG_HEADER_SIZE],
  MonoBgInfo *info)
{
    MonoBgInfo decoded;

    if (header == NULL || info == NULL ||
      memcmp(header, monobg_magic, sizeof(monobg_magic)) != 0 ||
      get_be16(header + 8) != MONOBG_VERSION ||
      get_be16(header + 10) != MONOBG_HEADER_SIZE ||
      get_be32(header + 28) != 0) {
        errno = EINVAL;
        return -1;
    }

    decoded.width = get_be16(header + 12);
    decoded.height = get_be16(header + 14);
    decoded.depth = get_be16(header + 16);
    decoded.pixel_format = get_be16(header + 18);
    decoded.line_bytes = get_be32(header + 20);
    decoded.payload_size = get_be32(header + 24);

    if (monobg_info_validate(&decoded) == -1)
        return -1;

    *info = decoded;
    return 0;
}

int
monobg_reader_open(const char *path, MonoBgReader *reader)
{
    uint8_t header[MONOBG_HEADER_SIZE];
    struct stat st;
    uint64_t expected_size;
    int fd;

    if (path == NULL || reader == NULL || reader->fd != -1) {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd == -1)
        return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    if (fstat(fd, &st) == -1) {
        int saved_errno = errno;

        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }
    if (read_full(fd, header, sizeof(header)) == -1) {
        int saved_errno = errno;

        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
    if (monobg_header_decode(header, &reader->info) == -1) {
        int saved_errno = errno;

        (void)close(fd);
        errno = saved_errno;
        return -1;
    }

    expected_size = MONOBG_HEADER_SIZE + (uint64_t)reader->info.payload_size;
    if (st.st_size < 0 || (uint64_t)st.st_size != expected_size) {
        (void)close(fd);
        memset(&reader->info, 0, sizeof(reader->info));
        errno = EINVAL;
        return -1;
    }

    reader->fd = fd;
    reader->next_row = 0;
    return 0;
}

int
monobg_reader_read_row(MonoBgReader *reader,
  uint8_t *line, size_t line_size)
{
    if (reader == NULL || reader->fd == -1 || line == NULL ||
      line_size != reader->info.line_bytes ||
      reader->next_row >= reader->info.height) {
        errno = EINVAL;
        return -1;
    }

    if (read_full(reader->fd, line, line_size) == -1)
        return -1;
    reader->next_row++;
    return 0;
}

int
monobg_write_file(const char *path, const MonoBgImage *image)
{
    uint8_t header[MONOBG_HEADER_SIZE];
    int fd;
    int saved_errno;

    if (path == NULL || image == NULL || image->pixels == NULL ||
      monobg_header_encode(header, &image->info) == -1)
        return -1;

    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd == -1)
        return -1;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    if (write_full(fd, header, sizeof(header)) == -1 ||
      write_full(fd, image->pixels, image->info.payload_size) == -1) {
        saved_errno = errno;
        (void)close(fd);
        (void)unlink(path);
        errno = saved_errno;
        return -1;
    }
    if (close(fd) == -1) {
        saved_errno = errno;
        (void)unlink(path);
        errno = saved_errno;
        return -1;
    }
    return 0;
}
