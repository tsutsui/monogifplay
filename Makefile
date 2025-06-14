PROG = monogifplay
SRCS = monogifplay.c
OBJS = ${SRCS:.c=.o}

CPPFLAGS+=	-Wall

# Enable dumb loop unrolling optimizations for slow machines
# that could have 1 bpp Xservers where this monogifplay is appreciated,
# but it may fail to determine endianness on some old environments.
CPPFLAGS+=	-DUNROLL_BITMAP_EXTRACT
#CPPFLAGS+=	-D__BYTE_ORDER__=__ORDER_LITTLE_ENDIAN__
#CPPFLAGS+=	-D__BYTE_ORDER__=__ORDER_BIG_ENDIAN__

# For NetBSD etc. where X environments are installed under /usr/X11R7.
CPPFLAGS+=	-I/usr/X11R7/include
LDFLAGS+=	-L/usr/X11R7/lib -Wl,-R/usr/X11R7/lib
LDLIBS+=	-lX11

# for pkgsrc/graphics/giflib
CPPFLAGS+=	-I/usr/pkg/include 
LDFLAGS+=	-L/usr/pkg/lib -Wl,-R/usr/pkg/lib
LDLIBS+=	-lgif

${PROG}:	${OBJS}
	${CC} -o ${PROG} ${CFLAGS} ${LDFLAGS} ${OBJS} ${LDLIBS}

clean:
	-rm -f *.o ${PROG}
