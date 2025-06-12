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
long pixmap_start_time = 0, pixmap_end_time = 0;
long total_frame_time = 0;

#define powerof2(x)	((((x) - 1) & (x)) == 0)
#define roundup(x, y)   ((((x) + ((y) - 1)) / (y)) * (y))

#define DEF_GIF_DELAY	75

#define DEF_GEOM_X	10
#define DEF_GEOM_Y	10

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
extract_mono_frames(GifFileType *gif, MonoFrame *frames)
{
    int i, frame_count;
    int swidth, sheight, line_bytes;

    frame_count = gif->ImageCount;
    swidth  = gif->SWidth;
    sheight = gif->SHeight;
    line_bytes = (swidth + 7) / 8;

    for (i = 0; i < frame_count; i++) {
        long frame_start_time = 0, frame_end_time = 0;
        int j, x, y;
        int screenx, screeny, frame_row_offset, bitmap_row_offset;
        int frame_width, frame_height, frame_left, frame_top;
        MonoFrame *frame = &frames[i];
        SavedImage *img;
        GifImageDesc *desc;
        ColorMapObject *cmap;
        GraphicsControlBlock gcb;
        uint8_t *bitmap;
        int delay, transparent_index;
        uint8_t bw_bit_cache[256];

        if (opt_progress) {
            /* Show progress for each frame */
            fprintf(stderr, "Preparing bitmap for frame %d/%d...",
              i + 1, frame_count);
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
                bw_bit_cache[j] = 0x80;
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
                bitmap[bitmap_byte_offset] &= ~(0x80 >> bit);
                bitmap[bitmap_byte_offset] |= bw_bit_cache[px] >> bit;
            }
        }
        if (opt_progress) {
            if (opt_duration) {
                /* End timing for this frame and report */
                long frame_time;

                frame_end_time = gettime_ms();
                frame_time = frame_end_time - frame_start_time;
                total_frame_time += frame_time;
                fprintf(stderr, " completed in %ld ms.\n", frame_time);
            } else {
                fprintf(stderr, "%s", i < frame_count - 1 ? "\r" : "\n");
            }
        }
    }

    return 0;
}

static int
create_pixmap_for_frames(Display *dpy, int screen,
  MonoFrame *frames, int frame_count, int swidth, int sheight)
{
    XImage *image;
    GC mono_gc = NULL;
    Window root;
    int i, line_bytes;

    line_bytes = (swidth + 7) / 8;
    image = XCreateImage(dpy, DefaultVisual(dpy, screen),
      1, XYBitmap, 0, NULL, swidth, sheight, 8, line_bytes);
    if (image == NULL) {
        return -1;
    }
    image->byte_order = MSBFirst;
    image->bitmap_bit_order = MSBFirst;

    root = RootWindow(dpy, screen);
    for (i = 0; i < frame_count; i++) {
        MonoFrame *frame = &frames[i];

        frame->pixmap = XCreatePixmap(dpy, root, swidth, sheight, 1);
        if (i == 0) {
            XGCValues gcv = { 0 };

            gcv.foreground = 0; /* black pixels in bitmap */
            gcv.background = 1; /* white pixels in bitmap */
            gcv.function   = GXcopy;
            gcv.graphics_exposures = False;
            mono_gc = XCreateGC(dpy, frame->pixmap,
              GCForeground | GCBackground | GCFunction | GCGraphicsExposures,
              &gcv);
            if (mono_gc == NULL) {
                XDestroyImage(image);
                return -1;
            }
        }

        image->data = (char *)frame->bitmap_data;
        XPutImage(dpy, frame->pixmap, mono_gc, image, 0, 0, 0, 0,
          swidth, sheight);
        image->data = NULL;
        free(frame->bitmap_data);
        frame->bitmap_data = NULL;
    }
    XFreeGC(dpy, mono_gc);
    XDestroyImage(image);
    return 0;
}

