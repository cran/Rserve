/*
 *  Rserv : R-server that allows to use embedded R via TCP/IP
 *          currently based on R-1.5.1 API (tested up to R 1.8.1)
 *  Copyright (C) 2002,3 Simon Urbanek
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  $Id: Rserv.c 179 2006-11-29 14:52:47Z urbanek $
 */

/* external defines:

   COOPERATIVE - forces cooperative version of Rserv on unix platforms
                 (default for non-unix platforms)

   THREADED    - results in threaded version of this server, i.e. each
                 new connection is run is a separate thread. Beware:
				 this approach is not recommended since R does not support
				 real multithreading yet

   FORKED      - each connection is forked to a new process. This is the
                 recommended way to use this server. The advantage is (beside
				 the fact that this works ;)) that each client has a separate
				 namespace since the processes are independent
				 (default for unix platforms)

   SWAPEND     - define if the platform has byte order inverse to Intel (like PPC)

   RSERV_DEBUG - if defined various verbose output is produced

   NOFULL   - dumps show first 100 bytes only
              (removed in 0.3-3 and replaced by dumpLimit variable)

   DAEMON      - if defined the server daemonizes (unix only)

   CONFIG_FILE - location of the config file (default /etc/Rserv.conf)

   FORCE_V0100 - if this macro is defined then Rserve reports version 0100 and
                 CMD_eval doesn't send data type header (DT_SEXP+length). This
				 was a buggy behavior in versions up to 0.1-9. This feature is
				 provided only for compatibility with old clients and should be
				 avoided. Update the clients instead, if possible.
				 (Warning: since 0.3 this feature is untested and not likely
				 to work!)

  reported versions:
 --------------------
 0100 - Rserve 0.1-1 .. 0.1-9
        CMD_eval sends SEXP directly without the data type header. This is in
		fact an inconsistency and was fixed in 0101. New clients should be aware
		of this and support this behavior or reject 0100 connections.

 0101 - Rserve 0.1-10 .. 0.2-x

 0102 - Rserve 0.3
        added support for large parameters/expressions

The current implementation uses DT_LARGE/XT_LARGE only for SEXPs larger than 0xfffff0.
No commands except for CMD_set/assignREXP with DT_REXP accept large input,
in particular all file operations. All objects smaller 8MB should be encoded without
the use of DT_LARGE/XT_LARGE.
          
*/

/* config file entries: [default]
   ----------------------
   workdir <path> [depends on the CONFIG_FILE define]
   pwdfile <file> [none=disabled]
   remote enable|disable [disable]
   auth required|disable [disable]
   plaintext enable|disable [disable] (strongly discouraged to enable)
   fileio enable|disable [enable]

   socket <unix-socket-name> [none]
   maxinbuf <size in kB> [262144 = 256MB]
   maxsendbuf <size in kB> [0 = no limit]
   
   unix only (works only if Rserve was started by root):
   uid <uid>
   gid <gid>

   source <file>
   eval <expression(s)>

   A note about security: Anyone with access to R has access to the shell
   via "system" command, so you should consider following rules:
   - NEVER EVER run Rserv as root (unless uid/gid is used) - this compromises
     the box totally
   - use "remote disable" whenever you don't need remote access.
   - if you need remote access use "auth required" and "plaintext disable"
     consider also that anyone with the access can decipher other's passwords
	 if he knows how to. the authentication prevents hackers from the net
	 to break into Rserv, but it doesn't (and cannot) protect from
	 inside attacks (since R has no security measures).
	 You should also use a special, restricted user for running Rserv
	 as a public server, so noone can try to hack the box it runs on.
   - don't enable plaintext unless you really have to. Passing passwords
     in plain text over the net is not wise and not necessary since both
	 Rserv and JRclient provide encrypted passwords with server-side
	 challenge (thus safe from sniffing).
*/

#define USE_RINTERNALS
#define SOCK_ERRORS
#define LISTENQ 16
#define MAIN

#if defined NODAEMON && defined DAEMON
#undef DAEMON
#endif

/* MacOS X hack. gcc on any (non-windows) platform is treated as unix */
#if defined __GNUC__ && !defined unix && !defined Win32
#define unix
#endif

/* FORKED is default for unix platforms */
#if defined unix && !defined THREADED && !defined COOPERATIVE && !defined FORKED
#define FORKED
#endif

#ifndef CONFIG_FILE
#ifdef unix
#define CONFIG_FILE "/etc/Rserv.conf"
#else
#define CONFIG_FILE "Rserv.cfg"
#endif
#endif

/* we have no configure for Win32 so we have to take care of socklen_t */
#ifdef Win32
typedef int socklen_t;
#define CAN_TCP_NODELAY
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sisocks.h>
#include <string.h>
#ifdef unix
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <unistd.h>
#include <sys/un.h> /* needed for unix sockets */
#endif
#ifdef THREADED
#include <sbthread.h>
#endif
#ifdef FORKED
#include <sys/wait.h>
#endif
#include <R.h>
#include <Rinternals.h>
#include <Rdefines.h>
#include <Rversion.h>
#if R_VERSION < 0x2010
#include "Parse.h"
#else
#include <R_ext/Parse.h>
#endif
#include "Rsrv.h"
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#if defined HAVE_NETINET_TCP_H && defined HAVE_NETINET_IN_H
#define CAN_TCP_NODELAY
#include <netinet/tcp.h>
#include <netinet/in.h>
#endif

/* AF_LOCAL is the POSIX version of AF_UNIX - we need this e.g. for AIX */
#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif

/* send buffer size (default 2MB)
   Currently Rserve stores entire responses in memory before sending it.
   This is not really neccessary and may (hopefully will) change in the future.
   Send buffer specifies the maximal amount of data sent from Rserve to
   the client in one response.
*/
#ifndef sndBS /* configure may have defined one already */
#define sndBS (2048*1024)
#endif

/* the # of arguments to R_ParseVector changed since R 2.5.0 */
#if R_VERSION < R_Version(2,5,0)
#define RS_ParseVector R_ParseVector
#else
#define RS_ParseVector(A,B,C) R_ParseVector(A,B,C,R_NilValue)
#endif

int dumpLimit=128;

int port = default_Rsrv_port;
int active = 1; /* 1=server loop is active, 0=shutdown */
int UCIX   = 1; /* unique connection index */

char *localSocketName = 0; /* if set listen on this local (unix) socket instead of TCP/IP */

int allowIO=1;  /* 1=allow I/O commands, 0=don't */

char **top_argv;
int top_argc;

char *workdir="/tmp/Rserv";
char *pwdfile=0;

SOCKET csock=-1;

int parentPID=-1;

int maxSendBufSize=0; /* max. sendbuf for auto-resize. 0=no limit */

static char **allowed_ips = 0;

#ifdef THREADED
int localUCIX;
#else
#define localUCIX UCIX
#endif

#ifdef RSERV_DEBUG
void printDump(void *b, int len) {
    int i=0;
    if (len<1) { printf("DUMP FAILED (len=%d)\n",len); };
    printf("DUMP [%d]:",len);
    while(i<len) {
		printf(" %02x",((unsigned char*)b)[i++]);
		if(dumpLimit && i>dumpLimit) { printf(" ..."); break; };
    }
    printf("\n");
}
#endif

void sendResp(int s, int rsp) {
    struct phdr ph;
    memset(&ph,0,sizeof(ph));
    ph.cmd=itop(rsp|CMD_RESP);
#ifdef RSERV_DEBUG
    printf("OUT.sendResp(void data)\n");
    printDump(&ph,sizeof(ph));
#endif
    send(s,&ph,sizeof(ph),0);
}

char *getParseName(int n) {
    switch(n) {
    case PARSE_NULL: return "null";
    case PARSE_OK: return "ok";
    case PARSE_INCOMPLETE: return "incomplete";
    case PARSE_ERROR: return "error";
    case PARSE_EOF: return "EOF";
    }
    return "<unknown>";
}

/* this is the type used to calculate pointer distances
   we should re-define it to 64-bit type on 64-bit archs */
typedef unsigned long rlen_t;
#define rlen_max 0xffffffff 

#define attrFixup if (hasAttr) buf=storeSEXP(buf,ATTRIB(x));
#define dist(A,B) (((rlen_t)(((char*)B)-((char*)A)))-4)

