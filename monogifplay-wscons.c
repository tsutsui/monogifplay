/*
 * MonoGIFPlayer for NetBSD/luna68k wsdisplay dumb framebuffer.
 *
 * GIF decoding and monochrome conversion are derived from monogifplay.c.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>

#include <dev/wscons/wsconsio.h>

#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <gif_lib.h>

#include "monobg_format.h"

#ifndef __NetBSD__
#error "monogifplay-wscons is supported only on NetBSD"
#endif

#ifdef UNROLL_BITMAP_EXTRACT
#include <sys/endian.h>

/* Try to check endianness without autoconf etc. */
#if defined(__BYTE_ORDER__) && \
  defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
# define TARGET_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
# define TARGET_BIG_ENDIAN    (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#elif defined(_BYTE_ORDER) && \
  defined(_LITTLE_ENDIAN) && defined(_BIG_ENDIAN)
# define TARGET_LITTLE_ENDIAN (_BYTE_ORDER == _LITTLE_ENDIAN)
# define TARGET_BIG_ENDIAN    (_BYTE_ORDER == _BIG_ENDIAN)
#elif defined(BYTE_ORDER) && \
  defined(LITTLE_ENDIAN) && defined(BIG_ENDIAN)
# define TARGET_LITTLE_ENDIAN (BYTE_ORDER == LITTLE_ENDIAN)
# define TARGET_BIG_ENDIAN    (BYTE_ORDER == BIG_ENDIAN)
#else
# error "Cannot determine endianness."
#endif

#ifndef bswap32
#define bswap32(x) __builtin_bswap32(x)
#endif
#endif /* UNROLL_BITMAP_EXTRACT */

#define DEF_FBDEV       "/dev/ttyE0"
#define DEF_GIF_DELAY   75U
#define LUNA_FB_OFFSET  8U

/*
 * GIF-to-monochrome data which can later be moved to a common module shared
 * with the X11 backend.  It contains no display-backend resources.
 */
typedef struct {
    unsigned int width;
    unsigned int height;
    size_t line_bytes;
    size_t frame_bytes;
    int frame_count;
} MonoGifInfo;

/*
 * Per-frame metadata produced by the backend-independent GIF renderer.
 * update_* preserves the original GIF image rectangle even though the initial
 * wscons storage format keeps a complete composited logical-screen bitmap.
 */
typedef struct {
    uint32_t delay;
    uint16_t update_left;
    uint16_t update_top;
    uint16_t update_width;
    uint16_t update_height;
} MonoGifFrameInfo;

enum {
    WSCONS_FRAME_FULL_1BPP = 0
};

/*
 * wscons-specific frame descriptor.  The initial format stores a complete
 * logical-screen 1bpp bitmap for every frame, but addresses are described by
 * pool offsets rather than inferred from the frame number.  This permits later
 * variable-size or differently aligned payload formats without changing the
 * playback-side ownership model.
 */
typedef struct {
    MonoGifFrameInfo gif;
    size_t data_offset;
    size_t data_size;
    size_t line_bytes;
    uint8_t format;
    uint8_t flags;
    uint16_t reserved;
} WsconsFrame;

typedef struct {
    MonoGifInfo info;
    WsconsFrame *frames;
    uint8_t *bitmap_pool;
    size_t bitmap_pool_size;
} WsconsAnimation;

typedef struct {
    unsigned int x;
    unsigned int y;
} DisplayPosition;

typedef struct {
    int fd;
    const char *device;
    unsigned int original_mode;
    unsigned int type;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
    unsigned int stride;
    bool used_extended_info;
    bool mode_changed;
    size_t fb_offset;
    size_t fb_size;
    size_t map_size;
    size_t visible_line_bytes;
    size_t saved_fb_size;
    uint8_t *map_base;
    uint8_t *fb_base;
    uint8_t *saved_fb;
    struct termios original_termios;
    bool termios_changed;
    bool stdin_is_tty;
} WsDisplay;

static const char *progname;
static int opt_center;
static int opt_clear;
static int opt_duration;
static int opt_progress;

static uint32_t total_start_time;
static uint32_t gifload_start_time;
static uint32_t gifload_end_time;
static uint32_t total_frame_time;
static uint32_t tv_sec_start;

static volatile sig_atomic_t stop_requested;

static int
size_mul(size_t a, size_t b, size_t *result)
{
    if (a != 0 && b > SIZE_MAX / a) {
        errno = EOVERFLOW;
        return -1;
    }
    *result = a * b;
    return 0;
}

static int
size_add(size_t a, size_t b, size_t *result)
{
    if (b > SIZE_MAX - a) {
        errno = EOVERFLOW;
        return -1;
    }
    *result = a + b;
    return 0;
}

static int
display_position_resolve(DisplayPosition *position,
  const WsDisplay *display, unsigned int gif_width,
  unsigned int gif_height, bool center_requested,
  long requested_x, long requested_y)
{
    unsigned int center_x, center_y;
    long x, y;

    if (gif_width == 0 || gif_height == 0 ||
      gif_width > display->width || gif_height > display->height) {
        errno = EINVAL;
        return -1;
    }

    center_x = (display->width - gif_width) / 2U;
    center_x &= ~7U;
    center_y = (display->height - gif_height) / 2U;

    x = requested_x >= 0 ? requested_x :
      center_requested ? (long)center_x : 0;
    y = requested_y >= 0 ? requested_y :
      center_requested ? (long)center_y : 0;

    if ((x & 7L) != 0 ||
      x > (long)(display->width - gif_width) ||
      y > (long)(display->height - gif_height)) {
        errno = EINVAL;
        return -1;
    }

    position->x = (unsigned int)x;
    position->y = (unsigned int)y;
    return 0;
}

static void
init_gettime_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
        err(EXIT_FAILURE, "clock_gettime");
    tv_sec_start = (uint32_t)ts.tv_sec;
}

static uint32_t
gettime_ms(void)
{
    struct timespec ts;
    uint32_t tv_sec;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
        return 0;
    tv_sec = (uint32_t)ts.tv_sec - tv_sec_start;
    return tv_sec * 1000U + (uint32_t)(ts.tv_nsec / 1000000L);
}

