#include "fstapi_internal.h"

#include <assert.h>

static int fstWriterUint64(FILE *handle, uint64_t v) {
  unsigned char buf[8];
  int i;

  for (i = 7; i >= 0; i--) {
    buf[i] = v & 0xff;
    v >>= 8;
  }

  fstFwrite(buf, 8, 1, handle);
  return (8);
}

static int fstWriterVarint(FILE *handle, uint64_t v) {
  uint64_t nxt;
  unsigned char buf[10]; /* ceil(64/7) = 10 */
  unsigned char *pnt = buf;
  int len;

  while ((nxt = v >> 7)) {
    *(pnt++) = ((unsigned char)v) | 0x80;
    v = nxt;
  }
  *(pnt++) = (unsigned char)v;

  len = pnt - buf;
  fstFwrite(buf, len, 1, handle);
  return (len);
}

#ifndef FST_DYNAMIC_ALIAS2_DISABLE
static int fstWriterSVarint(FILE *handle, int64_t v) {
  unsigned char buf[15]; /* ceil(64/7) = 10 + sign byte padded way up */
  unsigned char byt;
  unsigned char *pnt = buf;
  int more = 1;
  int len;

  do {
    byt = v | 0x80;
    v >>= 7;

    if (((!v) && (!(byt & 0x40))) || ((v == -1) && (byt & 0x40))) {
      more = 0;
      byt &= 0x7f;
    }

    *(pnt++) = byt;
  } while (more);

  len = pnt - buf;
  fstFwrite(buf, len, 1, handle);
  return (len);
}
#endif

/***********************/
/***                 ***/
/*** writer function ***/
/***                 ***/
/***********************/

/*
 * private structs
 */
struct fstBlackoutChain {
  struct fstBlackoutChain *next;
  uint64_t tim;
  unsigned active : 1;
};

struct fstWriterContext {
  FILE *handle;
  FILE *hier_handle;
  FILE *geom_handle;
  FILE *valpos_handle;
  FILE *curval_handle;
  FILE *tchn_handle;

  unsigned char *vchg_mem;

  fst_off_t hier_file_len;

  uint32_t *valpos_mem;
  unsigned char *curval_mem;

  unsigned char *outval_mem; /* for two-state / Verilator-style value changes */
  uint32_t outval_alloc_siz;

  char *filename;

  fstHandle maxhandle;
  fstHandle numsigs;
  uint32_t maxvalpos;

  unsigned vc_emitted : 1;
  unsigned is_initial_time : 1;
  unsigned fourpack : 1;
  unsigned fastpack : 1;

  int64_t timezero;
  fst_off_t section_header_truncpos;
  uint32_t tchn_cnt, tchn_idx;
  uint64_t curtime;
  uint64_t firsttime;
  uint32_t vchg_siz;
  uint32_t vchg_alloc_siz;

  uint32_t secnum;
  fst_off_t section_start;

  uint32_t numscopes;
  double nan; /* nan value for uninitialized doubles */

  struct fstBlackoutChain *blackout_head;
  struct fstBlackoutChain *blackout_curr;
  uint32_t num_blackouts;

  uint64_t dump_size_limit;

  unsigned char filetype; /* default is 0, FST_FT_VERILOG */

  unsigned compress_hier : 1;
  unsigned repack_on_close : 1;
  unsigned skip_writing_section_hdr : 1;
  unsigned size_limit_locked : 1;
  unsigned section_header_only : 1;
  unsigned flush_context_pending : 1;
  unsigned parallel_enabled : 1;
  unsigned parallel_was_enabled : 1;

  /* should really be semaphores, but are bytes to cut down on read-modify-write window size */
  unsigned char already_in_flush; /* in case control-c handlers interrupt */
  unsigned char already_in_close; /* in case control-c handlers interrupt */

#ifdef FST_WRITER_PARALLEL
  pthread_mutex_t mutex;
  pthread_t thread;
  pthread_attr_t thread_attr;
  struct fstWriterContext *xc_parent;
#endif
  unsigned in_pthread : 1;

  size_t fst_orig_break_size;
  size_t fst_orig_break_add_size;

  size_t fst_break_size;
  size_t fst_break_add_size;

  size_t fst_huge_break_size;

  fstHandle next_huge_break;

  Pvoid_t path_array;
  uint32_t path_array_count;

  unsigned fseek_failed : 1;

  char *geom_handle_nam;
  char *valpos_handle_nam;
  char *curval_handle_nam;
  char *tchn_handle_nam;

  fstEnumHandle max_enumhandle;
};

static int fstWriterFseeko(struct fstWriterContext *xc, FILE *stream, fst_off_t offset, int whence) {
  int rc = fseeko(stream, offset, whence);

  if (rc < 0) {
    xc->fseek_failed = 1;
#ifdef FST_DEBUG
    fprintf(stderr, FST_APIMESS "Seek to #%" PRId64 " (whence = %d) failed!\n", offset, whence);
    perror("Why");
#endif
  }

  return (rc);
}

static uint32_t fstWriterUint32WithVarint32(struct fstWriterContext *xc, uint32_t *u, uint32_t v, const void *dbuf,
                                            uint32_t siz) {
  unsigned char *buf = xc->vchg_mem + xc->vchg_siz;
  unsigned char *pnt = buf;
  uint32_t nxt;
  uint32_t len;

  memcpy(pnt, u, sizeof(uint32_t));
  pnt += 4;

  while ((nxt = v >> 7)) {
    *(pnt++) = ((unsigned char)v) | 0x80;
    v = nxt;
  }
  *(pnt++) = (unsigned char)v;
  memcpy(pnt, dbuf, siz);

  len = pnt - buf + siz;
  return (len);
}

static uint32_t fstWriterUint32WithVarint32AndLength(struct fstWriterContext *xc, uint32_t *u, uint32_t v,
                                                     const void *dbuf, uint32_t siz) {
  unsigned char *buf = xc->vchg_mem + xc->vchg_siz;
  unsigned char *pnt = buf;
  uint32_t nxt;
  uint32_t len;

  memcpy(pnt, u, sizeof(uint32_t));
  pnt += 4;

  while ((nxt = v >> 7)) {
    *(pnt++) = ((unsigned char)v) | 0x80;
    v = nxt;
  }
  *(pnt++) = (unsigned char)v;

  v = siz;
  while ((nxt = v >> 7)) {
    *(pnt++) = ((unsigned char)v) | 0x80;
    v = nxt;
  }
  *(pnt++) = (unsigned char)v;

  memcpy(pnt, dbuf, siz);

  len = pnt - buf + siz;
  return (len);
}

/*
 * header bytes, write here so defines are set up before anything else
 * that needs to use them
 */
static void fstWriterEmitHdrBytes(struct fstWriterContext *xc) {
  char vbuf[FST_HDR_SIM_VERSION_SIZE];
  char dbuf[FST_HDR_DATE_SIZE];
  double endtest = FST_DOUBLE_ENDTEST;
  time_t walltime;

#define FST_HDR_OFFS_TAG (0)
  fputc(FST_BL_HDR, xc->handle); /* +0 tag */

#define FST_HDR_OFFS_SECLEN (FST_HDR_OFFS_TAG + 1)
  fstWriterUint64(xc->handle, 329); /* +1 section length */

#define FST_HDR_OFFS_START_TIME (FST_HDR_OFFS_SECLEN + 8)
  fstWriterUint64(xc->handle, 0); /* +9 start time */

#define FST_HDR_OFFS_END_TIME (FST_HDR_OFFS_START_TIME + 8)
  fstWriterUint64(xc->handle, 0); /* +17 end time */

#define FST_HDR_OFFS_ENDIAN_TEST (FST_HDR_OFFS_END_TIME + 8)
  fstFwrite(&endtest, 8, 1, xc->handle); /* +25 endian test for reals */

#define FST_HDR_OFFS_MEM_USED (FST_HDR_OFFS_ENDIAN_TEST + 8)
  fstWriterUint64(xc->handle, xc->fst_break_size); /* +33 memory used by writer */

#define FST_HDR_OFFS_NUM_SCOPES (FST_HDR_OFFS_MEM_USED + 8)
  fstWriterUint64(xc->handle, 0); /* +41 scope creation count */

#define FST_HDR_OFFS_NUM_VARS (FST_HDR_OFFS_NUM_SCOPES + 8)
  fstWriterUint64(xc->handle, 0); /* +49 var creation count */

#define FST_HDR_OFFS_MAXHANDLE (FST_HDR_OFFS_NUM_VARS + 8)
  fstWriterUint64(xc->handle, 0); /* +57 max var idcode */

#define FST_HDR_OFFS_SECTION_CNT (FST_HDR_OFFS_MAXHANDLE + 8)
  fstWriterUint64(xc->handle, 0); /* +65 vc section count */

#define FST_HDR_OFFS_TIMESCALE (FST_HDR_OFFS_SECTION_CNT + 8)
  fputc((-9) & 255, xc->handle); /* +73 timescale 1ns */

#define FST_HDR_OFFS_SIM_VERSION (FST_HDR_OFFS_TIMESCALE + 1)
  memset(vbuf, 0, FST_HDR_SIM_VERSION_SIZE);
  strcpy(vbuf, FST_WRITER_STR);
  fstFwrite(vbuf, FST_HDR_SIM_VERSION_SIZE, 1, xc->handle); /* +74 version */

#define FST_HDR_OFFS_DATE (FST_HDR_OFFS_SIM_VERSION + FST_HDR_SIM_VERSION_SIZE)
  memset(dbuf, 0, FST_HDR_DATE_SIZE);
  time(&walltime);
  strcpy(dbuf, asctime(localtime(&walltime)));
  fstFwrite(dbuf, FST_HDR_DATE_SIZE, 1, xc->handle); /* +202 date */

  /* date size is deliberately overspecified at 119 bytes (originally 128) in order to provide backfill for new args */

#define FST_HDR_OFFS_FILETYPE (FST_HDR_OFFS_DATE + FST_HDR_DATE_SIZE)
  fputc(xc->filetype, xc->handle); /* +321 filetype */

#define FST_HDR_OFFS_TIMEZERO (FST_HDR_OFFS_FILETYPE + FST_HDR_FILETYPE_SIZE)
  fstWriterUint64(xc->handle, xc->timezero); /* +322 timezero */

#define FST_HDR_LENGTH (FST_HDR_OFFS_TIMEZERO + FST_HDR_TIMEZERO_SIZE)
  /* +330 next section starts here */
  fflush(xc->handle);
}

/*
 * mmap functions
 */
