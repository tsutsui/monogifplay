#include <sys/types.h>

#include <errno.h>
#include <err.h>
#include <libgen.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <gif_lib.h>

#include "monobg_format.h"

#define TARGET_WIDTH  1280U
#define TARGET_HEIGHT 1024U

static const char *progname;
static int opt_duration;
static int opt_progress;
static uint32_t tv_sec_start;

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
gif_background_validate(const GifFileType *gif,
  unsigned int target_width, unsigned int target_height)
{
    const GifImageDesc *desc;
    unsigned int left, top, width, height;

    if (gif == NULL || gif->SWidth != (int)target_width ||
      gif->SHeight != (int)target_height || gif->ImageCount != 1) {
        errno = EINVAL;
        return -1;
    }

    desc = &gif->SavedImages[0].ImageDesc;
    if (desc->Left < 0 || desc->Top < 0 ||
      desc->Width <= 0 || desc->Height <= 0) {
        errno = EINVAL;
        return -1;
    }

    left = (unsigned int)desc->Left;
    top = (unsigned int)desc->Top;
    width = (unsigned int)desc->Width;
    height = (unsigned int)desc->Height;
    if (left > target_width || top > target_height ||
      width > target_width - left || height > target_height - top) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int
gif_background_render(const GifFileType *gif, MonoBgImage *background)
{
    const SavedImage *image;
    const GifImageDesc *desc;
    const ColorMapObject *cmap;
    GraphicsControlBlock gcb;
    uint8_t bw_cache[256];
    unsigned int x, y;
    int transparent;

    if (gif_background_validate(gif, TARGET_WIDTH, TARGET_HEIGHT) == -1)
        return -1;
    if (monobg_info_init(&background->info,
      TARGET_WIDTH, TARGET_HEIGHT) == -1)
        return -1;

    background->pixels = malloc(background->info.payload_size);
    if (background->pixels == NULL)
        return -1;
    memset(background->pixels, 0xff, background->info.payload_size);

    image = &gif->SavedImages[0];
    desc = &image->ImageDesc;
    cmap = desc->ColorMap != NULL ? desc->ColorMap : gif->SColorMap;
    if (cmap == NULL || cmap->ColorCount <= 0 || cmap->ColorCount > 256) {
        errno = EINVAL;
        return -1;
    }

    memset(bw_cache, 0, sizeof(bw_cache));
    for (x = 0; x < (unsigned int)cmap->ColorCount; x++) {
        const GifColorType *color = &cmap->Colors[x];
        unsigned int brightness;

        brightness = color->Red * 299U +
          color->Green * 587U + color->Blue * 114U;
        bw_cache[x] = brightness > 128000U ? 1U : 0U;
    }

    memset(&gcb, 0, sizeof(gcb));
    gcb.TransparentColor = NO_TRANSPARENT_COLOR;
    (void)DGifSavedExtensionToGCB((GifFileType *)gif, 0, &gcb);
    transparent = gcb.TransparentColor;

    for (y = 0; y < (unsigned int)desc->Height; y++) {
        for (x = 0; x < (unsigned int)desc->Width; x++) {
            size_t raster_offset;
            size_t bitmap_offset;
            unsigned int screen_x, screen_y;
            unsigned int index;
            uint8_t mask;

            raster_offset = (size_t)y * (unsigned int)desc->Width + x;
            index = image->RasterBits[raster_offset];
            if ((int)index == transparent)
                continue;
            if (index >= (unsigned int)cmap->ColorCount) {
                errno = EINVAL;
                return -1;
            }

            screen_x = (unsigned int)desc->Left + x;
            screen_y = (unsigned int)desc->Top + y;
            bitmap_offset = (size_t)screen_y * background->info.line_bytes +
              screen_x / 8U;
            mask = (uint8_t)(0x80U >> (screen_x & 7U));
            if (bw_cache[index] != 0)
                background->pixels[bitmap_offset] |= mask;
            else
                background->pixels[bitmap_offset] &= (uint8_t)~mask;
        }
    }

    return 0;
}

static void
usage(void)
{
    fprintf(stderr, "Usage: %s [-d] [-p] gif-file background-file\n",
      progname != NULL ? progname : "gif2monobg");
    fprintf(stderr,
      "  -d  Show duration information (implies -p).\n"
      "  -p  Show progress messages.\n");
    exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
    MonoBgImage background;
    GifFileType *gif;
    const char *giffile, *background_file;
    char *progpath;
    int gif_error;
    int opt;
    uint32_t start_time, load_end_time, render_end_time, write_end_time;

    monobg_image_init(&background);
    gif = NULL;
    progpath = strdup(argv[0]);
    if (progpath == NULL)
        err(EXIT_FAILURE, "strdup");
    progname = basename(progpath);

    while ((opt = getopt(argc, argv, "dp")) != -1) {
        switch (opt) {
        case 'd':
            opt_duration = 1;
            opt_progress = 1;
            break;
        case 'p':
            opt_progress = 1;
            break;
        default:
            usage();
        }
    }
    if (optind + 2 != argc)
        usage();

    giffile = argv[optind];
    background_file = argv[optind + 1];
    init_gettime_ms();
    start_time = gettime_ms();

    if (opt_progress)
        fprintf(stderr, "Loading GIF file...");
    gif = DGifOpenFileName(giffile, &gif_error);
    if (gif == NULL)
        errx(EXIT_FAILURE, "cannot open %s: %s",
          giffile, GifErrorString(gif_error));
    if (DGifSlurp(gif) != GIF_OK)
        errx(EXIT_FAILURE, "cannot load %s: %s",
          giffile, GifErrorString(gif->Error));
    load_end_time = gettime_ms();
    if (opt_progress)
        fprintf(stderr, " completed%s\n", opt_duration ? "" : ".");

    if (gif_background_validate(gif, TARGET_WIDTH, TARGET_HEIGHT) == -1)
        errx(EXIT_FAILURE,
          "%s must be a %ux%u single-image GIF",
          giffile, TARGET_WIDTH, TARGET_HEIGHT);

    if (opt_progress)
        fprintf(stderr, "Converting to MonoBG...");
    if (gif_background_render(gif, &background) == -1)
        err(EXIT_FAILURE, "convert %s", giffile);
    render_end_time = gettime_ms();
    if (opt_progress)
        fprintf(stderr, " completed%s\n", opt_duration ? "" : ".");

    if (DGifCloseFile(gif, &gif_error) != GIF_OK) {
        gif = NULL;
        errx(EXIT_FAILURE, "close %s: %s",
          giffile, GifErrorString(gif_error));
    }
    gif = NULL;

    if (opt_progress)
        fprintf(stderr, "Writing %s...", background_file);
    if (monobg_write_file(background_file, &background) == -1)
        err(EXIT_FAILURE, "write %s", background_file);
    write_end_time = gettime_ms();
    if (opt_progress)
        fprintf(stderr, " completed%s\n", opt_duration ? "" : ".");

    if (opt_duration) {
        fprintf(stderr, "\nSummary:\n");
        fprintf(stderr, "GIF loading time: %u ms\n",
          load_end_time - start_time);
        fprintf(stderr, "Conversion time: %u ms\n",
          render_end_time - load_end_time);
        fprintf(stderr, "File writing time: %u ms\n",
          write_end_time - render_end_time);
        fprintf(stderr, "Total processing time: %u ms\n",
          write_end_time - start_time);
    }

    monobg_image_destroy(&background);
    free(progpath);
    return EXIT_SUCCESS;
}