rlen_t getStorageSize(SEXP x) {
    int t=TYPEOF(x);
    unsigned int tl=LENGTH(x);
    rlen_t len=4;
    
#ifdef RSERV_DEBUG
    printf("getStorageSize(type=%d,len=%d)\n",t,tl);
#endif
    if (TYPEOF(ATTRIB(x))>0) {
		rlen_t alen=getStorageSize(ATTRIB(x));
		len+=alen;
    }
    switch (t) {
    case LISTSXP:
    case LANGSXP:
		len+=getStorageSize(CAR(x));
		len+=getStorageSize(CDR(x));
		len+=getStorageSize(TAG(x));
		break;
    case CLOSXP:
		len+=getStorageSize(FORMALS(x));
		len+=getStorageSize(BODY(x));
		break;
    case REALSXP:
		len+=tl*8; break;
    case INTSXP:
		len+=tl*4; break;
    case LGLSXP:
	case RAWSXP:
		if (tl>1)
			len+=4+((tl+3)&0xfffffffc);
		else
			len+=4;	
		break;
		
    case CHARSXP:
		{
			char *ct=(char*)STRING_PTR(x);
			if (!ct)
				len+=4;
			else {
				unsigned int sl=strlen(ct)+1;
				sl=(sl+3)&0xfffffffc;
				len+=sl;
			}
		}
		break;
    case SYMSXP:
		len+=getStorageSize(PRINTNAME(x));
		break;
    case STRSXP:
		if (tl==1)
			return getStorageSize(VECTOR_ELT(x,0));
    case EXPRSXP:
    case VECSXP:
		{
			int i=0;
			while(i<LENGTH(x)) {
				len+=getStorageSize(VECTOR_ELT(x,i));
				i++;
			}
		}
		break;
    default:
		len+=4; /* unknown types are simply stored as int */
    }
    if (len>0xfffff0) /* large types must be stored in the new format */
		len+=4;
    return len;
}

unsigned int* storeSEXP(unsigned int* buf, SEXP x) {
    int t=TYPEOF(x);
    int i;
    int hasAttr=0;
    int isLarge=0;
    unsigned int *preBuf=buf;
    rlen_t txlen;
    
    if (!x) { /* null pointer will be treated as XT_NULL */
		*buf=itop(XT_NULL); buf++; goto didit;
    }
    
    if (TYPEOF(ATTRIB(x))>0) hasAttr=XT_HAS_ATTR;
    
    if (t==NILSXP) {
		*buf=itop(XT_NULL|hasAttr);
		buf++;
		attrFixup;
		goto didit;
    } 
    
    /* check storage size */
    txlen=getStorageSize(x);
    if (txlen>0xfffff0) { /* if the entry is too big, use large format */
		isLarge=1;
		buf++;
    }
    
    if (t==LISTSXP) {
		*buf=itop(XT_LIST|hasAttr);
		buf++;
		attrFixup;
		buf=storeSEXP(buf,CAR(x));
		buf=storeSEXP(buf,CDR(x));    
		buf=storeSEXP(buf,TAG(x));  /* since 1.22 (0.1-5) we store TAG as well */
		goto didit;
    }
    
    if (t==LANGSXP) { /* LANG are simply special lists */
		*buf=itop(XT_LANG|hasAttr);
		buf++;
		attrFixup;
		/* before 1.22 (0.1-5) contents was ignored */
		buf=storeSEXP(buf,CAR(x));
		buf=storeSEXP(buf,CDR(x));
		buf=storeSEXP(buf,TAG(x));  /* since 1.22 (0.1-5) we store TAG as well */
		goto didit;
    }
	
    if (t==CLOSXP) { /* closures (send FORMALS and BODY) */
		*buf=itop(XT_CLOS|hasAttr);
		buf++;
		attrFixup;
		buf=storeSEXP(buf,FORMALS(x));
		buf=storeSEXP(buf,BODY(x));
		goto didit;
    }
    
    if (t==REALSXP) {
		*buf=itop(XT_ARRAY_DOUBLE|hasAttr);
		buf++;
		attrFixup;
		i=0;
		while(i<LENGTH(x)) {
			fixdcpy(buf,REAL(x)+i);
			buf+=2; /* sizeof(double)=2*sizeof(int) */
			i++;
		}
		goto didit;
    }

	if (t==RAWSXP) {
		int ll=LENGTH(x);
		*buf=itop(XT_RAW|hasAttr);
		buf++;
		attrFixup;
		*buf=itop(ll); buf++;
		if (ll) memcpy(buf, RAW(x), ll);
		ll+=3; ll/=4;
		buf+=ll;
		goto didit;
	}
		
    if (t==LGLSXP) {
		int ll=LENGTH(x);
		int *lgl = LOGICAL(x);
		*buf=itop(((ll!=1)?XT_ARRAY_BOOL:XT_BOOL)|hasAttr);
		buf++;
		attrFixup;
		if (ll!=1) {
			*buf=itop(ll); buf++;
		}
		i=0;
		while(i<ll) { /* logical values are stored as bytes of values 0/1/2 */
			int bv=lgl[i];
			*((unsigned char*)buf)=(bv==0)?0:(bv==1)?1:2;
			buf=(unsigned int*)(((unsigned char*)buf)+1);
			i++;
		}
		/* pad by 0xff to a multiple of 4 */
		while (i&3) { *((unsigned char*)buf)=0xff; i++; buf=(unsigned int*)(((unsigned char*)buf)+1); };
		goto didit;
    }
    
    if (t==EXPRSXP || t==VECSXP || t==STRSXP) {
		if (t==STRSXP && LENGTH(x)==1) {
			buf=storeSEXP(buf,VECTOR_ELT(x,0));
			goto skipall; /* need to skip fixup since we didn't store anything */
		} else {
			*buf=itop(XT_VECTOR|hasAttr);
			buf++;
			attrFixup;
			i=0;
			while(i<LENGTH(x)) {
				buf=storeSEXP(buf,VECTOR_ELT(x,i));
				i++;
			}
		}
		goto didit;
    }
	
    if (t==INTSXP) {
		*buf=itop(XT_ARRAY_INT|hasAttr);
		buf++;
		attrFixup;
		i=0;
		while(i<LENGTH(x)) {
			*buf=itop(INTEGER(x)[i]);
			buf++;
			i++;
		}
		goto didit;
    }
	
    if (t==CHARSXP) {
		int sl;
		*buf=itop(XT_STR|hasAttr);
		buf++;
		attrFixup;
		strcpy((char*)buf,(char*)STRING_PTR(x));
		sl=strlen((char*)buf)+1;
		while (sl&3) { /* pad by 0 to a length divisible by 4 (since 0.1-10) */
			buf[sl]=0; sl++;
		}
		buf=(unsigned int*)(((char*)buf)+sl);
		goto didit;
    }
	
    if (t==SYMSXP) {
		*buf=itop(XT_SYM|hasAttr);
		buf++;
		attrFixup;
		buf=storeSEXP(buf,PRINTNAME(x));
		goto didit;
    }
	
    *buf=itop(XT_UNKNOWN|hasAttr);
    buf++;
    attrFixup;
    *buf=itop(TYPEOF(x));
    buf++;
    
 didit:
    if (isLarge) {
		txlen=dist(preBuf,buf)-4;
		preBuf[0]=itop(SET_PAR(PAR_TYPE(((unsigned char*) preBuf)[4]|XT_LARGE),txlen&0xffffff));
		preBuf[1]=itop(txlen>>24);
    } else
		*preBuf=itop(SET_PAR(PAR_TYPE(ptoi(*preBuf)),dist(preBuf,buf)));
    
 skipall:
    return buf;
}

void printSEXP(SEXP e) /* merely for debugging purposes
						  in fact Rserve binary transport supports
						  more types than this function. */
{
    int t=TYPEOF(e);
    int i;
    
    if (t==NILSXP) {
		printf("NULL value\n");
		return;
    }
    if (t==LANGSXP) {
		printf("language construct\n");
		return;
    }
    if (t==REALSXP) {
		if (LENGTH(e)>1) {
			printf("Vector of real variables: ");
			i=0;
			while(i<LENGTH(e)) {
				printf("%f",REAL(e)[i]);
				if (i<LENGTH(e)-1) printf(", ");
				if (dumpLimit && i>dumpLimit) {
					printf("..."); break;
				}
				i++;
			}
			putchar('\n');
		} else
			printf("Real variable %f\n",*REAL(e));
		return;
    }
    if (t==RAWSXP) {
		printf("Raw vector: ");
		i=0;
		while(i<LENGTH(e)) {
			printf("%02x",((unsigned int)((unsigned char*)RAW(e))[i])&0xff);
			if (i<LENGTH(e)-1) printf(" ");
			if (dumpLimit && i>dumpLimit) {
				printf("..."); break;
			}
			i++;
		}
		putchar('\n');
		return;
    }
    if (t==EXPRSXP) {
		printf("Vector of %d expressions:\n",LENGTH(e));
		i=0;
		while(i<LENGTH(e)) {
			if (dumpLimit && i>dumpLimit) { printf("..."); break; };
			printSEXP(VECTOR_ELT(e,i));
			i++;
		}
		return;
    }
    if (t==INTSXP) {
		printf("Vector of %d integers:\n",LENGTH(e));
		i=0;
		while(i<LENGTH(e)) {
			if (dumpLimit && i>dumpLimit) { printf("..."); break; }
			printf("%d",INTEGER(e)[i]);
			if (i<LENGTH(e)-1) printf(", ");
			i++;
		}
		putchar('\n');
		return;
    }
    if (t==VECSXP) {
		printf("Vector of %d fields:\n",LENGTH(e));
		i=0;
		while(i<LENGTH(e)) {
			if (dumpLimit && i>dumpLimit) { printf("..."); break; };
			printSEXP(VECTOR_ELT(e,i));
			i++;
		}
		return;
    }
    if (t==STRSXP) {
		i=0;
		printf("String vector of length %d:\n",LENGTH(e));
		while(i<LENGTH(e)) {
			if (dumpLimit && i>dumpLimit) { printf("..."); break; };
			printSEXP(VECTOR_ELT(e,i)); i++;
		}
		return;
    }
    if (t==CHARSXP) {
		printf("scalar string: \"%s\"\n",(char*) STRING_PTR(e));
		return;
    }
    if (t==SYMSXP) {
		printf("Symbol, name: "); printSEXP(PRINTNAME(e));
		return;
    }
    printf("Unknown type: %d\n",t);
}