static void fstWriterMmapSanity(void *pnt, const char *file, int line, const char *usage) {
  if (pnt == NULL
#ifdef MAP_FAILED
      || pnt == MAP_FAILED
#endif
  ) {
    fprintf(stderr, "fstMmap() assigned to %s failed: errno: %d, file %s, line %d.\n", usage, errno, file, line);
#if !defined(__MINGW32__)
    perror("Why");
#else
    LPSTR mbuf = NULL;
    FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                  GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&mbuf, 0, NULL);
    fprintf(stderr, "%s", mbuf);
    LocalFree(mbuf);
#endif
    pnt = NULL;
  }
}

static void fstWriterCreateMmaps(struct fstWriterContext *xc) {
  fst_off_t curpos = ftello(xc->handle);

  fflush(xc->hier_handle);

  /* write out intermediate header */
  fstWriterFseeko(xc, xc->handle, FST_HDR_OFFS_START_TIME, SEEK_SET);
  fstWriterUint64(xc->handle, xc->firsttime);
  fstWriterUint64(xc->handle, xc->curtime);
  fstWriterFseeko(xc, xc->handle, FST_HDR_OFFS_NUM_SCOPES, SEEK_SET);
  fstWriterUint64(xc->handle, xc->numscopes);
  fstWriterUint64(xc->handle, xc->numsigs);
  fstWriterUint64(xc->handle, xc->maxhandle);
  fstWriterUint64(xc->handle, xc->secnum);
  fstWriterFseeko(xc, xc->handle, curpos, SEEK_SET);
  fflush(xc->handle);

  /* do mappings */
  if (!xc->valpos_mem) {
    fflush(xc->valpos_handle);
    errno = 0;
    if (xc->maxhandle) {
      fstWriterMmapSanity(xc->valpos_mem =
                              (uint32_t *)fstMmap(NULL, xc->maxhandle * 4 * sizeof(uint32_t), PROT_READ | PROT_WRITE,
                                                  MAP_SHARED, fileno(xc->valpos_handle), 0),
                          __FILE__, __LINE__, "xc->valpos_mem");
    }
  }
  if (!xc->curval_mem) {
    fflush(xc->curval_handle);
    errno = 0;
    if (xc->maxvalpos) {
      fstWriterMmapSanity(xc->curval_mem = (unsigned char *)fstMmap(NULL, xc->maxvalpos, PROT_READ | PROT_WRITE,
                                                                    MAP_SHARED, fileno(xc->curval_handle), 0),
                          __FILE__, __LINE__, "xc->curval_handle");
    }
  }
}

static void fstDestroyMmaps(struct fstWriterContext *xc, int is_closing) {
  (void)is_closing;

  fstMunmap(xc->valpos_mem, xc->maxhandle * 4 * sizeof(uint32_t));
  xc->valpos_mem = NULL;

  fstMunmap(xc->curval_mem, xc->maxvalpos);
  xc->curval_mem = NULL;
}

/*
 * set up large and small memory usages
 * crossover point in model is FST_ACTIVATE_HUGE_BREAK number of signals
 */
static void fstDetermineBreakSize(struct fstWriterContext *xc) {
#if defined(__linux__) || defined(FST_MACOSX)
  int was_set = 0;

#ifdef __linux__
  FILE *f = fopen("/proc/meminfo", "rb");

  if (f) {
    char buf[257];
    char *s;
    while (!feof(f)) {
      buf[0] = 0;
      s = fgets(buf, 256, f);
      if (s && *s) {
        if (!strncmp(s, "MemTotal:", 9)) {
          size_t v = atol(s + 10);
          v *= 1024; /* convert to bytes */
          v /= 8;    /* chop down to 1/8 physical memory */
          if (v > FST_BREAK_SIZE) {
            if (v > FST_BREAK_SIZE_MAX) {
              v = FST_BREAK_SIZE_MAX;
            }

            xc->fst_huge_break_size = v;
            was_set = 1;
            break;
          }
        }
      }
    }

    fclose(f);
  }

  if (!was_set) {
    xc->fst_huge_break_size = FST_BREAK_SIZE;
  }
#else
  int mib[2];
  int64_t v;
  size_t length;

  mib[0] = CTL_HW;
  mib[1] = HW_MEMSIZE;
  length = sizeof(int64_t);
  if (!sysctl(mib, 2, &v, &length, NULL, 0)) {
    v /= 8;

    if (v > (int64_t)FST_BREAK_SIZE) {
      if (v > (int64_t)FST_BREAK_SIZE_MAX) {
        v = FST_BREAK_SIZE_MAX;
      }

      xc->fst_huge_break_size = v;
      was_set = 1;
    }
  }

  if (!was_set) {
    xc->fst_huge_break_size = FST_BREAK_SIZE;
  }
#endif
#else
  xc->fst_huge_break_size = FST_BREAK_SIZE;
#endif

  xc->fst_break_size = xc->fst_orig_break_size = FST_BREAK_SIZE;
  xc->fst_break_add_size = xc->fst_orig_break_add_size = FST_BREAK_ADD_SIZE;
  xc->next_huge_break = FST_ACTIVATE_HUGE_BREAK;
}

/*
 * file creation and close
 */
struct fstWriterContext *fstWriterCreate(const char *nam, int use_compressed_hier) {
  struct fstWriterContext *xc = (struct fstWriterContext *)calloc(1, sizeof(struct fstWriterContext));

  xc->compress_hier = use_compressed_hier;
  fstDetermineBreakSize(xc);

  if ((!nam) || (!(xc->handle = unlink_fopen(nam, "w+b")))) {
    free(xc);
    xc = NULL;
  } else {
    int flen = strlen(nam);
    char *hf = (char *)calloc(1, flen + 6);

    memcpy(hf, nam, flen);
    strcpy(hf + flen, ".hier");
    xc->hier_handle = unlink_fopen(hf, "w+b");

    xc->geom_handle = tmpfile_open(&xc->geom_handle_nam);     /* .geom */
    xc->valpos_handle = tmpfile_open(&xc->valpos_handle_nam); /* .offs */
    xc->curval_handle = tmpfile_open(&xc->curval_handle_nam); /* .bits */
    xc->tchn_handle = tmpfile_open(&xc->tchn_handle_nam);     /* .tchn */
    xc->vchg_alloc_siz = xc->fst_break_size + xc->fst_break_add_size;
    xc->vchg_mem = (unsigned char *)malloc(xc->vchg_alloc_siz);

    if (xc->hier_handle && xc->geom_handle && xc->valpos_handle && xc->curval_handle && xc->vchg_mem &&
        xc->tchn_handle) {
      xc->filename = strdup(nam);
      xc->is_initial_time = 1;

      fstWriterEmitHdrBytes(xc);
      xc->nan = strtod("NaN", NULL);
#ifdef FST_WRITER_PARALLEL
      pthread_mutex_init(&xc->mutex, NULL);
      pthread_attr_init(&xc->thread_attr);
      pthread_attr_setdetachstate(&xc->thread_attr, PTHREAD_CREATE_DETACHED);
#endif
    } else {
      fclose(xc->handle);
      if (xc->hier_handle) {
        fclose(xc->hier_handle);
        unlink(hf);
      }
      tmpfile_close(&xc->geom_handle, &xc->geom_handle_nam);
      tmpfile_close(&xc->valpos_handle, &xc->valpos_handle_nam);
      tmpfile_close(&xc->curval_handle, &xc->curval_handle_nam);
      tmpfile_close(&xc->tchn_handle, &xc->tchn_handle_nam);
      free(xc->vchg_mem);
      free(xc);
      xc = NULL;
    }

    free(hf);
  }

  return (xc);
}

/*
 * generation and writing out of value change data sections
 */
static void fstWriterEmitSectionHeader(struct fstWriterContext *xc) {
  unsigned long destlen;
  unsigned char *dmem;
  int rc;

  destlen = xc->maxvalpos;
  dmem = (unsigned char *)malloc(compressBound(destlen));
  rc = compress2(dmem, &destlen, xc->curval_mem, xc->maxvalpos,
                 4); /* was 9...which caused performance drag on traces with many signals */

  fputc(FST_BL_SKIP, xc->handle); /* temporarily tag the section, use FST_BL_VCDATA on finalize */
  xc->section_start = ftello(xc->handle);
#ifdef FST_WRITER_PARALLEL
  if (xc->xc_parent)
    xc->xc_parent->section_start = xc->section_start;
#endif
  xc->section_header_only = 1;    /* indicates truncate might be needed */
  fstWriterUint64(xc->handle, 0); /* placeholder = section length */
  fstWriterUint64(xc->handle, xc->is_initial_time ? xc->firsttime : xc->curtime); /* begin time of section */
  fstWriterUint64(xc->handle, xc->curtime); /* end time of section (placeholder) */
  fstWriterUint64(xc->handle, 0); /* placeholder = amount of buffer memory required in reader for full vc traversal */
  fstWriterVarint(xc->handle, xc->maxvalpos); /* maxvalpos = length of uncompressed data */

  if ((rc == Z_OK) && (destlen < xc->maxvalpos)) {
    fstWriterVarint(xc->handle, destlen); /* length of compressed data */
  } else {
    fstWriterVarint(xc->handle, xc->maxvalpos); /* length of (unable to be) compressed data */
  }
  fstWriterVarint(xc->handle,
                  xc->maxhandle); /* max handle associated with this data (in case of dynamic facility adds) */

  if ((rc == Z_OK) && (destlen < xc->maxvalpos)) {
    fstFwrite(dmem, destlen, 1, xc->handle);
  } else /* comparison between compressed / decompressed len tells if compressed */
  {
    fstFwrite(xc->curval_mem, xc->maxvalpos, 1, xc->handle);
  }

  free(dmem);
}

/*
 * only to be called directly by fst code...otherwise must
 * be synced up with time changes
 */
