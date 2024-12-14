#include "fstapi_internal.h"

static uint64_t fstReaderUint64(FILE *f) {
  uint64_t val = 0;
  unsigned char buf[sizeof(uint64_t)];
  unsigned int i;

  fstFread(buf, sizeof(uint64_t), 1, f);
  for (i = 0; i < sizeof(uint64_t); i++) {
    val <<= 8;
    val |= buf[i];
  }

  return (val);
}

static uint32_t fstReaderVarint32(FILE *f) {
  const int chk_len_max = 5; /* TALOS-2023-1783 */
  int chk_len = chk_len_max;
  unsigned char buf[chk_len_max];
  unsigned char *mem = buf;
  uint32_t rc = 0;
  int ch;

  do {
    ch = fgetc(f);
    *(mem++) = ch;
  } while ((ch & 0x80) && (--chk_len));

  if (ch & 0x80)
    chk_report_abort("TALOS-2023-1783");
  mem--;

  for (;;) {
    rc <<= 7;
    rc |= (uint32_t)(*mem & 0x7f);
    if (mem == buf) {
      break;
    }
    mem--;
  }

  return (rc);
}

static uint32_t fstReaderVarint32WithSkip(FILE *f, uint32_t *skiplen) {
  const int chk_len_max = 5; /* TALOS-2023-1783 */
  int chk_len = chk_len_max;
  unsigned char buf[chk_len_max];
  unsigned char *mem = buf;
  uint32_t rc = 0;
  int ch;

  do {
    ch = fgetc(f);
    *(mem++) = ch;
  } while ((ch & 0x80) && (--chk_len));

  if (ch & 0x80)
    chk_report_abort("TALOS-2023-1783");
  *skiplen = mem - buf;
  mem--;

  for (;;) {
    rc <<= 7;
    rc |= (uint32_t)(*mem & 0x7f);
    if (mem == buf) {
      break;
    }
    mem--;
  }

  return (rc);
}

static uint64_t fstReaderVarint64(FILE *f) {
  const int chk_len_max = 16; /* TALOS-2023-1783 */
  int chk_len = chk_len_max;
  unsigned char buf[chk_len_max];
  unsigned char *mem = buf;
  uint64_t rc = 0;
  int ch;

  do {
    ch = fgetc(f);
    *(mem++) = ch;
  } while ((ch & 0x80) && (--chk_len));

  if (ch & 0x80)
    chk_report_abort("TALOS-2023-1783");
  mem--;

  for (;;) {
    rc <<= 7;
    rc |= (uint64_t)(*mem & 0x7f);
    if (mem == buf) {
      break;
    }
    mem--;
  }

  return (rc);
}

/***********************/
/***                 ***/
/*** reader function ***/
/***                 ***/
/***********************/

/*
 * private structs
 */
static const char *vartypes[] = {
    "event", "integer",  "parameter", "real",    "real_parameter", "reg",       "supply0", "supply1",
    "time",  "tri",      "triand",    "trior",   "trireg",         "tri0",      "tri1",    "wand",
    "wire",  "wor",      "port",      "sparray", "realtime",       "string",    "bit",     "logic",
    "int",   "shortint", "longint",   "byte",    "enum",           "shortreal",
};

static const char *modtypes[] = {
    "module",
    "task",
    "function",
    "begin",
    "fork",
    "generate",
    "struct",
    "union",
    "class",
    "interface",
    "package",
    "program",
    "vhdl_architecture",
    "vhdl_procedure",
    "vhdl_function",
    "vhdl_record",
    "vhdl_process",
    "vhdl_block",
    "vhdl_for_generate",
    "vhdl_if_generate",
    "vhdl_generate",
    "vhdl_package",
};

static const char *attrtypes[] = {"misc", "array", "enum", "class"};

static const char *arraytypes[] = {"none", "unpacked", "packed", "sparse"};

static const char *enumvaluetypes[] = {
    "integer",
    "bit",
    "logic",
    "int",
    "shortint",
    "longint",
    "byte",
    "unsigned_integer",
    "unsigned_bit",
    "unsigned_logic",
    "unsigned_int",
    "unsigned_shortint",
    "unsigned_longint",
    "unsigned_byte",
};

static const char *packtypes[] = {"none", "unpacked", "packed", "tagged_packed"};

struct fstCurrHier {
  struct fstCurrHier *prev;
  void *user_info;
  int len;
};

struct fstReaderContext {
  /* common entries */

  FILE *f, *fh;

  uint64_t start_time, end_time;
  uint64_t mem_used_by_writer;
  uint64_t scope_count;
  uint64_t var_count;
  fstHandle maxhandle;
  uint64_t num_alias;
  uint64_t vc_section_count;

  uint32_t *signal_lens;                /* maxhandle sized */
  unsigned char *signal_typs;           /* maxhandle sized */
  unsigned char *process_mask;          /* maxhandle-based, bitwise sized */
  uint32_t longest_signal_value_len;    /* longest len value encountered */
  unsigned char *temp_signal_value_buf; /* malloced for len in longest_signal_value_len */

  signed char timescale;
  unsigned char filetype;

  unsigned use_vcd_extensions : 1;
  unsigned double_endian_match : 1;
  unsigned native_doubles_for_cb : 1;
  unsigned contains_geom_section : 1;
  unsigned contains_hier_section : 1;        /* valid for hier_pos */
  unsigned contains_hier_section_lz4duo : 1; /* valid for hier_pos (contains_hier_section_lz4 always also set) */
  unsigned contains_hier_section_lz4 : 1;    /* valid for hier_pos */
  unsigned limit_range_valid : 1;            /* valid for limit_range_start, limit_range_end */

  char version[FST_HDR_SIM_VERSION_SIZE + 1];
  char date[FST_HDR_DATE_SIZE + 1];
  int64_t timezero;

  char *filename, *filename_unpacked;
  fst_off_t hier_pos;

  uint32_t num_blackouts;
  uint64_t *blackout_times;
  unsigned char *blackout_activity;

  uint64_t limit_range_start, limit_range_end;

  /* entries specific to read value at time functions */

  unsigned rvat_data_valid : 1;
  uint64_t *rvat_time_table;
  uint64_t rvat_beg_tim, rvat_end_tim;
  unsigned char *rvat_frame_data;
  uint64_t rvat_frame_maxhandle;
  fst_off_t *rvat_chain_table;
  uint32_t *rvat_chain_table_lengths;
  uint64_t rvat_vc_maxhandle;
  fst_off_t rvat_vc_start;
  uint32_t *rvat_sig_offs;
  int rvat_packtype;

  uint32_t rvat_chain_len;
  unsigned char *rvat_chain_mem;
  fstHandle rvat_chain_facidx;

  uint32_t rvat_chain_pos_tidx;
  uint32_t rvat_chain_pos_idx;
  uint64_t rvat_chain_pos_time;
  unsigned rvat_chain_pos_valid : 1;

  /* entries specific to hierarchy traversal */

  struct fstHier hier;
  struct fstCurrHier *curr_hier;
  fstHandle current_handle;
  char *curr_flat_hier_nam;
  int flat_hier_alloc_len;
  unsigned do_rewind : 1;
  char str_scope_nam[FST_ID_NAM_SIZ + 1];
  char str_scope_comp[FST_ID_NAM_SIZ + 1];
  char *str_scope_attr;

  unsigned fseek_failed : 1;

  /* self-buffered I/O for writes */

#ifndef FST_WRITEX_DISABLE
  int writex_pos;
  int writex_fd;
  unsigned char writex_buf[FST_WRITEX_MAX];
#endif

  char *f_nam;
  char *fh_nam;
};

int fstReaderFseeko(struct fstReaderContext *xc, FILE *stream, fst_off_t offset, int whence) {
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

#ifndef FST_WRITEX_DISABLE
static void fstWritex(struct fstReaderContext *xc, void *v, uint32_t len) /* TALOS-2023-1793: change len to unsigned */
{
  unsigned char *s = (unsigned char *)v;

  if (len) {
    if (len < FST_WRITEX_MAX) {
      if (xc->writex_pos + len >= FST_WRITEX_MAX) {
        fstWritex(xc, NULL, 0);
      }

      memcpy(xc->writex_buf + xc->writex_pos, s, len);
      xc->writex_pos += len;
    } else {
      fstWritex(xc, NULL, 0);
      if (write(xc->writex_fd, s, len)) {
      };
    }
  } else {
    if (xc->writex_pos) {
      if (write(xc->writex_fd, xc->writex_buf, xc->writex_pos)) {
      };
      xc->writex_pos = 0;
    }
  }
}
#endif

/*
 * scope -> flat name handling
 */
static void fstReaderDeallocateScopeData(struct fstReaderContext *xc) {
  struct fstCurrHier *chp;

  free(xc->curr_flat_hier_nam);
  xc->curr_flat_hier_nam = NULL;
  while (xc->curr_hier) {
    chp = xc->curr_hier->prev;
    free(xc->curr_hier);
    xc->curr_hier = chp;
  }
}

const char *fstReaderGetCurrentFlatScope(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  if (xc) {
    return (xc->curr_flat_hier_nam ? xc->curr_flat_hier_nam : "");
  } else {
    return (NULL);
  }
}

void *fstReaderGetCurrentScopeUserInfo(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  if (xc) {
    return (xc->curr_hier ? xc->curr_hier->user_info : NULL);
  } else {
    return (NULL);
  }
}

const char *fstReaderPopScope(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  if (xc && xc->curr_hier) {
    struct fstCurrHier *ch = xc->curr_hier;
    if (xc->curr_hier->prev) {
      xc->curr_flat_hier_nam[xc->curr_hier->prev->len] = 0;
    } else {
      *xc->curr_flat_hier_nam = 0;
    }
    xc->curr_hier = xc->curr_hier->prev;
    free(ch);
    return (xc->curr_flat_hier_nam ? xc->curr_flat_hier_nam : "");
  }

  return (NULL);
}

void fstReaderResetScope(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc) {
    while (fstReaderPopScope(xc))
      ; /* remove any already-built scoping info */
  }
}

const char *fstReaderPushScope(void *ctx, const char *nam, void *user_info) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  if (xc) {
    struct fstCurrHier *ch = (struct fstCurrHier *)malloc(sizeof(struct fstCurrHier));
    int chl = xc->curr_hier ? xc->curr_hier->len : 0;
    int len = chl + 1 + strlen(nam);
    if (len >= xc->flat_hier_alloc_len) {
      xc->curr_flat_hier_nam =
          xc->curr_flat_hier_nam ? (char *)realloc(xc->curr_flat_hier_nam, len + 1) : (char *)malloc(len + 1);
    }

    if (chl) {
      xc->curr_flat_hier_nam[chl] = '.';
      strcpy(xc->curr_flat_hier_nam + chl + 1, nam);
    } else {
      strcpy(xc->curr_flat_hier_nam, nam);
      len--;
    }

    ch->len = len;
    ch->prev = xc->curr_hier;
    ch->user_info = user_info;
    xc->curr_hier = ch;
    return (xc->curr_flat_hier_nam);
  }

  return (NULL);
}

int fstReaderGetCurrentScopeLen(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc && xc->curr_hier) {
    return (xc->curr_hier->len);
  }

  return (0);
}

int fstReaderGetFseekFailed(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  if (xc) {
    return (xc->fseek_failed != 0);
  }

  return (0);
}

/*
 * iter mask manipulation util functions
 */
int fstReaderGetFacProcessMask(void *ctx, fstHandle facidx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc) {
    facidx--;
    if (facidx < xc->maxhandle) {
      int process_idx = facidx / 8;
      int process_bit = facidx & 7;

      return ((xc->process_mask[process_idx] & (1 << process_bit)) != 0);
    }
  }
  return (0);
}

void fstReaderSetFacProcessMask(void *ctx, fstHandle facidx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc) {
    facidx--;
    if (facidx < xc->maxhandle) {
      int idx = facidx / 8;
      int bitpos = facidx & 7;

      xc->process_mask[idx] |= (1 << bitpos);
    }
  }
}

void fstReaderClrFacProcessMask(void *ctx, fstHandle facidx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc) {
    facidx--;
    if (facidx < xc->maxhandle) {
      int idx = facidx / 8;
      int bitpos = facidx & 7;

      xc->process_mask[idx] &= (~(1 << bitpos));
    }
  }
}

void fstReaderSetFacProcessMaskAll(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc) {
    memset(xc->process_mask, 0xff, (xc->maxhandle + 7) / 8);
  }
}

void fstReaderClrFacProcessMaskAll(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc) {
    memset(xc->process_mask, 0x00, (xc->maxhandle + 7) / 8);
  }
}

/*
 * various utility read/write functions
 */
signed char fstReaderGetTimescale(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->timescale : 0);
}

uint64_t fstReaderGetStartTime(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->start_time : 0);
}

uint64_t fstReaderGetEndTime(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->end_time : 0);
}

uint64_t fstReaderGetMemoryUsedByWriter(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->mem_used_by_writer : 0);
}

uint64_t fstReaderGetScopeCount(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->scope_count : 0);
}

uint64_t fstReaderGetVarCount(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->var_count : 0);
}

fstHandle fstReaderGetMaxHandle(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->maxhandle : 0);
}

uint64_t fstReaderGetAliasCount(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->num_alias : 0);
}

uint64_t fstReaderGetValueChangeSectionCount(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->vc_section_count : 0);
}

int fstReaderGetDoubleEndianMatchState(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->double_endian_match : 0);
}

const char *fstReaderGetVersionString(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->version : NULL);
}

const char *fstReaderGetDateString(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->date : NULL);
}

int fstReaderGetFileType(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? (int)xc->filetype : (int)FST_FT_VERILOG);
}