/* decode_toSEXP is used to decode SEXPs from binary form and create
   corresponding objects in R. UPC is a pointer to a counter of
   UNPROTECT calls which will be necessary after we're done.
   The buffer position is advanced to the point where the SEXP ends
   (more precisely it points to the next stored SEXP). */
SEXP decode_to_SEXP(unsigned int **buf, int *UPC)
{
    unsigned int *b=*buf;
    char *c,*cc;
    SEXP val=0;
    int ty=PAR_TYPE(ptoi(*b));
    rlen_t ln=PAR_LEN(ptoi(*b));
    int i,j,l;
    
    if (IS_LARGE(ty)) {
		ty&=0xbf;
		b++;
		ln|=(ptoi(*b))<<24;
    }
#ifdef RSERV_DEBUG
    printf("decode: type=%x, len=%ld\n", ty, (long)ln);
#endif
    b++;
    
    switch(ty) {
    case XT_INT:
    case XT_ARRAY_INT:
		l=ln/4;
		PROTECT(val=NEW_INTEGER(l));
		(*UPC)++;
		i=0;
		while (i<l) {
			INTEGER(val)[i]=ptoi(*b); i++; b++;
		}
		*buf=b;
		break;
    case XT_DOUBLE:
    case XT_ARRAY_DOUBLE:
		l=ln/8;
		PROTECT(val=NEW_NUMERIC(l)); (*UPC)++;
		i=0;
		while (i<l) {
			fixdcpy(REAL(val)+i,b);
			i++; b+=2;
		}
		*buf=b;
		break;
    case XT_STR:
    case XT_ARRAY_STR:
		i=j=0;
		c=(char*)(b);
		while(i<ln) {
			if (!*c) j++;
			c++;
			i++; 
		}
		PROTECT(val=NEW_STRING(j)); (*UPC)++;
		i=j=0; c=(char*)b; cc=c;
		while(i<ln) {
			if (!*c) {
				VECTOR_ELT(val,j)=mkChar(cc);
				j++; cc=c+1;
			}
			c++; i++;
		}
		*buf=(unsigned int*)cc;
		break;
	case XT_RAW:
		i=ptoi(*b);
		b++;
		PROTECT(val=allocVector(RAWSXP, i)); (*UPC)++;
		memcpy(RAW(val), b, i);
		b+=ln/4 - 1; /* ln will include the length field */
		*buf=b;
		break;
    }
    return val;
}

/* if set Rserve doesn't accept other than local connections. */
int localonly=1;

/* server socket */
SOCKET ss;

/* arguments structure passed to a working thread */
struct args {
    int s;
    int ss;
    SAIN sa;
    int ucix;
#ifdef unix
    struct sockaddr_un su;
#endif
};

/* send a response including the data part */
void sendRespData(int s, int rsp, int len, void *buf) {
    struct phdr ph;
    memset(&ph,0,sizeof(ph));
    ph.cmd=itop(rsp|CMD_RESP);
    ph.len=itop(len);
#ifdef RSERV_DEBUG
    printf("OUT.sendRespData\nHEAD ");
    printDump(&ph,sizeof(ph));
    printf("BODY ");
    printDump(buf,len);
#endif
    
    send(s,&ph,sizeof(ph),0);
    send(s,buf,len,0);
}

/* initial ID string */
#ifdef FORCE_V0100
char *IDstring="Rsrv0100QAP1\r\n\r\n--------------\r\n";
#else
char *IDstring="Rsrv0102QAP1\r\n\r\n--------------\r\n";
#endif

/* require authentication flag (default: no) */
int authReq=0;
/* use plain password flag (default: no) */
int usePlain=0;

/* max. size of the input buffer (per connection) */
int maxInBuf=256*(1024*1024); /* default is 256MB */

struct source_entry {
    struct source_entry* next;
    char line[8];
} *src_list=0, *src_tail=0;

/* load config file */
int loadConfig(char *fn)
{
    FILE *f;
    char buf[512];
    char *c,*p,*c1;
    
#ifdef RSERV_DEBUG
    printf("Loading config file %s\n",fn);
#endif
    f=fopen(fn,"r");
    if (!f) {
#ifdef RSERV_DEBUG
		printf("Failed to find config file %s\n",fn);
#endif
		return -1;
    }
    buf[511]=0;
    while(!feof(f))
		if (fgets(buf,511,f)) {
			c=buf;
			while(*c==' '||*c=='\t') c++;
			p=c;
			while(*p && *p!='\t' && *p!=' ' && *p!='=' && *p!=':') p++;
			if (*p) {
				*p=0;
				p++;
				while(*p && (*p=='\t' || *p==' ')) p++;
			}
			c1=p;
			while(*c1) if(*c1=='\n'||*c1=='\r') *c1=0; else c1++;
#ifdef RSERV_DEBUG
			printf("conf> command=\"%s\", parameter=\"%s\"\n", c, p);
#endif
			if (!strcmp(c,"remote"))
				localonly=(*p=='1' || *p=='y' || *p=='e')?0:1;
			if (!strcmp(c,"port")) {
				if (*p) {
					int np=atoi(p);
					if (np>0) port=np;
				}
			}
			if (!strcmp(c,"maxinbuf")) {
				if (*p) {
					int ns=atoi(p);
					if (ns>32)
						maxInBuf=ns*1024;
				}
			}
			if (!strcmp(c,"source") || !strcmp(c,"eval")) {
#ifdef RSERV_DEBUG
				printf("Found source entry \"%s\"\n", p);
#endif
				if (*p) {
					struct source_entry* se= (struct source_entry*) malloc(sizeof(struct source_entry)+strlen(p)+16);
					if (!strcmp(c,"source")) {
						strcpy(se->line, "try(source(\"");
						strcat(se->line, p);
						strcat(se->line, "\"))");
					} else
						strcpy(se->line, p);
					se->next=0;
					if (!src_tail)
						src_tail=src_list=se;
					else {
						src_tail->next=se;
						src_tail=se;
					}
				}
			}
			if (!strcmp(c,"maxsendbuf")) {
				if (*p) {
					int ns=atoi(p);
					if (ns>32)
						maxSendBufSize=ns*1024;
				}
			}
#ifdef unix
			if (!strcmp(c,"uid") && *p) {
				int nuid=atoi(p);
				if (setuid(nuid))
					fprintf(stderr,"setuid(%d): failed. no user switch performed.",nuid);
			}
			if (!strcmp(c,"gid") && *p) {
				int ngid=atoi(p);
				if (setgid(ngid))
					fprintf(stderr,"setgid(%d): failed. no group switch performed.",ngid);
			}
#endif
			if (!strcmp(c,"allow")) {
				if (*p) {
					char **l;
					if (!allowed_ips) {
						allowed_ips=(char**) malloc(sizeof(char*)*128);
						*allowed_ips=0;
					}
					l=allowed_ips;
					while (*l) l++;
					if (l-allowed_ips>=127)
						fprintf(stderr, "Maximum of allowed IPs (127) exceeded, ignoring 'allow %s'\n", p);
					else {
						*l=strdup(p);
						l++;
						*l=0;
					}
				}
			}
			if (!strcmp(c,"workdir")) {
				if (*p) {
					workdir=(char*)malloc(strlen(p)+1);
					strcpy(workdir,p);
				} else workdir=0;
			}
			if (!strcmp(c,"socket")) {
				if (*p) {
					localSocketName=(char*)malloc(strlen(p)+1);
					strcpy(localSocketName,p);
				} else localSocketName=0;
			}
			if (!strcmp(c,"pwdfile")) {
				if (*p) {
					pwdfile=(char*)malloc(strlen(p)+1);
					strcpy(pwdfile,p);
				} else pwdfile=0;
			}
			if (!strcmp(c,"auth"))
				authReq=(*p=='1' || *p=='y' || *p=='r' || *p=='e')?1:0;
			if (!strcmp(c,"plaintext"))
				usePlain=(*p=='1' || *p=='y' || *p=='e')?1:0;
			if (!strcmp(c,"fileio"))
				allowIO=(*p=='1' || *p=='y' || *p=='e')?1:0;
		}
    fclose(f);
#ifndef HAS_CRYPT
    if (!usePlain) {
		fprintf(stderr,"Warning: useplain=no, but this Rserve has no crypt support!\nSet useplain=yes or compile with crypt support (if your system supports crypt).\nFalling back to plain text password.\n");
		usePlain=1;
    }
#endif
#ifdef RSERV_DEBUG
    printf("Loaded config file %s\n",fn);
#endif
    return 0;
}

