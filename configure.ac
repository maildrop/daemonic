#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

# see http://stackoverflow.com/questions/28558206/make-autotools-add-std-c11-to-cflags
# AX_CHECK_CFLAGS(ADDITIONAL-CFLAGS, ACTION-IF-FOUND, ACTION-IF-NOT-FOUND)
#
# checks whether the $(CC) compiler accepts the ADDITIONAL-CFLAGS
# if so, they are added to the CXXFLAGS
AC_DEFUN([AX_CHECK_CFLAGS],
[
  AC_MSG_CHECKING([whether compiler accepts "$1"])
  cat > conftest.c << EOF
  int main(){
    return 0;
  }
EOF
  if $CC $CPPFLAGS $CFLAGS [$1] -o conftest.o conftest.c # > /dev/null 2>&1
  then
  AC_MSG_RESULT([yes])
    CFLAGS="${CFLAGS} [$1]"
    [$2]
  else
    AC_MSG_RESULT([no])
    [$3]
  fi
])dnl AX_CHECK_CFLAGS


AC_PREREQ([2.69])
AC_INIT(deamonic,0.0.1,[mit@shalab.net])
AC_CONFIG_SRCDIR([daemonic.c])
AM_INIT_AUTOMAKE([foreign])


# Checks for programs.
AC_PROG_CC

AX_CHECK_CFLAGS( [-std=c11] , [] ,[
  echo "C compiler cannot compile C11 mode"
  exit 2
])

CFLAGS="${CFLAGS} -Wall -D_XOPEN_SOURCE=700"
AC_CONFIG_HEADERS([config.h])
AM_CONDITIONAL([DEBUG], [test x$debug = xtrue])

AC_ARG_ENABLE([debug],
[  --enable-debug    Turn on debugging],
[case "${enableval}" in
  yes) debug=true ;;
  no)  debug=false ;;
  *) AC_MSG_ERROR([bad value ${enableval} for --enable-debug]) ;;
esac],[debug=false])

# Checks for libraries.

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h limits.h stdlib.h string.h syslog.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_FUNC_STRERROR_R
AC_CHECK_FUNCS([dup2 fdatasync fsync pathconf realpath select strdup strrchr])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