static int
mono_gif_info_init(MonoGifInfo *info, unsigned int width,
  unsigned int height, int frame_count)
{
    size_t line_bytes;
    size_t frame_bytes;

    if (width == 0 || height == 0 ||
      width > UINT16_MAX || height > UINT16_MAX ||
      frame_count <= 0) {
        errno = EINVAL;
        return -1;
    }

    line_bytes = ((size_t)width + 7U) / 8U;
    if (size_mul(line_bytes, height, &frame_bytes) == -1)
        return -1;
    if (frame_bytes == 0) {
        errno = EOVERFLOW;
        return -1;
    }

    info->width = width;
    info->height = height;
    info->line_bytes = line_bytes;
    info->frame_bytes = frame_bytes;
    info->frame_count = frame_count;
    return 0;
}

static int
wscons_frame_range_valid(const WsconsAnimation *animation,
  const WsconsFrame *frame)
{
    if (animation == NULL || frame == NULL ||
      animation->bitmap_pool == MAP_FAILED ||
      frame->data_offset > animation->bitmap_pool_size ||
      frame->data_size >
      animation->bitmap_pool_size - frame->data_offset) {
        errno = EINVAL;
        return 0;
    }

    return 1;
}

static uint8_t *
wscons_frame_data(WsconsAnimation *animation, WsconsFrame *frame)
{
    if (!wscons_frame_range_valid(animation, frame))
        return NULL;

    return animation->bitmap_pool + frame->data_offset;
}

static const uint8_t *
wscons_frame_const_data(const WsconsAnimation *animation,
  const WsconsFrame *frame)
{
    if (!wscons_frame_range_valid(animation, frame))
        return NULL;

    return animation->bitmap_pool + frame->data_offset;
}

static void
wscons_animation_init(WsconsAnimation *animation)
{
    memset(animation, 0, sizeof(*animation));
    animation->bitmap_pool = MAP_FAILED;
}

static int
wscons_animation_allocate(WsconsAnimation *animation,
  const MonoGifInfo *info)
{
    size_t pool_size;
    int i;

    animation->info = *info;
    animation->frames = calloc((size_t)info->frame_count,
      sizeof(*animation->frames));
    if (animation->frames == NULL)
        return -1;

    /*
     * Assign offsets explicitly even though the initial format uses equal
     * full-frame payloads.  Future variable-size formats can change this
     * layout calculation without changing playback-side frame lookup.
     */
    pool_size = 0;
    for (i = 0; i < info->frame_count; i++) {
        WsconsFrame *frame = &animation->frames[i];

        frame->data_offset = pool_size;
        frame->data_size = info->frame_bytes;
        frame->line_bytes = info->line_bytes;
        frame->format = WSCONS_FRAME_FULL_1BPP;
        if (size_add(pool_size, frame->data_size, &pool_size) == -1)
            return -1;
    }
    if (pool_size == 0) {
        errno = EOVERFLOW;
        return -1;
    }

    animation->bitmap_pool = mmap(NULL, pool_size,
      PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (animation->bitmap_pool == MAP_FAILED)
        return -1;

    animation->bitmap_pool_size = pool_size;

    /* Touch no bitmap pages here; rendering commits them frame by frame. */
    return 0;
}

static void
wscons_animation_finish_loading(WsconsAnimation *animation)
{
    /* The frame pool is immutable while playing. */
    if (animation->bitmap_pool != MAP_FAILED) {
        if (mprotect(animation->bitmap_pool, animation->bitmap_pool_size,
          PROT_READ) == -1 && opt_progress)
            warn("mprotect frame bitmap pool");

        /*
         * Playback walks the mapping from low to high addresses.  On NetBSD,
         * MADV_SEQUENTIAL can lower the priority of pages already passed,
         * allowing old frames to be paged out before other useful memory.
         */
        if (madvise(animation->bitmap_pool, animation->bitmap_pool_size,
          MADV_SEQUENTIAL) == -1 && opt_progress)
            warn("madvise frame bitmap pool");
    }
}

static void
wscons_animation_destroy(WsconsAnimation *animation)
{
    if (animation->bitmap_pool != MAP_FAILED) {
        (void)munmap(animation->bitmap_pool, animation->bitmap_pool_size);
        animation->bitmap_pool = MAP_FAILED;
    }
    free(animation->frames);
    animation->frames = NULL;
    animation->bitmap_pool_size = 0;
}

#ifdef UNROLL_BITMAP_EXTRACT
/*
 * Return the number of leading pixels to process one by one before a safe,
 * byte-boundary and uint32_t-aligned 32-pixel store can be used.
 */
static unsigned int
pixels_to_word_alignment(uint8_t *row, unsigned int x, unsigned int width)
{
    unsigned int n;

    n = 0;
    while (n < width && ((x + n) & 7U) != 0)
        n++;

    while (n + 8U <= width &&
      (((uintptr_t)(row + ((x + n) >> 3))) & 3U) != 0)
        n += 8U;

    return n;
}
#endif

/*
 * Render one GIF frame into a complete MSB-first 1bpp logical-screen image.
 *
 * This function is deliberately independent of wsdisplay and of the final
 * frame storage policy.  A future X11 backend can pass one reusable work
 * buffer as both bitmap and previous; the wscons backend passes adjacent
 * frames in its mmap pool.
 */
static int
mono_render_frame(GifFileType *gif, const MonoGifInfo *info, int frame,
  uint8_t *bitmap, const uint8_t *previous, MonoGifFrameInfo *frame_info)
{
    unsigned int swidth, sheight;
    size_t line_bytes;
    unsigned int ci, x, y;
    unsigned int screenx;
    size_t frame_row_offset, bitmap_row_offset;
    unsigned int frame_width, frame_height, frame_left, frame_top;
    unsigned int ncolors;
    SavedImage *img;
    GifImageDesc *desc;
    ColorMapObject *cmap;
    GraphicsControlBlock gcb;
    int delay, transparent_index;
    uint32_t bw_bit_cache[256];

    if (gif == NULL || info == NULL || bitmap == NULL ||
      frame_info == NULL ||
      frame < 0 || frame >= info->frame_count) {
        errno = EINVAL;
        return -1;
    }

    swidth = info->width;
    sheight = info->height;
    line_bytes = info->line_bytes;

    img = &gif->SavedImages[frame];
    desc = &img->ImageDesc;
    cmap = desc->ColorMap != NULL ? desc->ColorMap : gif->SColorMap;
    if (cmap == NULL || cmap->ColorCount > 256) {
        errno = EINVAL;
        return -1;
    }

    frame_width = (unsigned int)desc->Width;
    frame_height = (unsigned int)desc->Height;
    frame_left = (unsigned int)desc->Left;
    frame_top = (unsigned int)desc->Top;
    if (frame_left > swidth || frame_top > sheight ||
      frame_width > swidth - frame_left ||
      frame_height > sheight - frame_top ||
      (frame_width != 0 && frame_height != 0 && img->RasterBits == NULL)) {
        errno = EINVAL;
        return -1;
    }

    memset(&gcb, 0, sizeof(gcb));
    gcb.TransparentColor = NO_TRANSPARENT_COLOR;
    /*
     * GIFLIB returns GIF_ERROR when no GCE exists, while leaving the
     * caller-supplied defaults in gcb.  This is valid for ordinary GIFs.
     */
    (void)DGifSavedExtensionToGCB(gif, frame, &gcb);
    delay = gcb.DelayTime * 10;
    frame_info->delay = delay > 0 ? (uint32_t)delay : DEF_GIF_DELAY;
    frame_info->update_left = (uint16_t)frame_left;
    frame_info->update_top = (uint16_t)frame_top;
    frame_info->update_width = (uint16_t)frame_width;
    frame_info->update_height = (uint16_t)frame_height;
    transparent_index = gcb.TransparentColor;

    if (transparent_index != NO_TRANSPARENT_COLOR ||
      swidth != frame_width || sheight != frame_height ||
      frame_left != 0 || frame_top != 0) {
        if (previous == NULL)
            memset(bitmap, 0, info->frame_bytes);
        else if (bitmap != previous)
            memcpy(bitmap, previous, info->frame_bytes);
    }

    memset(bw_bit_cache, 0, sizeof(bw_bit_cache));
    ncolors = (unsigned int)cmap->ColorCount;
    for (ci = 0; ci < ncolors; ci++) {
        GifColorType c = cmap->Colors[ci];
        if ((unsigned int)c.Red * 299U +
          (unsigned int)c.Green * 587U +
          (unsigned int)c.Blue * 114U > 128000U) {
            bw_bit_cache[ci] =
#ifdef UNROLL_BITMAP_EXTRACT
              0x80000000U;
#else
              0x80U;
#endif
        }
    }

#ifdef UNROLL_BITMAP_EXTRACT
    for (y = 0, bitmap_row_offset = (size_t)frame_top * line_bytes,
      frame_row_offset = 0;
      y < frame_height;
      y++, bitmap_row_offset += line_bytes,
      frame_row_offset += frame_width) {
        GifByteType *raster, px;
        uint8_t *bitmap_row, *bitmapp;
        unsigned int unaligned_pixels;

        bitmap_row = bitmap + bitmap_row_offset;
        unaligned_pixels = pixels_to_word_alignment(bitmap_row,
          frame_left, frame_width);
        raster = &img->RasterBits[frame_row_offset];

        /* 1. Pixel operations until a safe uint32_t boundary. */
        for (x = 0, screenx = frame_left;
          x < unaligned_pixels; x++, screenx++) {
            unsigned int byte, bit;
            size_t bitmap_byte_offset;

            px = *raster++;
            if (px == transparent_index)
                continue;

            byte = screenx >> 3;
            bitmap_byte_offset = bitmap_row_offset + byte;
            bit = screenx & 7U;
            bitmap[bitmap_byte_offset] &= (uint8_t)~(0x80U >> bit);
            bitmap[bitmap_byte_offset] |=
              (uint8_t)(bw_bit_cache[px] >> (bit + 24U));
        }

        /* 2. Unrolled 32-pixel operations. */
        for (bitmapp = &bitmap_row[screenx >> 3];
          x + 31U < frame_width;
          x += 32U, screenx += 32U, bitmapp += 4) {
            uint32_t bitmap32;

            if (transparent_index == NO_TRANSPARENT_COLOR) {
                bitmap32  = bw_bit_cache[*raster++] >> 0U;
                bitmap32 |= bw_bit_cache[*raster++] >> 1U;
                bitmap32 |= bw_bit_cache[*raster++] >> 2U;
                bitmap32 |= bw_bit_cache[*raster++] >> 3U;
                bitmap32 |= bw_bit_cache[*raster++] >> 4U;
                bitmap32 |= bw_bit_cache[*raster++] >> 5U;
                bitmap32 |= bw_bit_cache[*raster++] >> 6U;
                bitmap32 |= bw_bit_cache[*raster++] >> 7U;
                bitmap32 |= bw_bit_cache[*raster++] >> 8U;
                bitmap32 |= bw_bit_cache[*raster++] >> 9U;
                bitmap32 |= bw_bit_cache[*raster++] >> 10U;
                bitmap32 |= bw_bit_cache[*raster++] >> 11U;
                bitmap32 |= bw_bit_cache[*raster++] >> 12U;
                bitmap32 |= bw_bit_cache[*raster++] >> 13U;
                bitmap32 |= bw_bit_cache[*raster++] >> 14U;
                bitmap32 |= bw_bit_cache[*raster++] >> 15U;
                bitmap32 |= bw_bit_cache[*raster++] >> 16U;
                bitmap32 |= bw_bit_cache[*raster++] >> 17U;
                bitmap32 |= bw_bit_cache[*raster++] >> 18U;
                bitmap32 |= bw_bit_cache[*raster++] >> 19U;
                bitmap32 |= bw_bit_cache[*raster++] >> 20U;
                bitmap32 |= bw_bit_cache[*raster++] >> 21U;
                bitmap32 |= bw_bit_cache[*raster++] >> 22U;
                bitmap32 |= bw_bit_cache[*raster++] >> 23U;
                bitmap32 |= bw_bit_cache[*raster++] >> 24U;
                bitmap32 |= bw_bit_cache[*raster++] >> 25U;
                bitmap32 |= bw_bit_cache[*raster++] >> 26U;
                bitmap32 |= bw_bit_cache[*raster++] >> 27U;
                bitmap32 |= bw_bit_cache[*raster++] >> 28U;
                bitmap32 |= bw_bit_cache[*raster++] >> 29U;
                bitmap32 |= bw_bit_cache[*raster++] >> 30U;
                bitmap32 |= bw_bit_cache[*raster++] >> 31U;
#if TARGET_LITTLE_ENDIAN
                bitmap32 = bswap32(bitmap32);
#endif
                *(uint32_t *)(void *)bitmapp = bitmap32;
            } else {
                bitmap32 = *(uint32_t *)(void *)bitmapp;
#if TARGET_LITTLE_ENDIAN
                bitmap32 = bswap32(bitmap32);
#endif
#define UPDATE_BITMAP32_BIT(bitpos) do {                              \
                px = *raster++;                                   \
                if (px != transparent_index) {                    \
                    bitmap32 &= ~(0x80000000U >> (bitpos));        \
                    bitmap32 |= bw_bit_cache[px] >> (bitpos);      \
                }                                                  \
            } while (0)
                UPDATE_BITMAP32_BIT(0U);
                UPDATE_BITMAP32_BIT(1U);
                UPDATE_BITMAP32_BIT(2U);
                UPDATE_BITMAP32_BIT(3U);
                UPDATE_BITMAP32_BIT(4U);
                UPDATE_BITMAP32_BIT(5U);
                UPDATE_BITMAP32_BIT(6U);
                UPDATE_BITMAP32_BIT(7U);
                UPDATE_BITMAP32_BIT(8U);
                UPDATE_BITMAP32_BIT(9U);
                UPDATE_BITMAP32_BIT(10U);
                UPDATE_BITMAP32_BIT(11U);
                UPDATE_BITMAP32_BIT(12U);
                UPDATE_BITMAP32_BIT(13U);
                UPDATE_BITMAP32_BIT(14U);
                UPDATE_BITMAP32_BIT(15U);
                UPDATE_BITMAP32_BIT(16U);
                UPDATE_BITMAP32_BIT(17U);
                UPDATE_BITMAP32_BIT(18U);
                UPDATE_BITMAP32_BIT(19U);
                UPDATE_BITMAP32_BIT(20U);
                UPDATE_BITMAP32_BIT(21U);
                UPDATE_BITMAP32_BIT(22U);
                UPDATE_BITMAP32_BIT(23U);
                UPDATE_BITMAP32_BIT(24U);
                UPDATE_BITMAP32_BIT(25U);
                UPDATE_BITMAP32_BIT(26U);
                UPDATE_BITMAP32_BIT(27U);
                UPDATE_BITMAP32_BIT(28U);
                UPDATE_BITMAP32_BIT(29U);
                UPDATE_BITMAP32_BIT(30U);
                UPDATE_BITMAP32_BIT(31U);
#undef UPDATE_BITMAP32_BIT
#if TARGET_LITTLE_ENDIAN
                bitmap32 = bswap32(bitmap32);
#endif
                *(uint32_t *)(void *)bitmapp = bitmap32;
            }
        }

        /* 3. Remaining pixels. */
        for (; x < frame_width; x++, screenx++) {
            unsigned int byte, bit;
            size_t bitmap_byte_offset;

            px = *raster++;
            if (px == transparent_index)
                continue;

            byte = screenx >> 3;
            bitmap_byte_offset = bitmap_row_offset + byte;
            bit = screenx & 7U;
            bitmap[bitmap_byte_offset] &= (uint8_t)~(0x80U >> bit);
            bitmap[bitmap_byte_offset] |=
              (uint8_t)(bw_bit_cache[px] >> (bit + 24U));
        }
    }
#else
    for (y = 0, bitmap_row_offset = (size_t)frame_top * line_bytes,
      frame_row_offset = 0;
      y < frame_height;
      y++, bitmap_row_offset += line_bytes,
      frame_row_offset += frame_width) {
        for (x = 0, screenx = frame_left;
          x < frame_width; x++, screenx++) {
            unsigned int byte, bit;
            size_t bitmap_byte_offset;
            GifByteType px;

            px = img->RasterBits[frame_row_offset + x];
            if (px == transparent_index)
                continue;

            byte = screenx >> 3;
            bitmap_byte_offset = bitmap_row_offset + byte;
            bit = screenx & 7U;
            bitmap[bitmap_byte_offset] &= (uint8_t)~(0x80U >> bit);
            bitmap[bitmap_byte_offset] |=
              (uint8_t)(bw_bit_cache[px] >> bit);
        }
    }
#endif

    return 0;
}