#ifdef FST_WRITER_PARALLEL
static void fstWriterFlushContextPrivate2(struct fstWriterContext *xc)
#else
static void fstWriterFlushContextPrivate(struct fstWriterContext *xc)
#endif
{
#ifdef FST_DEBUG
  int cnt = 0;
#endif
  unsigned int i;
  unsigned char *vchg_mem;
  FILE *f;
  fst_off_t fpos, indxpos, endpos;
  uint32_t prevpos;
  int zerocnt;
  unsigned char *scratchpad;
  unsigned char *scratchpnt;
  unsigned char *tmem;
  fst_off_t tlen;
  fst_off_t unc_memreq = 0; /* for reader */
  unsigned char *packmem;
  unsigned int packmemlen;
  uint32_t *vm4ip;
#ifdef FST_WRITER_PARALLEL
  struct fstWriterContext *xc2 = xc->xc_parent;
#else
  struct fstWriterContext *xc2 = xc;
#endif

#ifndef FST_DYNAMIC_ALIAS_DISABLE
  Pvoid_t PJHSArray = (Pvoid_t)NULL;
#ifndef _WAVE_HAVE_JUDY
  uint32_t hashmask = xc->maxhandle;
  hashmask |= hashmask >> 1;
  hashmask |= hashmask >> 2;
  hashmask |= hashmask >> 4;
  hashmask |= hashmask >> 8;
  hashmask |= hashmask >> 16;
#endif
#endif

  if ((xc->vchg_siz <= 1) || (xc->already_in_flush))
    return;
  xc->already_in_flush = 1; /* should really do this with a semaphore */

  xc->section_header_only = 0;
  scratchpad = (unsigned char *)malloc(xc->vchg_siz);

  vchg_mem = xc->vchg_mem;

  f = xc->handle;
  fstWriterVarint(f, xc->maxhandle); /* emit current number of handles */
  fputc(xc->fourpack ? '4' : (xc->fastpack ? 'F' : 'Z'), f);
  fpos = 1;

  packmemlen = 1024;                             /* maintain a running "longest" allocation to */
  packmem = (unsigned char *)malloc(packmemlen); /* prevent continual malloc...free every loop iter */

  for (i = 0; i < xc->maxhandle; i++) {
    vm4ip = &(xc->valpos_mem[4 * i]);

    if (vm4ip[2]) {
      uint32_t offs = vm4ip[2];
      uint32_t next_offs;
      unsigned int wrlen;

      vm4ip[2] = fpos;

      scratchpnt = scratchpad + xc->vchg_siz; /* build this buffer backwards */
      if (vm4ip[1] <= 1) {
        if (vm4ip[1] == 1) {
          wrlen = fstGetVarint32Length(vchg_mem + offs + 4); /* used to advance and determine wrlen */
#ifndef FST_REMOVE_DUPLICATE_VC
          xc->curval_mem[vm4ip[0]] = vchg_mem[offs + 4 + wrlen]; /* checkpoint variable */
#endif
          while (offs) {
            unsigned char val;
            uint32_t time_delta, rcv;
            next_offs = fstGetUint32(vchg_mem + offs);
            offs += 4;

            time_delta = fstGetVarint32(vchg_mem + offs, (int *)&wrlen);
            val = vchg_mem[offs + wrlen];
            offs = next_offs;

            switch (val) {
            case '0':
            case '1':
              rcv = ((val & 1) << 1) | (time_delta << 2);
              break; /* pack more delta bits in for 0/1 vchs */

            case 'x':
            case 'X':
              rcv = FST_RCV_X | (time_delta << 4);
              break;
            case 'z':
            case 'Z':
              rcv = FST_RCV_Z | (time_delta << 4);
              break;
            case 'h':
            case 'H':
              rcv = FST_RCV_H | (time_delta << 4);
              break;
            case 'u':
            case 'U':
              rcv = FST_RCV_U | (time_delta << 4);
              break;
            case 'w':
            case 'W':
              rcv = FST_RCV_W | (time_delta << 4);
              break;
            case 'l':
            case 'L':
              rcv = FST_RCV_L | (time_delta << 4);
              break;
            default:
              rcv = FST_RCV_D | (time_delta << 4);
              break;
            }

            scratchpnt = fstCopyVarint32ToLeft(scratchpnt, rcv);
          }
        } else {
          /* variable length */
          /* fstGetUint32 (next_offs) + fstGetVarint32 (time_delta) + fstGetVarint32 (len) + payload */
          unsigned char *pnt;
          uint32_t record_len;
          uint32_t time_delta;

          while (offs) {
            next_offs = fstGetUint32(vchg_mem + offs);
            offs += 4;
            pnt = vchg_mem + offs;
            offs = next_offs;
            time_delta = fstGetVarint32(pnt, (int *)&wrlen);
            pnt += wrlen;
            record_len = fstGetVarint32(pnt, (int *)&wrlen);
            pnt += wrlen;

            scratchpnt -= record_len;
            memcpy(scratchpnt, pnt, record_len);

            scratchpnt = fstCopyVarint32ToLeft(scratchpnt, record_len);
            scratchpnt =
                fstCopyVarint32ToLeft(scratchpnt, (time_delta << 1)); /* reserve | 1 case for future expansion */
          }
        }
      } else {
        wrlen = fstGetVarint32Length(vchg_mem + offs + 4); /* used to advance and determine wrlen */
#ifndef FST_REMOVE_DUPLICATE_VC
        memcpy(xc->curval_mem + vm4ip[0], vchg_mem + offs + 4 + wrlen, vm4ip[1]); /* checkpoint variable */
#endif
        while (offs) {
          unsigned int idx;
          char is_binary = 1;
          unsigned char *pnt;
          uint32_t time_delta;

          next_offs = fstGetUint32(vchg_mem + offs);
          offs += 4;

          time_delta = fstGetVarint32(vchg_mem + offs, (int *)&wrlen);

          pnt = vchg_mem + offs + wrlen;
          offs = next_offs;

          for (idx = 0; idx < vm4ip[1]; idx++) {
            if ((pnt[idx] == '0') || (pnt[idx] == '1')) {
              continue;
            } else {
              is_binary = 0;
              break;
            }
          }

          if (is_binary) {
            unsigned char acc = 0;
            /* new algorithm */
            idx = ((vm4ip[1] + 7) & ~7);
            switch (vm4ip[1] & 7) {
            case 0:
              do {
                acc = (pnt[idx + 7 - 8] & 1) << 0; /* fallthrough */
              case 7:
                acc |= (pnt[idx + 6 - 8] & 1) << 1; /* fallthrough */
              case 6:
                acc |= (pnt[idx + 5 - 8] & 1) << 2; /* fallthrough */
              case 5:
                acc |= (pnt[idx + 4 - 8] & 1) << 3; /* fallthrough */
              case 4:
                acc |= (pnt[idx + 3 - 8] & 1) << 4; /* fallthrough */
              case 3:
                acc |= (pnt[idx + 2 - 8] & 1) << 5; /* fallthrough */
              case 2:
                acc |= (pnt[idx + 1 - 8] & 1) << 6; /* fallthrough */
              case 1:
                acc |= (pnt[idx + 0 - 8] & 1) << 7;
                *(--scratchpnt) = acc;
                idx -= 8;
              } while (idx);
            }

            scratchpnt = fstCopyVarint32ToLeft(scratchpnt, (time_delta << 1));
          } else {
            scratchpnt -= vm4ip[1];
            memcpy(scratchpnt, pnt, vm4ip[1]);

            scratchpnt = fstCopyVarint32ToLeft(scratchpnt, (time_delta << 1) | 1);
          }
        }
      }

      wrlen = scratchpad + xc->vchg_siz - scratchpnt;
      unc_memreq += wrlen;
      if (wrlen > 32) {
        unsigned long destlen = wrlen;
        unsigned char *dmem;
        unsigned int rc;

        if (!xc->fastpack) {
          if (wrlen <= packmemlen) {
            dmem = packmem;
          } else {
            free(packmem);
            dmem = packmem = (unsigned char *)malloc(compressBound(packmemlen = wrlen));
          }

          rc = compress2(dmem, &destlen, scratchpnt, wrlen, 4);
          if (rc == Z_OK) {
#ifndef FST_DYNAMIC_ALIAS_DISABLE
            PPvoid_t pv = JudyHSIns(&PJHSArray, dmem, destlen, NULL);
            if (*pv) {
              uint32_t pvi = (intptr_t)(*pv);
              vm4ip[2] = -pvi;
            } else {
              *pv = (void *)(intptr_t)(i + 1);
#endif
              fpos += fstWriterVarint(f, wrlen);
              fpos += destlen;
              fstFwrite(dmem, destlen, 1, f);
#ifndef FST_DYNAMIC_ALIAS_DISABLE
            }
#endif
          } else {
#ifndef FST_DYNAMIC_ALIAS_DISABLE
            PPvoid_t pv = JudyHSIns(&PJHSArray, scratchpnt, wrlen, NULL);
            if (*pv) {
              uint32_t pvi = (intptr_t)(*pv);
              vm4ip[2] = -pvi;
            } else {
              *pv = (void *)(intptr_t)(i + 1);
#endif
              fpos += fstWriterVarint(f, 0);
              fpos += wrlen;
              fstFwrite(scratchpnt, wrlen, 1, f);
#ifndef FST_DYNAMIC_ALIAS_DISABLE
            }
#endif
          }
        } else {
          /* this is extremely conservative: fastlz needs +5% for worst case, lz4 needs siz+(siz/255)+16 */
          if (((wrlen * 2) + 2) <= packmemlen) {
            dmem = packmem;
          } else {
            free(packmem);
            dmem = packmem = (unsigned char *)malloc(packmemlen = (wrlen * 2) + 2);
          }

          rc = (xc->fourpack) ? LZ4_compress_default((char *)scratchpnt, (char *)dmem, wrlen, packmemlen)
                              : fastlz_compress(scratchpnt, wrlen, dmem);
          if (rc < destlen) {
#ifndef FST_DYNAMIC_ALIAS_DISABLE
            PPvoid_t pv = JudyHSIns(&PJHSArray, dmem, rc, NULL);
            if (*pv) {
              uint32_t pvi = (intptr_t)(*pv);
              vm4ip[2] = -pvi;
            } else {
              *pv = (void *)(intptr_t)(i + 1);
#endif
              fpos += fstWriterVarint(f, wrlen);
              fpos += rc;
              fstFwrite(dmem, rc, 1, f);
#ifndef FST_DYNAMIC_ALIAS_DISABLE
            }
#endif
          } else {
#ifndef FST_DYNAMIC_ALIAS_DISABLE
            PPvoid_t pv = JudyHSIns(&PJHSArray, scratchpnt, wrlen, NULL);
            if (*pv) {
              uint32_t pvi = (intptr_t)(*pv);
              vm4ip[2] = -pvi;
            } else {
              *pv = (void *)(intptr_t)(i + 1);
#endif
              fpos += fstWriterVarint(f, 0);
              fpos += wrlen;
              fstFwrite(scratchpnt, wrlen, 1, f);
#ifndef FST_DYNAMIC_ALIAS_DISABLE
            }
#endif
          }
        }
      } else {
#ifndef FST_DYNAMIC_ALIAS_DISABLE
        PPvoid_t pv = JudyHSIns(&PJHSArray, scratchpnt, wrlen, NULL);
        if (*pv) {
          uint32_t pvi = (intptr_t)(*pv);
          vm4ip[2] = -pvi;
        } else {
          *pv = (void *)(intptr_t)(i + 1);
#endif
          fpos += fstWriterVarint(f, 0);
          fpos += wrlen;
          fstFwrite(scratchpnt, wrlen, 1, f);
#ifndef FST_DYNAMIC_ALIAS_DISABLE
        }
#endif
      }

      /* vm4ip[3] = 0; ...redundant with clearing below */
#ifdef FST_DEBUG
      cnt++;
#endif
    }
  }

#ifndef FST_DYNAMIC_ALIAS_DISABLE
  JudyHSFreeArray(&PJHSArray, NULL);
#endif

  free(packmem);
  packmem = NULL; /* packmemlen = 0; */ /* scan-build */

  prevpos = 0;
  zerocnt = 0;
  free(scratchpad);
  scratchpad = NULL;

  indxpos = ftello(f);
  xc->secnum++;

#ifndef FST_DYNAMIC_ALIAS2_DISABLE
  if (1) {
    uint32_t prev_alias = 0;

    for (i = 0; i < xc->maxhandle; i++) {
      vm4ip = &(xc->valpos_mem[4 * i]);

      if (vm4ip[2]) {
        if (zerocnt) {
          fpos += fstWriterVarint(f, (zerocnt << 1));
          zerocnt = 0;
        }

        if (vm4ip[2] & 0x80000000) {
          if (vm4ip[2] != prev_alias) {
            int32_t t_i32 = ((int32_t)(prev_alias = vm4ip[2])); /* vm4ip is generic unsigned data */
            int64_t t_i64 = (int64_t)t_i32;                     /* convert to signed */
            uint64_t t_u64 = (uint64_t)t_i64;                   /* sign extend through 64b */

            fpos += fstWriterSVarint(
                f, (int64_t)((t_u64 << 1) | 1)); /* all in this block was: fpos += fstWriterSVarint(f,
                                                    (((int64_t)((int32_t)(prev_alias = vm4ip[2]))) << 1) | 1); */
          } else {
            fpos += fstWriterSVarint(f, (0 << 1) | 1);
          }
        } else {
          fpos += fstWriterSVarint(f, ((vm4ip[2] - prevpos) << 1) | 1);
          prevpos = vm4ip[2];
        }
        vm4ip[2] = 0;
        vm4ip[3] = 0; /* clear out tchn idx */
      } else {
        zerocnt++;
      }
    }
  } else
#endif
  {
    for (i = 0; i < xc->maxhandle; i++) {
      vm4ip = &(xc->valpos_mem[4 * i]);

      if (vm4ip[2]) {
        if (zerocnt) {
          fpos += fstWriterVarint(f, (zerocnt << 1));
          zerocnt = 0;
        }

        if (vm4ip[2] & 0x80000000) {
          fpos += fstWriterVarint(
              f, 0); /* signal, note that using a *signed* varint would be more efficient than this byte escape! */
          fpos += fstWriterVarint(f, (-(int32_t)vm4ip[2]));
        } else {
          fpos += fstWriterVarint(f, ((vm4ip[2] - prevpos) << 1) | 1);
          prevpos = vm4ip[2];
        }
        vm4ip[2] = 0;
        vm4ip[3] = 0; /* clear out tchn idx */
      } else {
        zerocnt++;
      }
    }
  }

  if (zerocnt) {
    /* fpos += */ fstWriterVarint(f, (zerocnt << 1)); /* scan-build */
  }
#ifdef FST_DEBUG
  fprintf(stderr, FST_APIMESS "value chains: %d\n", cnt);
#endif

  xc->vchg_mem[0] = '!';
  xc->vchg_siz = 1;

  endpos = ftello(xc->handle);
  fstWriterUint64(xc->handle, endpos - indxpos); /* write delta index position at very end of block */

  /*emit time changes for block */
  fflush(xc->tchn_handle);
  tlen = ftello(xc->tchn_handle);
  fstWriterFseeko(xc, xc->tchn_handle, 0, SEEK_SET);

  errno = 0;
  fstWriterMmapSanity(
      tmem = (unsigned char *)fstMmap(NULL, tlen, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(xc->tchn_handle), 0),
      __FILE__, __LINE__, "tmem");
  if (tmem) {
    unsigned long destlen = tlen;
    unsigned char *dmem = (unsigned char *)malloc(compressBound(destlen));
    int rc = compress2(dmem, &destlen, tmem, tlen, 9);

    if ((rc == Z_OK) && (((fst_off_t)destlen) < tlen)) {
      fstFwrite(dmem, destlen, 1, xc->handle);
    } else /* comparison between compressed / decompressed len tells if compressed */
    {
      fstFwrite(tmem, tlen, 1, xc->handle);
      destlen = tlen;
    }
    free(dmem);
    fstMunmap(tmem, tlen);
    fstWriterUint64(xc->handle, tlen);         /* uncompressed */
    fstWriterUint64(xc->handle, destlen);      /* compressed */
    fstWriterUint64(xc->handle, xc->tchn_cnt); /* number of time items */
  }

  xc->tchn_cnt = xc->tchn_idx = 0;
  fstWriterFseeko(xc, xc->tchn_handle, 0, SEEK_SET);
  fstFtruncate(fileno(xc->tchn_handle), 0);

  /* write block trailer */
  endpos = ftello(xc->handle);
  fstWriterFseeko(xc, xc->handle, xc->section_start, SEEK_SET);
  fstWriterUint64(xc->handle, endpos - xc->section_start); /* write block length */
  fstWriterFseeko(xc, xc->handle, 8, SEEK_CUR);            /* skip begin time */
  fstWriterUint64(xc->handle, xc->curtime);                /* write end time for section */
  fstWriterUint64(xc->handle, unc_memreq); /* amount of buffer memory required in reader for full traversal */
  fflush(xc->handle);

  fstWriterFseeko(xc, xc->handle, xc->section_start - 1, SEEK_SET); /* write out FST_BL_VCDATA over FST_BL_SKIP */

#ifndef FST_DYNAMIC_ALIAS_DISABLE
#ifndef FST_DYNAMIC_ALIAS2_DISABLE
  fputc(FST_BL_VCDATA_DYN_ALIAS2, xc->handle);
#else
  fputc(FST_BL_VCDATA_DYN_ALIAS, xc->handle);
#endif
#else
  fputc(FST_BL_VCDATA, xc->handle);
#endif

  fflush(xc->handle);

  fstWriterFseeko(xc, xc->handle, endpos, SEEK_SET); /* seek to end of file */

  xc2->section_header_truncpos = endpos; /* cache in case of need to truncate */
  if (xc->dump_size_limit) {
    if (endpos >= ((fst_off_t)xc->dump_size_limit)) {
      xc2->skip_writing_section_hdr = 1;
      xc2->size_limit_locked = 1;
      xc2->is_initial_time = 1; /* to trick emit value and emit time change */
#ifdef FST_DEBUG
      fprintf(stderr, FST_APIMESS "<< dump file size limit reached, stopping dumping >>\n");
#endif
    }
  }

  if (!xc2->skip_writing_section_hdr) {
    fstWriterEmitSectionHeader(xc); /* emit next section header */
  }
  fflush(xc->handle);

  xc->already_in_flush = 0;
}