int64_t fstReaderGetTimezero(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->timezero : 0);
}

uint32_t fstReaderGetNumberDumpActivityChanges(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  return (xc ? xc->num_blackouts : 0);
}

uint64_t fstReaderGetDumpActivityChangeTime(void *ctx, uint32_t idx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc && (idx < xc->num_blackouts) && (xc->blackout_times)) {
    return (xc->blackout_times[idx]);
  } else {
    return (0);
  }
}

unsigned char fstReaderGetDumpActivityChangeValue(void *ctx, uint32_t idx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc && (idx < xc->num_blackouts) && (xc->blackout_activity)) {
    return (xc->blackout_activity[idx]);
  } else {
    return (0);
  }
}

void fstReaderSetLimitTimeRange(void *ctx, uint64_t start_time, uint64_t end_time) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc) {
    xc->limit_range_valid = 1;
    xc->limit_range_start = start_time;
    xc->limit_range_end = end_time;
  }
}

void fstReaderSetUnlimitedTimeRange(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc) {
    xc->limit_range_valid = 0;
  }
}

void fstReaderSetVcdExtensions(void *ctx, int enable) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc) {
    xc->use_vcd_extensions = (enable != 0);
  }
}

void fstReaderIterBlocksSetNativeDoublesOnCallback(void *ctx, int enable) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  if (xc) {
    xc->native_doubles_for_cb = (enable != 0);
  }
}

/*
 * hierarchy processing
 */
static void fstVcdID(char *buf, unsigned int value) {
  char *pnt = buf;

  /* zero is illegal for a value...it is assumed they start at one */
  while (value) {
    value--;
    *(pnt++) = (char)('!' + value % 94);
    value = value / 94;
  }

  *pnt = 0;
}

static int fstVcdIDForFwrite(char *buf, unsigned int value) {
  char *pnt = buf;

  /* zero is illegal for a value...it is assumed they start at one */
  while (value) {
    value--;
    *(pnt++) = (char)('!' + value % 94);
    value = value / 94;
  }

  return (pnt - buf);
}

static int fstReaderRecreateHierFile(struct fstReaderContext *xc) {
  int pass_status = 1;

  if (!xc->fh) {
    fst_off_t offs_cache = ftello(xc->f);
    int fnam_len = strlen(xc->filename) + 6 + 16 + 32 + 1;
    char *fnam = (char *)malloc(fnam_len);
    unsigned char *mem = (unsigned char *)malloc(FST_GZIO_LEN);
    fst_off_t hl, uclen;
    fst_off_t clen = 0;
    gzFile zhandle = NULL;
    int zfd;
    int htyp = FST_BL_SKIP;

    /* can't handle both set at once should never happen in a real file */
    if (!xc->contains_hier_section_lz4 && xc->contains_hier_section) {
      htyp = FST_BL_HIER;
    } else if (xc->contains_hier_section_lz4 && !xc->contains_hier_section) {
      htyp = xc->contains_hier_section_lz4duo ? FST_BL_HIER_LZ4DUO : FST_BL_HIER_LZ4;
    }

    snprintf(fnam, fnam_len, "%s.hier_%d_%p", xc->filename, getpid(), (void *)xc);
    fstReaderFseeko(xc, xc->f, xc->hier_pos, SEEK_SET);
    uclen = fstReaderUint64(xc->f);
#ifndef __MINGW32__
    fflush(xc->f);
#endif
    if (htyp == FST_BL_HIER) {
      fstReaderFseeko(xc, xc->f, xc->hier_pos, SEEK_SET);
      uclen = fstReaderUint64(xc->f);
#ifndef __MINGW32__
      fflush(xc->f);
#endif
      zfd = dup(fileno(xc->f));
      zhandle = gzdopen(zfd, "rb");
      if (!zhandle) {
        close(zfd);
        free(mem);
        free(fnam);
        return (0);
      }
    } else if ((htyp == FST_BL_HIER_LZ4) || (htyp == FST_BL_HIER_LZ4DUO)) {
      fstReaderFseeko(xc, xc->f, xc->hier_pos - 8, SEEK_SET); /* get section len */
      clen = fstReaderUint64(xc->f) - 16;
      uclen = fstReaderUint64(xc->f);
#ifndef __MINGW32__
      fflush(xc->f);
#endif
    }

#ifndef __MINGW32__
    xc->fh = fopen(fnam, "w+b");
    if (!xc->fh)
#endif
    {
      xc->fh = tmpfile_open(&xc->fh_nam);
      free(fnam);
      fnam = NULL;
      if (!xc->fh) {
        tmpfile_close(&xc->fh, &xc->fh_nam);
        free(mem);
        return (0);
      }
    }

#ifndef __MINGW32__
    if (fnam)
      unlink(fnam);
#endif

    if (htyp == FST_BL_HIER) {
      for (hl = 0; hl < uclen; hl += FST_GZIO_LEN) {
        size_t len = ((uclen - hl) > FST_GZIO_LEN) ? FST_GZIO_LEN : (uclen - hl);
        size_t gzreadlen = gzread(zhandle, mem, len); /* rc should equal len... */
        size_t fwlen;

        if (gzreadlen != len) {
          pass_status = 0;
          break;
        }

        fwlen = fstFwrite(mem, len, 1, xc->fh);
        if (fwlen != 1) {
          pass_status = 0;
          break;
        }
      }
      gzclose(zhandle);
    } else if (htyp == FST_BL_HIER_LZ4DUO) {
      unsigned char *lz4_cmem = (unsigned char *)malloc(clen);
      unsigned char *lz4_ucmem = (unsigned char *)malloc(uclen);
      unsigned char *lz4_ucmem2;
      uint64_t uclen2;
      int skiplen2 = 0;

      fstFread(lz4_cmem, clen, 1, xc->f);

      uclen2 = fstGetVarint64(lz4_cmem, &skiplen2);
      lz4_ucmem2 = (unsigned char *)malloc(uclen2);
      pass_status = (uclen2 == (uint64_t)LZ4_decompress_safe_partial((char *)lz4_cmem + skiplen2, (char *)lz4_ucmem2,
                                                                     clen - skiplen2, uclen2, uclen2));
      if (pass_status) {
        pass_status =
            (uclen == LZ4_decompress_safe_partial((char *)lz4_ucmem2, (char *)lz4_ucmem, uclen2, uclen, uclen));

        if (fstFwrite(lz4_ucmem, uclen, 1, xc->fh) != 1) {
          pass_status = 0;
        }
      }

      free(lz4_ucmem2);
      free(lz4_ucmem);
      free(lz4_cmem);
    } else if (htyp == FST_BL_HIER_LZ4) {
      unsigned char *lz4_cmem = (unsigned char *)malloc(clen);
      unsigned char *lz4_ucmem = (unsigned char *)malloc(uclen);

      fstFread(lz4_cmem, clen, 1, xc->f);
      pass_status = (uclen == LZ4_decompress_safe_partial((char *)lz4_cmem, (char *)lz4_ucmem, clen, uclen, uclen));

      if (fstFwrite(lz4_ucmem, uclen, 1, xc->fh) != 1) {
        pass_status = 0;
      }

      free(lz4_ucmem);
      free(lz4_cmem);
    } else /* FST_BL_SKIP */
    {
      pass_status = 0;
      if (xc->fh) {
        fclose(xc->fh);
        xc->fh = NULL; /* needed in case .hier file is missing and there are no hier sections */
      }
    }

    free(mem);
    free(fnam);

    fstReaderFseeko(xc, xc->f, offs_cache, SEEK_SET);
  }

  return (pass_status);
}

int fstReaderIterateHierRewind(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  int pass_status = 0;

  if (xc) {
    pass_status = 1;
    if (!xc->fh) {
      pass_status = fstReaderRecreateHierFile(xc);
    }

    xc->do_rewind = 1;
  }

  return (pass_status);
}

struct fstHier *fstReaderIterateHier(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  int isfeof;
  fstHandle alias;
  char *pnt;
  int ch;

  if (!xc)
    return (NULL);

  if (!xc->fh) {
    if (!fstReaderRecreateHierFile(xc)) {
      return (NULL);
    }
  }

  if (xc->do_rewind) {
    xc->do_rewind = 0;
    xc->current_handle = 0;
    fstReaderFseeko(xc, xc->fh, 0, SEEK_SET);
    clearerr(xc->fh);
  }

  if (!(isfeof = feof(xc->fh))) {
    int tag = fgetc(xc->fh);
    int cl;
    switch (tag) {
    case FST_ST_VCD_SCOPE:
      xc->hier.htyp = FST_HT_SCOPE;
      xc->hier.u.scope.typ = fgetc(xc->fh);
      xc->hier.u.scope.name = pnt = xc->str_scope_nam;
      cl = 0;
      while ((ch = fgetc(xc->fh))) {
        if (cl < FST_ID_NAM_SIZ) {
          pnt[cl++] = ch;
        }
      }; /* scopename */
      pnt[cl] = 0;
      xc->hier.u.scope.name_length = cl;

      xc->hier.u.scope.component = pnt = xc->str_scope_comp;
      cl = 0;
      while ((ch = fgetc(xc->fh))) {
        if (cl < FST_ID_NAM_SIZ) {
          pnt[cl++] = ch;
        }
      }; /* scopecomp */
      pnt[cl] = 0;
      xc->hier.u.scope.component_length = cl;
      break;

    case FST_ST_VCD_UPSCOPE:
      xc->hier.htyp = FST_HT_UPSCOPE;
      break;

    case FST_ST_GEN_ATTRBEGIN:
      xc->hier.htyp = FST_HT_ATTRBEGIN;
      xc->hier.u.attr.typ = fgetc(xc->fh);
      xc->hier.u.attr.subtype = fgetc(xc->fh);
      if (!xc->str_scope_attr) {
        xc->str_scope_attr = (char *)calloc(1, FST_ID_NAM_ATTR_SIZ + 1);
      }
      xc->hier.u.attr.name = pnt = xc->str_scope_attr;
      cl = 0;
      while ((ch = fgetc(xc->fh))) {
        if (cl < FST_ID_NAM_ATTR_SIZ) {
          pnt[cl++] = ch;
        }
      }; /* attrname */
      pnt[cl] = 0;
      xc->hier.u.attr.name_length = cl;

      xc->hier.u.attr.arg = fstReaderVarint64(xc->fh);

      if (xc->hier.u.attr.typ == FST_AT_MISC) {
        if ((xc->hier.u.attr.subtype == FST_MT_SOURCESTEM) || (xc->hier.u.attr.subtype == FST_MT_SOURCEISTEM)) {
          int sidx_skiplen_dummy = 0;
          xc->hier.u.attr.arg_from_name = fstGetVarint64((unsigned char *)xc->str_scope_attr, &sidx_skiplen_dummy);
        }
      }
      break;

    case FST_ST_GEN_ATTREND:
      xc->hier.htyp = FST_HT_ATTREND;
      break;

    case FST_VT_VCD_EVENT:
    case FST_VT_VCD_INTEGER:
    case FST_VT_VCD_PARAMETER:
    case FST_VT_VCD_REAL:
    case FST_VT_VCD_REAL_PARAMETER:
    case FST_VT_VCD_REG:
    case FST_VT_VCD_SUPPLY0:
    case FST_VT_VCD_SUPPLY1:
    case FST_VT_VCD_TIME:
    case FST_VT_VCD_TRI:
    case FST_VT_VCD_TRIAND:
    case FST_VT_VCD_TRIOR:
    case FST_VT_VCD_TRIREG:
    case FST_VT_VCD_TRI0:
    case FST_VT_VCD_TRI1:
    case FST_VT_VCD_WAND:
    case FST_VT_VCD_WIRE:
    case FST_VT_VCD_WOR:
    case FST_VT_VCD_PORT:
    case FST_VT_VCD_SPARRAY:
    case FST_VT_VCD_REALTIME:
    case FST_VT_GEN_STRING:
    case FST_VT_SV_BIT:
    case FST_VT_SV_LOGIC:
    case FST_VT_SV_INT:
    case FST_VT_SV_SHORTINT:
    case FST_VT_SV_LONGINT:
    case FST_VT_SV_BYTE:
    case FST_VT_SV_ENUM:
    case FST_VT_SV_SHORTREAL:
      xc->hier.htyp = FST_HT_VAR;
      xc->hier.u.var.svt_workspace = FST_SVT_NONE;
      xc->hier.u.var.sdt_workspace = FST_SDT_NONE;
      xc->hier.u.var.sxt_workspace = 0;
      xc->hier.u.var.typ = tag;
      xc->hier.u.var.direction = fgetc(xc->fh);
      xc->hier.u.var.name = pnt = xc->str_scope_nam;
      cl = 0;
      while ((ch = fgetc(xc->fh))) {
        if (cl < FST_ID_NAM_SIZ) {
          pnt[cl++] = ch;
        }
      }; /* varname */
      pnt[cl] = 0;
      xc->hier.u.var.name_length = cl;
      xc->hier.u.var.length = fstReaderVarint32(xc->fh);
      if (tag == FST_VT_VCD_PORT) {
        xc->hier.u.var.length -= 2; /* removal of delimiting spaces */
        xc->hier.u.var.length /= 3; /* port -> signal size adjust */
      }

      alias = fstReaderVarint32(xc->fh);

      if (!alias) {
        xc->current_handle++;
        xc->hier.u.var.handle = xc->current_handle;
        xc->hier.u.var.is_alias = 0;
      } else {
        xc->hier.u.var.handle = alias;
        xc->hier.u.var.is_alias = 1;
      }

      break;

    default:
      isfeof = 1;
      break;
    }
  }

  return (!isfeof ? &xc->hier : NULL);
}