/*
 * Release the source data for a frame after its rendered result has been
 * committed to backend-owned storage.  Keeping this separate from rendering
 * makes the ownership transition explicit and is suitable for later movement
 * into a shared GIF conversion module.
 */
static void
mono_release_saved_image(SavedImage *img)
{
    free(img->RasterBits);
    img->RasterBits = NULL;

    if (img->ImageDesc.ColorMap != NULL) {
        GifFreeMapObject(img->ImageDesc.ColorMap);
        img->ImageDesc.ColorMap = NULL;
    }

    GifFreeExtensions(&img->ExtensionBlockCount, &img->ExtensionBlocks);
}

/*
 * Render all GIF frames into the wscons-specific mmap pool.  The initial
 * layout uses fixed-size full-screen payloads, but frame lookup is performed
 * through descriptors rather than an implicit frame-number calculation.
 */
static int
wscons_extract_mono_frames(GifFileType *gif, WsconsAnimation *animation)
{
    int i;

    for (i = 0; i < animation->info.frame_count; i++) {
        uint32_t frame_start_time;
        WsconsFrame *frame;
        uint8_t *bitmap;
        const uint8_t *previous;

        if (opt_progress) {
            fprintf(stderr, "Preparing bitmap for frame %d/%d...",
              i + 1, animation->info.frame_count);
        }

        frame_start_time = opt_duration ? gettime_ms() : 0;
        frame = &animation->frames[i];
        if (frame->format != WSCONS_FRAME_FULL_1BPP ||
          frame->data_size != animation->info.frame_bytes ||
          frame->line_bytes != animation->info.line_bytes) {
            errno = EINVAL;
            if (opt_progress)
                fprintf(stderr, "\n");
            return -1;
        }

        bitmap = wscons_frame_data(animation, frame);
        if (bitmap == NULL) {
            if (opt_progress)
                fprintf(stderr, "\n");
            return -1;
        }

        if (i == 0) {
            previous = NULL;
        } else {
            previous = wscons_frame_const_data(animation,
              &animation->frames[i - 1]);
            if (previous == NULL) {
                if (opt_progress)
                    fprintf(stderr, "\n");
                return -1;
            }
        }

        if (mono_render_frame(gif, &animation->info, i, bitmap, previous,
          &frame->gif) == -1) {
            if (opt_progress)
                fprintf(stderr, "\n");
            return -1;
        }

        /*
         * The 1bpp result and its metadata now belong to the wscons frame
         * descriptor and bitmap pool.  The decoded 8bpp source and frame-local
         * giflib objects are no longer needed.
         */
        mono_release_saved_image(&gif->SavedImages[i]);

        if (opt_progress) {
            if (opt_duration) {
                uint32_t frame_time;

                frame_time = gettime_ms() - frame_start_time;
                total_frame_time += frame_time;
                fprintf(stderr, " completed in %u ms.\n", frame_time);
            } else {
                fprintf(stderr, "%s",
                  i < animation->info.frame_count - 1 ? "\r" : "\n");
            }
        }
    }

    return 0;
}

