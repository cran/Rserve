/*
 *  Simple console that uses C++ interface to Rserve
 *  Copyright (C) 2004 Simon Urbanek, All rights reserved.
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
 *  $Id: rcons.cc 113 2004-09-26 21:41:41Z urbaneks $
 */

#include <iostream>
using std::cout;

#define MAIN         // we are the main program, we need to define this
#define SOCK_ERRORS  // we will use verbose socket errors

#include <Rconnection.h>
#ifdef READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#ifdef READLINE
char *c=0;
#else
char c[1024];
#endif

int main(int argc, char **argv) {
    initsocks(); // this is needed for Win32 - it does noting on unix

    Rconnection *rc = new Rconnection();
    
    int i=rc->connect();
    if (i) {
        char msg[128];
        sockerrorchecks(msg, 128, -1);
        printf("unable to connect (result=%d, socket:%s).\n", i, msg); return i;
    }
    printf("connected. Type \"q\" to quit.\n");

    do {
#ifdef READLINE
        c=readline("Rcli> ");
        if (c && *c) add_history(c);
#else
        printf("Rcli> ");
        c[1023]=0;
        fgets(c,1023,stdin);
        char *rq=c; while(*rq && rq-c<1023) { if (*rq=='\n') *rq=0; rq++; }
#endif
        
        if (!strcmp(c,"q") || !strcmp(c,"quit") || !strcmp(c,"exit")) {
            cout << "ok, you got enough, right? leaving.\n";
            break;
        }
        int res=0;
        if (!strcmp(c,"shutdown")) { // this is a demo of low-level commands being send via "request"
            Rmessage *msg=new Rmessage();
            cout << "performing shutdown.\n";
            res=rc->request(msg,CMD_shutdown);
            delete(msg);
        } else if (*c) { // this is a regular "eval" demo
            Rexp *xp=rc->eval(c, &res);
            if (xp) {
                cout << "type=" << xp->type << ", len=" << xp->len << ", result:" << *xp << "\n"; 
                if (xp->attr) cout << "attributes: " << *xp->attr << "\n";
                delete(xp);
            } else cout << "eval failed with result " << res << "\n";
        }
        if (res) {
            cout << "request failed with error code " << res << "\n";
            break;
        }
    } while (1);
    
    delete(rc);
    
    return 0;
}