int fstReaderProcessHier(void *ctx, FILE *fv) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  char *str;
  char *pnt;
  int ch, scopetype;
  int vartype;
  uint32_t len, alias;
  /* uint32_t maxvalpos=0; */
  unsigned int num_signal_dyn = 65536;
  int attrtype, subtype;
  uint64_t attrarg;
  fstHandle maxhandle_scanbuild;
  int cl;

  if (!xc)
    return (0);

  xc->longest_signal_value_len = 32; /* arbitrarily set at 32...this is much longer than an expanded double */

  if (!xc->fh) {
    if (!fstReaderRecreateHierFile(xc)) {
      return (0);
    }
  }

  str = (char *)malloc(FST_ID_NAM_ATTR_SIZ + 1);

  if (fv) {
    char time_dimension[2] = {0, 0};
    int time_scale = 1;

    fprintf(fv, "$date\n\t%s\n$end\n", xc->date);
    fprintf(fv, "$version\n\t%s\n$end\n", xc->version);
    if (xc->timezero)
      fprintf(fv, "$timezero\n\t%" PRId64 "\n$end\n", xc->timezero);

    switch (xc->timescale) {
    case 2:
      time_scale = 100;
      time_dimension[0] = 0;
      break;
    case 1:
      time_scale = 10; /* fallthrough */
    case 0:
      time_dimension[0] = 0;
      break;

    case -1:
      time_scale = 100;
      time_dimension[0] = 'm';
      break;
    case -2:
      time_scale = 10; /* fallthrough */
    case -3:
      time_dimension[0] = 'm';
      break;

    case -4:
      time_scale = 100;
      time_dimension[0] = 'u';
      break;
    case -5:
      time_scale = 10; /* fallthrough */
    case -6:
      time_dimension[0] = 'u';
      break;

    case -10:
      time_scale = 100;
      time_dimension[0] = 'p';
      break;
    case -11:
      time_scale = 10; /* fallthrough */
    case -12:
      time_dimension[0] = 'p';
      break;

    case -13:
      time_scale = 100;
      time_dimension[0] = 'f';
      break;
    case -14:
      time_scale = 10; /* fallthrough */
    case -15:
      time_dimension[0] = 'f';
      break;

    case -16:
      time_scale = 100;
      time_dimension[0] = 'a';
      break;
    case -17:
      time_scale = 10; /* fallthrough */
    case -18:
      time_dimension[0] = 'a';
      break;

    case -19:
      time_scale = 100;
      time_dimension[0] = 'z';
      break;
    case -20:
      time_scale = 10; /* fallthrough */
    case -21:
      time_dimension[0] = 'z';
      break;

    case -7:
      time_scale = 100;
      time_dimension[0] = 'n';
      break;
    case -8:
      time_scale = 10; /* fallthrough */
    case -9:
    default:
      time_dimension[0] = 'n';
      break;
    }

    if (fv)
      fprintf(fv, "$timescale\n\t%d%ss\n$end\n", time_scale, time_dimension);
  }

  xc->maxhandle = 0;
  xc->num_alias = 0;

  free(xc->signal_lens);
  xc->signal_lens = (uint32_t *)malloc(num_signal_dyn * sizeof(uint32_t));

  free(xc->signal_typs);
  xc->signal_typs = (unsigned char *)malloc(num_signal_dyn * sizeof(unsigned char));

  fstReaderFseeko(xc, xc->fh, 0, SEEK_SET);
  while (!feof(xc->fh)) {
    int tag = fgetc(xc->fh);
    switch (tag) {
    case FST_ST_VCD_SCOPE:
      scopetype = fgetc(xc->fh);
      if ((scopetype < FST_ST_MIN) || (scopetype > FST_ST_MAX))
        scopetype = FST_ST_VCD_MODULE;
      pnt = str;
      cl = 0;
      while ((ch = fgetc(xc->fh))) {
        if (cl < FST_ID_NAM_ATTR_SIZ) {
          pnt[cl++] = ch;
        }
      }; /* scopename */
      pnt[cl] = 0;
      while (fgetc(xc->fh)) {
      }; /* scopecomp */

      if (fv)
        fprintf(fv, "$scope %s %s $end\n", modtypes[scopetype], str);
      break;

    case FST_ST_VCD_UPSCOPE:
      if (fv)
        fprintf(fv, "$upscope $end\n");
      break;

    case FST_ST_GEN_ATTRBEGIN:
      attrtype = fgetc(xc->fh);
      subtype = fgetc(xc->fh);
      pnt = str;
      cl = 0;
      while ((ch = fgetc(xc->fh))) {
        if (cl < FST_ID_NAM_ATTR_SIZ) {
          pnt[cl++] = ch;
        }
      }; /* attrname */
      pnt[cl] = 0;

      if (!str[0]) {
        strcpy(str, "\"\"");
      }

      attrarg = fstReaderVarint64(xc->fh);

      if (fv && xc->use_vcd_extensions) {
        switch (attrtype) {
        case FST_AT_ARRAY:
          if ((subtype < FST_AR_NONE) || (subtype > FST_AR_MAX))
            subtype = FST_AR_NONE;
          fprintf(fv, "$attrbegin %s %s %s %" PRId64 " $end\n", attrtypes[attrtype], arraytypes[subtype], str, attrarg);
          break;
        case FST_AT_ENUM:
          if ((subtype < FST_EV_SV_INTEGER) || (subtype > FST_EV_MAX))
            subtype = FST_EV_SV_INTEGER;
          fprintf(fv, "$attrbegin %s %s %s %" PRId64 " $end\n", attrtypes[attrtype], enumvaluetypes[subtype], str,
                  attrarg);
          break;
        case FST_AT_PACK:
          if ((subtype < FST_PT_NONE) || (subtype > FST_PT_MAX))
            subtype = FST_PT_NONE;
          fprintf(fv, "$attrbegin %s %s %s %" PRId64 " $end\n", attrtypes[attrtype], packtypes[subtype], str, attrarg);
          break;
        case FST_AT_MISC:
        default:
          attrtype = FST_AT_MISC;
          if (subtype == FST_MT_COMMENT) {
            fprintf(fv, "$comment\n\t%s\n$end\n", str);
          } else {
            if ((subtype == FST_MT_SOURCESTEM) || (subtype == FST_MT_SOURCEISTEM)) {
              int sidx_skiplen_dummy = 0;
              uint64_t sidx = fstGetVarint64((unsigned char *)str, &sidx_skiplen_dummy);

              fprintf(fv, "$attrbegin %s %02x %" PRId64 " %" PRId64 " $end\n", attrtypes[attrtype], subtype, sidx,
                      attrarg);
            } else {
              fprintf(fv, "$attrbegin %s %02x %s %" PRId64 " $end\n", attrtypes[attrtype], subtype, str, attrarg);
            }
          }
          break;
        }
      }
      break;

    case FST_ST_GEN_ATTREND:
      if (fv && xc->use_vcd_extensions)
        fprintf(fv, "$attrend $end\n");
      break;

    case FST_VT_VCD_EVENT:
    case FST_VT_VCD_INTEGER:
    case FST_VT_VCD_PARAMETER:
    case FST_VT_VCD_REAL:
    case FST_VT_VCD_REAL_PARAMETER:
    case FST_VT_VCD_REG:
    case FST_VT_VCD_SUPPLY0:
    case FST_VT_VCD_SUPPLY1:
    case FST_VT_VCD_TIME:
    case FST_VT_VCD_TRI:
    case FST_VT_VCD_TRIAND:
    case FST_VT_VCD_TRIOR:
    case FST_VT_VCD_TRIREG:
    case FST_VT_VCD_TRI0:
    case FST_VT_VCD_TRI1:
    case FST_VT_VCD_WAND:
    case FST_VT_VCD_WIRE:
    case FST_VT_VCD_WOR:
    case FST_VT_VCD_PORT:
    case FST_VT_VCD_SPARRAY:
    case FST_VT_VCD_REALTIME:
    case FST_VT_GEN_STRING:
    case FST_VT_SV_BIT:
    case FST_VT_SV_LOGIC:
    case FST_VT_SV_INT:
    case FST_VT_SV_SHORTINT:
    case FST_VT_SV_LONGINT:
    case FST_VT_SV_BYTE:
    case FST_VT_SV_ENUM:
    case FST_VT_SV_SHORTREAL:
      vartype = tag;
      /* vardir = */ fgetc(xc->fh); /* unused in VCD reader, but need to advance read pointer */
      pnt = str;
      cl = 0;
      while ((ch = fgetc(xc->fh))) {
        if (cl < FST_ID_NAM_ATTR_SIZ) {
          pnt[cl++] = ch;
        }
      }; /* varname */
      pnt[cl] = 0;
      len = fstReaderVarint32(xc->fh);
      alias = fstReaderVarint32(xc->fh);

      if (!alias) {
        if (xc->maxhandle == num_signal_dyn) {
          num_signal_dyn *= 2;
          xc->signal_lens = (uint32_t *)realloc(xc->signal_lens, num_signal_dyn * sizeof(uint32_t));
          xc->signal_typs = (unsigned char *)realloc(xc->signal_typs, num_signal_dyn * sizeof(unsigned char));
        }
        xc->signal_lens[xc->maxhandle] = len;
        xc->signal_typs[xc->maxhandle] = vartype;

        /* maxvalpos+=len; */
        if (len > xc->longest_signal_value_len) {
          xc->longest_signal_value_len = len;
        }

        if ((vartype == FST_VT_VCD_REAL) || (vartype == FST_VT_VCD_REAL_PARAMETER) ||
            (vartype == FST_VT_VCD_REALTIME) || (vartype == FST_VT_SV_SHORTREAL)) {
          len = (vartype != FST_VT_SV_SHORTREAL) ? 64 : 32;
          xc->signal_typs[xc->maxhandle] = FST_VT_VCD_REAL;
        }
        if (fv) {
          char vcdid_buf[16];
          uint32_t modlen = (vartype != FST_VT_VCD_PORT) ? len : ((len - 2) / 3);
          fstVcdID(vcdid_buf, xc->maxhandle + 1);
          fprintf(fv, "$var %s %" PRIu32 " %s %s $end\n", vartypes[vartype], modlen, vcdid_buf, str);
        }
        xc->maxhandle++;
      } else {
        if ((vartype == FST_VT_VCD_REAL) || (vartype == FST_VT_VCD_REAL_PARAMETER) ||
            (vartype == FST_VT_VCD_REALTIME) || (vartype == FST_VT_SV_SHORTREAL)) {
          len = (vartype != FST_VT_SV_SHORTREAL) ? 64 : 32;
          xc->signal_typs[xc->maxhandle] = FST_VT_VCD_REAL;
        }
        if (fv) {
          char vcdid_buf[16];
          uint32_t modlen = (vartype != FST_VT_VCD_PORT) ? len : ((len - 2) / 3);
          fstVcdID(vcdid_buf, alias);
          fprintf(fv, "$var %s %" PRIu32 " %s %s $end\n", vartypes[vartype], modlen, vcdid_buf, str);
        }
        xc->num_alias++;
      }

      break;

    default:
      break;
    }
  }
  if (fv)
    fprintf(fv, "$enddefinitions $end\n");

  maxhandle_scanbuild =
      xc->maxhandle ? xc->maxhandle : 1; /*scan-build warning suppression, in reality we have at least one signal */

  xc->signal_lens = (uint32_t *)realloc(xc->signal_lens, maxhandle_scanbuild * sizeof(uint32_t));
  xc->signal_typs = (unsigned char *)realloc(xc->signal_typs, maxhandle_scanbuild * sizeof(unsigned char));

  free(xc->process_mask);
  xc->process_mask = (unsigned char *)calloc(1, (maxhandle_scanbuild + 7) / 8);

  free(xc->temp_signal_value_buf);
  xc->temp_signal_value_buf = (unsigned char *)malloc(xc->longest_signal_value_len + 1);

  xc->var_count = xc->maxhandle + xc->num_alias;

  free(str);
  return (1);
}

/*
 * reader file open/close functions
 */