static int
monobg_validate_display(const MonoBgInfo *info, const WsDisplay *display)
{
    if (info->width != display->width ||
      info->height != display->height ||
      info->depth != display->depth ||
      info->pixel_format != MONOBG_PIXEL_MSB_WHITE_ONE ||
      display->stride < info->line_bytes) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int
wsdisplay_stream_background(const WsDisplay *display,
  MonoBgReader *reader, uint8_t *line)
{
    unsigned int y;

    if (display->fb_base == NULL || reader->fd == -1 || line == NULL ||
      monobg_validate_display(&reader->info, display) == -1)
        return -1;

    for (y = 0; y < reader->info.height; y++) {
        uint8_t *dst;

        if (stop_requested) {
            errno = EINTR;
            return -1;
        }
        if (monobg_reader_read_row(reader, line,
          reader->info.line_bytes) == -1)
            return -1;
        dst = display->fb_base + (size_t)y * display->stride;
        memcpy(dst, line, reader->info.line_bytes);
    }
    return 0;
}

static void
wsdisplay_init(WsDisplay *display)
{
    memset(display, 0, sizeof(*display));
    display->fd = -1;
    display->map_base = MAP_FAILED;
    display->saved_fb = MAP_FAILED;
}

static int
wsdisplay_open_and_query(WsDisplay *display, const char *device)
{
    unsigned int mode;
    bool got_extended;

    display->device = device;
    display->fd = open(device, O_RDWR);
    if (display->fd == -1)
        return -1;
    (void)fcntl(display->fd, F_SETFD, FD_CLOEXEC);

    if (ioctl(display->fd, WSDISPLAYIO_GMODE, &mode) == -1)
        return -1;
    display->original_mode = mode;

    if (ioctl(display->fd, WSDISPLAYIO_GTYPE, &display->type) == -1)
        return -1;

    got_extended = false;
#ifdef WSDISPLAYIO_GET_FBINFO
    {
        struct wsdisplayio_fbinfo info;

        memset(&info, 0, sizeof(info));
        if (ioctl(display->fd, WSDISPLAYIO_GET_FBINFO, &info) == 0 &&
          info.fbi_width != 0 && info.fbi_height != 0 &&
          info.fbi_stride != 0 && info.fbi_bitsperpixel != 0 &&
          info.fbi_fboffset <= SIZE_MAX) {
            display->width = info.fbi_width;
            display->height = info.fbi_height;
            display->depth = info.fbi_bitsperpixel;
            display->stride = info.fbi_stride;
            display->fb_offset = (size_t)info.fbi_fboffset;
            display->used_extended_info = true;
            got_extended = true;
        }
    }
#endif

    if (!got_extended) {
        struct wsdisplay_fbinfo info;

        memset(&info, 0, sizeof(info));
        if (ioctl(display->fd, WSDISPLAYIO_GINFO, &info) == -1)
            return -1;
        display->width = info.width;
        display->height = info.height;
        display->depth = info.depth;
        if (ioctl(display->fd, WSDISPLAYIO_LINEBYTES,
          &display->stride) == -1)
            return -1;

        /* Current lunafb does not implement GET_FBINFO. */
        display->fb_offset = display->type == WSDISPLAY_TYPE_LUNA ?
          LUNA_FB_OFFSET : 0;
    }

    display->visible_line_bytes =
      ((size_t)display->width + 7U) / 8U;
    if (display->stride < display->visible_line_bytes ||
      size_mul((size_t)display->stride, display->height,
      &display->fb_size) == -1 ||
      size_add(display->fb_offset, display->fb_size,
      &display->map_size) == -1 ||
      size_mul(display->visible_line_bytes, display->height,
      &display->saved_fb_size) == -1)
        return -1;

    return 0;
}

static int
wsdisplay_save_visible(WsDisplay *display)
{
    unsigned int y;

    display->saved_fb = mmap(NULL, display->saved_fb_size,
      PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (display->saved_fb == MAP_FAILED)
        return -1;

    for (y = 0; y < display->height; y++) {
        memcpy(display->saved_fb +
          (size_t)y * display->visible_line_bytes,
          display->fb_base + (size_t)y * display->stride,
          display->visible_line_bytes);
    }

    if (mprotect(display->saved_fb, display->saved_fb_size,
      PROT_READ) == -1 && opt_progress)
        warn("mprotect saved framebuffer");
    if (madvise(display->saved_fb, display->saved_fb_size,
      MADV_DONTNEED) == -1 && opt_progress)
        warn("madvise saved framebuffer");

    return 0;
}

static void
wsdisplay_restore_visible(WsDisplay *display)
{
    unsigned int y;

    if (display->saved_fb == MAP_FAILED || display->fb_base == NULL)
        return;

    if (madvise(display->saved_fb, display->saved_fb_size,
      MADV_WILLNEED) == -1 && opt_progress)
        warn("madvise saved framebuffer for restore");

    for (y = 0; y < display->height; y++) {
        memcpy(display->fb_base + (size_t)y * display->stride,
          display->saved_fb +
          (size_t)y * display->visible_line_bytes,
          display->visible_line_bytes);
    }
}

static int
wsdisplay_enter_dumbfb(WsDisplay *display, bool restore_screen)
{
    unsigned int mode;

    if (display->fb_size == 0 || display->map_size == 0 ||
      display->visible_line_bytes == 0 || display->saved_fb_size == 0) {
        errno = EINVAL;
        return -1;
    }

    mode = WSDISPLAYIO_MODE_DUMBFB;
    if (ioctl(display->fd, WSDISPLAYIO_SMODE, &mode) == -1)
        return -1;
    display->mode_changed = true;

    display->map_base = mmap(NULL, display->map_size,
      PROT_READ | PROT_WRITE, MAP_SHARED, display->fd, 0);
    if (display->map_base == MAP_FAILED)
        return -1;
    display->fb_base = display->map_base + display->fb_offset;

    if (restore_screen && wsdisplay_save_visible(display) == -1)
        return -1;

    display->stdin_is_tty = isatty(STDIN_FILENO) != 0;
    if (display->stdin_is_tty) {
        struct termios tm;

        if (tcgetattr(STDIN_FILENO, &display->original_termios) == -1)
            return -1;
        tm = display->original_termios;
        tm.c_lflag &= ~(ICANON | ECHO);
        tm.c_cc[VMIN] = 0;
        tm.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &tm) == -1)
            return -1;
        display->termios_changed = true;
    }

    if (opt_clear)
        memset(display->fb_base, 0xff, display->fb_size);

    return 0;
}