/* size of the input buffer (default 512kB)
   was 2k before 1.23, but since 1.22 we support CMD_assign/set and hence
   the incoming packets can be substantially bigger.

   since 1.29 we support input buffer resizing,
   therefore we start with a small buffer and allocate more if necessary
*/

int inBuf=32768; /* 32kB should be ok unless CMD_assign sends large data */

/* static buffer size used for file transfer.
   The user is still free to allocate its own size  */
#define sfbufSize 32768 /* static file buffer size */

#ifndef decl_sbthread
#define decl_sbthread void
#endif

/* pid of the last child (not really used ATM) */
int lastChild;

#ifdef FORKED
void sigHandler(int i) {
    if (i==SIGTERM || i==SIGHUP)
		active=0;
}

void brkHandler(int i) {
    printf("\nCaught break signal, shutting down Rserve.\n");
    active=0;
    /* kill(getpid(), SIGUSR1); */
}
#endif

/* used for generating salt code (2x random from this array) */
const char *code64="./0123456789ABCDEFGHIJKLMNOPQRSTUVWYXZabcdefghijklmnopqrstuvwxyz";

/** parses a string, stores the number of expressions in parts and the resulting statis in status.
    the returned SEXP may contain multiple expressions */ 
SEXP parseString(char *s, int *parts, ParseStatus *status) {
    int maxParts=1;
    char *c=s;
    SEXP cv, pr = R_NilValue;
    
    while (*c) {
		if (*c=='\n' || *c==';') maxParts++;
		c++;
    }
    
    PROTECT(cv=allocVector(STRSXP, 1));
    SET_VECTOR_ELT(cv, 0, mkChar(s));  
    
    while (maxParts>0) {
		pr=RS_ParseVector(cv, maxParts, status);
		if (*status!=PARSE_INCOMPLETE && *status!=PARSE_EOF) break;
		maxParts--;
    }
    UNPROTECT(1);
    *parts=maxParts;
    
    return pr;
}

/** parse a string containing the specified number of expressions */
SEXP parseExps(char *s, int exps, ParseStatus *status) {
    SEXP cv, pr;
    
    PROTECT(cv=allocVector(STRSXP, 1));
    SET_VECTOR_ELT(cv, 0, mkChar(s));  
    pr=RS_ParseVector(cv, 1, status);
    UNPROTECT(1);
    return pr;
}

void voidEval(char *cmd) {
    ParseStatus stat;
    int Rerror;
    int j=0;
    SEXP xp=parseString(cmd,&j,&stat);
    
    PROTECT(xp);
#ifdef RSERV_DEBUG
    printf("voidEval: buffer parsed, stat=%d, parts=%d\n",stat,j);
    if (xp)
		printf("result type: %d, length: %d\n",TYPEOF(xp),LENGTH(xp));
    else
		printf("result is <null>\n");
#endif
    if (stat!=1) {
		UNPROTECT(1);
		return;
    } else {
		SEXP exp=R_NilValue;
#ifdef RSERV_DEBUG
		printf("R_tryEval(xp,R_GlobalEnv,&Rerror);\n");
#endif
		if (TYPEOF(xp)==EXPRSXP && LENGTH(xp)>0) {
			int bi=0;
			while (bi<LENGTH(xp)) {
				SEXP pxp=VECTOR_ELT(xp, bi);
				Rerror=0;
#ifdef RSERV_DEBUG
				printf("Calling R_tryEval for expression %d [type=%d] ...\n",bi+1,TYPEOF(pxp));
#endif
				exp=R_tryEval(pxp, R_GlobalEnv, &Rerror);
				bi++;
#ifdef RSERV_DEBUG
				printf("Expression %d, error code: %d\n",bi, Rerror);
				if (Rerror) printf(">> early error, aborting further evaluations\n");
#endif
				if (Rerror) break;
			}
		} else {
			Rerror=0;
			exp=R_tryEval(xp, R_GlobalEnv, &Rerror);
		}
		UNPROTECT(1);
    }
    return;
}


struct sockaddr_in session_peer_sa;
SOCKET session_socket;
unsigned char session_key[32];

/* detach session and setup everything such that in can be resumed at some point */
int detach_session(SOCKET s) {
    SAIN ssa;
	int port=32768;
	SOCKET ss=FCF("open socket",socket(AF_INET,SOCK_STREAM,0));
    int reuse=1; /* enable socket address reusage */
	socklen_t sl = sizeof(session_peer_sa);
	struct dsresp {
		int pt1;
		int port;
		int pt2;
		unsigned char key[32];
	} dsr;

	if (getpeername(s, (SA*) &session_peer_sa, &sl)) {
		sendResp(s,SET_STAT(RESP_ERR,ERR_detach_failed));
		return -1;
	}

    setsockopt(ss,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

#ifdef Win32
	while ((port = (((int) rand()) & 0x7fff)+32768)>65000) {};
#else
	while ((port = (((int) random()) & 0x7fff)+32768)>65000) {};
#endif

	while (bind(ss,build_sin(&ssa,0,port),sizeof(ssa))) {
		if (errno!=EADDRINUSE) {
#ifdef RSERV_DEBUG
			printf("session: error in bind other than EADDRINUSE (0x%x)",  errno);
#endif
			closesocket(ss);
			sendResp(s,SET_STAT(RESP_ERR,ERR_detach_failed));
			return -1;
		}
		port++;
		if (port>65530) {
#ifdef RSERV_DEBUG
			printf("session: can't find available prot to listed on.\n");
#endif
			closesocket(ss);
			sendResp(s,SET_STAT(RESP_ERR,ERR_detach_failed));
			return -1;
		}
	}

    if (listen(ss,LISTENQ)) {
#ifdef RSERV_DEBUG
		printf("session: cannot listen.\n");
#endif
		closesocket(ss);
		sendResp(s,SET_STAT(RESP_ERR,ERR_detach_failed));
		return -1;
	}

	{
		int i=0;
		while (i<32) session_key[i++]=(unsigned char) rand();
	}

#ifdef RSERV_DEBUG
	printf("session: listening on port %d\n", port);
#endif

	dsr.pt1  = itop(SET_PAR(DT_INT,sizeof(int)));
	dsr.port = itop(port);
	dsr.pt2  = itop(SET_PAR(DT_BYTESTREAM,32));
	memcpy(dsr.key, session_key, 32);							
	
	sendRespData(s, RESP_OK, 3*sizeof(int)+32, &dsr);
	closesocket(s);
#ifdef RSERV_DEBUG
	printf("session: detached, closing connection.\n");
#endif
	session_socket=ss;
	return 0;
}

/* static char *sres_id = "RsS1                        \r\n\r\n"; */

/* resume detached session. return the new socket after resume is complete, but don't send the response message */
SOCKET resume_session() {
	SOCKET s=-1;
	SAIN lsa;
	socklen_t al=sizeof(lsa);
	char clk[32];

#ifdef RSERV_DEBUG
	printf("session: resuming session, waiting for connections.\n");
#endif

	while ((s=accept(session_socket, (SA*)&lsa,&al))>1) {
		if (lsa.sin_addr.s_addr != session_peer_sa.sin_addr.s_addr) {
#ifdef RSERV_DEBUG
			printf("session: different IP, rejecting\n");
#endif
			closesocket(s);
		} else {
			int n=0;
			if ((n=recv(s, clk, 32, 0)) != 32) {
#ifdef RSERV_DEBUG
				printf("session: expected 32, got %d = closing\n", n);
#endif
				closesocket(s);
			} else if (memcmp(clk, session_key, 32)) {
#ifdef RSERV_DEBUG
				printf("session: wrong key, closing\n");
#endif
				closesocket(s);
			} else {
#ifdef RSERV_DEBUG
				printf("session: accepted\n");
#endif
				return s;
			}
		}
	}
	return -1;
}


/* working thread/function. the parameter is of the type struct args* */
decl_sbthread newConn(void *thp) {
    SOCKET s;
    struct args *a=(struct args*)thp;
    struct phdr ph;
    char *buf, *c,*cc,*c1,*c2;
    int pars;
    int i,j,n;
    int process;
    ParseStatus stat;
    char *sendbuf;
    int sendBufSize;
    char *tail;
    char *fbuf;
    char *sfbuf;
    int fbufl;
    int Rerror;
    char wdname[512];
    int authed=0;
    int unaligned=0;
    char salt[5];
    rlen_t tempSB=0;
    
    int parT[16];
    rlen_t parL[16];
    void *parP[16];
    
    SEXP xp,exp;
    FILE *cf=0;
    
#ifdef FORKED  
    long rseed=random();
    rseed^=time(0);
    if ((lastChild=fork())!=0) {
		/* close the connection socket - the child has it already */
		closesocket(a->s);
		return;
    }
    srandom(rseed);
    
    parentPID=getppid();
    closesocket(a->ss); /* close server socket */
#endif
    
    buf=(char*) malloc(inBuf+8);
    sfbuf=(char*) malloc(sfbufSize);
    if (!buf || !sfbuf) {
		fprintf(stderr,"FATAL: cannot allocate initial buffers. closing client connection.\n");
		s=a->s;
		free(a);
		closesocket(s);
		return;
    }
    memset(buf,0,inBuf+8);
    
#ifdef unix
    if (workdir) {
		if (chdir(workdir))
			mkdir(workdir,0777);
		wdname[511]=0;
		snprintf(wdname,511,"%s/conn%d",workdir,a->ucix);
		mkdir(wdname,0777);
		chdir(wdname);
    }
#endif
    
    sendBufSize=sndBS;
    sendbuf=(char*)malloc(sendBufSize);
#ifdef RSERV_DEBUG
    printf("connection accepted.\n");
#endif
    s=a->s;
    free(a);
    
#ifndef THREADED /* in all but threaded environments we can keep the
					current socket globally for R-error handler */
    csock=s;
#endif
    
#ifdef CAN_TCP_NODELAY
    {
		int opt=1;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*) &opt, sizeof(opt));
    }