#ifdef FST_WRITER_PARALLEL
static void *fstWriterFlushContextPrivate1(struct fstWriterContext *xc) {
  struct fstWriterContext *xc_parent;

  pthread_mutex_lock(&(xc->xc_parent->mutex));
  fstWriterFlushContextPrivate2(xc);

#ifdef FST_REMOVE_DUPLICATE_VC
  free(xc->curval_mem);
#endif
  free(xc->valpos_mem);
  free(xc->vchg_mem);
  tmpfile_close(&xc->tchn_handle, &xc->tchn_handle_nam);
  xc_parent = xc->xc_parent;
  free(xc);

  xc_parent->in_pthread = 0;
  pthread_mutex_unlock(&(xc_parent->mutex));

  return (NULL);
}

static void fstWriterFlushContextPrivate(struct fstWriterContext *xc) {
  if (xc->parallel_enabled) {
    struct fstWriterContext *xc2 = (struct fstWriterContext *)malloc(sizeof(struct fstWriterContext));
    unsigned int i;

    pthread_mutex_lock(&xc->mutex);
    pthread_mutex_unlock(&xc->mutex);

    xc->xc_parent = xc;
    memcpy(xc2, xc, sizeof(struct fstWriterContext));

    if (sizeof(size_t) < sizeof(uint64_t)) {
      /* TALOS-2023-1777 for 32b overflow */
      uint64_t chk_64 = xc->maxhandle * 4 * sizeof(uint32_t);
      size_t chk_32 = xc->maxhandle * 4 * sizeof(uint32_t);
      if (chk_64 != chk_32)
        chk_report_abort("TALOS-2023-1777");
    }

    xc2->valpos_mem = (uint32_t *)malloc(xc->maxhandle * 4 * sizeof(uint32_t));
    memcpy(xc2->valpos_mem, xc->valpos_mem, xc->maxhandle * 4 * sizeof(uint32_t));

    /* curval mem is updated in the thread */
#ifdef FST_REMOVE_DUPLICATE_VC
    xc2->curval_mem = (unsigned char *)malloc(xc->maxvalpos);
    memcpy(xc2->curval_mem, xc->curval_mem, xc->maxvalpos);
#endif

    xc->vchg_mem = (unsigned char *)malloc(xc->vchg_alloc_siz);
    xc->vchg_mem[0] = '!';
    xc->vchg_siz = 1;

    for (i = 0; i < xc->maxhandle; i++) {
      uint32_t *vm4ip = &(xc->valpos_mem[4 * i]);
      vm4ip[2] = 0; /* zero out offset val */
      vm4ip[3] = 0; /* zero out last time change val */
    }

    xc->tchn_cnt = xc->tchn_idx = 0;
    xc->tchn_handle = tmpfile_open(&xc->tchn_handle_nam); /* child thread will deallocate file/name */
    fstWriterFseeko(xc, xc->tchn_handle, 0, SEEK_SET);
    fstFtruncate(fileno(xc->tchn_handle), 0);

    xc->section_header_only = 0;
    xc->secnum++;

    while (xc->in_pthread) {
      pthread_mutex_lock(&xc->mutex);
      pthread_mutex_unlock(&xc->mutex);
    };

    pthread_mutex_lock(&xc->mutex);
    xc->in_pthread = 1;
    pthread_mutex_unlock(&xc->mutex);

    pthread_create(&xc->thread, &xc->thread_attr, fstWriterFlushContextPrivate1, xc2);
  } else {
    if (xc->parallel_was_enabled) /* conservatively block */
    {
      pthread_mutex_lock(&xc->mutex);
      pthread_mutex_unlock(&xc->mutex);
    }

    xc->xc_parent = xc;
    fstWriterFlushContextPrivate2(xc);
  }
}
#endif

/*
 * queues up a flush context operation
 */
void fstWriterFlushContext(struct fstWriterContext *xc) {
  if (xc->tchn_idx > 1) {
    xc->flush_context_pending = 1;
  }
}

/*
 * close out FST file
 */
