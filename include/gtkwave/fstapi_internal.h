/*
 * Copyright (c) 2009-2023 Tony Bybell.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * possible disables:
 *
 * FST_DYNAMIC_ALIAS_DISABLE : dynamic aliases are not processed
 * FST_DYNAMIC_ALIAS2_DISABLE : new encoding for dynamic aliases is not generated
 * FST_WRITEX_DISABLE : fast write I/O routines are disabled
 *
 * possible enables:
 *
 * FST_DEBUG : not for production use, only enable for development
 * FST_REMOVE_DUPLICATE_VC : glitch removal (has writer performance impact)
 * HAVE_LIBPTHREAD -> FST_WRITER_PARALLEL : enables inclusion of parallel writer code
 *
 */

#ifndef _FSTAPI_INTERNAL_H
#define _FSTAPI_INTERNAL_H

#ifdef FST_CONFIG_INCLUDE
#include FST_CONFIG_INCLUDE
#endif

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#if defined(_MSC_VER)
#include "fst_win_unistd.h"
#else
#include <unistd.h>
#endif
#include <errno.h>
#include <time.h>

#include "fstapi.h"
#include "lz4.h"

#ifndef HAVE_LIBPTHREAD
#undef FST_WRITER_PARALLEL
#endif

#ifdef FST_WRITER_PARALLEL
#include <pthread.h>
#endif

#ifdef __MINGW32__
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#elif defined(__GNUC__)
#ifndef __MINGW32__
#ifndef alloca
#define alloca __builtin_alloca
#endif
#else
#include <malloc.h>
#endif
#elif defined(_MSC_VER)
#include <malloc.h>
#define alloca _alloca
#endif

#ifndef PATH_MAX
#define PATH_MAX (4096)
#endif

#if defined(_MSC_VER)
typedef int64_t fst_off_t;
#else
typedef off_t fst_off_t;
#endif

#ifndef FST_WRITEX_DISABLE
#define FST_WRITEX_MAX (64 * 1024)
#else
#define fstWritex(a, b, c) fstFwrite((b), (c), 1, fv)
#endif

/* these defines have a large impact on writer speed when a model has a */
/* huge number of symbols.  as a default, use 128MB and increment when  */
/* every 1M signals are defined.                                        */
#define FST_BREAK_SIZE (1UL << 27)
#define FST_BREAK_ADD_SIZE (1UL << 22)
#define FST_BREAK_SIZE_MAX (1UL << 31)
#define FST_ACTIVATE_HUGE_BREAK (1000000)
#define FST_ACTIVATE_HUGE_INC (1000000)

#define FST_WRITER_STR "fstWriter"
#define FST_ID_NAM_SIZ (512)
#define FST_ID_NAM_ATTR_SIZ (65536 + 4096)
#define FST_DOUBLE_ENDTEST (2.7182818284590452354)
#define FST_HDR_SIM_VERSION_SIZE (128)
#define FST_HDR_DATE_SIZE (119)
#define FST_HDR_FILETYPE_SIZE (1)
#define FST_HDR_TIMEZERO_SIZE (8)
#define FST_GZIO_LEN (32768)
#define FST_HDR_FOURPACK_DUO_SIZE (4 * 1024 * 1024)
#define FST_ZWRAPPER_HDR_SIZE (1 + 8 + 8)

#if defined(__APPLE__) && defined(__MACH__)
#define FST_MACOSX
#include <sys/sysctl.h>
#endif

#if defined(FST_MACOSX) || defined(__MINGW32__) || defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__)
#define FST_UNBUFFERED_IO
#endif

#ifdef __GNUC__
/* Boolean expression more often true than false */
#define FST_LIKELY(x) __builtin_expect(!!(x), 1)
/* Boolean expression more often false than true */
#define FST_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define FST_LIKELY(x) (!!(x))
#define FST_UNLIKELY(x) (!!(x))
#endif

#define FST_APIMESS "FSTAPI  | "

/***********************/
/***                 ***/
/*** common function ***/
/***                 ***/
/***********************/

#ifdef __MINGW32__
#include <io.h>
#ifndef HAVE_FSEEKO
#define ftello _ftelli64
#define fseeko _fseeki64
#endif
#endif

/*
 * the recoded "extra" values...
 * note that FST_RCV_Q is currently unused and is for future expansion.
 * its intended use is as another level of escape such that any arbitrary
 * value can be stored as the value: { time_delta, 8 bits, FST_RCV_Q }.
 * this is currently not implemented so that the branchless decode is:
 * uint32_t shcnt = 2 << (vli & 1); tdelta = vli >> shcnt;
 */