#endif
    
    strcpy(buf,IDstring);
    if (authReq) {
		memcpy(buf+16,"ARuc",4);
		salt[0]='K';
		salt[1]=code64[rand()&63];
		salt[2]=code64[rand()&63];
		salt[3]=' '; salt[4]=0;
		memcpy(buf+20,salt,4);
		if (usePlain) memcpy(buf+24,"ARpt",4);
    }
#ifdef RSERV_DEBUG
    printf("sending ID string.\n");
#endif
    send(s,buf,32,0);
    while((n=recv(s,&ph,sizeof(ph),0))==sizeof(ph)) {
#ifdef RSERV_DEBUG
		printf("\nheader read result: %d\n",n);
		if (n>0) printDump(&ph,n);
#endif
		ph.len=ptoi(ph.len);
		ph.cmd=ptoi(ph.cmd);
		ph.dof=ptoi(ph.dof);
		process=0;
		pars=0;
		if (ph.len>0) {
			unsigned int phead;
			int parType=0;
			rlen_t parLen=0;
	    
			if (!maxInBuf || ph.len<maxInBuf) {
				if (ph.len>=inBuf) {
#ifdef RSERV_DEBUG
					printf("resizing input buffer (was %d, need %d) to %d\n",inBuf,ph.len,((ph.len|0x1fff)+1));
#endif
					free(buf); /* the buffer is just a scratchpad, so we don't need to use realloc */
					buf=(char*)malloc(inBuf=((ph.len|0x1fff)+1)); /* use 8kB granularity */
					if (!buf) {
#ifdef RSERV_DEBUG
						fprintf(stderr,"FATAL: out of memory while resizing buffer to %d,\n",inBuf);
#endif
						sendResp(s,SET_STAT(RESP_ERR,ERR_out_of_mem));
						free(sendbuf); free(sfbuf);
						closesocket(s);
						return;
					}	    
				}
#ifdef RSERV_DEBUG
				printf("loading buffer (awaiting %d bytes)\n",ph.len);
#endif
				i=0;
				while((n=recv(s,buf+i,ph.len-i,0))) {
					if (n>0) i+=n;
					if (i>=ph.len || n<1) break;
				}
				if (i<ph.len) break;
				memset(buf+ph.len,0,8);
		
				unaligned=0;
#ifdef RSERV_DEBUG
				printf("parsing parameters\n");
				if (ph.len>0) printDump(buf,ph.len);
#endif
				c=buf+ph.dof;
				while((c<buf+ph.dof+ph.len) && (phead=ptoi(*((unsigned int*)c)))) {
					rlen_t headSize=4;
					parType=PAR_TYPE(phead);
					parLen=PAR_LEN(phead);
					if ((parType&DT_LARGE)>0) { /* large parameter */
						headSize+=4;
						parLen|=((rlen_t)(ptoi(*(unsigned int*)(c+4))))<<24;
						parType^=DT_LARGE;
					} 
#ifdef RSERV_DEBUG
					printf("PAR[%d]: %08x (PAR_LEN=%ld, PAR_TYPE=%d, large=%s)\n", pars, i,
						   (long)parLen, parType, (headSize==8)?"yes":"no");
#endif
#ifdef ALIGN_DOUBLES
					if (unaligned) { /* on Sun machines it is deadly to process unaligned parameters,
										therefore we respond with ERR_inv_par */
#ifdef RSERV_DEBUG
						printf("Platform specific: last parameter resulted in unaligned stream for the current one, sending ERR_inv_par.\n");
#endif
						sendResp(s,SET_STAT(RESP_ERR,ERR_inv_par));
						process=1; ph.cmd=0;
						break;
					}
#endif
					if (parLen&3) unaligned=1;         
					parT[pars]=parType;
					parL[pars]=parLen;
					parP[pars]=c+headSize;
					pars++;
					c+=parLen+headSize; /* par length plus par head */
					if (pars>15) break;
				} /* we don't parse more than 16 parameters */
			} else {
				printf("discarding buffer because too big (awaiting %d bytes)\n",ph.len);
				i=ph.len;
				while((n=recv(s,buf,i>inBuf?inBuf:i,0))) {
					if (n>0) i-=n;
					if (i<1 || n<1) break;
				}
				if (i>0) break;
				/* if the pars are bigger than my buffer, send data_overflow response
				   (since 1.23/0.1-6; was inv_par before) */
				sendResp(s,SET_STAT(RESP_ERR,ERR_data_overflow));
				process=1; ph.cmd=0;
			}
		}
	
		/** IMPORTANT! The pointers in par[..] point to RAW data, i.e. you have
			to use ptoi(..) in order to get the real integer value. */
	
		/** NOTE: Rserve doesn't check for alignment of parameters. This is ok
			for most platforms, but on Sun hardware this means that an user
			can send a package that will cause segfault in the client thread
			by sending unaligned parameters. This won't affect the server, only
			the connection child process dies.
			Since 0.1-10 we report ERR_inv_par on Sun for non-aligned parameters.
		*/
	
#ifdef RSERV_DEBUG
		printf("CMD=%08x, pars=%d\n",ph.cmd,pars);
#endif
	
		if (!authed && ph.cmd==CMD_login) {
			if (pars<1 || parT[0]!=DT_STRING) 
				sendResp(s,SET_STAT(RESP_ERR,ERR_inv_par));
			else {
				c=(char*)parP[0];
				cc=c;
				while(*cc && *cc!='\n') cc++;
				if (*cc) { *cc=0; cc++; };
				c1=cc;
				while(*c1) if(*c1=='\n'||*c1=='\r') *c1=0; else c1++;
				/* c=login, cc=pwd */
				authed=1;
#ifdef RSERV_DEBUG
				printf("Authentication attempt (login='%s',pwd='%s',pwdfile='%s')\n",c,cc,pwdfile);
#endif
				if (pwdfile) {
					authed=0; /* if pwdfile exists, default is access denied */
					/* TODO: opening pwd file, parsing it and responding
					   might be a bad idea, since it allows DOS attacks as this
					   operation is fairly costly. We should actually cache
					   the user list and reload it only on HUP or something */
					/* we abuse variables of other commands since we are
					   the first command ever used so we can trash them */
					cf=fopen(pwdfile,"r");
					if (cf) {
						sfbuf[sfbufSize-1]=0;
						while(!feof(cf))
							if (fgets(sfbuf,sfbufSize-1,cf)) {
								c1=sfbuf;
								while(*c1 && *c1!=' ' && *c1!='\t') c1++;
								if (*c1) {
									*c1=0;
									c1++;
									while(*c1==' ' || *c1=='\t') c1++;
								};
								c2=c1;
								while(*c2) if (*c2=='\r'||*c2=='\n') *c2=0; else c2++;
								if (!strcmp(sfbuf,c)) { /* login found */
#ifdef RSERV_DEBUG
									printf("Found login '%s', checking password.\n", c);
#endif
									if (usePlain && !strcmp(c1,cc)) {
										authed=1;
#ifdef RSERV_DEBUG
										puts(" - plain pasword matches.");
#endif
									} else {
#ifdef HAS_CRYPT
										c2=crypt(c1,salt+1);
#ifdef RSERV_DEBUG
										printf(" - checking crypted '%s' vs '%s'\n", c2, cc);
#endif
										if (!strcmp(c2,cc)) authed=1;
#endif
									}
#ifdef DEBUG_RSERV
									printf(" - authentication %s\n",(authed)?"succeeded":"failed");
#endif
								}
								if (authed) break;
							} /* if fgets */
						fclose(cf);
					} /* if (cf) */
					cf=0;
					if (authed) {
						process=1;
						sendResp(s,RESP_OK);
					}
				}
			}
		}
		/* if not authed by now, close connection */
		if (authReq && !authed) {
			sendResp(s,SET_STAT(RESP_ERR,ERR_auth_failed));
			closesocket(s);
			free(sendbuf); free(sfbuf); free(buf);
			return;
		}
	
		if (ph.cmd==CMD_shutdown) {
			sendResp(s,RESP_OK);
#ifdef RSERV_DEBUG
			printf("initiating clean shutdown.\n");
#endif
			active=0;
			closesocket(s);
			free(sendbuf); free(sfbuf); free(buf);
#ifdef FORKED
			if (parentPID>0) kill(parentPID,SIGTERM);
			exit(0);
#endif
			return;
		}

		if (ph.cmd==CMD_setBufferSize) {
			process=1;
			if (pars<1 || parT[0]!=DT_INT) 
				sendResp(s,SET_STAT(RESP_ERR,ERR_inv_par));
			else {
				rlen_t ns=ptoi(*(unsigned int*)parP);
#ifdef RSERV_DEBUG
				printf(">>CMD_setSendBuf to %ld bytes.\n", (long)ns);
#endif
				if (ns>0) { /* 0 means don't touch the buffer size */
					if (ns<32768) ns=32768; /* we enforce a minimum of 32kB */
					free(sendbuf);
					sendbuf=(char*)malloc(sendBufSize);
					if (!sendbuf) {
#ifdef RSERV_DEBUG
						fprintf(stderr,"FATAL: out of memory while resizing send buffer to %d,\n",sendBufSize);
#endif
						sendResp(s,SET_STAT(RESP_ERR,ERR_out_of_mem));
						free(buf); free(sfbuf);
						closesocket(s);
						return;
					}
					sendBufSize=ns;
				}
				sendResp(s,RESP_OK);
			}
		}
	
		if (ph.cmd==CMD_openFile||ph.cmd==CMD_createFile) {
			process=1;
			if (!allowIO) sendResp(s,SET_STAT(RESP_ERR,ERR_accessDenied));
			else {
				if (pars<1 || parT[0]!=DT_STRING) 
					sendResp(s,SET_STAT(RESP_ERR,ERR_inv_par));
				else {
					c=(char*)(parP[0]);
					if (cf) fclose(cf);
#ifdef RSERV_DEBUG
					printf(">>CMD_open/createFile(%s)\n",c);
#endif
					cf=fopen(c,(ph.cmd==CMD_openFile)?"rb":"wb");
					if (!cf)
						sendResp(s,SET_STAT(RESP_ERR,ERR_IOerror));
					else
						sendResp(s,RESP_OK);
				}
			}
		}
	
		if (ph.cmd==CMD_removeFile) {
			process=1;
			if (!allowIO) sendResp(s,SET_STAT(RESP_ERR,ERR_accessDenied));
			else {
				if (pars<1 || parT[0]!=DT_STRING) 
					sendResp(s,SET_STAT(RESP_ERR,ERR_inv_par));
				else {
					c=(char*)parP[0];
#ifdef RSERV_DEBUG
					printf(">>CMD_removeFile(%s)\n",c);
#endif
					if (remove(c))
						sendResp(s,SET_STAT(RESP_ERR,ERR_IOerror));
					else
						sendResp(s,RESP_OK);
				}
			}
		}
	
		if (ph.cmd==CMD_closeFile) {
			process=1;
			if (!allowIO) sendResp(s,SET_STAT(RESP_ERR,ERR_accessDenied));
			else {
				if (cf) fclose(cf);
#ifdef RSERV_DEBUG
				printf(">>CMD_closeFile\n");
#endif
				cf=0;
				sendResp(s,RESP_OK);
			}
		}
	
		if (ph.cmd==CMD_readFile) {
			process=1;
			if (!allowIO) sendResp(s,SET_STAT(RESP_ERR,ERR_accessDenied));
			else {
				if (!cf)
					sendResp(s,SET_STAT(RESP_ERR,ERR_notOpen));
				else {
					fbufl=sfbufSize; fbuf=sfbuf;
					if (pars==1 && parT[0]==DT_INT)
						fbufl=ptoi(*(unsigned int*)parP[0]);
#ifdef RSERV_DEBUG
					printf(">>CMD_readFile(%d)\n",fbufl);
#endif
					if (fbufl<0) fbufl=sfbufSize;
					if (fbufl>sfbufSize)
						fbuf=(char*)malloc(fbufl);
					if (!fbuf) /* well, logically not clean, but in practice true */
						sendResp(s,SET_STAT(RESP_ERR,ERR_inv_par));
					else {
						i=fread(fbuf,1,fbufl,cf);
						if (i>0)
							sendRespData(s,RESP_OK,i,fbuf);
						else
							sendResp(s,RESP_OK);
						if (fbuf!=sfbuf)
							free(fbuf);
					}
				}
			}
		}
	
		if (ph.cmd==CMD_writeFile) {
			process=1;
			if (!allowIO) sendResp(s,SET_STAT(RESP_ERR,ERR_accessDenied));
			else {
				if (!cf)
					sendResp(s,SET_STAT(RESP_ERR,ERR_notOpen));
				else {
					if (pars<1 || parT[0]!=DT_BYTESTREAM)
						sendResp(s,SET_STAT(RESP_ERR,ERR_inv_par));
					else {
#ifdef RSERV_DEBUG
						printf(">>CMD_writeFile(%ld,...)\n", (long) parL[0]);
#endif
						i=0;
						c=(char*)parP[0];
						if (parL[0]>0)
							i=fwrite(c,1,parL[0],cf);
						if (i>0 && i!=parL[0])
							sendResp(s,SET_STAT(RESP_ERR,ERR_IOerror));
						else
							sendResp(s,RESP_OK);
					}
				}
			}
		}
	
		/*--- CMD_setSEXP / CMD_assignSEXP ---*/
	
		if (ph.cmd==CMD_setSEXP || ph.cmd==CMD_assignSEXP) {
			process=1;
			if (pars<2 || parT[0]!=DT_STRING) 
				sendResp(s,SET_STAT(RESP_ERR,ERR_inv_par));
			else {
				SEXP val, sym=0;
				unsigned int *sptr;
				int parType=parT[1];
				int globalUPC=0;
				int boffs=0;
		
				c=(char*)parP[0]; /* name of the symbol */
#ifdef RSERV_DEBUG
				printf(">>CMD_set/assignREXP (%s, REXP)\n",c);
#endif
		
				if (ph.cmd==CMD_assignSEXP) {
					sym=parseExps(c,1,&stat);
					if (stat!=1) {
#ifdef RSERV_DEBUG
						printf(">>CMD_assignREXP-failed to parse \"%s\", stat=%d\n",c,stat);
#endif
						sendResp(s,SET_STAT(RESP_ERR,stat));
						goto respSt;
					}
					if (TYPEOF(sym)==EXPRSXP && LENGTH(sym)>0) {
						sym=VECTOR_ELT(sym,0);
						/* we should de-allocate the vector here .. if we can .. */
					}
				}
		
				switch (parType) {
				case DT_STRING:
#ifdef RSERV_DEBUG
					printf("  assigning string \"%s\"\n",((char*)(parP[1])));
#endif
					PROTECT(val = allocVector(STRSXP,1));
					SET_STRING_ELT(val,0,mkChar((char*)(parP[1])));
					defineVar((sym)?sym:install(c),val,R_GlobalEnv);
					UNPROTECT(1);
					sendResp(s,RESP_OK);
					break;
				case DT_SEXP|DT_LARGE:
					boffs=1; /* we're not using the size, so in fact we just
								advance the pointer and don't care about the length */
				case DT_SEXP:
					sptr=((unsigned int*)parP[1])+boffs;
					val=decode_to_SEXP(&sptr,&globalUPC);
					if (val==0)
						sendResp(s,SET_STAT(RESP_ERR,ERR_inv_par));
					else {
#ifdef RSERV_DEBUG
						printf("  assigning SEXP: ");
						printSEXP(val);
#endif
						defineVar((sym)?sym:install(c),val,R_GlobalEnv);
						sendResp(s,RESP_OK);
					}
					if (globalUPC>0) UNPROTECT(globalUPC);
					break;
				default:
					sendResp(s,SET_STAT(RESP_ERR,ERR_inv_par));
				}
			}
		}
	
		if (ph.cmd==CMD_detachSession) {
			process=1;
			if (!detach_session(s)) {
				s=resume_session();
				sendResp(s,RESP_OK);
			}
		}
		
		if (ph.cmd==CMD_voidEval || ph.cmd==CMD_eval || ph.cmd==CMD_detachedVoidEval) {
			process=1;
			if (pars<1 || parT[0]!=DT_STRING) 
				sendResp(s,SET_STAT(RESP_ERR,ERR_inv_par));
			else {
				c=(char*)parP[0];
#ifdef RSERV_DEBUG
				printf("parseString(\"%s\")\n",c);
#endif
				j=0;
				xp=parseString(c,&j,&stat);
				PROTECT(xp);
#ifdef RSERV_DEBUG
				printf("buffer parsed, stat=%d, parts=%d\n",stat,j);
				if (xp)
					printf("result type: %d, length: %d\n",TYPEOF(xp),LENGTH(xp));
				else
					printf("result is <null>\n");
#endif				
				if (stat==1 && ph.cmd==CMD_detachedVoidEval && detach_session(s))
					sendResp(s,SET_STAT(RESP_ERR,ERR_detach_failed));
				else if (stat!=1)
					sendResp(s,SET_STAT(RESP_ERR,stat));
				else {
#ifdef RSERV_DEBUG
					printf("R_tryEval(xp,R_GlobalEnv,&Rerror);\n");
#endif
					if (ph.cmd==CMD_detachedVoidEval)
						s=-1;
					exp=R_NilValue;
					if (TYPEOF(xp)==EXPRSXP && LENGTH(xp)>0) {
						int bi=0;
						while (bi<LENGTH(xp)) {
							SEXP pxp=VECTOR_ELT(xp, bi);
							Rerror=0;
#ifdef RSERV_DEBUG
							printf("Calling R_tryEval for expression %d [type=%d] ...\n",bi+1,TYPEOF(pxp));
#endif
							exp=R_tryEval(pxp, R_GlobalEnv, &Rerror);
							bi++;
#ifdef RSERV_DEBUG
							printf("Expression %d, error code: %d\n",bi, Rerror);
							if (Rerror) printf(">> early error, aborting further evaluations\n");
#endif
							if (Rerror) break;
						}
					} else {
						Rerror=0;
						exp=R_tryEval(xp, R_GlobalEnv, &Rerror);
					}
					PROTECT(exp);
#ifdef RSERV_DEBUG
					printf("expression(s) evaluated (Rerror=%d).\n",Rerror);
					if (!Rerror) printSEXP(exp);
#endif
					if (ph.cmd==CMD_detachedVoidEval && s==-1)
						s=resume_session();
					if (Rerror) {
						sendResp(s,SET_STAT(RESP_ERR,(Rerror<0)?Rerror:-Rerror));
					} else {
						if (ph.cmd==CMD_voidEval || ph.cmd==CMD_detachedVoidEval)
							sendResp(s,RESP_OK);
						else {
							char *sendhead=0;
							int canProceed=1;
							/* check buffer size vs REXP size to avoid dangerous overflows
							   todo: resize the buffer as necessary
							*/
							rlen_t rs=getStorageSize(exp);
#ifdef RSERV_DEBUG
							printf("result storage size = %d bytes\n",(int)rs);
#endif
							if (rs>sendBufSize-64) { /* is the send buffer too small ? */
								canProceed=0;
								if (maxSendBufSize && rs+64>maxSendBufSize) { /* first check if we're allowed to resize */
									unsigned int osz=(rs>0xffffffff)?0xffffffff:rs;
									osz=itop(osz);
#ifdef RSERV_DEBUG
									printf("ERROR: object too big (sendBuf=%d)\n",sendBufSize);
#endif
									sendRespData(s,SET_STAT(RESP_ERR,ERR_object_too_big),4,&osz);
								} else { /* try to allocate a large, temporary send buffer */
									tempSB=rs+64; tempSB&=rlen_max<<12; tempSB+=0x1000;
#ifdef RSERV_DEBUG
									printf("Trying to allocate temporary send buffer of %ld bytes.\n", (long)tempSB);
#endif
									free(sendbuf);
									sendbuf=(char*)malloc(tempSB);
									if (!sendbuf) {
										tempSB=0;
#ifdef RSERV_DEBUG
										printf("Failed to allocate temporary send buffer of %ld bytes. Restoring old send buffer of %ld bytes.\n", (long)tempSB, (long)sendBufSize);
#endif
										sendbuf=(char*)malloc(sendBufSize);
										if (!sendbuf) { /* we couldn't re-allocate the buffer */
#ifdef RSERV_DEBUG
											fprintf(stderr,"FATAL: out of memory while re-allocating send buffer to %d (fallback#1)\n",sendBufSize);
#endif
											sendResp(s,SET_STAT(RESP_ERR,ERR_out_of_mem));
											free(buf); free(sfbuf);
											closesocket(s);
											return;
										} else {
											unsigned int osz=(rs>0xffffffff)?0xffffffff:rs;
											osz=itop(osz);
#ifdef RSERV_DEBUG
											printf("ERROR: object too big (sendBuf=%d) and couldn't allocate big enough send buffer\n",sendBufSize);
#endif
											sendRespData(s,SET_STAT(RESP_ERR,ERR_object_too_big),4,&osz);
										}
									} else canProceed=1;
								}
							}
							if (canProceed) {
								/* if this is defined then the old (<=0.1-9) "broken" behavior is requested where no data type header is sent */
#ifdef FORCE_V0100 
								tail=(char*)storeSEXP((unsigned int*)sendbuf,exp);
								sendhead=sendbuf;
#else
								/* first we have 4 bytes of a header saying this is an encoded SEXP, then comes the SEXP */
								char *sxh=sendbuf+8;
								tail=(char*)storeSEXP((unsigned int*)sxh,exp);
								/* set type to DT_SEXP and correct length */
								if ((tail-sxh)>0xfffff0) { /* we must use the "long" format */
									rlen_t ll=tail-sxh;
									((unsigned int*)sendbuf)[0]=itop(SET_PAR(DT_SEXP|DT_LARGE,ll&0xffffff));
									((unsigned int*)sendbuf)[1]=itop(ll>>24);
									sendhead=sendbuf;
								} else {
									sendhead=sendbuf+4;
									((unsigned int*)sendbuf)[1]=itop(SET_PAR(DT_SEXP,tail-sxh));
								}
#endif
#ifdef RSERV_DEBUG
								printf("stored SEXP; length=%d (incl. DT_SEXP header)\n",tail-sendhead);
#endif
								sendRespData(s,RESP_OK,tail-sendhead,sendhead);
								if (tempSB) { /* if this is just a temporary sendbuffer then shrink it back to normal */
#ifdef RSERV_DEBUG
									printf("Releasing temporary sendbuf and restoring old size of %d bytes.\n",sendBufSize);
#endif
									free(sendbuf);
									sendbuf=(char*)malloc(sendBufSize);
									if (!sendbuf) { /* this should be really rare since tempSB was much larger */
#ifdef RSERV_DEBUG
										fprintf(stderr,"FATAL: out of memory while re-allocating send buffer to %d (fallback#2),\n",sendBufSize);
#endif
										sendResp(s,SET_STAT(RESP_ERR,ERR_out_of_mem));
										free(buf); free(sfbuf);
										closesocket(s);
										return;		    
									}
								}
							}
						}
						UNPROTECT(1); /* exp */
					}
					UNPROTECT(1); /* xp */
				}
#ifdef RSERV_DEBUG
				printf("reply sent.\n");
#endif
			}
		}
    respSt:

		if (s==-1) { n=0; break; }

		if (!process)
			sendResp(s,SET_STAT(RESP_ERR,ERR_inv_cmd));
    }
