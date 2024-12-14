#include "fstapi.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/************************/
/***                  ***/
/*** utility function ***/
/***                  ***/
/************************/

int fstUtilityBinToEscConvertedLen(const unsigned char *s, int len) {
  const unsigned char *src = s;
  int dlen = 0;
  int i;

  for (i = 0; i < len; i++) {
    switch (src[i]) {
    case '\a': /* fallthrough */
    case '\b': /* fallthrough */
    case '\f': /* fallthrough */
    case '\n': /* fallthrough */
    case '\r': /* fallthrough */
    case '\t': /* fallthrough */
    case '\v': /* fallthrough */
    case '\'': /* fallthrough */
    case '\"': /* fallthrough */
    case '\\': /* fallthrough */
    case '\?':
      dlen += 2;
      break;
    default:
      if ((src[i] > ' ') && (src[i] <= '~')) /* no white spaces in output */
      {
        dlen++;
      } else {
        dlen += 4;
      }
      break;
    }
  }

  return (dlen);
}

int fstUtilityBinToEsc(unsigned char *d, const unsigned char *s, int len) {
  const unsigned char *src = s;
  unsigned char *dst = d;
  unsigned char val;
  int i;

  for (i = 0; i < len; i++) {
    switch (src[i]) {
    case '\a':
      *(dst++) = '\\';
      *(dst++) = 'a';
      break;
    case '\b':
      *(dst++) = '\\';
      *(dst++) = 'b';
      break;
    case '\f':
      *(dst++) = '\\';
      *(dst++) = 'f';
      break;
    case '\n':
      *(dst++) = '\\';
      *(dst++) = 'n';
      break;
    case '\r':
      *(dst++) = '\\';
      *(dst++) = 'r';
      break;
    case '\t':
      *(dst++) = '\\';
      *(dst++) = 't';
      break;
    case '\v':
      *(dst++) = '\\';
      *(dst++) = 'v';
      break;
    case '\'':
      *(dst++) = '\\';
      *(dst++) = '\'';
      break;
    case '\"':
      *(dst++) = '\\';
      *(dst++) = '\"';
      break;
    case '\\':
      *(dst++) = '\\';
      *(dst++) = '\\';
      break;
    case '\?':
      *(dst++) = '\\';
      *(dst++) = '\?';
      break;
    default:
      if ((src[i] > ' ') && (src[i] <= '~')) /* no white spaces in output */
      {
        *(dst++) = src[i];
      } else {
        val = src[i];
        *(dst++) = '\\';
        *(dst++) = (val / 64) + '0';
        val = val & 63;
        *(dst++) = (val / 8) + '0';
        val = val & 7;
        *(dst++) = (val) + '0';
      }
      break;
    }
  }

  return (dst - d);
}

/*
 * this overwrites the original string if the destination pointer is NULL
 */
int fstUtilityEscToBin(unsigned char *d, unsigned char *s, int len) {
  unsigned char *src = s;
  unsigned char *dst = (!d) ? s : (s = d);
  unsigned char val[3];
  int i;

  for (i = 0; i < len; i++) {
    if (src[i] != '\\') {
      *(dst++) = src[i];
    } else {
      switch (src[++i]) {
      case 'a':
        *(dst++) = '\a';
        break;
      case 'b':
        *(dst++) = '\b';
        break;
      case 'f':
        *(dst++) = '\f';
        break;
      case 'n':
        *(dst++) = '\n';
        break;
      case 'r':
        *(dst++) = '\r';
        break;
      case 't':
        *(dst++) = '\t';
        break;
      case 'v':
        *(dst++) = '\v';
        break;
      case '\'':
        *(dst++) = '\'';
        break;
      case '\"':
        *(dst++) = '\"';
        break;
      case '\\':
        *(dst++) = '\\';
        break;
      case '\?':
        *(dst++) = '\?';
        break;

      case 'x':
        val[0] = toupper(src[++i]);
        val[1] = toupper(src[++i]);
        val[0] = ((val[0] >= 'A') && (val[0] <= 'F')) ? (val[0] - 'A' + 10) : (val[0] - '0');
        val[1] = ((val[1] >= 'A') && (val[1] <= 'F')) ? (val[1] - 'A' + 10) : (val[1] - '0');
        *(dst++) = val[0] * 16 + val[1];
        break;

      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
        val[0] = src[i] - '0';
        val[1] = src[++i] - '0';
        val[2] = src[++i] - '0';
        *(dst++) = val[0] * 64 + val[1] * 8 + val[2];
        break;

      default:
        *(dst++) = src[i];
        break;
      }
    }
  }

  return (dst - s);
}

struct fstETab *fstUtilityExtractEnumTableFromString(const char *s) {
  struct fstETab *et = NULL;
  int num_spaces = 0;
  int i;
  int newlen;

  if (s) {
    const char *csp = strchr(s, ' ');
    int cnt = atoi(csp + 1);

    for (;;) {
      csp = strchr(csp + 1, ' ');
      if (csp) {
        num_spaces++;
      } else {
        break;
      }
    }

    if (num_spaces == (2 * cnt)) {
      char *sp, *sp2;

      et = (struct fstETab *)calloc(1, sizeof(struct fstETab));
      et->elem_count = cnt;
      et->name = strdup(s);
      et->literal_arr = (char **)calloc(cnt, sizeof(char *));
      et->val_arr = (char **)calloc(cnt, sizeof(char *));

      sp = strchr(et->name, ' ');
      *sp = 0;

      sp = strchr(sp + 1, ' ');

      for (i = 0; i < cnt; i++) {
        sp2 = strchr(sp + 1, ' ');
        *(char *)sp2 = 0;
        et->literal_arr[i] = sp + 1;
        sp = sp2;

        newlen = fstUtilityEscToBin(NULL, (unsigned char *)et->literal_arr[i], strlen(et->literal_arr[i]));
        et->literal_arr[i][newlen] = 0;
      }

      for (i = 0; i < cnt; i++) {
        sp2 = strchr(sp + 1, ' ');
        if (sp2) {
          *sp2 = 0;
        }
        et->val_arr[i] = sp + 1;
        sp = sp2;

        newlen = fstUtilityEscToBin(NULL, (unsigned char *)et->val_arr[i], strlen(et->val_arr[i]));
        et->val_arr[i][newlen] = 0;
      }
    }
  }

  return (et);
}

void fstUtilityFreeEnumTable(struct fstETab *etab) {
  if (etab) {
    free(etab->literal_arr);
    free(etab->val_arr);
    free(etab->name);
    free(etab);
  }
}