int fstReaderInit(struct fstReaderContext *xc) {
  fst_off_t blkpos = 0;
  fst_off_t endfile;
  uint64_t seclen;
  int sectype;
  uint64_t vc_section_count_actual = 0;
  int hdr_incomplete = 0;
  int hdr_seen = 0;
  int gzread_pass_status = 1;

  sectype = fgetc(xc->f);
  if (sectype == FST_BL_ZWRAPPER) {
    FILE *fcomp;
    fst_off_t offpnt, uclen;
    char gz_membuf[FST_GZIO_LEN];
    gzFile zhandle;
    int zfd;
    int flen = strlen(xc->filename);
    char *hf;
    int hf_len;

    seclen = fstReaderUint64(xc->f);
    uclen = fstReaderUint64(xc->f);

    if (!seclen)
      return (0); /* not finished compressing, this is a failed read */

    hf_len = flen + 16 + 32 + 1;
    hf = (char *)calloc(1, hf_len);

    snprintf(hf, hf_len, "%s.upk_%d_%p", xc->filename, getpid(), (void *)xc);
    fcomp = fopen(hf, "w+b");
    if (!fcomp) {
      fcomp = tmpfile_open(&xc->f_nam);
      free(hf);
      hf = NULL;
      if (!fcomp) {
        tmpfile_close(&fcomp, &xc->f_nam);
        return (0);
      }
    }

#if defined(FST_UNBUFFERED_IO)
    setvbuf(fcomp, (char *)NULL, _IONBF, 0); /* keeps gzip from acting weird in tandem with fopen */
#endif

#ifdef __MINGW32__
    xc->filename_unpacked = hf;
#else
    if (hf) {
      unlink(hf);
      free(hf);
    }
#endif

    fstReaderFseeko(xc, xc->f, FST_ZWRAPPER_HDR_SIZE, SEEK_SET);
#ifndef __MINGW32__
    fflush(xc->f);
#else
    /* Windows UCRT runtime library reads one byte ahead in the file
       even with buffering disabled and does not synchronise the
       file position after fseek. */
    _lseek(fileno(xc->f), FST_ZWRAPPER_HDR_SIZE, SEEK_SET);
#endif

    zfd = dup(fileno(xc->f));
    zhandle = gzdopen(zfd, "rb");
    if (zhandle) {
      for (offpnt = 0; offpnt < uclen; offpnt += FST_GZIO_LEN) {
        size_t this_len = ((uclen - offpnt) > FST_GZIO_LEN) ? FST_GZIO_LEN : (uclen - offpnt);
        size_t gzreadlen = gzread(zhandle, gz_membuf, this_len);
        size_t fwlen;

        if (gzreadlen != this_len) {
          gzread_pass_status = 0;
          break;
        }
        fwlen = fstFwrite(gz_membuf, this_len, 1, fcomp);
        if (fwlen != 1) {
          gzread_pass_status = 0;
          break;
        }
      }
      gzclose(zhandle);
    } else {
      close(zfd);
    }
    fflush(fcomp);
    fclose(xc->f);
    xc->f = fcomp;
  }

  if (gzread_pass_status) {
    fstReaderFseeko(xc, xc->f, 0, SEEK_END);
    endfile = ftello(xc->f);

    while (blkpos < endfile) {
      fstReaderFseeko(xc, xc->f, blkpos, SEEK_SET);

      sectype = fgetc(xc->f);
      seclen = fstReaderUint64(xc->f);

      if (sectype == EOF) {
        break;
      }

      if ((hdr_incomplete) && (!seclen)) {
        break;
      }

      if (!hdr_seen && (sectype != FST_BL_HDR)) {
        break;
      }

      blkpos++;
      if (sectype == FST_BL_HDR) {
        if (!hdr_seen) {
          int ch;
          double dcheck;

          xc->start_time = fstReaderUint64(xc->f);
          xc->end_time = fstReaderUint64(xc->f);

          hdr_incomplete = (xc->start_time == 0) && (xc->end_time == 0);

          fstFread(&dcheck, 8, 1, xc->f);
          xc->double_endian_match = (dcheck == FST_DOUBLE_ENDTEST);
          if (!xc->double_endian_match) {
            union {
              unsigned char rvs_buf[8];
              double d;
            } vu;

            unsigned char *dcheck_alias = (unsigned char *)&dcheck;
            int rvs_idx;

            for (rvs_idx = 0; rvs_idx < 8; rvs_idx++) {
              vu.rvs_buf[rvs_idx] = dcheck_alias[7 - rvs_idx];
            }
            if (vu.d != FST_DOUBLE_ENDTEST) {
              break; /* either corrupt file or wrong architecture (offset +33 also functions as matchword) */
            }
          }

          hdr_seen = 1;

          xc->mem_used_by_writer = fstReaderUint64(xc->f);
          xc->scope_count = fstReaderUint64(xc->f);
          xc->var_count = fstReaderUint64(xc->f);
          xc->maxhandle = fstReaderUint64(xc->f);
          xc->num_alias = xc->var_count - xc->maxhandle;
          xc->vc_section_count = fstReaderUint64(xc->f);
          ch = fgetc(xc->f);
          xc->timescale = (signed char)ch;
          fstFread(xc->version, FST_HDR_SIM_VERSION_SIZE, 1, xc->f);
          xc->version[FST_HDR_SIM_VERSION_SIZE] = 0;
          fstFread(xc->date, FST_HDR_DATE_SIZE, 1, xc->f);
          xc->date[FST_HDR_DATE_SIZE] = 0;
          ch = fgetc(xc->f);
          xc->filetype = (unsigned char)ch;
          xc->timezero = fstReaderUint64(xc->f);
        }
      } else if ((sectype == FST_BL_VCDATA) || (sectype == FST_BL_VCDATA_DYN_ALIAS) ||
                 (sectype == FST_BL_VCDATA_DYN_ALIAS2)) {
        if (hdr_incomplete) {
          uint64_t bt = fstReaderUint64(xc->f);
          xc->end_time = fstReaderUint64(xc->f);

          if (!vc_section_count_actual) {
            xc->start_time = bt;
          }
        }

        vc_section_count_actual++;
      } else if (sectype == FST_BL_GEOM) {
        if (!hdr_incomplete) {
          uint64_t clen = seclen - 24;
          uint64_t uclen = fstReaderUint64(xc->f);
          unsigned char *ucdata = (unsigned char *)malloc(uclen);
          unsigned char *pnt = ucdata;
          unsigned int i;

          xc->contains_geom_section = 1;
          xc->maxhandle = fstReaderUint64(xc->f);
          xc->longest_signal_value_len = 32; /* arbitrarily set at 32...this is much longer than an expanded double */

          free(xc->process_mask);
          xc->process_mask = (unsigned char *)calloc(1, (xc->maxhandle + 7) / 8);

          if (clen != uclen) {
            unsigned char *cdata = (unsigned char *)malloc(clen);
            unsigned long destlen = uclen;
            unsigned long sourcelen = clen;
            int rc;

            fstFread(cdata, clen, 1, xc->f);
            rc = uncompress(ucdata, &destlen, cdata, sourcelen);

            if (rc != Z_OK) {
              fprintf(stderr, FST_APIMESS "fstReaderInit(), geom uncompress rc = %d, exiting.\n", rc);
              exit(255);
            }

            free(cdata);
          } else {
            fstFread(ucdata, uclen, 1, xc->f);
          }

          free(xc->signal_lens);
          xc->signal_lens = (uint32_t *)malloc(sizeof(uint32_t) * xc->maxhandle);
          free(xc->signal_typs);
          xc->signal_typs = (unsigned char *)malloc(sizeof(unsigned char) * xc->maxhandle);

          for (i = 0; i < xc->maxhandle; i++) {
            int skiplen;
            uint64_t val = fstGetVarint32(pnt, &skiplen);

            pnt += skiplen;

            if (val) {
              xc->signal_lens[i] = (val != 0xFFFFFFFF) ? val : 0;
              xc->signal_typs[i] = FST_VT_VCD_WIRE;
              if (xc->signal_lens[i] > xc->longest_signal_value_len) {
                xc->longest_signal_value_len = xc->signal_lens[i];
              }
            } else {
              xc->signal_lens[i] = 8; /* backpatch in real */
              xc->signal_typs[i] = FST_VT_VCD_REAL;
              /* xc->longest_signal_value_len handled above by overly large init size */
            }
          }

          free(xc->temp_signal_value_buf);
          xc->temp_signal_value_buf = (unsigned char *)malloc(xc->longest_signal_value_len + 1);

          free(ucdata);
        }
      } else if (sectype == FST_BL_HIER) {
        xc->contains_hier_section = 1;
        xc->hier_pos = ftello(xc->f);
      } else if (sectype == FST_BL_HIER_LZ4DUO) {
        xc->contains_hier_section_lz4 = 1;
        xc->contains_hier_section_lz4duo = 1;
        xc->hier_pos = ftello(xc->f);
      } else if (sectype == FST_BL_HIER_LZ4) {
        xc->contains_hier_section_lz4 = 1;
        xc->hier_pos = ftello(xc->f);
      } else if (sectype == FST_BL_BLACKOUT) {
        uint32_t i;
        uint64_t cur_bl = 0;
        uint64_t delta;

        xc->num_blackouts = fstReaderVarint32(xc->f);
        free(xc->blackout_times);
        xc->blackout_times = (uint64_t *)calloc(xc->num_blackouts, sizeof(uint64_t));
        free(xc->blackout_activity);
        xc->blackout_activity = (unsigned char *)calloc(xc->num_blackouts, sizeof(unsigned char));

        for (i = 0; i < xc->num_blackouts; i++) {
          xc->blackout_activity[i] = fgetc(xc->f) != 0;
          delta = fstReaderVarint64(xc->f);
          cur_bl += delta;
          xc->blackout_times[i] = cur_bl;
        }
      }

      blkpos += seclen;
      if (!hdr_seen)
        break;
    }

    if (hdr_seen) {
      if (xc->vc_section_count != vc_section_count_actual) {
        xc->vc_section_count = vc_section_count_actual;
      }

      if (!xc->contains_geom_section) {
        fstReaderProcessHier(xc, NULL); /* recreate signal_lens/signal_typs info */
      }
    }
  }

  return (hdr_seen);
}

void *fstReaderOpenForUtilitiesOnly(void) {
  struct fstReaderContext *xc = (struct fstReaderContext *)calloc(1, sizeof(struct fstReaderContext));

  return (xc);
}

void *fstReaderOpen(const char *nam) {
  struct fstReaderContext *xc = (struct fstReaderContext *)calloc(1, sizeof(struct fstReaderContext));

  if ((!nam) || (!(xc->f = fopen(nam, "rb")))) {
    free(xc);
    xc = NULL;
  } else {
    int flen = strlen(nam);
    char *hf = (char *)calloc(1, flen + 6);
    int rc;

#if defined(FST_UNBUFFERED_IO)
    setvbuf(xc->f, (char *)NULL, _IONBF, 0); /* keeps gzip from acting weird in tandem with fopen */
#endif

    memcpy(hf, nam, flen);
    strcpy(hf + flen, ".hier");
    xc->fh = fopen(hf, "rb");

    free(hf);
    xc->filename = strdup(nam);
    rc = fstReaderInit(xc);

    if ((rc) && (xc->vc_section_count) && (xc->maxhandle) &&
        ((xc->fh) || (xc->contains_hier_section || (xc->contains_hier_section_lz4)))) {
      /* more init */
      xc->do_rewind = 1;
    } else {
      fstReaderClose(xc);
      xc = NULL;
    }
  }

  return (xc);
}

static void fstReaderDeallocateRvatData(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  if (xc) {
    free(xc->rvat_chain_mem);
    xc->rvat_chain_mem = NULL;
    free(xc->rvat_frame_data);
    xc->rvat_frame_data = NULL;
    free(xc->rvat_time_table);
    xc->rvat_time_table = NULL;
    free(xc->rvat_chain_table);
    xc->rvat_chain_table = NULL;
    free(xc->rvat_chain_table_lengths);
    xc->rvat_chain_table_lengths = NULL;

    xc->rvat_data_valid = 0;
  }
}

void fstReaderClose(void *ctx) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  if (xc) {
    fstReaderDeallocateScopeData(xc);
    fstReaderDeallocateRvatData(xc);
    free(xc->rvat_sig_offs);
    xc->rvat_sig_offs = NULL;

    free(xc->process_mask);
    xc->process_mask = NULL;
    free(xc->blackout_times);
    xc->blackout_times = NULL;
    free(xc->blackout_activity);
    xc->blackout_activity = NULL;
    free(xc->temp_signal_value_buf);
    xc->temp_signal_value_buf = NULL;
    free(xc->signal_typs);
    xc->signal_typs = NULL;
    free(xc->signal_lens);
    xc->signal_lens = NULL;
    free(xc->filename);
    xc->filename = NULL;
    free(xc->str_scope_attr);
    xc->str_scope_attr = NULL;

    if (xc->fh) {
      tmpfile_close(&xc->fh, &xc->fh_nam);
    }

    if (xc->f) {
      tmpfile_close(&xc->f, &xc->f_nam);
      if (xc->filename_unpacked) {
        unlink(xc->filename_unpacked);
        free(xc->filename_unpacked);
      }
    }

    free(xc);
  }
}

/*
 * read processing
 */

/* normal read which re-interleaves the value change data */
int fstReaderIterBlocks(void *ctx,
                        void (*value_change_callback)(void *user_callback_data_pointer, uint64_t time, fstHandle facidx,
                                                      const unsigned char *value),
                        void *user_callback_data_pointer, FILE *fv) {
  return (fstReaderIterBlocks2(ctx, value_change_callback, NULL, user_callback_data_pointer, fv));
}