static void
wsdisplay_cleanup(WsDisplay *display)
{
    if (display->saved_fb != MAP_FAILED && display->fb_base != NULL)
        wsdisplay_restore_visible(display);

    if (display->termios_changed) {
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH,
          &display->original_termios) == -1)
            warn("restore terminal settings");
        display->termios_changed = false;
    }

    if (display->map_base != MAP_FAILED) {
        if (munmap(display->map_base, display->map_size) == -1)
            warn("munmap framebuffer");
        display->map_base = MAP_FAILED;
        display->fb_base = NULL;
    }

    if (display->mode_changed && display->fd != -1) {
        unsigned int mode = display->original_mode;

        if (ioctl(display->fd, WSDISPLAYIO_SMODE, &mode) == -1)
            warn("restore wsdisplay mode");
        display->mode_changed = false;
    }

    if (display->saved_fb != MAP_FAILED) {
        if (munmap(display->saved_fb, display->saved_fb_size) == -1)
            warn("munmap saved framebuffer");
        display->saved_fb = MAP_FAILED;
    }

    if (display->fd != -1) {
        if (close(display->fd) == -1)
            warn("close %s", display->device);
        display->fd = -1;
    }
}

static int
wsdisplay_blit_frame(const WsDisplay *display,
  const WsconsAnimation *animation, int frame_number,
  unsigned int dst_x, unsigned int dst_y)
{
    const WsconsFrame *frame;
    const uint8_t *bitmap;
    unsigned int y;
    size_t full_bytes;
    unsigned int rem_bits;

    if (frame_number < 0 || frame_number >= animation->info.frame_count ||
      animation->info.width > display->width ||
      animation->info.height > display->height ||
      (dst_x & 7U) != 0 ||
      dst_x > display->width - animation->info.width ||
      dst_y > display->height - animation->info.height) {
        errno = EINVAL;
        return -1;
    }

    frame = &animation->frames[frame_number];
    if (frame->format != WSCONS_FRAME_FULL_1BPP ||
      frame->data_size != animation->info.frame_bytes ||
      frame->line_bytes != animation->info.line_bytes) {
        errno = ENOTSUP;
        return -1;
    }

    bitmap = wscons_frame_const_data(animation, frame);
    if (bitmap == NULL)
        return -1;

    full_bytes = animation->info.width / 8U;
    rem_bits = animation->info.width & 7U;

    for (y = 0; y < animation->info.height; y++) {
        const uint8_t *src;
        uint8_t *dst;

        src = bitmap + (size_t)y * frame->line_bytes;
        dst = display->fb_base +
          (size_t)(dst_y + y) * display->stride + dst_x / 8U;

        if (full_bytes != 0)
            memcpy(dst, src, full_bytes);

        if (rem_bits != 0) {
            uint8_t mask = (uint8_t)(0xffU << (8U - rem_bits));

            dst[full_bytes] = (uint8_t)((dst[full_bytes] &
              (uint8_t)~mask) | (src[full_bytes] & mask));
        }
    }

    return 0;
}

static void
handle_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static int
install_signal_handlers(void)
{
    static const int signals[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
    struct sigaction sa;
    size_t i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    if (sigemptyset(&sa.sa_mask) == -1)
        return -1;

    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        if (sigaction(signals[i], &sa, NULL) == -1)
            return -1;
    }
    return 0;
}

