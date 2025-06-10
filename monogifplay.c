/*
 * MonoGIFPlayer: a monochrome GIF player optimized for 1 bpp Xserver.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <libgen.h>
#include <time.h>
#include <err.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <gif_lib.h>

/* monochrome frame structure */
typedef struct {
    int width, height;
    unsigned int delay;         /* in ms */
    uint8_t *bitmap_data;       /* packed bitmap for XImage */
    Pixmap pixmap;
} MonoFrame;

static const char *progname = NULL;

/* Option flags */
int opt_duration = 0;
int opt_progress = 0;

/* Global variables for time measurement (in milliseconds). */
long total_start_time = 0, total_end_time = 0;
long gifload_start_time = 0, gifload_end_time = 0;
long total_frame_time = 0;

#define DEF_GIF_DELAY	75

/* sleep specified number of ms */
static void
msleep(unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000U;
    ts.tv_nsec = (ms % 1000U) * 1000000U;
    nanosleep(&ts, NULL);
}

/* get the current monotonic clock time in ms */
static time_t
gettime_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000U + ts.tv_nsec / 1000000U;
}

/* extract monochrome frames from gif file */
static int
extract_mono_frames(GifFileType *gif, MonoFrame *frames, int bitorder)
{
    GraphicsControlBlock gcb;
    int i, frame_count;
    int swidth, sheight;
    static const uint8_t
        bebits[8] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01},
        lebits[8] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80},
        *bits;

    frame_count = gif->ImageCount;
    swidth  = gif->SWidth;
    sheight = gif->SHeight;
    bits = bitorder == MSBFirst ? bebits : lebits;

    for (i = 0; i < frame_count; i++) {
        long frame_start_time = 0, frame_end_time = 0;
        int j, x, y;
        int screenx, screeny, frame_row_offset, bitmap_row_offset;
        int frame_width, frame_height, frame_left, frame_top;
        MonoFrame *frame = &frames[i];
        SavedImage *img;
        GifImageDesc *desc;
        ColorMapObject *cmap;
        uint8_t *bitmap;
        int delay, transparent_index, line_bytes;
        uint8_t bw_bit_cache[256];

        if (opt_progress) {
            /* Show progress for each frame */
            fprintf(stderr, "Preparing bitmap for frame %d/%d...%s",
            i + 1, frame_count,
            opt_duration ? "" : (i < frame_count - 1 ? "\r" : "\n"));
        }
        if (opt_duration) {
            /* Start timing for this frame */
            frame_start_time = gettime_ms();
        }
        img = &gif->SavedImages[i];
        desc = &img->ImageDesc;

        cmap = desc->ColorMap ? desc->ColorMap : gif->SColorMap;
        if (cmap == NULL) {
            if (opt_progress) {
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "No valid color map in frame %d\n", i);
            return -1;
        }

        frame->width  = swidth;
        frame->height = sheight;
        DGifSavedExtensionToGCB(gif, i, &gcb);
        delay = gcb.DelayTime * 10; /* delay is stored in 1/100 sec */
        frame->delay = delay > 0 ? delay : DEF_GIF_DELAY;
        transparent_index = gcb.TransparentColor;

        line_bytes = (swidth + 7) / 8;
        bitmap = malloc(line_bytes * sheight);
        if (bitmap == NULL) {
            if (opt_progress) {
                fprintf(stderr, "\n");
            }
            fprintf(stderr, "Failed to allocate bitmap for frame %d\n", i);
            return -1;
        }
        frame->bitmap_data = bitmap;

        frame_width  = desc->Width;
        frame_height = desc->Height;
        frame_left   = desc->Left;
        frame_top    = desc->Top;

        if (transparent_index != NO_TRANSPARENT_COLOR ||
          swidth != frame_width || sheight != frame_height ||
          frame_left != 0 || frame_top != 0) {
            if (i == 0) {
                /* first frame should have whole screen data */
                memset(bitmap, 0, line_bytes * sheight);
            } else {
                /* copy the previous frame for transparent color etc. */
                memcpy(bitmap, frames[i - 1].bitmap_data,
                    line_bytes * sheight);
            }
        }

        memset(bw_bit_cache, 0, sizeof(bw_bit_cache));
        for (j = 0; j < cmap->ColorCount; j++) {
            GifColorType c = cmap->Colors[j];
            if (c.Red * 299 + c.Green * 587 + c.Blue * 114 > 128000) {
                bw_bit_cache[j] = 0xff;
            }
        }

        for (y = 0, screeny = frame_top,
          bitmap_row_offset = frame_top * line_bytes,
          frame_row_offset = 0;
          y < frame_height;
          y++, screeny++,
          bitmap_row_offset += line_bytes,
          frame_row_offset += frame_width) {
            int frame_byte_offset;
            for (x = 0, screenx = frame_left,
              frame_byte_offset = frame_row_offset;
              x < frame_width;
              x++, screenx++, frame_byte_offset++) {
                unsigned int byte, bit, bitmap_byte_offset;
                GifByteType px;

                px = img->RasterBits[frame_byte_offset];
                if (px == transparent_index) {
                    /* leave transparent pixels */
                    continue;
                }

                byte = screenx >> 3;
                bitmap_byte_offset = bitmap_row_offset + byte;
                bit  = screenx & 0x07;
                /* set/reset pixels using cached b&w data per palette */
                bitmap[bitmap_byte_offset] &= ~bits[bit];
                bitmap[bitmap_byte_offset] |= bw_bit_cache[px] & bits[bit];
            }
        }
        if (opt_duration) {
            /* End timing for this frame and report */
            long frame_time;

            frame_end_time = gettime_ms();
            frame_time = frame_end_time - frame_start_time;
            total_frame_time += frame_time;
            if (!opt_progress) {
                fprintf(stderr, "Frame %d", i + 1);
            }
            fprintf(stderr, " completed in %ld ms.\n", frame_time);
        }
    }

    return 0;
}