int fstReaderIterBlocks2(void *ctx,
                         void (*value_change_callback)(void *user_callback_data_pointer, uint64_t time,
                                                       fstHandle facidx, const unsigned char *value),
                         void (*value_change_callback_varlen)(void *user_callback_data_pointer, uint64_t time,
                                                              fstHandle facidx, const unsigned char *value,
                                                              uint32_t len),
                         void *user_callback_data_pointer, FILE *fv) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;

  uint64_t previous_time = UINT64_MAX;
  uint64_t *time_table = NULL;
  uint64_t tsec_nitems;
  unsigned int secnum = 0;
  int blocks_skipped = 0;
  fst_off_t blkpos = 0;
  uint64_t seclen, beg_tim;
  uint64_t end_tim;
  uint64_t frame_uclen, frame_clen, frame_maxhandle, vc_maxhandle;
  fst_off_t vc_start;
  fst_off_t indx_pntr, indx_pos;
  fst_off_t *chain_table = NULL;
  uint32_t *chain_table_lengths = NULL;
  unsigned char *chain_cmem;
  unsigned char *pnt;
  long chain_clen;
  fstHandle idx, pidx = 0, i;
  uint64_t pval;
  uint64_t vc_maxhandle_largest = 0;
  uint64_t tsec_uclen = 0, tsec_clen = 0;
  int sectype;
  uint64_t mem_required_for_traversal;
  unsigned char *mem_for_traversal = NULL;
  uint32_t traversal_mem_offs;
  uint32_t *scatterptr, *headptr, *length_remaining;
  uint32_t cur_blackout = 0;
  int packtype;
  unsigned char *mc_mem = NULL;
  uint32_t mc_mem_len; /* corresponds to largest value encountered in chain_table_lengths[i] */
  int dumpvars_state = 0;

  if (!xc)
    return (0);

  scatterptr = (uint32_t *)calloc(xc->maxhandle, sizeof(uint32_t));
  headptr = (uint32_t *)calloc(xc->maxhandle, sizeof(uint32_t));
  length_remaining = (uint32_t *)calloc(xc->maxhandle, sizeof(uint32_t));

  if (fv) {
#ifndef FST_WRITEX_DISABLE
    fflush(fv);
    setvbuf(fv, (char *)NULL, _IONBF,
            0); /* even buffered IO is slow so disable it and use our own routines that don't need seeking */
    xc->writex_fd = fileno(fv);
#endif
  }

  for (;;) {
    uint32_t *tc_head = NULL;
    uint32_t tc_head_items = 0;
    traversal_mem_offs = 0;

    fstReaderFseeko(xc, xc->f, blkpos, SEEK_SET);

    sectype = fgetc(xc->f);
    seclen = fstReaderUint64(xc->f);

    if ((sectype == EOF) || (sectype == FST_BL_SKIP)) {
#ifdef FST_DEBUG
      fprintf(stderr, FST_APIMESS "<< EOF >>\n");
#endif
      break;
    }

    blkpos++;
    if ((sectype != FST_BL_VCDATA) && (sectype != FST_BL_VCDATA_DYN_ALIAS) && (sectype != FST_BL_VCDATA_DYN_ALIAS2)) {
      blkpos += seclen;
      continue;
    }

    if (!seclen)
      break;

    beg_tim = fstReaderUint64(xc->f);
    end_tim = fstReaderUint64(xc->f);

    if (xc->limit_range_valid) {
      if (end_tim < xc->limit_range_start) {
        blocks_skipped++;
        blkpos += seclen;
        continue;
      }

      if (beg_tim >
          xc->limit_range_end) /* likely the compare in for(i=0;i<tsec_nitems;i++) below would do this earlier */
      {
        break;
      }
    }

    mem_required_for_traversal = fstReaderUint64(xc->f) + 66; /* add in potential fastlz overhead */
    mem_for_traversal = (unsigned char *)malloc(mem_required_for_traversal);
#ifdef FST_DEBUG
    fprintf(stderr, FST_APIMESS "sec: %u seclen: %d begtim: %d endtim: %d\n", secnum, (int)seclen, (int)beg_tim,
            (int)end_tim);
    fprintf(stderr, FST_APIMESS "mem_required_for_traversal: %d\n", (int)mem_required_for_traversal - 66);
#endif
    /* process time block */
    {
      unsigned char *ucdata;
      unsigned char *cdata;
      unsigned long destlen /* = tsec_uclen */; /* scan-build */
      unsigned long sourcelen /*= tsec_clen */; /* scan-build */
      int rc;
      unsigned char *tpnt;
      uint64_t tpval;
      unsigned int ti;

      if (fstReaderFseeko(xc, xc->f, blkpos + seclen - 24, SEEK_SET) != 0)
        break;
      tsec_uclen = fstReaderUint64(xc->f);
      tsec_clen = fstReaderUint64(xc->f);
      tsec_nitems = fstReaderUint64(xc->f);
#ifdef FST_DEBUG
      fprintf(stderr, FST_APIMESS "time section unc: %d, com: %d (%d items)\n", (int)tsec_uclen, (int)tsec_clen,
              (int)tsec_nitems);
#endif
      if (tsec_clen > seclen)
        break; /* corrupted tsec_clen: by definition it can't be larger than size of section */
      ucdata = (unsigned char *)malloc(tsec_uclen);
      if (!ucdata)
        break; /* malloc fail as tsec_uclen out of range from corrupted file */
      destlen = tsec_uclen;
      sourcelen = tsec_clen;

      fstReaderFseeko(xc, xc->f, -24 - ((fst_off_t)tsec_clen), SEEK_CUR);

      if (tsec_uclen != tsec_clen) {
        cdata = (unsigned char *)malloc(tsec_clen);
        fstFread(cdata, tsec_clen, 1, xc->f);

        rc = uncompress(ucdata, &destlen, cdata, sourcelen);

        if (rc != Z_OK) {
          fprintf(stderr, FST_APIMESS "fstReaderIterBlocks2(), tsec uncompress rc = %d, exiting.\n", rc);
          exit(255);
        }

        free(cdata);
      } else {
        fstFread(ucdata, tsec_uclen, 1, xc->f);
      }

      free(time_table);

      if (sizeof(size_t) < sizeof(uint64_t)) {
        /* TALOS-2023-1792 for 32b overflow */
        uint64_t chk_64 = tsec_nitems * sizeof(uint64_t);
        size_t chk_32 = ((size_t)tsec_nitems) * sizeof(uint64_t);
        if (chk_64 != chk_32)
          chk_report_abort("TALOS-2023-1792");
      } else {
        uint64_t chk_64 = tsec_nitems * sizeof(uint64_t);
        if ((chk_64 / sizeof(uint64_t)) != tsec_nitems) {
          chk_report_abort("TALOS-2023-1792");
        }
      }
      time_table = (uint64_t *)calloc(tsec_nitems, sizeof(uint64_t));
      tpnt = ucdata;
      tpval = 0;
      for (ti = 0; ti < tsec_nitems; ti++) {
        int skiplen;
        uint64_t val = fstGetVarint64(tpnt, &skiplen);
        tpval = time_table[ti] = tpval + val;
        tpnt += skiplen;
      }

      tc_head_items = tsec_nitems /* scan-build */ ? tsec_nitems : 1;
      if (sizeof(size_t) < sizeof(uint64_t)) {
        /* TALOS-2023-1792 for 32b overflow */
        uint64_t chk_64 = tc_head_items * sizeof(uint32_t);
        size_t chk_32 = ((size_t)tc_head_items) * sizeof(uint32_t);
        if (chk_64 != chk_32)
          chk_report_abort("TALOS-2023-1792");
      } else {
        uint64_t chk_64 = tc_head_items * sizeof(uint32_t);
        if ((chk_64 / sizeof(uint32_t)) != tc_head_items) {
          chk_report_abort("TALOS-2023-1792");
        }
      }
      tc_head = (uint32_t *)calloc(tc_head_items, sizeof(uint32_t));
      free(ucdata);
    }

    fstReaderFseeko(xc, xc->f, blkpos + 32, SEEK_SET);

    frame_uclen = fstReaderVarint64(xc->f);
    frame_clen = fstReaderVarint64(xc->f);
    frame_maxhandle = fstReaderVarint64(xc->f);

    if (secnum == 0) {
      if ((beg_tim != time_table[0]) || (blocks_skipped)) {
        unsigned char *mu = (unsigned char *)malloc(frame_uclen);
        uint32_t sig_offs = 0;

        if (fv) {
          char wx_buf[32];
          int wx_len;

          if (beg_tim) {
            if (dumpvars_state == 1) {
              wx_len = snprintf(wx_buf, 32, "$end\n");
              fstWritex(xc, wx_buf, wx_len);
              dumpvars_state = 2;
            }
            wx_len = snprintf(wx_buf, 32, "#%" PRIu64 "\n", beg_tim);
            fstWritex(xc, wx_buf, wx_len);
            if (!dumpvars_state) {
              wx_len = snprintf(wx_buf, 32, "$dumpvars\n");
              fstWritex(xc, wx_buf, wx_len);
              dumpvars_state = 1;
            }
          }
          if ((xc->num_blackouts) && (cur_blackout != xc->num_blackouts)) {
            if (beg_tim == xc->blackout_times[cur_blackout]) {
              wx_len = snprintf(wx_buf, 32, "$dump%s $end\n", (xc->blackout_activity[cur_blackout++]) ? "on" : "off");
              fstWritex(xc, wx_buf, wx_len);
            }
          }
        }

        if (frame_uclen == frame_clen) {
          fstFread(mu, frame_uclen, 1, xc->f);
        } else {
          unsigned char *mc = (unsigned char *)malloc(frame_clen);
          int rc;

          unsigned long destlen = frame_uclen;
          unsigned long sourcelen = frame_clen;

          fstFread(mc, sourcelen, 1, xc->f);
          rc = uncompress(mu, &destlen, mc, sourcelen);
          if (rc != Z_OK) {
            fprintf(stderr, FST_APIMESS "fstReaderIterBlocks2(), frame uncompress rc: %d, exiting.\n", rc);
            exit(255);
          }
          free(mc);
        }

        for (idx = 0; idx < frame_maxhandle; idx++) {
          int process_idx = idx / 8;
          int process_bit = idx & 7;

          if (xc->process_mask[process_idx] & (1 << process_bit)) {
            if (xc->signal_lens[idx] <= 1) {
              if (xc->signal_lens[idx] == 1) {
                unsigned char val = mu[sig_offs];
                if (value_change_callback) {
                  xc->temp_signal_value_buf[0] = val;
                  xc->temp_signal_value_buf[1] = 0;
                  value_change_callback(user_callback_data_pointer, beg_tim, idx + 1, xc->temp_signal_value_buf);
                } else {
                  if (fv) {
                    char vcd_id[16];

                    int vcdid_len = fstVcdIDForFwrite(vcd_id + 1, idx + 1);
                    vcd_id[0] = val; /* collapse 3 writes into one I/O call */
                    vcd_id[vcdid_len + 1] = '\n';
                    fstWritex(xc, vcd_id, vcdid_len + 2);
                  }
                }
              } else {
                /* variable-length ("0" length) records have no initial state */
              }
            } else {
              if (xc->signal_typs[idx] != FST_VT_VCD_REAL) {
                if (value_change_callback) {
                  if (xc->signal_lens[idx] > xc->longest_signal_value_len) {
                    chk_report_abort("TALOS-2023-1797");
                  }
                  memcpy(xc->temp_signal_value_buf, mu + sig_offs, xc->signal_lens[idx]);
                  xc->temp_signal_value_buf[xc->signal_lens[idx]] = 0;
                  value_change_callback(user_callback_data_pointer, beg_tim, idx + 1, xc->temp_signal_value_buf);
                } else {
                  if (fv) {
                    char vcd_id[16];
                    int vcdid_len = fstVcdIDForFwrite(vcd_id + 1, idx + 1);

                    vcd_id[0] = (xc->signal_typs[idx] != FST_VT_VCD_PORT) ? 'b' : 'p';
                    fstWritex(xc, vcd_id, 1);
                    if ((sig_offs + xc->signal_lens[idx]) > frame_uclen) {
                      chk_report_abort("TALOS-2023-1793");
                    }
                    fstWritex(xc, mu + sig_offs, xc->signal_lens[idx]);

                    vcd_id[0] = ' '; /* collapse 3 writes into one I/O call */
                    vcd_id[vcdid_len + 1] = '\n';
                    fstWritex(xc, vcd_id, vcdid_len + 2);
                  }
                }
              } else {
                double d;
                unsigned char *clone_d;
                unsigned char *srcdata = mu + sig_offs;

                if (value_change_callback) {
                  if (xc->native_doubles_for_cb) {
                    if (xc->double_endian_match) {
                      clone_d = srcdata;
                    } else {
                      int j;

                      clone_d = (unsigned char *)&d;
                      for (j = 0; j < 8; j++) {
                        clone_d[j] = srcdata[7 - j];
                      }
                    }
                    value_change_callback(user_callback_data_pointer, beg_tim, idx + 1, clone_d);
                  } else {
                    clone_d = (unsigned char *)&d;
                    if (xc->double_endian_match) {
                      memcpy(clone_d, srcdata, 8);
                    } else {
                      int j;

                      for (j = 0; j < 8; j++) {
                        clone_d[j] = srcdata[7 - j];
                      }
                    }
                    snprintf((char *)xc->temp_signal_value_buf, xc->longest_signal_value_len + 1, "%.16g", d);
                    value_change_callback(user_callback_data_pointer, beg_tim, idx + 1, xc->temp_signal_value_buf);
                  }
                } else {
                  if (fv) {
                    char vcdid_buf[16];
                    char wx_buf[64];
                    int wx_len;

                    clone_d = (unsigned char *)&d;
                    if (xc->double_endian_match) {
                      memcpy(clone_d, srcdata, 8);
                    } else {
                      int j;

                      for (j = 0; j < 8; j++) {
                        clone_d[j] = srcdata[7 - j];
                      }
                    }

                    fstVcdID(vcdid_buf, idx + 1);
                    wx_len = snprintf(wx_buf, 64, "r%.16g %s\n", d, vcdid_buf);
                    fstWritex(xc, wx_buf, wx_len);
                  }
                }
              }
            }
          }

          sig_offs += xc->signal_lens[idx];
        }

        free(mu);
        fstReaderFseeko(xc, xc->f, -((fst_off_t)frame_clen), SEEK_CUR);
      }
    }

    fstReaderFseeko(xc, xc->f, (fst_off_t)frame_clen, SEEK_CUR); /* skip past compressed data */

    vc_maxhandle = fstReaderVarint64(xc->f);
    vc_start = ftello(xc->f); /* points to '!' character */
    packtype = fgetc(xc->f);

#ifdef FST_DEBUG
    fprintf(stderr, FST_APIMESS "frame_uclen: %d, frame_clen: %d, frame_maxhandle: %d\n", (int)frame_uclen,
            (int)frame_clen, (int)frame_maxhandle);
    fprintf(stderr, FST_APIMESS "vc_maxhandle: %d, packtype: %c\n", (int)vc_maxhandle, packtype);
#endif

    indx_pntr = blkpos + seclen - 24 - tsec_clen - 8;
    fstReaderFseeko(xc, xc->f, indx_pntr, SEEK_SET);
    chain_clen = fstReaderUint64(xc->f);
    indx_pos = indx_pntr - chain_clen;
#ifdef FST_DEBUG
    fprintf(stderr, FST_APIMESS "indx_pos: %d (%d bytes)\n", (int)indx_pos, (int)chain_clen);
#endif
    chain_cmem = (unsigned char *)malloc(chain_clen);
    if (!chain_cmem)
      goto block_err;
    fstReaderFseeko(xc, xc->f, indx_pos, SEEK_SET);
    fstFread(chain_cmem, chain_clen, 1, xc->f);

    if (vc_maxhandle > vc_maxhandle_largest) {
      free(chain_table);
      free(chain_table_lengths);

      vc_maxhandle_largest = vc_maxhandle;

      if (!(vc_maxhandle + 1)) {
        chk_report_abort("TALOS-2023-1798");
      }

      if (sizeof(size_t) < sizeof(uint64_t)) {
        /* TALOS-2023-1798 for 32b overflow */
        uint64_t chk_64 = (vc_maxhandle + 1) * sizeof(fst_off_t);
        size_t chk_32 = ((size_t)(vc_maxhandle + 1)) * sizeof(fst_off_t);
        if (chk_64 != chk_32)
          chk_report_abort("TALOS-2023-1798");
      } else {
        uint64_t chk_64 = (vc_maxhandle + 1) * sizeof(fst_off_t);
        if ((chk_64 / sizeof(fst_off_t)) != (vc_maxhandle + 1)) {
          chk_report_abort("TALOS-2023-1798");
        }
      }
      chain_table = (fst_off_t *)calloc((vc_maxhandle + 1), sizeof(fst_off_t));

      if (sizeof(size_t) < sizeof(uint64_t)) {
        /* TALOS-2023-1798 for 32b overflow */
        uint64_t chk_64 = (vc_maxhandle + 1) * sizeof(uint32_t);
        size_t chk_32 = ((size_t)(vc_maxhandle + 1)) * sizeof(uint32_t);
        if (chk_64 != chk_32)
          chk_report_abort("TALOS-2023-1798");
      } else {
        uint64_t chk_64 = (vc_maxhandle + 1) * sizeof(uint32_t);
        if ((chk_64 / sizeof(uint32_t)) != (vc_maxhandle + 1)) {
          chk_report_abort("TALOS-2023-1798");
        }
      }
      chain_table_lengths = (uint32_t *)calloc((vc_maxhandle + 1), sizeof(uint32_t));
    }

    if (!chain_table || !chain_table_lengths)
      goto block_err;

    pnt = chain_cmem;
    idx = 0;
    pval = 0;

    if (sectype == FST_BL_VCDATA_DYN_ALIAS2) {
      uint32_t prev_alias = 0;

      do {
        int skiplen;

        if (*pnt & 0x01) {
          int64_t shval = fstGetSVarint64(pnt, &skiplen) >> 1;
          if (shval > 0) {
            pval = chain_table[idx] = pval + shval;
            if (idx) {
              chain_table_lengths[pidx] = pval - chain_table[pidx];
            }
            pidx = idx++;
          } else if (shval < 0) {
            chain_table[idx] = 0;                          /* need to explicitly zero as calloc above might not run */
            chain_table_lengths[idx] = prev_alias = shval; /* because during this loop iter would give stale data! */
            idx++;
          } else {
            chain_table[idx] = 0;                  /* need to explicitly zero as calloc above might not run */
            chain_table_lengths[idx] = prev_alias; /* because during this loop iter would give stale data! */
            idx++;
          }
        } else {
          uint64_t val = fstGetVarint32(pnt, &skiplen);

          fstHandle loopcnt = val >> 1;
          if ((idx + loopcnt - 1) > vc_maxhandle) /* TALOS-2023-1789 */
          {
            chk_report_abort("TALOS-2023-1789");
          }

          for (i = 0; i < loopcnt; i++) {
            chain_table[idx++] = 0;
          }
        }

        pnt += skiplen;
      } while (pnt != (chain_cmem + chain_clen));
    } else {
      do {
        int skiplen;
        uint64_t val = fstGetVarint32(pnt, &skiplen);

        if (!val) {
          pnt += skiplen;
          val = fstGetVarint32(pnt, &skiplen);
          chain_table[idx] = 0;            /* need to explicitly zero as calloc above might not run */
          chain_table_lengths[idx] = -val; /* because during this loop iter would give stale data! */
          idx++;
        } else if (val & 1) {
          pval = chain_table[idx] = pval + (val >> 1);
          if (idx) {
            chain_table_lengths[pidx] = pval - chain_table[pidx];
          }
          pidx = idx++;
        } else {
          fstHandle loopcnt = val >> 1;

          if ((idx + loopcnt - 1) > vc_maxhandle) /* TALOS-2023-1789 */
          {
            chk_report_abort("TALOS-2023-1789");
          }

          for (i = 0; i < loopcnt; i++) {
            chain_table[idx++] = 0;
          }
        }

        pnt += skiplen;
      } while (pnt != (chain_cmem + chain_clen));
    }

    chain_table[idx] = indx_pos - vc_start;
    chain_table_lengths[pidx] = chain_table[idx] - chain_table[pidx];

    for (i = 0; i < idx; i++) {
      int32_t v32 = chain_table_lengths[i];
      if ((v32 < 0) && (!chain_table[i])) {
        v32 = -v32;
        v32--;
        if (((uint32_t)v32) < i) /* sanity check */
        {
          chain_table[i] = chain_table[v32];
          chain_table_lengths[i] = chain_table_lengths[v32];
        }
      }
    }

#ifdef FST_DEBUG
    fprintf(stderr, FST_APIMESS "decompressed chain idx len: %" PRIu32 "\n", idx);
#endif

    mc_mem_len = 16384;
    mc_mem = (unsigned char *)malloc(mc_mem_len); /* buffer for compressed reads */

    /* check compressed VC data */
    if (idx > xc->maxhandle)
      idx = xc->maxhandle;
    for (i = 0; i < idx; i++) {
      if (chain_table[i]) {
        int process_idx = i / 8;
        int process_bit = i & 7;

        if (xc->process_mask[process_idx] & (1 << process_bit)) {
          int rc = Z_OK;
          uint32_t val;
          uint32_t skiplen;
          uint32_t tdelta;

          fstReaderFseeko(xc, xc->f, vc_start + chain_table[i], SEEK_SET);
          val = fstReaderVarint32WithSkip(xc->f, &skiplen);
          if (val) {
            unsigned char *mu = mem_for_traversal + traversal_mem_offs; /* uncomp: dst */
            unsigned char *mc;                                          /* comp:   src */
            unsigned long destlen = val;
            unsigned long sourcelen = chain_table_lengths[i];

            if (traversal_mem_offs >= mem_required_for_traversal) {
              chk_report_abort("TALOS-2023-1785");
            }

            if (mc_mem_len < chain_table_lengths[i]) {
              free(mc_mem);
              mc_mem = (unsigned char *)malloc(mc_mem_len = chain_table_lengths[i]);
            }
            mc = mc_mem;

            fstFread(mc, chain_table_lengths[i], 1, xc->f);

            switch (packtype) {
            case '4':
              rc = (destlen ==
                    (unsigned long)LZ4_decompress_safe_partial((char *)mc, (char *)mu, sourcelen, destlen, destlen))
                       ? Z_OK
                       : Z_DATA_ERROR;
              break;
            case 'F':
              fastlz_decompress(mc, sourcelen, mu, destlen); /* rc appears unreliable */
              break;
            default:
              rc = uncompress(mu, &destlen, mc, sourcelen);
              break;
            }

            /* data to process is for(j=0;j<destlen;j++) in mu[j] */
            headptr[i] = traversal_mem_offs;
            length_remaining[i] = val;
            traversal_mem_offs += val;
          } else {
            int destlen = chain_table_lengths[i] - skiplen;
            unsigned char *mu = mem_for_traversal + traversal_mem_offs;

            if (traversal_mem_offs >= mem_required_for_traversal) {
              chk_report_abort("TALOS-2023-1785");
            }

            fstFread(mu, destlen, 1, xc->f);
            /* data to process is for(j=0;j<destlen;j++) in mu[j] */
            headptr[i] = traversal_mem_offs;
            length_remaining[i] = destlen;
            traversal_mem_offs += destlen;
          }

          if (rc != Z_OK) {
            fprintf(stderr, FST_APIMESS "fstReaderIterBlocks2(), fac: %d clen: %d (rc=%d), exiting.\n", (int)i,
                    (int)val, rc);
            exit(255);
          }

          if (xc->signal_lens[i] == 1) {
            uint32_t vli = fstGetVarint32NoSkip(mem_for_traversal + headptr[i]);
            uint32_t shcnt = 2 << (vli & 1);
            tdelta = vli >> shcnt;
          } else {
            uint32_t vli = fstGetVarint32NoSkip(mem_for_traversal + headptr[i]);
            tdelta = vli >> 1;
          }

          if (tdelta >= tc_head_items) {
            chk_report_abort("TALOS-2023-1791");
          }

          scatterptr[i] = tc_head[tdelta];
          tc_head[tdelta] = i + 1;
        }
      }
    }

    free(mc_mem); /* there is no usage below for this, no real need to clear out mc_mem or mc_mem_len */

    for (i = 0; i < tsec_nitems; i++) {
      uint32_t tdelta;
      int skiplen, skiplen2;
      uint32_t vli;

      if (fv) {
        char wx_buf[32];
        int wx_len;

        if (time_table[i] != previous_time) {
          if (xc->limit_range_valid) {
            if (time_table[i] > xc->limit_range_end) {
              break;
            }
          }

          if (dumpvars_state == 1) {
            wx_len = snprintf(wx_buf, 32, "$end\n");
            fstWritex(xc, wx_buf, wx_len);
            dumpvars_state = 2;
          }
          wx_len = snprintf(wx_buf, 32, "#%" PRIu64 "\n", time_table[i]);
          fstWritex(xc, wx_buf, wx_len);
          if (!dumpvars_state) {
            wx_len = snprintf(wx_buf, 32, "$dumpvars\n");
            fstWritex(xc, wx_buf, wx_len);
            dumpvars_state = 1;
          }

          if ((xc->num_blackouts) && (cur_blackout != xc->num_blackouts)) {
            if (time_table[i] == xc->blackout_times[cur_blackout]) {
              wx_len = snprintf(wx_buf, 32, "$dump%s $end\n", (xc->blackout_activity[cur_blackout++]) ? "on" : "off");
              fstWritex(xc, wx_buf, wx_len);
            }
          }
          previous_time = time_table[i];
        }
      }

      while (tc_head[i]) {
        idx = tc_head[i] - 1;
        vli = fstGetVarint32(mem_for_traversal + headptr[idx], &skiplen);

        if (xc->signal_lens[idx] <= 1) {
          if (xc->signal_lens[idx] == 1) {
            unsigned char val;
            if (!(vli & 1)) {
              /* tdelta = vli >> 2; */ /* scan-build */
              val = ((vli >> 1) & 1) | '0';
            } else {
              /* tdelta = vli >> 4; */ /* scan-build */
              val = FST_RCV_STR[((vli >> 1) & 7)];
            }

            if (value_change_callback) {
              xc->temp_signal_value_buf[0] = val;
              xc->temp_signal_value_buf[1] = 0;
              value_change_callback(user_callback_data_pointer, time_table[i], idx + 1, xc->temp_signal_value_buf);
            } else {
              if (fv) {
                char vcd_id[16];
                int vcdid_len = fstVcdIDForFwrite(vcd_id + 1, idx + 1);

                vcd_id[0] = val;
                vcd_id[vcdid_len + 1] = '\n';
                fstWritex(xc, vcd_id, vcdid_len + 2);
              }
            }
            headptr[idx] += skiplen;
            length_remaining[idx] -= skiplen;

            tc_head[i] = scatterptr[idx];
            scatterptr[idx] = 0;

            if (length_remaining[idx]) {
              int shamt;
              vli = fstGetVarint32NoSkip(mem_for_traversal + headptr[idx]);
              shamt = 2 << (vli & 1);
              tdelta = vli >> shamt;

              if ((tdelta + i) >= tc_head_items) {
                chk_report_abort("TALOS-2023-1791");
              }

              scatterptr[idx] = tc_head[i + tdelta];
              tc_head[i + tdelta] = idx + 1;
            }
          } else {
            unsigned char *vdata;
            uint32_t len;

            vli = fstGetVarint32(mem_for_traversal + headptr[idx], &skiplen);
            len = fstGetVarint32(mem_for_traversal + headptr[idx] + skiplen, &skiplen2);
            /* tdelta = vli >> 1; */ /* scan-build */
            skiplen += skiplen2;
            vdata = mem_for_traversal + headptr[idx] + skiplen;

            if (!(vli & 1)) {
              if (value_change_callback_varlen) {
                value_change_callback_varlen(user_callback_data_pointer, time_table[i], idx + 1, vdata, len);
              } else {
                if (fv) {
                  char vcd_id[16];
                  int vcdid_len;

                  vcd_id[0] = 's';
                  fstWritex(xc, vcd_id, 1);

                  vcdid_len = fstVcdIDForFwrite(vcd_id + 1, idx + 1);
                  {
                    if (sizeof(size_t) < sizeof(uint64_t)) {
                      /* TALOS-2023-1790 for 32b overflow */
                      uint64_t chk_64 = len * 4 + 1;
                      size_t chk_32 = len * 4 + 1;
                      if (chk_64 != chk_32)
                        chk_report_abort("TALOS-2023-1790");
                    }

                    unsigned char *vesc = (unsigned char *)malloc(len * 4 + 1);
                    int vlen = fstUtilityBinToEsc(vesc, vdata, len);
                    fstWritex(xc, vesc, vlen);
                    free(vesc);
                  }

                  vcd_id[0] = ' ';
                  vcd_id[vcdid_len + 1] = '\n';
                  fstWritex(xc, vcd_id, vcdid_len + 2);
                }
              }
            }

            skiplen += len;
            headptr[idx] += skiplen;
            length_remaining[idx] -= skiplen;

            tc_head[i] = scatterptr[idx];
            scatterptr[idx] = 0;

            if (length_remaining[idx]) {
              vli = fstGetVarint32NoSkip(mem_for_traversal + headptr[idx]);
              tdelta = vli >> 1;

              if ((tdelta + i) >= tc_head_items) {
                chk_report_abort("TALOS-2023-1791");
              }

              scatterptr[idx] = tc_head[i + tdelta];
              tc_head[i + tdelta] = idx + 1;
            }
          }
        } else {
          uint32_t len = xc->signal_lens[idx];
          unsigned char *vdata;

          vli = fstGetVarint32(mem_for_traversal + headptr[idx], &skiplen);
          /* tdelta = vli >> 1; */ /* scan-build */
          vdata = mem_for_traversal + headptr[idx] + skiplen;

          if (xc->signal_typs[idx] != FST_VT_VCD_REAL) {
            if (len > xc->longest_signal_value_len) {
              chk_report_abort("TALOS-2023-1797");
            }

            if (!(vli & 1)) {
              int byte = 0;
              int bit;
              unsigned int j;

              for (j = 0; j < len; j++) {
                unsigned char ch;
                byte = j / 8;
                bit = 7 - (j & 7);
                ch = ((vdata[byte] >> bit) & 1) | '0';
                xc->temp_signal_value_buf[j] = ch;
              }
              xc->temp_signal_value_buf[j] = 0;

              if (value_change_callback) {
                value_change_callback(user_callback_data_pointer, time_table[i], idx + 1, xc->temp_signal_value_buf);
              } else {
                if (fv) {
                  unsigned char ch_bp = (xc->signal_typs[idx] != FST_VT_VCD_PORT) ? 'b' : 'p';

                  fstWritex(xc, &ch_bp, 1);
                  fstWritex(xc, xc->temp_signal_value_buf, len);
                }
              }

              len = byte + 1;
            } else {
              if (value_change_callback) {
                memcpy(xc->temp_signal_value_buf, vdata, len);
                xc->temp_signal_value_buf[len] = 0;
                value_change_callback(user_callback_data_pointer, time_table[i], idx + 1, xc->temp_signal_value_buf);
              } else {
                if (fv) {
                  unsigned char ch_bp = (xc->signal_typs[idx] != FST_VT_VCD_PORT) ? 'b' : 'p';
                  uint64_t mem_required_for_traversal_chk = vdata - mem_for_traversal + len;

                  fstWritex(xc, &ch_bp, 1);
                  if (mem_required_for_traversal_chk > mem_required_for_traversal) {
                    chk_report_abort("TALOS-2023-1793");
                  }
                  fstWritex(xc, vdata, len);
                }
              }
            }
          } else {
            double d;
            unsigned char *clone_d /*= (unsigned char *)&d */; /* scan-build */
            unsigned char buf[8];
            unsigned char *srcdata;

            if (!(vli & 1)) /* very rare case, but possible */
            {
              int bit;
              int j;

              for (j = 0; j < 8; j++) {
                unsigned char ch;
                bit = 7 - (j & 7);
                ch = ((vdata[0] >> bit) & 1) | '0';
                buf[j] = ch;
              }

              len = 1;
              srcdata = buf;
            } else {
              srcdata = vdata;
            }

            if (value_change_callback) {
              if (xc->native_doubles_for_cb) {
                if (xc->double_endian_match) {
                  clone_d = srcdata;
                } else {
                  int j;

                  clone_d = (unsigned char *)&d;
                  for (j = 0; j < 8; j++) {
                    clone_d[j] = srcdata[7 - j];
                  }
                }
                value_change_callback(user_callback_data_pointer, time_table[i], idx + 1, clone_d);
              } else {
                clone_d = (unsigned char *)&d;
                if (xc->double_endian_match) {
                  memcpy(clone_d, srcdata, 8);
                } else {
                  int j;

                  for (j = 0; j < 8; j++) {
                    clone_d[j] = srcdata[7 - j];
                  }
                }
                snprintf((char *)xc->temp_signal_value_buf, xc->longest_signal_value_len + 1, "%.16g", d);
                value_change_callback(user_callback_data_pointer, time_table[i], idx + 1, xc->temp_signal_value_buf);
              }
            } else {
              if (fv) {
                char wx_buf[32];
                int wx_len;

                clone_d = (unsigned char *)&d;
                if (xc->double_endian_match) {
                  memcpy(clone_d, srcdata, 8);
                } else {
                  int j;

                  for (j = 0; j < 8; j++) {
                    clone_d[j] = srcdata[7 - j];
                  }
                }

                wx_len = snprintf(wx_buf, 32, "r%.16g", d);
                fstWritex(xc, wx_buf, wx_len);
              }
            }
          }

          if (fv) {
            char vcd_id[16];
            int vcdid_len = fstVcdIDForFwrite(vcd_id + 1, idx + 1);
            vcd_id[0] = ' ';
            vcd_id[vcdid_len + 1] = '\n';
            fstWritex(xc, vcd_id, vcdid_len + 2);
          }

          skiplen += len;
          headptr[idx] += skiplen;
          length_remaining[idx] -= skiplen;

          tc_head[i] = scatterptr[idx];
          scatterptr[idx] = 0;

          if (length_remaining[idx]) {
            vli = fstGetVarint32NoSkip(mem_for_traversal + headptr[idx]);
            tdelta = vli >> 1;

            if ((tdelta + i) >= tc_head_items) {
              chk_report_abort("TALOS-2023-1791");
            }

            scatterptr[idx] = tc_head[i + tdelta];
            tc_head[i + tdelta] = idx + 1;
          }
        }
      }
    }

  block_err:
    free(tc_head);
    free(chain_cmem);
    free(mem_for_traversal);
    mem_for_traversal = NULL;

    secnum++;
    if (secnum == xc->vc_section_count)
      break; /* in case file is growing, keep with original block count */
    blkpos += seclen;
  }

  if (mem_for_traversal)
    free(mem_for_traversal); /* scan-build */
  free(length_remaining);
  free(headptr);
  free(scatterptr);

  if (chain_table)
    free(chain_table);
  if (chain_table_lengths)
    free(chain_table_lengths);

  free(time_table);

