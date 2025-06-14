dnl configure.ac for monoGIFPlayer
AC_PREREQ([2.71])
AC_INIT([monogifplay], [0.0], [@tsutsui])
AC_CONFIG_SRCDIR([monogifplay.c])
AC_CONFIG_AUX_DIR([config])

dnl -------------------------------------------
dnl Check build host environments.
AC_CANONICAL_HOST
AC_SUBST([host_cpu])

dnl -------------------------------------------
dnl Check X11 headers and libs
case "$host_os" in
  netbsd*)
    x_includes="/usr/X11R7/include"
    x_libraries="/usr/X11R7/lib"
    ;;
  *)
    ;;
esac
	AC_PATH_X
AC_PATH_XTRA

if test "x$no_x" = xyes; then
  AC_MSG_ERROR([X11 development libraries not found.])
fi
X_LIBS="$X_LIBS -lX11"


dnl -------------------------------------------
dnl Check if and where giflib installed
AC_ARG_WITH([giflib-prefix],
  [AS_HELP_STRING([--with-giflib-prefix=PATH], [prefix where giflib is installed])],
  [giflib_prefix="$withval"],
  [giflib_prefix=""])

lib_search_prefixes="/usr /usr/local /usr/pkg /opt/local"

if test -z "$giflib_prefix"; then
  AC_MSG_CHECKING([for giflib])
  for prefix in $lib_search_prefixes; do
    if test -f "$prefix/include/gif_lib.h" && \
       (test -f "$prefix/lib/libgif.so" || \
        test -f "$prefix/lib/libgif.a" || \
        test -f "$prefix/lib/libgif.dylib"); then
      giflib_prefix="$prefix"
      CPPFLAGS="$CPPFLAGS -I$giflib_prefix/include"
      LDFLAGS="$LDFLAGS -L$giflib_prefix/lib"
      LIBS="$LIBS -R$giflib_prefix/lib -lgif"
      AC_MSG_RESULT(found in [$prefix])
      break
    fi
  done
  if test -z "$giflib_prefix"; then
    AC_MSG_ERROR([*** giflib not found])
  fi
fi

case "$host_os" in
  netbsd*)
    # NetBSD requires -Wl,-R
    LIBS=`echo $LIBS | sed 's/-R/-Wl,-R/g'`
    X_LIBS=`echo $X_LIBS | sed 's/-R/-Wl,-R/g'`
    ;;
esac

dnl Check giflib version >= 5.1
AC_MSG_CHECKING([for DGifSlurp()])
AC_COMPILE_IFELSE(
  [AC_LANG_PROGRAM([[#include <gif_lib.h>]],
    [[
    #if !(GIFLIB_MAJOR > 5 || (GIFLIB_MAJOR == 5 && GIFLIB_MINOR >= 1))
    #error "giflib >= 5.1 required"
    #endif
    ]])],
  [AC_MSG_RESULT(yes)],
  [AC_MSG_RESULT(no)
   LIBS=$old_LIBS
   AC_MSG_ERROR([*** giflib >= 5.1 is required for DGifSlurp()])]
)

dnl -------------------------------------------
dnl Check endian and builtin bswap32
AC_C_BIGENDIAN
AC_MSG_CHECKING([for __builtin_bswap32])
AC_LINK_IFELSE(
  [AC_LANG_PROGRAM(
    [#include <inttypes.h>],
    [
      uint32_t x = 0x12345678;
      uint32_t y = __builtin_bswap32(x);
      (void)y;
    ]
  )],
  [AC_DEFINE([HAVE___BUILTIN_BSWAP32], [1], [Define if __builtin_bswap32 exists])],
  []
)

dnl -------------------------------------------
dnl _GNU_SOURCE (to explicitly enable POSIX or XPG functions)
dnl case "$host_os" in
dnl   linux*) AC_DEFINE([_GNU_SOURCE], [1], [Enable GNU extensions]) ;;
dnl esac

dnl -------------------------------------------
dnl Check other standard headers
AC_CHECK_HEADERS([unistd.h libgen.h stdlib.h stdio.h string.h])

dnl -------------------------------------------
dnl Output generated files
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT