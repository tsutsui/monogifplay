#ifndef MONOBG_FORMAT_H
#define MONOBG_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#define MONOBG_HEADER_SIZE 32U
#define MONOBG_VERSION 1U
#define MONOBG_PIXEL_MSB_WHITE_ONE 1U

typedef struct {
    uint16_t width;
    uint16_t height;
    uint16_t depth;
    uint16_t pixel_format;
    uint32_t line_bytes;
    uint32_t payload_size;
} MonoBgInfo;

typedef struct {
    MonoBgInfo info;
    uint8_t *pixels;
} MonoBgImage;

typedef struct {
    MonoBgInfo info;
    int fd;
    uint32_t next_row;
} MonoBgReader;

void monobg_image_init(MonoBgImage *image);
void monobg_image_destroy(MonoBgImage *image);

void monobg_reader_init(MonoBgReader *reader);
void monobg_reader_close(MonoBgReader *reader);

int monobg_info_init(MonoBgInfo *info,
    unsigned int width, unsigned int height);

int monobg_header_encode(uint8_t header[MONOBG_HEADER_SIZE],
    const MonoBgInfo *info);
int monobg_header_decode(const uint8_t header[MONOBG_HEADER_SIZE],
    MonoBgInfo *info);

int monobg_reader_open(const char *path, MonoBgReader *reader);
int monobg_reader_read_row(MonoBgReader *reader,
    uint8_t *line, size_t line_size);

int monobg_write_file(const char *path, const MonoBgImage *image);

#endif /* MONOBG_FORMAT_H */