static Window
create_and_map_window(Display *dpy, int screen, const char *geometry,
  int swidth, int sheight,
  unsigned long border, unsigned long background,
  const char *title)
{
    Window win;
    XSizeHints wmhints = { 0 };
    int gmask;
    int win_x = DEF_GEOM_X, win_y = DEF_GEOM_Y;
    unsigned int win_w, win_h;
    int mapped, exposed, configured;

    /* parse geometry and set size hints for WM */
    wmhints.flags = PWinGravity;
    wmhints.win_gravity = NorthWestGravity;
    gmask = 0;
    if (geometry != NULL) {
        gmask = XParseGeometry(geometry, &win_x, &win_y, &win_w, &win_h);
    }
    if ((gmask & WidthValue) != 0) {
        wmhints.flags |= USSize;
    } else {
        win_w = swidth;
    }
    if ((gmask & HeightValue) != 0) {
        wmhints.flags |= USSize;
    } else {
        win_h = sheight;
    }
    if ((gmask & XValue) != 0) {
        if ((gmask & XNegative) != 0) {
            win_x += DisplayWidth(dpy, screen) - swidth;
            wmhints.win_gravity = NorthEastGravity;
        }
        wmhints.flags |= USPosition;
    }
    if ((gmask & YValue) != 0) {
        if ((gmask & YNegative) != 0) {
            win_y += DisplayHeight(dpy, screen) - sheight;
            if ((gmask & XNegative) != 0) {
                wmhints.win_gravity = SouthEastGravity;
            } else {
                wmhints.win_gravity = SouthWestGravity;
            }
        wmhints.flags |= USPosition;
        }
    }
    wmhints.width = win_w;
    wmhints.height = win_h;
    wmhints.x = win_x;
    wmhints.y = win_y;

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
      win_x, win_y, win_w, win_h, 1, border, background);

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);

    XSetWMNormalHints(dpy, win, &wmhints);
    XStoreName(dpy, win, title);
    XMapWindow(dpy, win);

    mapped = 0;
    exposed = 0;
    configured = 0;
    while (mapped == 0 || exposed == 0 || configured == 0) {
        XEvent event;
        XNextEvent(dpy, &event);
        if (event.type == MapNotify && event.xmap.window == win) {
            mapped = 1;
        }
        if (event.type == Expose && event.xexpose.window == win) {
            exposed = 1;
        }
        if (event.type == ConfigureNotify && event.xconfigure.window == win) {
            configured = 1;
        }
    }
    return win;
}

static void
align_window_x(Display *dpy, Window win, int screen, unsigned int align)
{
    XWindowAttributes attr;
    Window root = RootWindow(dpy, screen);
    Window child;
    int client_x, client_y, aligned_x, new_win_x, new_win_y;

    if (align == 0 || !powerof2(align) || align > 32) {
        return;
    }

    /* クライアントウインドウ相対位置 (WM枠を含む左上→クライアント領域左上) */
    XGetWindowAttributes(dpy, win, &attr);

    /* クライアントウインドウのルートウインドウ座標位置 */
    XTranslateCoordinates(dpy, win, root, 0, 0, &client_x, &client_y, &child);

    /* クライントウインドウX座標を調整 */
    aligned_x = roundup(client_x, align);
    new_win_x = aligned_x - attr.x;
    new_win_y = client_y - attr.y;

    /* ウインドウが画面右端から出てしまう場合はalign分左にずらす */
    if (new_win_x > DisplayWidth(dpy, screen)) {
        new_win_x -= align;
    }

    /* align位置にウインドウを移動 */
    XMoveWindow(dpy, win, new_win_x, new_win_y);

    /* ウインドウ移動が完了するまで待つ */
    for (;;) {
        XEvent event;
        XNextEvent(dpy, &event);
        if (event.type == ConfigureNotify && event.xconfigure.window == win) {
            break;
        }
    }
}