#define FST_RCV_X (1 | (0 << 1))
#define FST_RCV_Z (1 | (1 << 1))
#define FST_RCV_H (1 | (2 << 1))
#define FST_RCV_U (1 | (3 << 1))
#define FST_RCV_W (1 | (4 << 1))
#define FST_RCV_L (1 | (5 << 1))
#define FST_RCV_D (1 | (6 << 1))
#define FST_RCV_Q (1 | (7 << 1))

#define FST_RCV_STR "xzhuwl-?"
/*                   01234567 */

/*
 * report abort messages
 */
inline void chk_report_abort(const char *s) {
  fprintf(stderr, "Triggered %s security check, exiting.\n", s);
  abort();
}

/*
 * prevent old file overwrite when currently being read
 */
inline FILE *unlink_fopen(const char *nam, const char *mode) {
  unlink(nam);
  return (fopen(nam, mode));
}

/*
 * system-specific temp file handling
 */
#ifdef __MINGW32__

inline FILE *tmpfile_open(char **nam) {
  char *fname = NULL;
  TCHAR szTempFileName[MAX_PATH];
  TCHAR lpTempPathBuffer[MAX_PATH];
  DWORD dwRetVal = 0;
  UINT uRetVal = 0;
  FILE *fh = NULL;

  if (nam) /* cppcheck warning fix: nam is always defined, so this is not needed */
  {
    dwRetVal = GetTempPath(MAX_PATH, lpTempPathBuffer);
    if ((dwRetVal > MAX_PATH) || (dwRetVal == 0)) {
      fprintf(stderr, FST_APIMESS "GetTempPath() failed in " __FILE__ " line %d, exiting.\n", __LINE__);
      exit(255);
    } else {
      uRetVal = GetTempFileName(lpTempPathBuffer, TEXT("FSTW"), 0, szTempFileName);
      if (uRetVal == 0) {
        fprintf(stderr, FST_APIMESS "GetTempFileName() failed in " __FILE__ " line %d, exiting.\n", __LINE__);
        exit(255);
      } else {
        fname = strdup(szTempFileName);
      }
    }

    if (fname) {
      *nam = fname;
      fh = unlink_fopen(fname, "w+b");
    }
  }

  return (fh);
}

#else

inline FILE *tmpfile_open(char **nam) {
  FILE *f = tmpfile(); /* replace with mkstemp() + fopen(), etc if this is not good enough */
  if (nam) {
    *nam = NULL;
  }
  return (f);
}

#endif

inline void tmpfile_close(FILE **f, char **nam) {
  if (f) {
    if (*f) {
      fclose(*f);
      *f = NULL;
    }
  }

  if (nam) {
    if (*nam) {
      unlink(*nam);
      free(*nam);
      *nam = NULL;
    }
  }
}

/*****************************************/

/*
 * to remove warn_unused_result compile time messages
 * (in the future there needs to be results checking)
 */
inline size_t fstFread(void *buf, size_t siz, size_t cnt, FILE *fp) { return (fread(buf, siz, cnt, fp)); }

inline size_t fstFwrite(const void *buf, size_t siz, size_t cnt, FILE *fp) { return (fwrite(buf, siz, cnt, fp)); }

inline int fstFtruncate(int fd, fst_off_t length) { return (ftruncate(fd, length)); }

/*
 * realpath compatibility
 */
inline char *fstRealpath(const char *path, char *resolved_path) {
#if defined __USE_BSD || defined __USE_XOPEN_EXTENDED || defined __CYGWIN__ || defined HAVE_REALPATH
#if (defined(__MACH__) && defined(__APPLE__))
  if (!resolved_path) {
    resolved_path = (char *)malloc(PATH_MAX + 1); /* fixes bug on Leopard when resolved_path == NULL */
  }
#endif

  return (realpath(path, resolved_path));

#else
#ifdef __MINGW32__
  if (!resolved_path) {
    resolved_path = (char *)malloc(PATH_MAX + 1);
  }
  return (_fullpath(resolved_path, path, PATH_MAX));
#else
  (void)path;
  (void)resolved_path;
  return (NULL);
#endif
#endif
}

/*
 * mmap compatibility
 */
#if defined __MINGW32__
#include <limits.h>
#define fstMmap(__addr, __len, __prot, __flags, __fd, __off) fstMmap2((__len), (__fd), (__off))
#define fstMunmap(__addr, __len) UnmapViewOfFile((LPCVOID)__addr)