static int
wait_until(uint32_t deadline, bool monitor_stdin)
{
    while (!stop_requested) {
        uint32_t now;
        int32_t remaining;
        struct timeval tv;
        fd_set readfds;
        int nfds, rv;

        now = gettime_ms();
        remaining = (int32_t)(deadline - now);
        if (remaining <= 0)
            break;

        tv.tv_sec = remaining / 1000;
        tv.tv_usec = (remaining % 1000) * 1000;
        FD_ZERO(&readfds);
        nfds = 0;
        if (monitor_stdin) {
            FD_SET(STDIN_FILENO, &readfds);
            nfds = STDIN_FILENO + 1;
        }

        rv = select(nfds, monitor_stdin ? &readfds : NULL,
          NULL, NULL, &tv);
        if (rv == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (rv > 0 && monitor_stdin && FD_ISSET(STDIN_FILENO, &readfds)) {
            char buf[32];
            ssize_t len;
            ssize_t i;

            len = read(STDIN_FILENO, buf, sizeof(buf));
            if (len == -1) {
                if (errno == EINTR || errno == EAGAIN)
                    continue;
                return -1;
            }
            for (i = 0; i < len; i++) {
                if (buf[i] == 'q') {
                    stop_requested = 1;
                    break;
                }
            }
        }
    }

    return 0;
}

static void
usage(void)
{
    fprintf(stderr,
      "Usage: %s [-C] [-c] [-d] [-p] [-r] [-f framebuffer-device]\n"
      "       [-b background-file] [-x x-position] [-y y-position] gif-file\n",
      progname != NULL ? progname : "monogifplay-wscons");
    fprintf(stderr,
      "  -C  Center the GIF in the framebuffer.\n"
      "  -b  Display a MonoBG background before playback.\n"
      "  -c  Clear the whole screen to white before playback.\n"
      "  -d  Show duration information (implies -p).\n"
      "  -p  Show progress messages.\n"
      "  -r  Restore the visible pre-playback screen on exit.\n"
      "  -f  Select wsdisplay device (default: $FRAMEBUFFER or %s).\n"
      "  -x  Set the left X position in pixels (must be a multiple of 8).\n"
      "  -y  Set the top Y position in pixels.\n",
      DEF_FBDEV);
    exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
    WsDisplay display;
    DisplayPosition position;
    MonoGifInfo gif_info;
    WsconsAnimation animation;
    MonoBgReader background;
    GifFileType *gif;
    const char *device, *giffile, *background_file;
    uint8_t *background_line;
    char *progpath;
    char errmsg[512];
    int opt, gif_error;
    int saved_errno;
    bool have_error;
    bool restore_screen;
    int exit_status;
    uint64_t raster_total;
    long requested_x, requested_y;
    int i;

    wsdisplay_init(&display);
    memset(&position, 0, sizeof(position));
    memset(&gif_info, 0, sizeof(gif_info));
    wscons_animation_init(&animation);
    monobg_reader_init(&background);
    gif = NULL;
    background_line = NULL;
    progpath = strdup(argv[0]);
    if (progpath == NULL)
        err(EXIT_FAILURE, "strdup");
    progname = basename(progpath);

    device = NULL;
    background_file = NULL;
    restore_screen = false;
    requested_x = -1;
    requested_y = -1;
    while ((opt = getopt(argc, argv, "Cb:cdf:prx:y:")) != -1) {
        switch (opt) {
        char *endptr;
        case 'C':
            opt_center = 1;
            break;
        case 'b':
            background_file = optarg;
            break;
        case 'c':
            opt_clear = 1;
            break;
        case 'd':
            opt_duration = 1;
            opt_progress = 1;
            break;
        case 'f':
            device = optarg;
            break;
        case 'p':
            opt_progress = 1;
            break;
        case 'r':
            restore_screen = true;
            break;
        case 'x':
            requested_x = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || requested_x < 0)
                usage();
            break;
        case 'y':
            requested_y = strtol(optarg, &endptr, 10);
            if (*endptr != '\0' || requested_y < 0)
                usage();
            break;
        default:
            usage();
        }
    }
    if (optind + 1 != argc ||
      (background_file != NULL && opt_clear))
        usage();

    if (device == NULL)
        device = getenv("FRAMEBUFFER");
    if (device == NULL || *device == '\0')
        device = DEF_FBDEV;
    giffile = argv[optind];

    init_gettime_ms();
    if (opt_duration)
        total_start_time = gettime_ms();

    have_error = false;
    saved_errno = 0;
    exit_status = EXIT_FAILURE;
    errmsg[0] = '\0';

#define FAIL_ERRNO(...) do {                                           \
        saved_errno = errno;                                           \
        (void)snprintf(errmsg, sizeof(errmsg), __VA_ARGS__);           \
        have_error = true;                                             \
        goto cleanup;                                                  \
    } while (0)