void fstWriterClose(struct fstWriterContext *xc) {
  if (!xc) {
    return;
  }

#ifdef FST_WRITER_PARALLEL
  if (xc) {
    pthread_mutex_lock(&xc->mutex);
    pthread_mutex_unlock(&xc->mutex);
  }
#endif

  if (!xc->already_in_close && !xc->already_in_flush) {
    unsigned char *tmem = NULL;
    fst_off_t fixup_offs, tlen, hlen;

    xc->already_in_close = 1; /* never need to zero this out as it is freed at bottom */

    if (xc->section_header_only && xc->section_header_truncpos && (xc->vchg_siz <= 1) && (!xc->is_initial_time)) {
      fstFtruncate(fileno(xc->handle), xc->section_header_truncpos);
      fstWriterFseeko(xc, xc->handle, xc->section_header_truncpos, SEEK_SET);
      xc->section_header_only = 0;
    } else {
      xc->skip_writing_section_hdr = 1;
      if (!xc->size_limit_locked) {
        if (FST_UNLIKELY(
                xc->is_initial_time)) /* simulation time never advanced so mock up the changes as time zero ones */
        {
          fstHandle dupe_idx;

          fstWriterEmitTimeChange(xc, 0);                          /* emit some time change just to have one */
          for (dupe_idx = 0; dupe_idx < xc->maxhandle; dupe_idx++) /* now clone the values */
          {
            fstWriterEmitValueChange(xc, dupe_idx + 1, xc->curval_mem + xc->valpos_mem[4 * dupe_idx]);
          }
        }
        fstWriterFlushContextPrivate(xc);
#ifdef FST_WRITER_PARALLEL
        pthread_mutex_lock(&xc->mutex);
        pthread_mutex_unlock(&xc->mutex);

        while (xc->in_pthread) {
          pthread_mutex_lock(&xc->mutex);
          pthread_mutex_unlock(&xc->mutex);
        };
#endif
      }
    }
    fstDestroyMmaps(xc, 1);
    if (xc->outval_mem) {
      free(xc->outval_mem);
      xc->outval_mem = NULL;
      xc->outval_alloc_siz = 0;
    }

    /* write out geom section */
    fflush(xc->geom_handle);
    tlen = ftello(xc->geom_handle);
    errno = 0;
    if (tlen) {
      fstWriterMmapSanity(
          tmem = (unsigned char *)fstMmap(NULL, tlen, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(xc->geom_handle), 0),
          __FILE__, __LINE__, "tmem");
    }

    if (tmem) {
      unsigned long destlen = tlen;
      unsigned char *dmem = (unsigned char *)malloc(compressBound(destlen));
      int rc = compress2(dmem, &destlen, tmem, tlen, 9);

      if ((rc != Z_OK) || (((fst_off_t)destlen) > tlen)) {
        destlen = tlen;
      }

      fixup_offs = ftello(xc->handle);
      fputc(FST_BL_SKIP, xc->handle);             /* temporary tag */
      fstWriterUint64(xc->handle, destlen + 24);  /* section length */
      fstWriterUint64(xc->handle, tlen);          /* uncompressed */
                                                  /* compressed len is section length - 24 */
      fstWriterUint64(xc->handle, xc->maxhandle); /* maxhandle */
      fstFwrite((((fst_off_t)destlen) != tlen) ? dmem : tmem, destlen, 1, xc->handle);
      fflush(xc->handle);

      fstWriterFseeko(xc, xc->handle, fixup_offs, SEEK_SET);
      fputc(FST_BL_GEOM, xc->handle); /* actual tag */

      fstWriterFseeko(xc, xc->handle, 0, SEEK_END); /* move file pointer to end for any section adds */
      fflush(xc->handle);

      free(dmem);
      fstMunmap(tmem, tlen);
    }

    if (xc->num_blackouts) {
      uint64_t cur_bl = 0;
      fst_off_t bpos, eos;
      uint32_t i;

      fixup_offs = ftello(xc->handle);
      fputc(FST_BL_SKIP, xc->handle); /* temporary tag */
      bpos = fixup_offs + 1;
      fstWriterUint64(xc->handle, 0); /* section length */
      fstWriterVarint(xc->handle, xc->num_blackouts);

      for (i = 0; i < xc->num_blackouts; i++) {
        fputc(xc->blackout_head->active, xc->handle);
        fstWriterVarint(xc->handle, xc->blackout_head->tim - cur_bl);
        cur_bl = xc->blackout_head->tim;
        xc->blackout_curr = xc->blackout_head->next;
        free(xc->blackout_head);
        xc->blackout_head = xc->blackout_curr;
      }

      eos = ftello(xc->handle);
      fstWriterFseeko(xc, xc->handle, bpos, SEEK_SET);
      fstWriterUint64(xc->handle, eos - bpos);
      fflush(xc->handle);

      fstWriterFseeko(xc, xc->handle, fixup_offs, SEEK_SET);
      fputc(FST_BL_BLACKOUT, xc->handle); /* actual tag */

      fstWriterFseeko(xc, xc->handle, 0, SEEK_END); /* move file pointer to end for any section adds */
      fflush(xc->handle);
    }

    if (xc->compress_hier) {
      fst_off_t hl, eos;
      gzFile zhandle;
      int zfd;
      int fourpack_duo = 0;
#ifndef __MINGW32__
      int fnam_len = strlen(xc->filename) + 5 + 1;
      char *fnam = (char *)malloc(fnam_len);
#endif

      fixup_offs = ftello(xc->handle);
      fputc(FST_BL_SKIP, xc->handle); /* temporary tag */
      hlen = ftello(xc->handle);
      fstWriterUint64(xc->handle, 0);                 /* section length */
      fstWriterUint64(xc->handle, xc->hier_file_len); /* uncompressed length */

      if (!xc->fourpack) {
        unsigned char *mem = (unsigned char *)malloc(FST_GZIO_LEN);
        zfd = dup(fileno(xc->handle));
        fflush(xc->handle);
        zhandle = gzdopen(zfd, "wb4");
        if (zhandle) {
          fstWriterFseeko(xc, xc->hier_handle, 0, SEEK_SET);
          for (hl = 0; hl < xc->hier_file_len; hl += FST_GZIO_LEN) {
            unsigned len = ((xc->hier_file_len - hl) > FST_GZIO_LEN) ? FST_GZIO_LEN : (xc->hier_file_len - hl);
            fstFread(mem, len, 1, xc->hier_handle);
            gzwrite(zhandle, mem, len);
          }
          gzclose(zhandle);
        } else {
          close(zfd);
        }
        free(mem);
      } else {
        int lz4_maxlen;
        unsigned char *mem;
        unsigned char *hmem = NULL;
        int packed_len;

        fflush(xc->handle);

        lz4_maxlen = LZ4_compressBound(xc->hier_file_len);
        mem = (unsigned char *)malloc(lz4_maxlen);
        errno = 0;
        if (xc->hier_file_len) {
          fstWriterMmapSanity(hmem = (unsigned char *)fstMmap(NULL, xc->hier_file_len, PROT_READ | PROT_WRITE,
                                                              MAP_SHARED, fileno(xc->hier_handle), 0),
                              __FILE__, __LINE__, "hmem");
        }
        packed_len = LZ4_compress_default((char *)hmem, (char *)mem, xc->hier_file_len, lz4_maxlen);
        fstMunmap(hmem, xc->hier_file_len);

        fourpack_duo = (!xc->repack_on_close) &&
                       (xc->hier_file_len > FST_HDR_FOURPACK_DUO_SIZE); /* double pack when hierarchy is large */

        if (fourpack_duo) /* double packing with LZ4 is faster than gzip */
        {
          unsigned char *mem_duo;
          int lz4_maxlen_duo;
          int packed_len_duo;

          lz4_maxlen_duo = LZ4_compressBound(packed_len);
          mem_duo = (unsigned char *)malloc(lz4_maxlen_duo);
          packed_len_duo = LZ4_compress_default((char *)mem, (char *)mem_duo, packed_len, lz4_maxlen_duo);

          fstWriterVarint(xc->handle, packed_len); /* 1st round compressed length */
          fstFwrite(mem_duo, packed_len_duo, 1, xc->handle);
          free(mem_duo);
        } else {
          fstFwrite(mem, packed_len, 1, xc->handle);
        }

        free(mem);
      }

      fstWriterFseeko(xc, xc->handle, 0, SEEK_END);
      eos = ftello(xc->handle);
      fstWriterFseeko(xc, xc->handle, hlen, SEEK_SET);
      fstWriterUint64(xc->handle, eos - hlen);
      fflush(xc->handle);

      fstWriterFseeko(xc, xc->handle, fixup_offs, SEEK_SET);
      fputc(xc->fourpack ? (fourpack_duo ? FST_BL_HIER_LZ4DUO : FST_BL_HIER_LZ4) : FST_BL_HIER,
            xc->handle); /* actual tag now also == compression type */

      fstWriterFseeko(xc, xc->handle, 0, SEEK_END); /* move file pointer to end for any section adds */
      fflush(xc->handle);

#ifndef __MINGW32__
      snprintf(fnam, fnam_len, "%s.hier", xc->filename);
      unlink(fnam);
      free(fnam);
#endif
    }

    /* finalize out header */
    fstWriterFseeko(xc, xc->handle, FST_HDR_OFFS_START_TIME, SEEK_SET);
    fstWriterUint64(xc->handle, xc->firsttime);
    fstWriterUint64(xc->handle, xc->curtime);
    fstWriterFseeko(xc, xc->handle, FST_HDR_OFFS_NUM_SCOPES, SEEK_SET);
    fstWriterUint64(xc->handle, xc->numscopes);
    fstWriterUint64(xc->handle, xc->numsigs);
    fstWriterUint64(xc->handle, xc->maxhandle);
    fstWriterUint64(xc->handle, xc->secnum);
    fflush(xc->handle);

    tmpfile_close(&xc->tchn_handle, &xc->tchn_handle_nam);
    free(xc->vchg_mem);
    xc->vchg_mem = NULL;
    tmpfile_close(&xc->curval_handle, &xc->curval_handle_nam);
    tmpfile_close(&xc->valpos_handle, &xc->valpos_handle_nam);
    tmpfile_close(&xc->geom_handle, &xc->geom_handle_nam);
    if (xc->hier_handle) {
      fclose(xc->hier_handle);
      xc->hier_handle = NULL;
    }
    if (xc->handle) {
      if (xc->repack_on_close) {
        FILE *fp;
        fst_off_t offpnt, uclen;
        int flen = strlen(xc->filename);
        char *hf = (char *)calloc(1, flen + 5);

        strcpy(hf, xc->filename);
        strcpy(hf + flen, ".pak");
        fp = fopen(hf, "wb");

        if (fp) {
          gzFile dsth;
          int zfd;
          char gz_membuf[FST_GZIO_LEN];

          fstWriterFseeko(xc, xc->handle, 0, SEEK_END);
          uclen = ftello(xc->handle);

          fputc(FST_BL_ZWRAPPER, fp);
          fstWriterUint64(fp, 0);
          fstWriterUint64(fp, uclen);
          fflush(fp);

          fstWriterFseeko(xc, xc->handle, 0, SEEK_SET);
          zfd = dup(fileno(fp));
          dsth = gzdopen(zfd, "wb4");
          if (dsth) {
            for (offpnt = 0; offpnt < uclen; offpnt += FST_GZIO_LEN) {
              size_t this_len = ((uclen - offpnt) > FST_GZIO_LEN) ? FST_GZIO_LEN : (uclen - offpnt);
              fstFread(gz_membuf, this_len, 1, xc->handle);
              gzwrite(dsth, gz_membuf, this_len);
            }
            gzclose(dsth);
          } else {
            close(zfd);
          }
          fstWriterFseeko(xc, fp, 0, SEEK_END);
          offpnt = ftello(fp);
          fstWriterFseeko(xc, fp, 1, SEEK_SET);
          fstWriterUint64(fp, offpnt - 1);
          fclose(fp);
          fclose(xc->handle);
          xc->handle = NULL;

          unlink(xc->filename);
          rename(hf, xc->filename);
        } else {
          xc->repack_on_close = 0;
          fclose(xc->handle);
          xc->handle = NULL;
        }

        free(hf);
      } else {
        fclose(xc->handle);
        xc->handle = NULL;
      }
    }

#ifdef __MINGW32__
    {
      int flen = strlen(xc->filename);
      char *hf = (char *)calloc(1, flen + 6);
      strcpy(hf, xc->filename);

      if (xc->compress_hier) {
        strcpy(hf + flen, ".hier");
        unlink(hf); /* no longer needed as a section now exists for this */
      }

      free(hf);
    }
#endif

#ifdef FST_WRITER_PARALLEL
    pthread_mutex_destroy(&xc->mutex);
    pthread_attr_destroy(&xc->thread_attr);
#endif

    if (xc->path_array) {
#ifndef _WAVE_HAVE_JUDY
      const uint32_t hashmask = FST_PATH_HASHMASK;
#endif
      JudyHSFreeArray(&(xc->path_array), NULL);
    }

    free(xc->filename);
    xc->filename = NULL;
    free(xc);
  }
}

