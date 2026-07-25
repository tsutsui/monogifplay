PROGS = monogifplay monogifplay-wscons gif2monobg

COMMON_CPPFLAGS = -Wall

# Enable dumb loop unrolling optimizations for slow machines
# where this monogifplay is appreciated, but it may fail to determine
# endianness on some old environments.
COMMON_CPPFLAGS+= -DUNROLL_BITMAP_EXTRACT
#COMMON_CPPFLAGS+= -D__BYTE_ORDER__=__ORDER_LITTLE_ENDIAN__
#COMMON_CPPFLAGS+= -D__BYTE_ORDER__=__ORDER_BIG_ENDIAN__

# for pkgsrc/graphics/giflib
GIF_CPPFLAGS = -I/usr/pkg/include
GIF_LDFLAGS  = -L/usr/pkg/lib -Wl,-R/usr/pkg/lib
GIF_LDLIBS   = -lgif

# For NetBSD etc. where X environments are installed under /usr/X11R7.
X11_CPPFLAGS = -I/usr/X11R7/include
X11_LDFLAGS  = -L/usr/X11R7/lib -Wl,-R/usr/X11R7/lib
X11_LDLIBS   = -lX11

all: ${PROGS}

monogifplay: monogifplay.o
	${CC} -o $@ ${CFLAGS} ${LDFLAGS} ${GIF_LDFLAGS} ${X11_LDFLAGS} \
	    monogifplay.o ${X11_LDLIBS} ${GIF_LDLIBS} ${LDLIBS}

monogifplay.o: monogifplay.c
	${CC} ${CPPFLAGS} ${COMMON_CPPFLAGS} ${GIF_CPPFLAGS} ${X11_CPPFLAGS} \
	    ${CFLAGS} -c monogifplay.c -o $@

monogifplay-wscons: monogifplay-wscons.o monobg_format.o
	${CC} -o $@ ${CFLAGS} ${LDFLAGS} ${GIF_LDFLAGS} \
	    monogifplay-wscons.o monobg_format.o ${GIF_LDLIBS} ${LDLIBS}

monogifplay-wscons.o: monogifplay-wscons.c monobg_format.h
	${CC} ${CPPFLAGS} ${COMMON_CPPFLAGS} ${GIF_CPPFLAGS} ${CFLAGS} \
	    -c monogifplay-wscons.c -o $@

gif2monobg: gif2monobg.o monobg_format.o
	${CC} -o $@ ${CFLAGS} ${LDFLAGS} ${GIF_LDFLAGS} \
	    gif2monobg.o monobg_format.o ${GIF_LDLIBS} ${LDLIBS}

gif2monobg.o: gif2monobg.c monobg_format.h
	${CC} ${CPPFLAGS} ${COMMON_CPPFLAGS} ${GIF_CPPFLAGS} ${CFLAGS} \
	    -c gif2monobg.c -o $@

monobg_format.o: monobg_format.c monobg_format.h
	${CC} ${CPPFLAGS} ${COMMON_CPPFLAGS} ${CFLAGS} \
	    -c monobg_format.c -o $@

clean:
	-rm -f *.o ${PROGS}