#define FAIL_MSG(...) do {                                             \
        saved_errno = 0;                                               \
        (void)snprintf(errmsg, sizeof(errmsg), __VA_ARGS__);           \
        have_error = true;                                             \
        goto cleanup;                                                  \
    } while (0)

    if (wsdisplay_open_and_query(&display, device) == -1)
        FAIL_ERRNO("initialize wsdisplay device %s", device);

    if (display.original_mode != WSDISPLAYIO_MODE_EMUL)
        FAIL_MSG("%s is not in WSDISPLAYIO_MODE_EMUL", device);
    if (display.type != WSDISPLAY_TYPE_LUNA)
        FAIL_MSG("%s is not a WSDISPLAY_TYPE_LUNA framebuffer", device);
    if (display.width == 0 || display.height == 0 ||
      display.depth != 1 || display.stride < (display.width + 7U) / 8U)
        FAIL_MSG("unsupported framebuffer geometry: %ux%u, depth %u, stride %u",
          display.width, display.height, display.depth, display.stride);

    if (background_file != NULL) {
        if (monobg_reader_open(background_file, &background) == -1)
            FAIL_ERRNO("open background file %s", background_file);
        if (monobg_validate_display(&background.info, &display) == -1)
            FAIL_MSG("background %s does not match framebuffer %ux%u, depth %u",
              background_file, display.width, display.height, display.depth);
        background_line = malloc(background.info.line_bytes);
        if (background_line == NULL)
            FAIL_ERRNO("allocate background line buffer");
    }

    if (opt_progress) {
        fprintf(stderr,
          "%s: %ux%u, depth %u, stride %u, offset %zu (%s)\n",
          device, display.width, display.height, display.depth,
          display.stride, display.fb_offset,
          display.used_extended_info ? "GET_FBINFO" : "GINFO fallback");
        if (background_file != NULL) {
            fprintf(stderr,
              "%s: MonoBG %ux%u, %u bytes per line, %u bytes payload\n",
              background_file, background.info.width, background.info.height,
              background.info.line_bytes, background.info.payload_size);
        }
        fprintf(stderr, "Loading GIF file...");
    }
    if (opt_duration)
        gifload_start_time = gettime_ms();

    gif = DGifOpenFileName(giffile, &gif_error);
    if (gif == NULL)
        FAIL_MSG("cannot open %s: %s", giffile, GifErrorString(gif_error));

    if (gif->SWidth <= 0 || gif->SHeight <= 0)
        FAIL_MSG("invalid GIF logical screen size: %dx%d",
          gif->SWidth, gif->SHeight);
    if ((unsigned int)gif->SWidth > display.width ||
      (unsigned int)gif->SHeight > display.height) {
        FAIL_MSG("GIF logical screen %dx%d does not fit framebuffer %ux%u",
          gif->SWidth, gif->SHeight, display.width, display.height);
    }
    if (display_position_resolve(&position, &display,
      (unsigned int)gif->SWidth, (unsigned int)gif->SHeight,
      opt_center != 0, requested_x, requested_y) == -1) {
        FAIL_MSG("requested position does not fit GIF logical screen "
          "%dx%d in framebuffer %ux%u",
          gif->SWidth, gif->SHeight, display.width, display.height);
    }

    if (DGifSlurp(gif) != GIF_OK)
        FAIL_MSG("cannot load %s: %s", giffile, GifErrorString(gif->Error));

    if (opt_duration)
        gifload_end_time = gettime_ms();
    if (opt_progress) {
        if (opt_duration)
            fprintf(stderr, " completed in %u ms.",
              gifload_end_time - gifload_start_time);
        fprintf(stderr, "\n");
        fprintf(stderr, "position: %u,%u\n", position.x, position.y);
    }

    if (gif->ImageCount <= 0)
        FAIL_MSG("%s contains no GIF image frames", giffile);

    if (mono_gif_info_init(&gif_info, (unsigned int)gif->SWidth,
      (unsigned int)gif->SHeight, gif->ImageCount) == -1)
        FAIL_ERRNO("initialize monochrome GIF geometry");

    if (wscons_animation_allocate(&animation, &gif_info) == -1)
        FAIL_ERRNO("allocate monochrome frame pool");

    raster_total = 0;
    for (i = 0; i < gif->ImageCount; i++) {
        const GifImageDesc *desc = &gif->SavedImages[i].ImageDesc;
        raster_total += (uint64_t)desc->Width * (uint64_t)desc->Height;
    }

    if (opt_progress) {
        fprintf(stderr,
          "%s: %ux%u, %d frames, RasterBits %llu bytes, 1bpp pool %zu bytes\n",
          giffile, animation.info.width,
          animation.info.height, animation.info.frame_count,
          (unsigned long long)raster_total, animation.bitmap_pool_size);
    }

    if (wscons_extract_mono_frames(gif, &animation) == -1)
        FAIL_ERRNO("extract monochrome GIF frames");

    if (DGifCloseFile(gif, &gif_error) != GIF_OK) {
        gif = NULL;
        FAIL_MSG("close %s: %s", giffile, GifErrorString(gif_error));
    }
    gif = NULL;

    wscons_animation_finish_loading(&animation);

    if (opt_duration) {
        uint32_t total_end_time = gettime_ms();

        fprintf(stderr, "\nSummary:\n");
        fprintf(stderr, "Total processing time: %u ms\n",
          total_end_time - total_start_time);
        fprintf(stderr, "GIF file loading time: %u ms\n",
          gifload_end_time - gifload_start_time);
        fprintf(stderr, "Total frame processing time: %u ms\n",
          total_frame_time);
        fprintf(stderr, "Average frame processing time: %u ms\n",
          total_frame_time / (uint32_t)animation.info.frame_count);
        fprintf(stderr, "1bpp frame pool: %zu bytes\n",
          animation.bitmap_pool_size);
    }

    if (install_signal_handlers() == -1)
        FAIL_ERRNO("install signal handlers");
    if (wsdisplay_enter_dumbfb(&display, restore_screen) == -1)
        FAIL_ERRNO("enter wsdisplay dumb framebuffer mode");

    if (background_file != NULL) {
        if (opt_progress)
            fprintf(stderr, "Displaying background %s...", background_file);
        if (wsdisplay_stream_background(&display, &background,
          background_line) == -1)
            FAIL_ERRNO("display background %s", background_file);
        if (opt_progress)
            fprintf(stderr, " completed.\n");
        monobg_reader_close(&background);
        free(background_line);
        background_line = NULL;
    }

    for (;;) {
        for (i = 0; i < animation.info.frame_count; i++) {
            uint32_t nextframe_time;

            if (stop_requested)
                goto playback_done;

            nextframe_time = gettime_ms() + animation.frames[i].gif.delay;
            if (wsdisplay_blit_frame(&display, &animation, i,
              position.x, position.y) == -1)
                FAIL_ERRNO("draw GIF frame %d", i);
            if (wait_until(nextframe_time, display.stdin_is_tty) == -1)
                FAIL_ERRNO("wait for GIF frame %d", i);
        }
    }

playback_done:
    exit_status = EXIT_SUCCESS;

cleanup:
    if (gif != NULL)
        (void)DGifCloseFile(gif, NULL);
    monobg_reader_close(&background);
    free(background_line);
    wsdisplay_cleanup(&display);
    wscons_animation_destroy(&animation);
    free(progpath);

    if (have_error) {
        if (saved_errno != 0) {
            errno = saved_errno;
            warn("%s", errmsg);
        } else {
            warnx("%s", errmsg);
        }
    }

    return exit_status;
#undef FAIL_ERRNO
#undef FAIL_MSG
}