#ifndef FST_WRITEX_DISABLE
  if (fv) {
    fstWritex(xc, NULL, 0);
  }
#endif

  return (1);
}

/* rvat functions */

static char *fstExtractRvatDataFromFrame(struct fstReaderContext *xc, fstHandle facidx, char *buf) {
  if (facidx >= xc->rvat_frame_maxhandle) {
    return (NULL);
  }

  if (xc->signal_lens[facidx] == 1) {
    buf[0] = (char)xc->rvat_frame_data[xc->rvat_sig_offs[facidx]];
    buf[1] = 0;
  } else {
    if (xc->signal_typs[facidx] != FST_VT_VCD_REAL) {
      memcpy(buf, xc->rvat_frame_data + xc->rvat_sig_offs[facidx], xc->signal_lens[facidx]);
      buf[xc->signal_lens[facidx]] = 0;
    } else {
      double d;
      unsigned char *clone_d = (unsigned char *)&d;
      unsigned char *srcdata = xc->rvat_frame_data + xc->rvat_sig_offs[facidx];

      if (xc->double_endian_match) {
        memcpy(clone_d, srcdata, 8);
      } else {
        int j;

        for (j = 0; j < 8; j++) {
          clone_d[j] = srcdata[7 - j];
        }
      }

      snprintf((char *)buf, 32, "%.16g", d); /* this will write 18 bytes */
    }
  }

  return (buf);
}