#ifdef RSERV_DEBUG
    if (n==0)
		printf("Connection closed by peer.\n");
    else {
		printf("malformed packet (n=%d). closing socket to prevent garbage.\n",n);
		if (n>0) printDump(&ph,n);
    }
#endif
    if (n>0)
		sendResp(s,SET_STAT(RESP_ERR,ERR_conn_broken));
    closesocket(s);
    free(sendbuf); free(sfbuf); free(buf);
#ifdef unix
    if (workdir) {
		chdir(workdir);
		rmdir(wdname);
    }
#endif
    
#ifdef RSERV_DEBUG
    printf("done.\n");
#endif
#ifdef FORKED
    /* we should not return to the main loop, but terminate instead */
    exit(0);
#endif
}

void serverLoop() {
    SAIN ssa;
    socklen_t al;
    int reuse;
    int selRet=0;
    struct args *sa;
    struct sockaddr_in lsa;
    
#ifdef unix
    struct sockaddr_un lusa;
    struct timeval timv;
    fd_set readfds;
#endif
    
    lsa.sin_addr.s_addr=inet_addr("127.0.0.1");
    
#ifdef FORKED
    signal(SIGHUP,sigHandler);
    signal(SIGTERM,sigHandler);
#ifdef RSERV_DEBUG
    signal(SIGINT,brkHandler);
#endif
#endif
    
    initsocks();
    if (localSocketName) {
#ifndef unix
		fprintf(stderr,"Local sockets are not supported on non-unix systems.\n");
		return;
#else
		ss=FCF("open socket",socket(AF_LOCAL,SOCK_STREAM,0));
		memset(&lusa,0,sizeof(lusa));
		lusa.sun_family=AF_LOCAL;
		if (strlen(localSocketName)>sizeof(lusa.sun_path)-2) {
			fprintf(stderr,"Local socket name is too long for this system.\n");
			return;
		}
		strcpy(lusa.sun_path,localSocketName);
		remove(localSocketName); /* remove existing if possible */
#endif
	} else
		ss=FCF("open socket",socket(AF_INET,SOCK_STREAM,0));
    reuse=1; /* enable socket address reusage */
    setsockopt(ss,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
#ifdef unix
    if (localSocketName)
		FCF("bind",bind(ss,(SA*) &lusa, sizeof(lusa)));    
    else
#endif
		FCF("bind",bind(ss,build_sin(&ssa,0,port),sizeof(ssa)));
    
    FCF("listen",listen(ss,LISTENQ));
    while(active) { /* main serving loop */
#ifdef FORKED
		while (waitpid(-1,0,WNOHANG)>0);
#endif
#ifdef unix
		timv.tv_sec=0; timv.tv_usec=10000;
		FD_ZERO(&readfds); FD_SET(ss,&readfds);
		selRet=select(ss+1,&readfds,0,0,&timv);
		if (selRet>0 && FD_ISSET(ss,&readfds)) {
#endif
			sa=(struct args*)malloc(sizeof(struct args));
			memset(sa,0,sizeof(struct args));
			al=sizeof(sa->sa);
#ifdef unix
			if (localSocketName) {
				al=sizeof(sa->su);
				sa->s=CF("accept",accept(ss,(SA*)&(sa->su),&al));
			} else
#endif
				sa->s=CF("accept",accept(ss,(SA*)&(sa->sa),&al));
			sa->ucix=UCIX++;
			sa->ss=ss;
			/*
			memset(sa->sk,0,16);
			sa->sfd=-1;
#if defined SESSIONS && defined FORKED
			{
				int pd[2];
				if (!pipe(&pd)) {
					
				}
			}
#endif
			*/
			if (localonly && !localSocketName) {
				char **laddr=allowed_ips;
				int allowed=0;
				if (!laddr) { 
					allowed_ips = (char**) malloc(sizeof(char*)*2);
					allowed_ips[0] = strdup("127.0.0.1");
					allowed_ips[1] = 0;
					laddr=allowed_ips;
				}
				while (*laddr) if (sa->sa.sin_addr.s_addr==inet_addr(*(laddr++))) { allowed=1; break; };
				if (allowed)
#ifdef THREADED
					sbthread_create(newConn,sa);
#else
				newConn(sa);
#endif
				else
					closesocket(sa->s);
			} else
#ifdef THREADED
				sbthread_create(newConn,sa); 
#else
			newConn(sa);
#endif
#ifdef unix
		}
#endif
    }
}

extern int Rf_initEmbeddedR(int, char**);

/* main function - start Rserve */
int main(int argc, char **argv)
{
    int stat,i;
    
#ifdef RSERV_DEBUG
    printf("Rserve %d.%d-%d (C)Copyright 2002,3 Simon Urbanek\n\n",RSRV_VER>>16,(RSRV_VER>>8)&255,RSRV_VER&255);
#endif
    if (!isByteSexOk()) {
		printf("FATAL ERROR: This program was not correctly compiled - the endianess is wrong!\nUse -DSWAPEND when compiling on PPC or similar platforms.\n");
		return -100;
    }
    
    loadConfig(CONFIG_FILE);
    
    /** copy argv while removing Rserve specific parameters */
    top_argc=1;
    top_argv=(char**) malloc(sizeof(char*)*(argc+1));
    top_argv[0]=argv[0];
    i=1;
    while (i<argc) {
		int isRSP=0;
		if (argv[i] && *argv[i]=='-' && argv[i][1]=='-') {
			if (!strcmp(argv[i]+2,"RS-port")) {
				isRSP=1;
				if (i+1==argc)
					fprintf(stderr,"Missing port specification for --RS-port.\n");
				else {
					port=atoi(argv[++i]);
					if (port<1) {
						fprintf(stderr,"Invalid port number in --RS-port, using default port.\n");
						port=default_Rsrv_port;
					}
				}
			}
			if (!strcmp(argv[i]+2,"RS-dumplimit")) {
				isRSP=1;
				if (i+1==argc)
					fprintf(stderr,"Missing limit specification for --RS-dumplimit.\n");
				else
					dumpLimit=atoi(argv[++i]);
			}
			if (!strcmp(argv[i]+2,"RS-socket")) {
				isRSP=1;
				if (i+1==argc)
					fprintf(stderr,"Missing socket specification for --RS-socket.\n");
				else
					localSocketName=argv[++i];
			}
			if (!strcmp(argv[i]+2,"RS-workdir")) {
				isRSP=1;
				if (i+1==argc)
					fprintf(stderr,"Missing directory specification for --RS-workdir.\n");
				else
					workdir=argv[++i];
			}
			if (!strcmp(argv[i]+2,"RS-conf")) {
				isRSP=1;
				if (i+1==argc)
					fprintf(stderr,"Missing config file specification for --RS-conf.\n");
				else
					loadConfig(argv[++i]);
			}
			if (!strcmp(argv[i]+2,"RS-settings")) {
				printf("Rserve v%d.%d-%d\n\nconfig file: %s\nworking root: %s\nport: %d\nlocal socket: %s\nauthorization required: %s\nplain text password: %s\npasswords file: %s\nallow I/O: %s\nmax.input buffer size: %d kB\n\n",
					   RSRV_VER>>16,(RSRV_VER>>8)&255,RSRV_VER&255,
					   CONFIG_FILE,workdir,port,localSocketName?localSocketName:"[none, TCP/IP used]",
					   authReq?"yes":"no",usePlain?"allowed":"not allowed",pwdfile?pwdfile:"[none]",allowIO?"yes":"no",
					   maxInBuf/1024);
				return 0;	       
			}
			if (!strcmp(argv[i]+2,"version")) {
				printf("Rserve v%d.%d-%d\n",RSRV_VER>>16,(RSRV_VER>>8)&255,RSRV_VER&255);
			}
			if (!strcmp(argv[i]+2,"help")) {
				printf("Usage: R CMD Rserve [<options>]\n\nOptions: --help  this help screen\n --version  prints Rserve version (also passed to R)\n --RS-port <port> listen on the specified TCP port\n --RS-socket <socket> use specified local (unix) socket instead of TCP/IP.\n --RS-workdir <path> use specified working directory root for connections.\n --RS-conf <file> load additional config file.\n --RS-settings  dumps current settings of the Rserve\n\nAll other options are passed to the R engine.\n\n");
#ifdef RSERV_DEBUG
				printf("debugging flag:\n --RS-dumplimit <number>  sets limit of items/bytes to dump in debugging output. set to 0 for unlimited\n\n");
#endif
				return 0;
			}
		}
		if (!isRSP)
			top_argv[top_argc++]=argv[i];
		i++;
    }
    
    stat=Rf_initEmbeddedR(top_argc,top_argv);
    if (stat<0) {
		printf("Failed to initialize embedded R! (stat=%d)\n",stat);
		return -1;
    }

    if (src_list) { /* do any sourcing if necessary */
		struct source_entry *se=src_list;
#ifdef RSERV_DEBUG
		printf("Executing source/eval commands from the config file.\n");
#endif
		while (se) {
#ifdef RSERV_DEBUG
			printf("voidEval(\"%s\")\n", se->line);
#endif
			voidEval(se->line);
			se=se->next;
		}
#ifdef RSERV_DEBUG
		printf("Done with initial commands.\n");
#endif
    }
#if defined RSERV_DEBUG || defined Win32
    printf("Rserve: Ok, ready to answer queries.\n");
#endif      
    
#if defined DAEMON && defined unix
    /* ok, we're in unix, so let's daemonize properly */
    if (fork()!=0) {
		puts("Rserv started in daemon mode.");
		exit(0);
    }
    
    setsid();
    chdir("/");
    umask(0);
#endif
    
    serverLoop();
#ifdef unix
    if (localSocketName)
		remove(localSocketName);
#endif
    
#ifdef RSERV_DEBUG
    printf("\nServer terminated normally.\n");
#endif
    return 0;
}

/*--- The following makes the indenting behavior of emacs compatible
      with Xcode's 4/4 setting ---*/
/* Local Variables: */
/* indent-tabs-mode: t */
/* tab-width: 4 */
/* c-basic-offset: 4 */
/* End: */
