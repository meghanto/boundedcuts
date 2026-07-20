#ifndef MSVC_COMPAT_UNISTD_H
#define MSVC_COMPAT_UNISTD_H

#ifdef _MSC_VER
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#define access _access
#define isatty _isatty
#define fileno _fileno
#define getpid _getpid
#define strdup _strdup
#define getc_unlocked getc
#define putc_unlocked putc
#define popen _popen
#define pclose _pclose
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISFIFO
#define S_ISFIFO(mode) 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#endif

#endif