/*
 * functions to set miscellaneous header/block information
 */
void fstWriterSetDate(struct fstWriterContext *xc, const char *dat) {
  char s[FST_HDR_DATE_SIZE];
  fst_off_t fpos = ftello(xc->handle);
  int len = strlen(dat);

  fstWriterFseeko(xc, xc->handle, FST_HDR_OFFS_DATE, SEEK_SET);
  memset(s, 0, FST_HDR_DATE_SIZE);
  memcpy(s, dat, (len < FST_HDR_DATE_SIZE) ? len : FST_HDR_DATE_SIZE);
  fstFwrite(s, FST_HDR_DATE_SIZE, 1, xc->handle);
  fflush(xc->handle);
  fstWriterFseeko(xc, xc->handle, fpos, SEEK_SET);
}

void fstWriterSetVersion(struct fstWriterContext *xc, const char *vers) {
  assert(vers);

  char s[FST_HDR_SIM_VERSION_SIZE];
  fst_off_t fpos = ftello(xc->handle);
  int len = strlen(vers);

  fstWriterFseeko(xc, xc->handle, FST_HDR_OFFS_SIM_VERSION, SEEK_SET);
  memset(s, 0, FST_HDR_SIM_VERSION_SIZE);
  memcpy(s, vers, (len < FST_HDR_SIM_VERSION_SIZE) ? len : FST_HDR_SIM_VERSION_SIZE);
  fstFwrite(s, FST_HDR_SIM_VERSION_SIZE, 1, xc->handle);
  fflush(xc->handle);
  fstWriterFseeko(xc, xc->handle, fpos, SEEK_SET);
}

void fstWriterSetFileType(struct fstWriterContext *xc, enum fstFileType filetype) {
  assert(/*(filetype >= FST_FT_MIN) &&*/ (filetype <= FST_FT_MAX));

  fst_off_t fpos = ftello(xc->handle);

  xc->filetype = filetype;

  fstWriterFseeko(xc, xc->handle, FST_HDR_OFFS_FILETYPE, SEEK_SET);
  fputc(xc->filetype, xc->handle);
  fflush(xc->handle);
  fstWriterFseeko(xc, xc->handle, fpos, SEEK_SET);
}

static void fstWriterSetAttrDoubleArgGeneric(struct fstWriterContext *xc, int typ, uint64_t arg1, uint64_t arg2) {
  unsigned char buf[11]; /* ceil(64/7) = 10 + null term */
  unsigned char *pnt = fstCopyVarint64ToRight(buf, arg1);
  if (arg1) {
    *pnt = 0; /* this converts any *nonzero* arg1 when made a varint into a null-term string */
  }

  fstWriterSetAttrBegin(xc, FST_AT_MISC, typ, (char *)buf, arg2);
}

static void fstWriterSetAttrGeneric(struct fstWriterContext *xc, const char *comm, int typ, uint64_t arg) {
  assert(comm);

  char *s = strdup(comm);
  char *sf = s;

  while (*s) {
    if ((*s == '\n') || (*s == '\r'))
      *s = ' ';
    s++;
  }

  fstWriterSetAttrBegin(xc, FST_AT_MISC, typ, sf, arg);
  free(sf);
}

static void fstWriterSetSourceStem_2(struct fstWriterContext *xc, const char *path, unsigned int line,
                                     unsigned int use_realpath, int typ) {
  assert(path && path[0]);

  uint64_t sidx = 0;
  int slen = strlen(path);
#ifndef _WAVE_HAVE_JUDY
  const uint32_t hashmask = FST_PATH_HASHMASK;
  const unsigned char *path2 = (const unsigned char *)path;
  PPvoid_t pv;
#else
  char *path2 = (char *)alloca(slen + 1); /* judy lacks const qualifier in its JudyHSIns definition */
  PPvoid_t pv;
  strcpy(path2, path);
#endif

  pv = JudyHSIns(&(xc->path_array), path2, slen, NULL);
  if (*pv) {
    sidx = (intptr_t)(*pv);
  } else {
    char *rp = NULL;

    sidx = ++xc->path_array_count;
    *pv = (void *)(intptr_t)(xc->path_array_count);

    if (use_realpath) {
      rp = fstRealpath(
#ifndef _WAVE_HAVE_JUDY
          (const char *)
#endif
              path2,
          NULL);
    }

    fstWriterSetAttrGeneric(xc,
                            rp ? rp :
#ifndef _WAVE_HAVE_JUDY
                               (const char *)
#endif
                                    path2,
                            FST_MT_PATHNAME, sidx);

    if (rp) {
      free(rp);
    }
  }

  fstWriterSetAttrDoubleArgGeneric(xc, typ, sidx, line);
}

void fstWriterSetSourceStem(struct fstWriterContext *xc, const char *path, unsigned int line,
                            unsigned int use_realpath) {
  fstWriterSetSourceStem_2(xc, path, line, use_realpath, FST_MT_SOURCESTEM);
}

void fstWriterSetSourceInstantiationStem(struct fstWriterContext *xc, const char *path, unsigned int line,
                                         unsigned int use_realpath) {
  fstWriterSetSourceStem_2(xc, path, line, use_realpath, FST_MT_SOURCEISTEM);
}

void fstWriterSetComment(struct fstWriterContext *xc, const char *comm) {
  fstWriterSetAttrGeneric(xc, comm, FST_MT_COMMENT, 0);
}

void fstWriterSetValueList(struct fstWriterContext *xc, const char *vl) {
  fstWriterSetAttrGeneric(xc, vl, FST_MT_VALUELIST, 0);
}

void fstWriterSetEnvVar(struct fstWriterContext *xc, const char *envvar) {
  fstWriterSetAttrGeneric(xc, envvar, FST_MT_ENVVAR, 0);
}

void fstWriterSetTimescale(struct fstWriterContext *xc, int ts) {
  fst_off_t fpos = ftello(xc->handle);
  fstWriterFseeko(xc, xc->handle, FST_HDR_OFFS_TIMESCALE, SEEK_SET);
  fputc(ts & 255, xc->handle);
  fflush(xc->handle);
  fstWriterFseeko(xc, xc->handle, fpos, SEEK_SET);
}

void fstWriterSetTimescaleFromString(struct fstWriterContext *xc, const char *s) {
  assert(s);
  int mat = 0;
  int seconds_exp = -9;
  int tv = atoi(s);
  const char *pnt = s;

  while (*pnt) {
    switch (*pnt) {
    case 'm':
      seconds_exp = -3;
      mat = 1;
      break;
    case 'u':
      seconds_exp = -6;
      mat = 1;
      break;
    case 'n':
      seconds_exp = -9;
      mat = 1;
      break;
    case 'p':
      seconds_exp = -12;
      mat = 1;
      break;
    case 'f':
      seconds_exp = -15;
      mat = 1;
      break;
    case 'a':
      seconds_exp = -18;
      mat = 1;
      break;
    case 'z':
      seconds_exp = -21;
      mat = 1;
      break;
    case 's':
      seconds_exp = 0;
      mat = 1;
      break;
    default:
      break;
    }

    if (mat)
      break;
    pnt++;
  }

  if (tv == 10) {
    seconds_exp++;
  } else if (tv == 100) {
    seconds_exp += 2;
  }

  fstWriterSetTimescale(xc, seconds_exp);
}

void fstWriterSetTimezero(struct fstWriterContext *xc, int64_t tim) {

  fst_off_t fpos = ftello(xc->handle);
  fstWriterFseeko(xc, xc->handle, FST_HDR_OFFS_TIMEZERO, SEEK_SET);
  fstWriterUint64(xc->handle, (xc->timezero = tim));
  fflush(xc->handle);
  fstWriterFseeko(xc, xc->handle, fpos, SEEK_SET);
}

void fstWriterSetPackType(struct fstWriterContext *xc, enum fstWriterPackType typ) {

  xc->fastpack = (typ != FST_WR_PT_ZLIB);
  xc->fourpack = (typ == FST_WR_PT_LZ4);
}

void fstWriterSetRepackOnClose(struct fstWriterContext *xc, int enable) { xc->repack_on_close = (enable != 0); }

void fstWriterSetParallelMode(struct fstWriterContext *xc, int enable) {

  xc->parallel_was_enabled |= xc->parallel_enabled; /* make sticky */
  xc->parallel_enabled = (enable != 0);
#ifndef FST_WRITER_PARALLEL
  if (xc->parallel_enabled) {
    fprintf(stderr,
            FST_APIMESS "fstWriterSetParallelMode(), FST_WRITER_PARALLEL not enabled during compile, exiting.\n");
    exit(255);
  }
#endif
}

void fstWriterSetDumpSizeLimit(struct fstWriterContext *xc, uint64_t numbytes) { xc->dump_size_limit = numbytes; }

int fstWriterGetDumpSizeLimitReached(struct fstWriterContext *xc) { return (xc->size_limit_locked != 0); }