inline void *fstMmap2(size_t __len, int __fd, fst_off_t __off) {
  DWORD64 len64 = __len; /* Must be 64-bit for shift below */
  HANDLE handle =
      CreateFileMapping((HANDLE)_get_osfhandle(__fd), NULL, PAGE_READWRITE, (DWORD)(len64 >> 32), (DWORD)__len, NULL);
  if (!handle) {
    return NULL;
  }

  void *ptr = MapViewOfFileEx(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, (DWORD)__off, (SIZE_T)__len, (LPVOID)NULL);
  CloseHandle(handle);
  return ptr;
}
#else
#include <sys/mman.h>
#if defined(__SUNPRO_C)
#define FST_CADDR_T_CAST (caddr_t)
#else
#define FST_CADDR_T_CAST
#endif
#define fstMmap(__addr, __len, __prot, __flags, __fd, __off)                                                           \
  (void *)mmap(FST_CADDR_T_CAST(__addr), (__len), (__prot), (__flags), (__fd), (__off))
#define fstMunmap(__addr, __len)                                                                                       \
  {                                                                                                                    \
    if (__addr)                                                                                                        \
      munmap(FST_CADDR_T_CAST(__addr), (__len));                                                                       \
  }
#endif

/*
 * regular and variable-length integer access functions
 */

inline uint32_t fstGetUint32(unsigned char *mem) {
  uint32_t u32;
  unsigned char *buf = (unsigned char *)(&u32);

  memcpy(buf, mem, sizeof(uint32_t));

  return (*(uint32_t *)buf);
}

inline uint32_t fstGetVarint32(unsigned char *mem, int *skiplen) {
  unsigned char *mem_orig = mem;
  uint32_t rc = 0;
  while (*mem & 0x80) {
    mem++;
  }

  *skiplen = mem - mem_orig + 1;
  for (;;) {
    rc <<= 7;
    rc |= (uint32_t)(*mem & 0x7f);
    if (mem == mem_orig) {
      break;
    }
    mem--;
  }

  return (rc);
}

inline uint32_t fstGetVarint32Length(unsigned char *mem) {
  unsigned char *mem_orig = mem;

  while (*mem & 0x80) {
    mem++;
  }

  return (mem - mem_orig + 1);
}

inline uint32_t fstGetVarint32NoSkip(unsigned char *mem) {
  unsigned char *mem_orig = mem;
  uint32_t rc = 0;
  while (*mem & 0x80) {
    mem++;
  }

  for (;;) {
    rc <<= 7;
    rc |= (uint32_t)(*mem & 0x7f);
    if (mem == mem_orig) {
      break;
    }
    mem--;
  }

  return (rc);
}

inline unsigned char *fstCopyVarint32ToLeft(unsigned char *pnt, uint32_t v) {
  unsigned char *spnt;
  uint32_t nxt = v;
  int cnt = 1;
  int i;

  while ((nxt = nxt >> 7)) /* determine len to avoid temp buffer copying to cut down on load-hit-store */
  {
    cnt++;
  }

  pnt -= cnt;
  spnt = pnt;
  cnt--;

  for (i = 0; i < cnt; i++) /* now generate left to right as normal */
  {
    nxt = v >> 7;
    *(spnt++) = ((unsigned char)v) | 0x80;
    v = nxt;
  }
  *spnt = (unsigned char)v;

  return (pnt);
}

inline unsigned char *fstCopyVarint64ToRight(unsigned char *pnt, uint64_t v) {
  uint64_t nxt;

  while ((nxt = v >> 7)) {
    *(pnt++) = ((unsigned char)v) | 0x80;
    v = nxt;
  }
  *(pnt++) = (unsigned char)v;

  return (pnt);
}

inline uint64_t fstGetVarint64(unsigned char *mem, int *skiplen) {
  unsigned char *mem_orig = mem;
  uint64_t rc = 0;
  while (*mem & 0x80) {
    mem++;
  }

  *skiplen = mem - mem_orig + 1;
  for (;;) {
    rc <<= 7;
    rc |= (uint64_t)(*mem & 0x7f);
    if (mem == mem_orig) {
      break;
    }
    mem--;
  }

  return (rc);
}

/* signed integer read/write routines are currently unused */
inline int64_t fstGetSVarint64(unsigned char *mem, int *skiplen) {
  unsigned char *mem_orig = mem;
  int64_t rc = 0;
  const int64_t one = 1;
  const int siz = sizeof(int64_t) * 8;
  int shift = 0;
  unsigned char byt;

  do {
    byt = *(mem++);
    rc |= ((int64_t)(byt & 0x7f)) << shift;
    shift += 7;

  } while (byt & 0x80);

  if ((shift < siz) && (byt & 0x40)) {
    rc |= -(one << shift); /* sign extend */
  }

  *skiplen = mem - mem_orig;

  return (rc);
}

#endif // _FSTAPI_INTERNAL_H
