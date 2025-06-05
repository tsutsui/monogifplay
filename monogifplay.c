#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <err.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#include <gif_lib.h>

/* sleep specified ms */
static void
msleep(unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000U;
    ts.tv_nsec = (ms % 1000U) * 1000000U;
    nanosleep(&ts, NULL);
}

/* monochrome frame structure */
typedef struct {
    int width, height;
    uint8_t *data; /* 1 pixel per byte: 0 or 1 */
    unsigned int delay; /* ms */
} MonoFrame;

/* extract monochrome frames from gif file */
static int
extract_mono_frames(GifFileType *gif, MonoFrame **out_frames, int *out_count)
{
    GraphicsControlBlock gcb;
    int i, frame_count = 0;
    MonoFrame *frames = NULL;

    for (i = 0; i < gif->ImageCount; i++) {
        int x, y;
        MonoFrame frame;
        SavedImage *img;
        ColorMapObject *cmap;

        img = &gif->SavedImages[i];
        DGifSavedExtensionToGCB(gif, i, &gcb);

        cmap = img->ImageDesc.ColorMap ?
          img->ImageDesc.ColorMap : gif->SColorMap;
        if (cmap == NULL)
            continue;

        frame.width = img->ImageDesc.Width;
        frame.height = img->ImageDesc.Height;
        frame.delay = gcb.DelayTime * 10; /* delay is stored in 1/100 sec */

        frame.data = calloc(frame.width * frame.height, 1);
        if (frame.data == NULL)
            return -1;

        for (y = 0; y < frame.height; y++) {
            for (x = 0; x < frame.width; x++) {
                int idx = y * frame.width + x;
                GifByteType px = img->RasterBits[idx];
                GifColorType c = cmap->Colors[px];
                /* convert to monochrome per RGB values */
                frame.data[idx] = (c.Red + c.Green + c.Blue > 128 * 3) ? 1 : 0;
            }
        }

        frames = realloc(frames, sizeof(MonoFrame) * (frame_count + 1));
        if (frames == NULL)
            return -1;
        frames[frame_count++] = frame;
    }

    *out_frames = frames;
    *out_count = frame_count;
    return 0;
}

static void
usage(void)
{
    fprintf(stderr, "Usage: %s gif-file\n", getprogname());
    exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
    int err, screen, depth;
    int frame_count;
    int line_bytes;
    int i, x, y;
    char title[512];
    GifFileType *gif;
    MonoFrame *frames;
    Display *dpy;
    Window win;
    Atom wm_delete_window;
    GC gc;
    XImage *img;
    int whitepixel;

    setprogname(argv[0]);

    if (argc != 2) {
        usage();
    }

    gif = DGifOpenFileName(argv[1], &err);
    if (gif == NULL || DGifSlurp(gif) != GIF_OK) {
        errx(EXIT_FAILURE, "Failed to load a gif file: %s",
          GifErrorString(err));
    }

    frames = NULL;
    frame_count = 0;
    if (extract_mono_frames(gif, &frames, &frame_count) < 0 ||
      frame_count == 0) {
        errx(EXIT_FAILURE, "Failed to extract mono frames");
    }

    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        errx(EXIT_FAILURE, "Cannot connect Xserver\n");
    }

    screen = DefaultScreen(dpy);
    depth = DefaultDepth(dpy, screen);
    if (depth != 1) {
        errx(EXIT_FAILURE, "Not a monochrome display (depth=%d)", depth);
    }

    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen),
      10, 10, gif->SWidth, gif->SHeight, 1,
      BlackPixel(dpy, screen), WhitePixel(dpy, screen));

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);

    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);
    XMapWindow(dpy, win);

    /* set window title */
    snprintf(title, sizeof(title), "%s - MonoGIFPlayer", argv[1]);
    XStoreName(dpy, win, title);

    gc = DefaultGC(dpy, screen);

    line_bytes = (gif->SWidth + 7) / 8;
    img = XCreateImage(dpy, DefaultVisual(dpy, screen), 1,
      XYBitmap, 0, NULL, gif->SWidth, gif->SHeight, 8, line_bytes);
    img->data = malloc(line_bytes * gif->SHeight);
    if (img->data == NULL) {
        errx(EXIT_FAILURE, "Cannot allocate memory for images");
    }
    img->bitmap_bit_order = MSBFirst;

    whitepixel = WhitePixel(dpy, screen);

    for (;;) {
        for (i = 0; i < frame_count; i++) {
            while (XPending(dpy)) {
                XEvent event;
                XNextEvent(dpy, &event);
                if (event.type == KeyPress) {
                    char buf[16];
                    KeySym keysym;
                    XLookupString(&event.xkey, buf, sizeof(buf), &keysym, NULL);
                    if (buf[0] == 'q')
                        goto cleanup;
                } else if (event.type == ClientMessage) {
                    if ((Atom)event.xclient.data.l[0] == wm_delete_window) {
                        goto cleanup;
                    }
                }
            }

            memset(img->data, 0, line_bytes * gif->SHeight);
            for (y = 0; y < frames[i].height; y++) {
                for (x = 0; x < frames[i].width; x++) {
                    if (frames[i].data[y * frames[i].width + x] == whitepixel) {
                        unsigned int byte = x / 8;
                        unsigned int bit = 7 - (x % 8);
                        img->data[y * line_bytes + byte] |= (1 << bit);
                    }
                }
            }

            XPutImage(dpy, win, gc, img, 0, 0, 0, 0, gif->SWidth, gif->SHeight);
            XFlush(dpy);
            msleep(frames[i].delay);
        }
    }

 cleanup:
    for (i = 0; i < frame_count; i++)
        free(frames[i].data);
    free(img->data);
    XDestroyImage(img);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    DGifCloseFile(gif, NULL);
    exit(EXIT_SUCCESS);
}
