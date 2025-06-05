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

/* monochrome frame structure */
typedef struct {
    int width, height;
    unsigned int delay; /* ms */
    uint8_t *bitmap_data; /* packed bitmap for XImage */
    XImage *image;
    Pixmap pixmap;
} MonoFrame;

#define DEF_GIF_DELAY	75

/* sleep specified ms */
static void
msleep(unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000U;
    ts.tv_nsec = (ms % 1000U) * 1000000U;
    nanosleep(&ts, NULL);
}

/* extract monochrome frames from gif file */
static int
extract_mono_frames(GifFileType *gif, MonoFrame **out_frames, int *out_count)
{
    GraphicsControlBlock gcb;
    int i, frame_count = 0;
    MonoFrame *frames = NULL;

    for (i = 0; i < gif->ImageCount; i++) {
        int x, y;
        MonoFrame frame, *tmp;
        SavedImage *img;
        ColorMapObject *cmap;
        int delay, line_bytes;

        img = &gif->SavedImages[i];
        DGifSavedExtensionToGCB(gif, i, &gcb);

        cmap = img->ImageDesc.ColorMap ?
          img->ImageDesc.ColorMap : gif->SColorMap;
        if (cmap == NULL)
            continue;

        frame.width = img->ImageDesc.Width;
        frame.height = img->ImageDesc.Height;
        delay = gcb.DelayTime * 10; /* delay is stored in 1/100 sec */
        frame.delay = delay > 0 ? delay : DEF_GIF_DELAY;

        line_bytes = (frame.width + 7) / 8;
        frame.bitmap_data = calloc(line_bytes * frame.height, 1);
        if (frame.bitmap_data == NULL)
            return -1;

        for (y = 0; y < frame.height; y++) {
            const int bidx = line_bytes * y;
            for (x = 0; x < frame.width; x++) {
                int idx = y * frame.width + x;
                int pixel;

                GifByteType px = img->RasterBits[idx];
                GifColorType c = cmap->Colors[px];
                /* convert to monochrome per RGB values */
                pixel = (c.Red + c.Green + c.Blue > 128 * 3) ? 1 : 0;
                if (pixel != 0) {
                    unsigned byte = x >> 3;
                    unsigned bit  = 7 - (x & 0x07);
                    frame.bitmap_data[bidx + byte] |= 1 << bit;
                }
            }
        }

        tmp = realloc(frames, sizeof(MonoFrame) * (frame_count + 1));
        if (tmp == NULL)
            return -1;
        frames = tmp;
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
    int err, screen;
    int frame_count;
    int line_bytes;
    int i, x, y;
    char title[512];
    GifFileType *gif;
    MonoFrame *frame, *frames;
    Display *dpy;
    Window win;
    Atom wm_delete_window;
    GC gc;

    setprogname(argv[0]);

    if (argc != 2) {
        usage();
    }

    gif = DGifOpenFileName(argv[1], &err);
    if (gif == NULL || DGifSlurp(gif) != GIF_OK) {
        errx(EXIT_FAILURE, "Failed to load a gif file: %s",
          GifErrorString(err));
    }

    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        errx(EXIT_FAILURE, "Cannot connect Xserver\n");
    }

    screen = DefaultScreen(dpy);

    frames = NULL;
    frame_count = 0;
    if (extract_mono_frames(gif, &frames, &frame_count) < 0 ||
      frame_count == 0) {
        errx(EXIT_FAILURE, "Failed to extract mono frames");
    }

    for (i = 0; i < frame_count; i++) {
      Visual *visual = DefaultVisual(dpy, screen);
      GC mono_gc;

      frame = &frames[i];
      line_bytes = (frame->width + 7) / 8;
      frame->image = XCreateImage(dpy, visual, 1, XYBitmap, 0,
        frame->bitmap_data, frame->width, frame->height, 8, line_bytes);
      if (frame->image == NULL) {
          errx(EXIT_FAILURE, "XCreateImage() failed for frame %d", i);
      }
      frame->image->byte_order = MSBFirst;
      frame->image->bitmap_bit_order = MSBFirst;

      frame->pixmap = XCreatePixmap(dpy, RootWindow(dpy, screen),
        frame->width, frame->height, 1);
      mono_gc = XCreateGC(dpy, frame->pixmap, 0, NULL);
      XPutImage(dpy, frame->pixmap, mono_gc, frame->image, 0, 0, 0, 0,
        frame->width, frame->height);
      XFreeGC(dpy, mono_gc);
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
    XSetForeground(dpy, gc, BlackPixel(dpy, screen));
    XSetBackground(dpy, gc, WhitePixel(dpy, screen));

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

            frame = &frames[i];
            XCopyPlane(dpy, frame->pixmap, win, gc, 0, 0,
              frame->width, frame->height, 0, 0, 1);
            XFlush(dpy);
            msleep(frame->delay);
        }
    }

 cleanup:
    for (i = 0; i < frame_count; i++) {
        if (frames[i].image != NULL)
            XDestroyImage(frames[i].image);
        if (frames[i].pixmap != 0)
            XFreePixmap(dpy, frames[i].pixmap);
    }
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    DGifCloseFile(gif, NULL);
    exit(EXIT_SUCCESS);
}
