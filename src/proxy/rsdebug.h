/* debugging tools such as dump */
#ifndef RSDEBUG_H__
#define RSDEBUG_H__

#ifndef NO_CONFIG_H
#include "config.h"
#endif

#ifdef unix
#if HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#include <time.h>
#else
#define gettimeofday(X,Y) 1
#endif
#include <stdio.h>

#if defined RSERV_DEBUG || defined RSERV_IOLOG

static int io_log = 0;  /* log all I/O operations */
static int dumpLimit = 128;
static double first_ts = 0.0;
static char io_log_fn[128];

static void fprintDump(FILE *f, const void *b, int len) {
    int i = 0;
    if (len < 1) { fprintf(f, "DUMP FAILED (len=%d)\n", len); };
    fprintf(f, "DUMP [%d]:",len);
    while(i < len) {
	fprintf(f, " %02x",((const unsigned char*)b)[i++]);
	if (dumpLimit && i > dumpLimit) { fprintf(f, " ..."); break; };
    }
    i = 0;
    fprintf(f, "  |");
    while (i < len) {
	unsigned char c = ((const unsigned char*)b)[i++];
	if (c < 32 || c > 127) c = '.';
	fputc(c, f);
	if (dumpLimit && i > dumpLimit) break;
    }
    fprintf(f, "\n");
}
#define printDump(B,L) fprintDump(stdout, B, L)

#endif
#endif