char *fstReaderGetValueFromHandleAtTime(void *ctx, uint64_t tim, fstHandle facidx, char *buf) {
  struct fstReaderContext *xc = (struct fstReaderContext *)ctx;
  fst_off_t blkpos = 0, prev_blkpos;
  uint64_t beg_tim, end_tim, beg_tim2, end_tim2;
  int sectype;
#ifdef FST_DEBUG
  unsigned int secnum = 0;
#endif
  uint64_t seclen;
  uint64_t tsec_uclen = 0, tsec_clen = 0;
  uint64_t tsec_nitems;
  uint64_t frame_uclen, frame_clen;
#ifdef FST_DEBUG
  uint64_t mem_required_for_traversal;
#endif
  fst_off_t indx_pntr, indx_pos;
  long chain_clen;
  unsigned char *chain_cmem;
  unsigned char *pnt;
  fstHandle idx, pidx = 0, i;
  uint64_t pval;

  if ((!xc) || (!facidx) || (facidx > xc->maxhandle) || (!buf) || (!xc->signal_lens[facidx - 1])) {
    return (NULL);
  }

  if (!xc->rvat_sig_offs) {
    uint32_t cur_offs = 0;

    xc->rvat_sig_offs = (uint32_t *)calloc(xc->maxhandle, sizeof(uint32_t));
    for (i = 0; i < xc->maxhandle; i++) {
      xc->rvat_sig_offs[i] = cur_offs;
      cur_offs += xc->signal_lens[i];
    }
  }

  if (xc->rvat_data_valid) {
    if ((xc->rvat_beg_tim <= tim) && (tim <= xc->rvat_end_tim)) {
      goto process_value;
    }

    fstReaderDeallocateRvatData(xc);
  }

  xc->rvat_chain_pos_valid = 0;

  for (;;) {
    fstReaderFseeko(xc, xc->f, (prev_blkpos = blkpos), SEEK_SET);

    sectype = fgetc(xc->f);
    seclen = fstReaderUint64(xc->f);

    if ((sectype == EOF) || (sectype == FST_BL_SKIP) || (!seclen)) {
      return (NULL); /* if this loop exits on break, it's successful */
    }

    blkpos++;
    if ((sectype != FST_BL_VCDATA) && (sectype != FST_BL_VCDATA_DYN_ALIAS) && (sectype != FST_BL_VCDATA_DYN_ALIAS2)) {
      blkpos += seclen;
      continue;
    }

    beg_tim = fstReaderUint64(xc->f);
    end_tim = fstReaderUint64(xc->f);

    if ((beg_tim <= tim) && (tim <= end_tim)) {
      if ((tim == end_tim) && (tim != xc->end_time)) {
        fst_off_t cached_pos = ftello(xc->f);
        fstReaderFseeko(xc, xc->f, blkpos, SEEK_SET);

        sectype = fgetc(xc->f);
        seclen = fstReaderUint64(xc->f);

        beg_tim2 = fstReaderUint64(xc->f);
        end_tim2 = fstReaderUint64(xc->f);

        if (((sectype != FST_BL_VCDATA) && (sectype != FST_BL_VCDATA_DYN_ALIAS) &&
             (sectype != FST_BL_VCDATA_DYN_ALIAS2)) ||
            (!seclen) || (beg_tim2 != tim)) {
          blkpos = prev_blkpos;
          break;
        }
        beg_tim = beg_tim2;
        end_tim = end_tim2;
        fstReaderFseeko(xc, xc->f, cached_pos, SEEK_SET);
      }
      break;
    }

    blkpos += seclen;
#ifdef FST_DEBUG
    secnum++;
#endif
  }

  xc->rvat_beg_tim = beg_tim;
  xc->rvat_end_tim = end_tim;

#ifdef FST_DEBUG
  mem_required_for_traversal =
#endif
      fstReaderUint64(xc->f);

#ifdef FST_DEBUG
  fprintf(stderr, FST_APIMESS "rvat sec: %u seclen: %d begtim: %d endtim: %d\n", secnum, (int)seclen, (int)beg_tim,
          (int)end_tim);
  fprintf(stderr, FST_APIMESS "mem_required_for_traversal: %d\n", (int)mem_required_for_traversal);
#endif

  /* process time block */
  {
    unsigned char *ucdata;
    unsigned char *cdata;
    unsigned long destlen /* = tsec_uclen */;  /* scan-build */
    unsigned long sourcelen /* = tsec_clen */; /* scan-build */
    int rc;
    unsigned char *tpnt;
    uint64_t tpval;
    unsigned int ti;

    fstReaderFseeko(xc, xc->f, blkpos + seclen - 24, SEEK_SET);
    tsec_uclen = fstReaderUint64(xc->f);
    tsec_clen = fstReaderUint64(xc->f);
    tsec_nitems = fstReaderUint64(xc->f);
#ifdef FST_DEBUG
    fprintf(stderr, FST_APIMESS "time section unc: %d, com: %d (%d items)\n", (int)tsec_uclen, (int)tsec_clen,
            (int)tsec_nitems);
#endif
    ucdata = (unsigned char *)malloc(tsec_uclen);
    destlen = tsec_uclen;
    sourcelen = tsec_clen;

    fstReaderFseeko(xc, xc->f, -24 - ((fst_off_t)tsec_clen), SEEK_CUR);
    if (tsec_uclen != tsec_clen) {
      cdata = (unsigned char *)malloc(tsec_clen);
      fstFread(cdata, tsec_clen, 1, xc->f);

      rc = uncompress(ucdata, &destlen, cdata, sourcelen);

      if (rc != Z_OK) {
        fprintf(stderr, FST_APIMESS "fstReaderGetValueFromHandleAtTime(), tsec uncompress rc = %d, exiting.\n", rc);
        exit(255);
      }

      free(cdata);
    } else {
      fstFread(ucdata, tsec_uclen, 1, xc->f);
    }

    xc->rvat_time_table = (uint64_t *)calloc(tsec_nitems, sizeof(uint64_t));
    tpnt = ucdata;
    tpval = 0;
    for (ti = 0; ti < tsec_nitems; ti++) {
      int skiplen;
      uint64_t val = fstGetVarint64(tpnt, &skiplen);
      tpval = xc->rvat_time_table[ti] = tpval + val;
      tpnt += skiplen;
    }

    free(ucdata);
  }

  fstReaderFseeko(xc, xc->f, blkpos + 32, SEEK_SET);

  frame_uclen = fstReaderVarint64(xc->f);
  frame_clen = fstReaderVarint64(xc->f);
  xc->rvat_frame_maxhandle = fstReaderVarint64(xc->f);
  xc->rvat_frame_data = (unsigned char *)malloc(frame_uclen);

  if (frame_uclen == frame_clen) {
    fstFread(xc->rvat_frame_data, frame_uclen, 1, xc->f);
  } else {
    unsigned char *mc = (unsigned char *)malloc(frame_clen);
    int rc;

    unsigned long destlen = frame_uclen;
    unsigned long sourcelen = frame_clen;

    fstFread(mc, sourcelen, 1, xc->f);
    rc = uncompress(xc->rvat_frame_data, &destlen, mc, sourcelen);
    if (rc != Z_OK) {
      fprintf(stderr, FST_APIMESS "fstReaderGetValueFromHandleAtTime(), frame decompress rc: %d, exiting.\n", rc);
      exit(255);
    }
    free(mc);
  }

  xc->rvat_vc_maxhandle = fstReaderVarint64(xc->f);
  xc->rvat_vc_start = ftello(xc->f); /* points to '!' character */
  xc->rvat_packtype = fgetc(xc->f);

#ifdef FST_DEBUG
  fprintf(stderr, FST_APIMESS "frame_uclen: %d, frame_clen: %d, frame_maxhandle: %d\n", (int)frame_uclen,
          (int)frame_clen, (int)xc->rvat_frame_maxhandle);
  fprintf(stderr, FST_APIMESS "vc_maxhandle: %d\n", (int)xc->rvat_vc_maxhandle);
#endif

  indx_pntr = blkpos + seclen - 24 - tsec_clen - 8;
  fstReaderFseeko(xc, xc->f, indx_pntr, SEEK_SET);
  chain_clen = fstReaderUint64(xc->f);
  indx_pos = indx_pntr - chain_clen;
#ifdef FST_DEBUG
  fprintf(stderr, FST_APIMESS "indx_pos: %d (%d bytes)\n", (int)indx_pos, (int)chain_clen);
#endif
  chain_cmem = (unsigned char *)malloc(chain_clen);
  fstReaderFseeko(xc, xc->f, indx_pos, SEEK_SET);
  fstFread(chain_cmem, chain_clen, 1, xc->f);

  xc->rvat_chain_table = (fst_off_t *)calloc((xc->rvat_vc_maxhandle + 1), sizeof(fst_off_t));
  xc->rvat_chain_table_lengths = (uint32_t *)calloc((xc->rvat_vc_maxhandle + 1), sizeof(uint32_t));

  pnt = chain_cmem;
  idx = 0;
  pval = 0;

  if (sectype == FST_BL_VCDATA_DYN_ALIAS2) {
    uint32_t prev_alias = 0;

    do {
      int skiplen;

      if (*pnt & 0x01) {
        int64_t shval = fstGetSVarint64(pnt, &skiplen) >> 1;
        if (shval > 0) {
          pval = xc->rvat_chain_table[idx] = pval + shval;
          if (idx) {
            xc->rvat_chain_table_lengths[pidx] = pval - xc->rvat_chain_table[pidx];
          }
          pidx = idx++;
        } else if (shval < 0) {
          xc->rvat_chain_table[idx] = 0; /* need to explicitly zero as calloc above might not run */
          xc->rvat_chain_table_lengths[idx] = prev_alias =
              shval; /* because during this loop iter would give stale data! */
          idx++;
        } else {
          xc->rvat_chain_table[idx] = 0;                  /* need to explicitly zero as calloc above might not run */
          xc->rvat_chain_table_lengths[idx] = prev_alias; /* because during this loop iter would give stale data! */
          idx++;
        }
      } else {
        uint64_t val = fstGetVarint32(pnt, &skiplen);

        fstHandle loopcnt = val >> 1;
        for (i = 0; i < loopcnt; i++) {
          xc->rvat_chain_table[idx++] = 0;
        }
      }

      pnt += skiplen;
    } while (pnt != (chain_cmem + chain_clen));
  } else {
    do {
      int skiplen;
      uint64_t val = fstGetVarint32(pnt, &skiplen);

      if (!val) {
        pnt += skiplen;
        val = fstGetVarint32(pnt, &skiplen);
        xc->rvat_chain_table[idx] = 0;
        xc->rvat_chain_table_lengths[idx] = -val;
        idx++;
      } else if (val & 1) {
        pval = xc->rvat_chain_table[idx] = pval + (val >> 1);
        if (idx) {
          xc->rvat_chain_table_lengths[pidx] = pval - xc->rvat_chain_table[pidx];
        }
        pidx = idx++;
      } else {
        fstHandle loopcnt = val >> 1;
        for (i = 0; i < loopcnt; i++) {
          xc->rvat_chain_table[idx++] = 0;
        }
      }

      pnt += skiplen;
    } while (pnt != (chain_cmem + chain_clen));
  }

  free(chain_cmem);
  xc->rvat_chain_table[idx] = indx_pos - xc->rvat_vc_start;
  xc->rvat_chain_table_lengths[pidx] = xc->rvat_chain_table[idx] - xc->rvat_chain_table[pidx];

  for (i = 0; i < idx; i++) {
    int32_t v32 = xc->rvat_chain_table_lengths[i];
    if ((v32 < 0) && (!xc->rvat_chain_table[i])) {
      v32 = -v32;
      v32--;
      if (((uint32_t)v32) < i) /* sanity check */
      {
        xc->rvat_chain_table[i] = xc->rvat_chain_table[v32];
        xc->rvat_chain_table_lengths[i] = xc->rvat_chain_table_lengths[v32];
      }
    }
  }

#ifdef FST_DEBUG
  fprintf(stderr, FST_APIMESS "decompressed chain idx len: %" PRIu32 "\n", idx);
#endif

  xc->rvat_data_valid = 1;

/* all data at this point is loaded or resident in fst cache, process and return appropriate value */
process_value:
  if (facidx > xc->rvat_vc_maxhandle) {
    return (NULL);
  }

  facidx--; /* scale down for array which starts at zero */

  if (((tim == xc->rvat_beg_tim) && (!xc->rvat_chain_table[facidx])) || (!xc->rvat_chain_table[facidx])) {
    return (fstExtractRvatDataFromFrame(xc, facidx, buf));
  }

  if (facidx != xc->rvat_chain_facidx) {
    if (xc->rvat_chain_mem) {
      free(xc->rvat_chain_mem);
      xc->rvat_chain_mem = NULL;

      xc->rvat_chain_pos_valid = 0;
    }
  }

  if (!xc->rvat_chain_mem) {
    uint32_t skiplen;
    fstReaderFseeko(xc, xc->f, xc->rvat_vc_start + xc->rvat_chain_table[facidx], SEEK_SET);
    xc->rvat_chain_len = fstReaderVarint32WithSkip(xc->f, &skiplen);
    if (xc->rvat_chain_len) {
      unsigned char *mu = (unsigned char *)malloc(xc->rvat_chain_len);
      unsigned char *mc = (unsigned char *)malloc(xc->rvat_chain_table_lengths[facidx]);
      unsigned long destlen = xc->rvat_chain_len;
      unsigned long sourcelen = xc->rvat_chain_table_lengths[facidx];
      int rc = Z_OK;

      fstFread(mc, xc->rvat_chain_table_lengths[facidx], 1, xc->f);

      switch (xc->rvat_packtype) {
      case '4':
        rc =
            (destlen == (unsigned long)LZ4_decompress_safe_partial((char *)mc, (char *)mu, sourcelen, destlen, destlen))
                ? Z_OK
                : Z_DATA_ERROR;
        break;
      case 'F':
        fastlz_decompress(mc, sourcelen, mu, destlen); /* rc appears unreliable */
        break;
      default:
        rc = uncompress(mu, &destlen, mc, sourcelen);
        break;
      }

      free(mc);

      if (rc != Z_OK) {
        fprintf(stderr, FST_APIMESS "fstReaderGetValueFromHandleAtTime(), rvat decompress clen: %d (rc=%d), exiting.\n",
                (int)xc->rvat_chain_len, rc);
        exit(255);
      }

      /* data to process is for(j=0;j<destlen;j++) in mu[j] */
      xc->rvat_chain_mem = mu;
    } else {
      int destlen = xc->rvat_chain_table_lengths[facidx] - skiplen;
      unsigned char *mu = (unsigned char *)malloc(xc->rvat_chain_len = destlen);
      fstFread(mu, destlen, 1, xc->f);
      /* data to process is for(j=0;j<destlen;j++) in mu[j] */
      xc->rvat_chain_mem = mu;
    }

    xc->rvat_chain_facidx = facidx;
  }

  /* process value chain here */

  {
    uint32_t tidx = 0, ptidx = 0;
    uint32_t tdelta;
    int skiplen;
    unsigned int iprev = xc->rvat_chain_len;
    uint32_t pvli = 0;
    int pskip = 0;

    if ((xc->rvat_chain_pos_valid) && (tim >= xc->rvat_chain_pos_time)) {
      i = xc->rvat_chain_pos_idx;
      tidx = xc->rvat_chain_pos_tidx;
    } else {
      i = 0;
      tidx = 0;
      xc->rvat_chain_pos_time = xc->rvat_beg_tim;
    }

    if (xc->signal_lens[facidx] == 1) {
      while (i < xc->rvat_chain_len) {
        uint32_t vli = fstGetVarint32(xc->rvat_chain_mem + i, &skiplen);
        uint32_t shcnt = 2 << (vli & 1);
        tdelta = vli >> shcnt;

        if (xc->rvat_time_table[tidx + tdelta] <= tim) {
          iprev = i;
          pvli = vli;
          ptidx = tidx;
          /* pskip = skiplen; */ /* scan-build */

          tidx += tdelta;
          i += skiplen;
        } else {
          break;
        }
      }
      if (iprev != xc->rvat_chain_len) {
        xc->rvat_chain_pos_tidx = ptidx;
        xc->rvat_chain_pos_idx = iprev;
        xc->rvat_chain_pos_time = tim;
        xc->rvat_chain_pos_valid = 1;

        if (!(pvli & 1)) {
          buf[0] = ((pvli >> 1) & 1) | '0';
        } else {
          buf[0] = FST_RCV_STR[((pvli >> 1) & 7)];
        }
        buf[1] = 0;
        return (buf);
      } else {
        return (fstExtractRvatDataFromFrame(xc, facidx, buf));
      }
    } else {
      while (i < xc->rvat_chain_len) {
        uint32_t vli = fstGetVarint32(xc->rvat_chain_mem + i, &skiplen);
        tdelta = vli >> 1;

        if (xc->rvat_time_table[tidx + tdelta] <= tim) {
          iprev = i;
          pvli = vli;
          ptidx = tidx;
          pskip = skiplen;

          tidx += tdelta;
          i += skiplen;

          if (!(pvli & 1)) {
            i += ((xc->signal_lens[facidx] + 7) / 8);
          } else {
            i += xc->signal_lens[facidx];
          }
        } else {
          break;
        }
      }

      if (iprev != xc->rvat_chain_len) {
        unsigned char *vdata = xc->rvat_chain_mem + iprev + pskip;

        xc->rvat_chain_pos_tidx = ptidx;
        xc->rvat_chain_pos_idx = iprev;
        xc->rvat_chain_pos_time = tim;
        xc->rvat_chain_pos_valid = 1;

        if (xc->signal_typs[facidx] != FST_VT_VCD_REAL) {
          if (!(pvli & 1)) {
            int byte = 0;
            int bit;
            unsigned int j;

            for (j = 0; j < xc->signal_lens[facidx]; j++) {
              unsigned char ch;
              byte = j / 8;
              bit = 7 - (j & 7);
              ch = ((vdata[byte] >> bit) & 1) | '0';
              buf[j] = ch;
            }
            buf[j] = 0;

            return (buf);
          } else {
            memcpy(buf, vdata, xc->signal_lens[facidx]);
            buf[xc->signal_lens[facidx]] = 0;
            return (buf);
          }
        } else {
          double d;
          unsigned char *clone_d = (unsigned char *)&d;
          unsigned char bufd[8];
          unsigned char *srcdata;

          if (!(pvli & 1)) /* very rare case, but possible */
          {
            int bit;
            int j;

            for (j = 0; j < 8; j++) {
              unsigned char ch;
              bit = 7 - (j & 7);
              ch = ((vdata[0] >> bit) & 1) | '0';
              bufd[j] = ch;
            }

            srcdata = bufd;
          } else {
            srcdata = vdata;
          }

          if (xc->double_endian_match) {
            memcpy(clone_d, srcdata, 8);
          } else {
            int j;

            for (j = 0; j < 8; j++) {
              clone_d[j] = srcdata[7 - j];
            }
          }

          snprintf(buf, 32, "r%.16g", d); /* this will write 19 bytes */
          return (buf);
        }
      } else {
        return (fstExtractRvatDataFromFrame(xc, facidx, buf));
      }
    }
  }

  /* return(NULL); */
}