int fstWriterGetFseekFailed(struct fstWriterContext *xc) { return (xc->fseek_failed != 0); }

/*
 * writer attr/scope/var creation:
 * fstWriterCreateVar2() is used to dump VHDL or other languages, but the
 * underlying variable needs to map to Verilog/SV via the proper fstVarType vt
 */
fstHandle fstWriterCreateVar2(struct fstWriterContext *xc, enum fstVarType vt, enum fstVarDir vd, uint32_t len,
                              const char *nam, fstHandle aliasHandle, const char *type, enum fstSupplementalVarType svt,
                              enum fstSupplementalDataType sdt) {
  fstWriterSetAttrGeneric(xc, type ? type : "", FST_MT_SUPVAR,
                          (svt << FST_SDT_SVT_SHIFT_COUNT) | (sdt & FST_SDT_ABS_MAX));
  return fstWriterCreateVar(xc, vt, vd, len, nam, aliasHandle);
}

fstHandle fstWriterCreateVar(struct fstWriterContext *xc, enum fstVarType vt, enum fstVarDir vd, uint32_t len,
                             const char *nam, fstHandle aliasHandle) {
  unsigned int i;
  int nlen, is_real;

  assert(nam);

  if (xc->valpos_mem) {
    fstDestroyMmaps(xc, 0);
  }

  fputc(vt, xc->hier_handle);
  fputc(vd, xc->hier_handle);
  nlen = strlen(nam);
  fstFwrite(nam, nlen, 1, xc->hier_handle);
  fputc(0, xc->hier_handle);
  xc->hier_file_len += (nlen + 3);

  if ((vt == FST_VT_VCD_REAL) || (vt == FST_VT_VCD_REAL_PARAMETER) || (vt == FST_VT_VCD_REALTIME) ||
      (vt == FST_VT_SV_SHORTREAL)) {
    is_real = 1;
    len = 8; /* recast number of bytes to that of what a double is */
  } else {
    is_real = 0;
    if (vt == FST_VT_GEN_STRING) {
      len = 0;
    }
  }

  xc->hier_file_len += fstWriterVarint(xc->hier_handle, len);

  if (aliasHandle > xc->maxhandle)
    aliasHandle = 0;
  xc->hier_file_len += fstWriterVarint(xc->hier_handle, aliasHandle);
  xc->numsigs++;
  if (xc->numsigs == xc->next_huge_break) {
    if (xc->fst_break_size < xc->fst_huge_break_size) {
      xc->next_huge_break += FST_ACTIVATE_HUGE_INC;
      xc->fst_break_size += xc->fst_orig_break_size;
      xc->fst_break_add_size += xc->fst_orig_break_add_size;

      xc->vchg_alloc_siz = xc->fst_break_size + xc->fst_break_add_size;
      if (xc->vchg_mem) {
        xc->vchg_mem = (unsigned char *)realloc(xc->vchg_mem, xc->vchg_alloc_siz);
      }
    }
  }

  if (!aliasHandle) {
    uint32_t zero = 0;

    if (len) {
      fstWriterVarint(xc->geom_handle, !is_real ? len : 0); /* geom section encodes reals as zero byte */
    } else {
      fstWriterVarint(xc->geom_handle, 0xFFFFFFFF); /* geom section encodes zero len as 32b -1 */
    }

    fstFwrite(&xc->maxvalpos, sizeof(uint32_t), 1, xc->valpos_handle);
    fstFwrite(&len, sizeof(uint32_t), 1, xc->valpos_handle);
    fstFwrite(&zero, sizeof(uint32_t), 1, xc->valpos_handle);
    fstFwrite(&zero, sizeof(uint32_t), 1, xc->valpos_handle);

    if (!is_real) {
      for (i = 0; i < len; i++) {
        fputc('x', xc->curval_handle);
      }
    } else {
      fstFwrite(&xc->nan, 8, 1, xc->curval_handle); /* initialize doubles to NaN rather than x */
    }

    xc->maxvalpos += len;
    xc->maxhandle++;
    return (xc->maxhandle);
  } else {
    return (aliasHandle);
  }
}

void fstWriterSetScope(struct fstWriterContext *xc, enum fstScopeType scopetype, const char *scopename,
                       const char *scopecomp) {
  fputc(FST_ST_VCD_SCOPE, xc->hier_handle);
  if (/*(scopetype < FST_ST_VCD_MODULE) ||*/ (scopetype > FST_ST_MAX)) {
    scopetype = FST_ST_VCD_MODULE;
  }
  fputc(scopetype, xc->hier_handle);
  fprintf(xc->hier_handle, "%s%c%s%c", scopename ? scopename : "", 0, scopecomp ? scopecomp : "", 0);

  if (scopename) {
    xc->hier_file_len += strlen(scopename);
  }
  if (scopecomp) {
    xc->hier_file_len += strlen(scopecomp);
  }

  xc->hier_file_len += 4; /* FST_ST_VCD_SCOPE + scopetype + two string terminating zeros */
  xc->numscopes++;
}

void fstWriterSetUpscope(struct fstWriterContext *xc) {
  fputc(FST_ST_VCD_UPSCOPE, xc->hier_handle);
  xc->hier_file_len++;
}

void fstWriterSetAttrBegin(struct fstWriterContext *xc, enum fstAttrType attrtype, int subtype, const char *attrname,
                           uint64_t arg) {

  fputc(FST_ST_GEN_ATTRBEGIN, xc->hier_handle);
  if (/*(attrtype < FST_AT_MISC) ||*/ (attrtype > FST_AT_MAX)) {
    attrtype = FST_AT_MISC;
    subtype = FST_MT_UNKNOWN;
  }
  fputc(attrtype, xc->hier_handle);

  switch (attrtype) {
  case FST_AT_ARRAY:
    if ((subtype < FST_AR_NONE) || (subtype > FST_AR_MAX))
      subtype = FST_AR_NONE;
    break;
  case FST_AT_ENUM:
    if ((subtype < FST_EV_SV_INTEGER) || (subtype > FST_EV_MAX))
      subtype = FST_EV_SV_INTEGER;
    break;
  case FST_AT_PACK:
    if ((subtype < FST_PT_NONE) || (subtype > FST_PT_MAX))
      subtype = FST_PT_NONE;
    break;

  case FST_AT_MISC:
  default:
    break;
  }

  fputc(subtype, xc->hier_handle);
  fprintf(xc->hier_handle, "%s%c", attrname ? attrname : "", 0);

  if (attrname) {
    xc->hier_file_len += strlen(attrname);
  }

  xc->hier_file_len += 4; /* FST_ST_GEN_ATTRBEGIN + type + subtype + string terminating zero */
  xc->hier_file_len += fstWriterVarint(xc->hier_handle, arg);
}

void fstWriterSetAttrEnd(struct fstWriterContext *xc) {
  fputc(FST_ST_GEN_ATTREND, xc->hier_handle);
  xc->hier_file_len++;
}

fstEnumHandle fstWriterCreateEnumTable(struct fstWriterContext *xc, const char *name, uint32_t elem_count,
                                       unsigned int min_valbits, const char **literal_arr, const char **val_arr) {
  fstEnumHandle handle = 0;
  unsigned int *literal_lens = NULL;
  unsigned int *val_lens = NULL;
  int lit_len_tot = 0;
  int val_len_tot = 0;
  int name_len;
  char elem_count_buf[16];
  int elem_count_len;
  int total_len;
  int pos = 0;
  char *attr_str = NULL;

  assert(name && literal_arr && val_arr && (elem_count != 0));

  uint32_t i;

  name_len = strlen(name);
  elem_count_len = snprintf(elem_count_buf, 16, "%" PRIu32, elem_count);

  literal_lens = (unsigned int *)calloc(elem_count, sizeof(unsigned int));
  val_lens = (unsigned int *)calloc(elem_count, sizeof(unsigned int));

  for (i = 0; i < elem_count; i++) {
    literal_lens[i] = strlen(literal_arr[i]);
    lit_len_tot += fstUtilityBinToEscConvertedLen((unsigned char *)literal_arr[i], literal_lens[i]);

    val_lens[i] = strlen(val_arr[i]);
    val_len_tot += fstUtilityBinToEscConvertedLen((unsigned char *)val_arr[i], val_lens[i]);

    if (min_valbits > 0) {
      if (val_lens[i] < min_valbits) {
        val_len_tot += (min_valbits - val_lens[i]); /* additional converted len is same for '0' character */
      }
    }
  }

  total_len = name_len + 1 + elem_count_len + 1 + lit_len_tot + elem_count + val_len_tot + elem_count;

  attr_str = (char *)malloc(total_len);
  pos = 0;

  memcpy(attr_str + pos, name, name_len);
  pos += name_len;
  attr_str[pos++] = ' ';

  memcpy(attr_str + pos, elem_count_buf, elem_count_len);
  pos += elem_count_len;
  attr_str[pos++] = ' ';

  for (i = 0; i < elem_count; i++) {
    pos += fstUtilityBinToEsc((unsigned char *)attr_str + pos, (unsigned char *)literal_arr[i], literal_lens[i]);
    attr_str[pos++] = ' ';
  }

  for (i = 0; i < elem_count; i++) {
    if (min_valbits > 0) {
      if (val_lens[i] < min_valbits) {
        memset(attr_str + pos, '0', min_valbits - val_lens[i]);
        pos += (min_valbits - val_lens[i]);
      }
    }

    pos += fstUtilityBinToEsc((unsigned char *)attr_str + pos, (unsigned char *)val_arr[i], val_lens[i]);
    attr_str[pos++] = ' ';
  }

  attr_str[pos - 1] = 0;

#ifdef FST_DEBUG
  fprintf(stderr, FST_APIMESS "fstWriterCreateEnumTable() total_len: %d, pos: %d\n", total_len, pos);
  fprintf(stderr, FST_APIMESS "*%s*\n", attr_str);
#endif

  fstWriterSetAttrBegin(xc, FST_AT_MISC, FST_MT_ENUMTABLE, attr_str, handle = ++xc->max_enumhandle);

  free(attr_str);
  free(val_lens);
  free(literal_lens);

  return (handle);
}

void fstWriterEmitEnumTableRef(struct fstWriterContext *xc, fstEnumHandle handle) {
  if (handle) {
    fstWriterSetAttrBegin(xc, FST_AT_MISC, FST_MT_ENUMTABLE, NULL, handle);
  }
}

/*
 * value and time change emission
 */
