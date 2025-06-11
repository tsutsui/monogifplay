PROG = monogifplay
SRCS = monogifplay.c
OBJS = ${SRCS:.c=.o}

CPPFLAGS+=	-Wall
CPPFLAGS+=	-I/usr/X11R7/include
LDFLAGS+=	-L/usr/X11R7/lib -Wl,-R/usr/X11R7/lib
LDLIBS+=	-lX11

# for pkgsrc/graphics/giflib
CPPFLAGS+=	-I/usr/pkg/include 
LDFLAGS+=	-L/usr/pkg/lib -Wl,-R/usr/pkg/lib
LDLIBS+=	-lgif

${PROG}:	${OBJS}
	${CC} -o ${.TARGET} ${CFLAGS} ${LDFLAGS} ${OBJS} ${LDLIBS}

clean:
	-rm -f *.o ${PROG}