static void
usage(void)
{
    fprintf(stderr, "Usage: %s [-d] [-p] gif-file\n",
      progname != NULL ? progname : "monogifplay");
    fprintf(stderr,
      "  -d    Show duration (time) info for each process. (assume -p)\n");
    fprintf(stderr,
      "  -p    Show progress messages for each process.\n");
    exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
    char *progpath, *giffile;
    int opt;
    int err, screen, depth;
    Display *dpy;
    int bitorder;
    int frame_count;
    int i;
    char title[512];
    GifFileType *gif;
    MonoFrame *frame, *frames;
    int swidth, sheight, line_bytes;
    XImage *image;
    unsigned long white, black;
    Window win;
    Atom wm_delete_window;
    GC mono_gc = NULL, gc;
    int xfd;
    int skipped = 0;

    progpath = strdup(argv[0]);
    progname = basename(progpath);

    while ((opt = getopt(argc, argv, "dp")) != -1) {
        switch (opt) {
        case 'd':
            opt_duration = 1;
            /* FALLTHROUGH */
        case 'p':
            opt_progress = 1;
            break;
        default:
            usage();
        }
    }
    if (optind + 1 != argc) {
        usage();
    }

    giffile = strdup(argv[optind]);

    if (opt_progress) {
        /* Show progress for GIF file loading and processing */
        fprintf(stderr, "Loading and extracting GIF file...%s",
          opt_duration ? "" : "\n");
    }
    if (opt_duration) {
        /* Start timing for total and GIF loading/processing */
        long now = gettime_ms();
        total_start_time = now;
        gifload_start_time = now;
    }
    gif = DGifOpenFileName(giffile, &err);
    if (gif == NULL) {
        if (opt_duration) {
            fprintf(stderr, "\n");
        }
        errx(EXIT_FAILURE, "Failed to open a gif file: %s",
          GifErrorString(err));
    }

    if (DGifSlurp(gif) != GIF_OK) {
        if (opt_duration) {
            fprintf(stderr, "\n");
        }
        errx(EXIT_FAILURE, "Failed to load a gif file: %s",
          GifErrorString(gif->Error));
    }

    if (opt_duration) {
        /* End timing for GIF loading/processing and report */
        gifload_end_time = gettime_ms();
	/* opt_progress is also enabled */
        fprintf(stderr, " completed in %ld ms.\n",
          gifload_end_time - gifload_start_time);
    }

    swidth = gif->SWidth;
    sheight = gif->SHeight;
    frame_count = gif->ImageCount;
    if (opt_progress) {
        /* Show GIF file infomation */
        fprintf(stderr, "%s: %dx%d, %d frames, %d colors\n", basename(giffile),
            swidth, sheight, frame_count, gif->SColorMap->ColorCount);
    }

    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        errx(EXIT_FAILURE, "Cannot connect Xserver\n");
    }
    screen = DefaultScreen(dpy);
    depth = DefaultDepth(dpy, screen);
    bitorder = depth == 1 ? BitmapBitOrder(dpy) : MSBFirst;

    if (opt_progress) {
        fprintf(stderr, "Display: depth: %d, BitOrder: %s First\n",
          depth, bitorder == MSBFirst ? "MSB" : "LSB");
    }

    frames = calloc(frame_count, sizeof(MonoFrame));
    if (frames == NULL) {
        errx(EXIT_FAILURE, "Failed to allocate memory for frame data");
    }

    if (extract_mono_frames(gif, frames, bitorder) < 0 || frame_count == 0) {
        errx(EXIT_FAILURE, "Failed to extract mono frames");
    }

    line_bytes = (swidth + 7) / 8;
    image = XCreateImage(dpy, DefaultVisual(dpy, screen),
      1, XYBitmap, 0, NULL, swidth, sheight, 8, line_bytes);
    if (image == NULL)
        errx(EXIT_FAILURE, "XCreateImage() failed");
    image->byte_order = MSBFirst;
    image->bitmap_bit_order = bitorder;

    for (i = 0; i < frame_count; i++) {
        frame = &frames[i];

        frame->pixmap = XCreatePixmap(dpy, RootWindow(dpy, screen),
          swidth, sheight, 1);
        if (i == 0) {
            XGCValues gcv = { 0 };

            gcv.function   = GXcopy;
            gcv.graphics_exposures = False;
            mono_gc = XCreateGC(dpy, frame->pixmap,
              GCFunction | GCGraphicsExposures,
              &gcv);
        }

        image->data = (char *)frame->bitmap_data;
        XPutImage(dpy, frame->pixmap, mono_gc, image, 0, 0, 0, 0,
          swidth, sheight);
    }
    image->data = NULL; /* Prevent XDestroyImage(3) from freeing image->data */

    /* Print summary of processing times before creating the X11 window. */
    if (opt_duration) {
        total_end_time = gettime_ms();
        fprintf(stderr, "\n");
        fprintf(stderr, "Summary:\n");
        fprintf(stderr, "Total processing time: %ld ms\n",
          total_end_time - total_start_time);
        fprintf(stderr, "Total GIF file loading+processing time: %ld ms\n",
          gifload_end_time - gifload_start_time);
        fprintf(stderr, "Total frame processing time: %ld ms\n",
          total_frame_time);
        if (frame_count > 0)
            fprintf(stderr, "Average frame processing time: %ld ms\n",
              total_frame_time / frame_count);
    }

    black = BlackPixel(dpy, screen);
    white = WhitePixel(dpy, screen);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
      10, 10, swidth, sheight, 1, black, white);

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);

    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);
    XMapWindow(dpy, win);

    /* set window title */
    snprintf(title, sizeof(title), "%s - MonoGIFPlayer", basename(giffile));
    XStoreName(dpy, win, title);

    gc = DefaultGC(dpy, screen);
    XSetForeground(dpy, gc, black);
    XSetBackground(dpy, gc, white);

    xfd = ConnectionNumber(dpy);

    /*
     * Main animation loop: display each frame and handle events.
     */
    for (;;) {
        for (i = 0; i < frame_count; i++) {
            time_t start, elapsed;

            start = gettime_ms();
            frame = &frames[i];
            XCopyPlane(dpy, frame->pixmap, win, gc, 0, 0,
              swidth, sheight, 0, 0, 1);
            XFlush(dpy);
            elapsed = gettime_ms() - start;
            if (frame->delay > elapsed || skipped >= frame_count) {
                int wait_ms;
                int rv;
                struct timeval tv;
                fd_set fds;

                skipped = 0;
                FD_ZERO(&fds);
                FD_SET(xfd, &fds);
                if (frame->delay > elapsed) {
                    wait_ms = frame->delay - elapsed;
                } else {
                    wait_ms = 0;
                }
                tv.tv_sec = wait_ms / 1000U;
                tv.tv_usec = (wait_ms % 1000U) * 1000U;

                rv = select(xfd + 1, &fds, NULL, NULL, &tv);
                if (rv > 0 && FD_ISSET(xfd, &fds)) {
                    while (XPending(dpy) > 0) {
                        XEvent event;
                        XNextEvent(dpy, &event);
                        if (event.type == KeyPress) {
                            char buf[16];
                            KeySym keysym;
                            XLookupString(&event.xkey, buf, sizeof(buf),
                              &keysym, NULL);
                            if (buf[0] == 'q')
                                goto cleanup;
                        } else if (event.type == ClientMessage) {
                            if ((Atom)event.xclient.data.l[0] ==
                              wm_delete_window) {
                                goto cleanup;
                            }
                        }
                    }
                }
                elapsed = gettime_ms() - start;
                if (frame->delay > elapsed) {
                    msleep(frame->delay - elapsed);
                }
            } else {
                skipped++;
            }
        }
    }

 cleanup:
    /* Cleanup resources before exit. */
    if (mono_gc != NULL)
        XFreeGC(dpy, mono_gc);
    XDestroyImage(image);
    for (i = 0; i < frame_count; i++) {
        if (frames[i].pixmap != 0)
            XFreePixmap(dpy, frames[i].pixmap);
        if (frames[i].bitmap_data != NULL)
            free(frames[i].bitmap_data);
    }
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    free(frames);
    DGifCloseFile(gif, NULL);
    exit(EXIT_SUCCESS);
}