void fstWriterEmitValueChange(struct fstWriterContext *xc, fstHandle handle, const void *val) {
  const unsigned char *buf = (const unsigned char *)val;
  uint32_t offs;
  int len;

  assert(handle <= xc->maxhandle);

  uint32_t fpos;
  uint32_t *vm4ip;

  if (FST_UNLIKELY(!xc->valpos_mem)) {
    xc->vc_emitted = 1;
    fstWriterCreateMmaps(xc);
  }

  handle--; /* move starting at 1 index to starting at 0 */
  vm4ip = &(xc->valpos_mem[4 * handle]);

  len = vm4ip[1];
  if (FST_LIKELY(len)) /* len of zero = variable length, use fstWriterEmitVariableLengthValueChange */
  {
    if (FST_LIKELY(!xc->is_initial_time)) {
      fpos = xc->vchg_siz;

      if (FST_UNLIKELY((fpos + len + 10) > xc->vchg_alloc_siz)) {
        xc->vchg_alloc_siz += (xc->fst_break_add_size +
                               len); /* +len added in the case of extremely long vectors and small break add sizes */
        xc->vchg_mem = (unsigned char *)realloc(xc->vchg_mem, xc->vchg_alloc_siz);
        if (FST_UNLIKELY(!xc->vchg_mem)) {
          fprintf(stderr, FST_APIMESS "Could not realloc() in fstWriterEmitValueChange, exiting.\n");
          exit(255);
        }
      }
#ifdef FST_REMOVE_DUPLICATE_VC
      offs = vm4ip[0];

      if (len != 1) {
        if ((vm4ip[3] == xc->tchn_idx) && (vm4ip[2])) {
          unsigned char *old_value = xc->vchg_mem + vm4ip[2] + 4; /* the +4 skips old vm4ip[2] value */
          while (*(old_value++) & 0x80) { /* skips over varint encoded "xc->tchn_idx - vm4ip[3]" */
          }
          memcpy(old_value, buf, len); /* overlay new value */

          memcpy(xc->curval_mem + offs, buf, len);
          return;
        } else {
          if (!memcmp(xc->curval_mem + offs, buf, len)) {
            if (!xc->curtime) {
              int i;
              for (i = 0; i < len; i++) {
                if (buf[i] != 'x')
                  break;
              }

              if (i < len)
                return;
            } else {
              return;
            }
          }
        }

        memcpy(xc->curval_mem + offs, buf, len);
      } else {
        if ((vm4ip[3] == xc->tchn_idx) && (vm4ip[2])) {
          unsigned char *old_value = xc->vchg_mem + vm4ip[2] + 4; /* the +4 skips old vm4ip[2] value */
          while (*(old_value++) & 0x80) { /* skips over varint encoded "xc->tchn_idx - vm4ip[3]" */
          }
          *old_value = *buf; /* overlay new value */

          *(xc->curval_mem + offs) = *buf;
          return;
        } else {
          if ((*(xc->curval_mem + offs)) == (*buf)) {
            if (!xc->curtime) {
              if (*buf != 'x')
                return;
            } else {
              return;
            }
          }
        }

        *(xc->curval_mem + offs) = *buf;
      }
#endif
      xc->vchg_siz +=
          fstWriterUint32WithVarint32(xc, &vm4ip[2], xc->tchn_idx - vm4ip[3], buf, len); /* do one fwrite op only */
      vm4ip[3] = xc->tchn_idx;
      vm4ip[2] = fpos;
    } else {
      offs = vm4ip[0];
      memcpy(xc->curval_mem + offs, buf, len);
    }
  }
}

void fstWriterEmitValueChange32(struct fstWriterContext *xc, fstHandle handle, uint32_t bits, uint32_t val) {
  char buf[32];
  char *s = buf;
  uint32_t i;
  for (i = 0; i < bits; ++i) {
    *s++ = '0' + ((val >> (bits - i - 1)) & 1);
  }
  fstWriterEmitValueChange(xc, handle, buf);
}
void fstWriterEmitValueChange64(struct fstWriterContext *xc, fstHandle handle, uint32_t bits, uint64_t val) {
  char buf[64];
  char *s = buf;
  uint32_t i;
  for (i = 0; i < bits; ++i) {
    *s++ = '0' + ((val >> (bits - i - 1)) & 1);
  }
  fstWriterEmitValueChange(xc, handle, buf);
}
void fstWriterEmitValueChangeVec32(struct fstWriterContext *xc, fstHandle handle, uint32_t bits, const uint32_t *val) {
  if (FST_UNLIKELY(bits <= 32)) {
    fstWriterEmitValueChange32(xc, handle, bits, val[0]);
    return;
  }

  int bq = bits / 32;
  int br = bits & 31;
  int i;
  int w;
  uint32_t v;
  unsigned char *s;
  if (FST_UNLIKELY(bits > xc->outval_alloc_siz)) {
    xc->outval_alloc_siz = bits * 2 + 1;
    xc->outval_mem = (unsigned char *)realloc(xc->outval_mem, xc->outval_alloc_siz);
    if (FST_UNLIKELY(!xc->outval_mem)) {
      fprintf(stderr, FST_APIMESS "Could not realloc() in fstWriterEmitValueChangeVec32, exiting.\n");
      exit(255);
    }
  }
  s = xc->outval_mem;
  {
    w = bq;
    v = val[w];
    for (i = 0; i < br; ++i) {
      *s++ = '0' + ((v >> (br - i - 1)) & 1);
    }
  }
  for (w = bq - 1; w >= 0; --w) {
    v = val[w];
    for (i = (32 - 4); i >= 0; i -= 4) {
      s[0] = '0' + ((v >> (i + 3)) & 1);
      s[1] = '0' + ((v >> (i + 2)) & 1);
      s[2] = '0' + ((v >> (i + 1)) & 1);
      s[3] = '0' + ((v >> (i + 0)) & 1);
      s += 4;
    }
  }
  fstWriterEmitValueChange(xc, handle, xc->outval_mem);
}
void fstWriterEmitValueChangeVec64(struct fstWriterContext *xc, fstHandle handle, uint32_t bits, const uint64_t *val) {
  if (FST_UNLIKELY(bits <= 64)) {
    fstWriterEmitValueChange64(xc, handle, bits, val[0]);
    return;
  }

  int bq = bits / 64;
  int br = bits & 63;
  int i;
  int w;
  uint32_t v;
  unsigned char *s;
  if (FST_UNLIKELY(bits > xc->outval_alloc_siz)) {
    xc->outval_alloc_siz = bits * 2 + 1;
    xc->outval_mem = (unsigned char *)realloc(xc->outval_mem, xc->outval_alloc_siz);
    if (FST_UNLIKELY(!xc->outval_mem)) {
      fprintf(stderr, FST_APIMESS "Could not realloc() in fstWriterEmitValueChangeVec64, exiting.\n");
      exit(255);
    }
  }
  s = xc->outval_mem;
  {
    w = bq;
    v = val[w];
    for (i = 0; i < br; ++i) {
      *s++ = '0' + ((v >> (br - i - 1)) & 1);
    }
  }
  for (w = bq - 1; w >= 0; --w) {
    v = val[w];
    for (i = (64 - 4); i >= 0; i -= 4) {
      s[0] = '0' + ((v >> (i + 3)) & 1);
      s[1] = '0' + ((v >> (i + 2)) & 1);
      s[2] = '0' + ((v >> (i + 1)) & 1);
      s[3] = '0' + ((v >> (i + 0)) & 1);
      s += 4;
    }
  }
  fstWriterEmitValueChange(xc, handle, xc->outval_mem);
}

void fstWriterEmitVariableLengthValueChange(struct fstWriterContext *xc, fstHandle handle, const void *val,
                                            uint32_t len) {
  const unsigned char *buf = (const unsigned char *)val;

  assert(handle <= xc->maxhandle);

  uint32_t fpos;
  uint32_t *vm4ip;

  if (FST_UNLIKELY(!xc->valpos_mem)) {
    xc->vc_emitted = 1;
    fstWriterCreateMmaps(xc);
  }

  handle--; /* move starting at 1 index to starting at 0 */
  vm4ip = &(xc->valpos_mem[4 * handle]);

  /* there is no initial time dump for variable length value changes */
  if (FST_LIKELY(!vm4ip[1])) /* len of zero = variable length */
  {
    fpos = xc->vchg_siz;

    if (FST_UNLIKELY((fpos + len + 10 + 5) > xc->vchg_alloc_siz)) {
      xc->vchg_alloc_siz += (xc->fst_break_add_size + len +
                             5); /* +len added in the case of extremely long vectors and small break add sizes */
      xc->vchg_mem = (unsigned char *)realloc(xc->vchg_mem, xc->vchg_alloc_siz);
      if (FST_UNLIKELY(!xc->vchg_mem)) {
        fprintf(stderr, FST_APIMESS "Could not realloc() in fstWriterEmitVariableLengthValueChange, exiting.\n");
        exit(255);
      }
    }

    xc->vchg_siz += fstWriterUint32WithVarint32AndLength(xc, &vm4ip[2], xc->tchn_idx - vm4ip[3], buf,
                                                         len); /* do one fwrite op only */
    vm4ip[3] = xc->tchn_idx;
    vm4ip[2] = fpos;
  }
}

void fstWriterEmitTimeChange(struct fstWriterContext *xc, uint64_t tim) {
  unsigned int i;
  int skip = 0;

  if (FST_UNLIKELY(xc->is_initial_time)) {
    if (xc->size_limit_locked) /* this resets xc->is_initial_time to one */
    {
      return;
    }

    if (!xc->valpos_mem) {
      fstWriterCreateMmaps(xc);
    }

    skip = 1;

    xc->firsttime = (xc->vc_emitted) ? 0 : tim;
    xc->curtime = 0;
    xc->vchg_mem[0] = '!';
    xc->vchg_siz = 1;
    fstWriterEmitSectionHeader(xc);
    for (i = 0; i < xc->maxhandle; i++) {
      xc->valpos_mem[4 * i + 2] = 0; /* zero out offset val */
      xc->valpos_mem[4 * i + 3] = 0; /* zero out last time change val */
    }
    xc->is_initial_time = 0;
  } else {
    if ((xc->vchg_siz >= xc->fst_break_size) || (xc->flush_context_pending)) {
      xc->flush_context_pending = 0;
      fstWriterFlushContextPrivate(xc);
      xc->tchn_cnt++;
      fstWriterVarint(xc->tchn_handle, xc->curtime);
    }
  }

  if (!skip) {
    xc->tchn_idx++;
  }
  fstWriterVarint(xc->tchn_handle, tim - xc->curtime);
  xc->tchn_cnt++;
  xc->curtime = tim;
}

void fstWriterEmitDumpActive(struct fstWriterContext *xc, int enable) {
  struct fstBlackoutChain *b = (struct fstBlackoutChain *)calloc(1, sizeof(struct fstBlackoutChain));

  b->tim = xc->curtime;
  b->active = (enable != 0);

  xc->num_blackouts++;
  if (xc->blackout_curr) {
    xc->blackout_curr->next = b;
    xc->blackout_curr = b;
  } else {
    xc->blackout_head = b;
    xc->blackout_curr = b;
  }
}