static void
usage(void)
{
    fprintf(stderr, "Usage: %s [-a] [-d] [-p] [-g geometry] gif-file\n",
      progname != NULL ? progname : "monogifplay");
    fprintf(stderr,
      "  -a align     Align client window to multiple of align at startup\n"
      "               (alginx must be power of 2 and <=32)\n"
      "  -d           Show duration (time) info for each process. (assume -p)\n"
      "  -p           Show progress messages for each process.\n"
      "  -g geometry  Set window geometry (WxH+X+Y).\n"
    );
    exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
    char *progpath, *giffile;
    int opt;
    int err, screen;
    int frame_count;
    int i;
    char title[512];
    GifFileType *gif;
    MonoFrame *frame, *frames;
    Display *dpy;
    int swidth, sheight;
    char *geometry = NULL;
    unsigned int alignx = 0;
    unsigned long white, black;
    Window win;
    Atom wm_delete_window;
    GC gc;
    int xfd;

    progpath = strdup(argv[0]);
    progname = basename(progpath);

    while ((opt = getopt(argc, argv, "a:dg:p")) != -1) {
        switch (opt) {
        char *endptr;
        case 'a':
            alignx = (int)strtoul(optarg, &endptr, 10);
            if (*endptr != '\0' || !powerof2(alignx) || alignx > 32) {
                usage();
            } 
            break;
        case 'd':
            opt_duration = 1;
            /* FALLTHROUGH */
        case 'p':
            opt_progress = 1;
            break;
        case 'g':
            geometry = strdup(optarg);
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
        fprintf(stderr, "Loading and extracting GIF file...");
    }
    if (opt_duration) {
        /* Start timing for total and GIF loading/processing */
        long now = gettime_ms();
        total_start_time = now;
        gifload_start_time = now;
    }
    gif = DGifOpenFileName(giffile, &err);
    if (gif == NULL) {
        if (opt_progress) {
            fprintf(stderr, "\n");
        }
        errx(EXIT_FAILURE, "Failed to open a gif file: %s",
          GifErrorString(err));
    }

    if (DGifSlurp(gif) != GIF_OK) {
        if (opt_progress) {
            fprintf(stderr, "\n");
        }
        errx(EXIT_FAILURE, "Failed to load a gif file: %s",
          GifErrorString(gif->Error));
    }

    if (opt_progress) {
        if (opt_duration) {
            /* End timing for GIF loading/processing and report */
            gifload_end_time = gettime_ms();
            fprintf(stderr, " completed in %ld ms.",
              gifload_end_time - gifload_start_time);
        }
        fprintf(stderr, "\n");
    }

    swidth = gif->SWidth;
    sheight = gif->SHeight;
    frame_count = gif->ImageCount;
    if (opt_progress) {
        /* Show GIF file infomation */
        fprintf(stderr, "%s: %dx%d, %d frames, %d colors\n", basename(giffile),
            swidth, sheight, frame_count, gif->SColorMap->ColorCount);
    }

    frames = calloc(frame_count, sizeof(MonoFrame));
    if (frames == NULL) {
        errx(EXIT_FAILURE, "Failed to allocate memory for frame data");
    }

    if (extract_mono_frames(gif, frames) < 0 || frame_count == 0) {
        errx(EXIT_FAILURE, "Failed to extract mono frames");
    }

    /* All necessary GIF image data are stored into frames[]. */
    DGifCloseFile(gif, NULL);

    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        errx(EXIT_FAILURE, "Cannot connect Xserver\n");
    }

    screen = DefaultScreen(dpy);
    if (opt_progress) {
        /* Show progress for pixmap processing */
        fprintf(stderr, "Creating pixmap for all frames...");
    }
    if (opt_duration) {
        /* Start timing for pixmap processing */
        pixmap_start_time = gettime_ms();
    }
    if (create_pixmap_for_frames(dpy, screen, frames, frame_count,
      swidth, sheight) != 0) {
        if (opt_progress) {
            fprintf(stderr, "\n");
        }
        errx(EXIT_FAILURE, "Failed to create pixmap for frames");
    }
    if (opt_progress) {
        if (opt_duration) {
            /* End timing for pixmap processing and report */
            pixmap_end_time = gettime_ms();
            fprintf(stderr, " completed in %ld ms.",
              pixmap_end_time - pixmap_start_time);
        }
        fprintf(stderr, "\n");
    }

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
        if (frame_count > 0) {
            fprintf(stderr, "Average frame processing time: %ld ms\n",
              total_frame_time / frame_count);
        }
        fprintf(stderr, "Total pixmap processing time: %ld ms\n",
          pixmap_end_time - pixmap_start_time);
    }

    black = BlackPixel(dpy, screen);
    white = WhitePixel(dpy, screen);
    snprintf(title, sizeof(title), "%s - MonoGIFPlayer", basename(giffile));
    free(giffile);
    win = create_and_map_window(dpy, screen, geometry, swidth, sheight,
      black, white, title);
    if (geometry != NULL) {
        free(geometry);
    }

    align_window_x(dpy, win, screen, alignx);

    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);

    gc = DefaultGC(dpy, screen);
    XSetForeground(dpy, gc, black);
    XSetBackground(dpy, gc, white);

    xfd = ConnectionNumber(dpy);

    /*
     * Main animation loop: display each frame and handle events.
     */
    for (;;) {
        for (i = 0; i < frame_count; i++) {
            time_t nextframe_time;

            frame = &frames[i];
            nextframe_time = gettime_ms() + frame->delay;
            XCopyPlane(dpy, frame->pixmap, win, gc, 0, 0,
              swidth, sheight, 0, 0, 1);
            XFlush(dpy);
            while (gettime_ms() < nextframe_time) {
                fd_set fds;
                int rv;
                struct timeval tv =
                    { .tv_sec = 0, .tv_usec = 10 * 1000 }; /* 10 ms */

                FD_ZERO(&fds);
                FD_SET(xfd, &fds);
                rv = select(xfd + 1, &fds, NULL, NULL, &tv);
                if (rv > 0 && FD_ISSET(xfd, &fds) && XPending(dpy) > 0) {
                    /* one event per 10 ms is enough */
                    XEvent event;
                    XNextEvent(dpy, &event);
                    if (event.type == KeyPress) {
                        char buf[16];
                        KeySym keysym;
                        XLookupString(&event.xkey, buf, sizeof(buf),
                          &keysym, NULL);
                        if (buf[0] == 'q') {
                            goto cleanup;
                        }
                    } else if (event.type == ClientMessage &&
                      (Atom)event.xclient.data.l[0] ==
                      wm_delete_window) {
                        goto cleanup;
                    }
                }
            }
        }
    }

 cleanup:
    /* Cleanup resources before exit. */
    for (i = 0; i < frame_count; i++) {
        if (frames[i].pixmap != 0)
            XFreePixmap(dpy, frames[i].pixmap);
    }
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    free(frames);
    exit(EXIT_SUCCESS);
}
