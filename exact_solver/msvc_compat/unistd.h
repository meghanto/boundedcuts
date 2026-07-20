#ifndef MSVC_COMPAT_UNISTD_H
#define MSVC_COMPAT_UNISTD_H

#ifdef _MSC_VER
#include <io.h>
#include <process.h>
#define access _access
#define fstat _fstat
#define isatty _isatty
#define fileno _fileno
#define getpid _getpid
#define strdup _strdup
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#endif

#endif
