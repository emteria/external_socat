/* source: xio-listen.c */
/* Copyright Gerhard Rieger 2001-2008 */
/* Published under the GNU General Public License V.2, see file COPYING */

/* this file contains the source for listen socket options */

#include "xiosysincludes.h"

#if WITH_LISTEN

#include "xioopen.h"
#include "xio-named.h"
#include "xio-socket.h"
#include "xio-ip.h"
#include "xio-ip4.h"
#include "xio-listen.h"
#include "xio-tcpwrap.h"

/***** LISTEN options *****/
const struct optdesc opt_backlog = { "backlog",   NULL, OPT_BACKLOG,     GROUP_LISTEN, PH_LISTEN, TYPE_INT,    OFUNC_SPEC };
const struct optdesc opt_fork    = { "fork",      NULL, OPT_FORK,        GROUP_CHILD,   PH_PASTACCEPT, TYPE_BOOL,  OFUNC_SPEC };
/**/
#if (WITH_UDP || WITH_TCP)
const struct optdesc opt_range   = { "range",     NULL, OPT_RANGE,       GROUP_RANGE,  PH_ACCEPT, TYPE_STRING, OFUNC_SPEC };
#endif


int
   xioopen_listen(struct single *xfd, int xioflags,
		  struct sockaddr *us, socklen_t uslen,
		  struct opt *opts, struct opt *opts0,
		  int pf, int socktype, int proto) {
   int level;
   int result;

#if WITH_RETRY
   if (xfd->forever || xfd->retry) {
      level = E_INFO;
   } else
#endif /* WITH_RETRY */
      level = E_ERROR;

   while (true) {	/* loop over failed attempts */

      /* tcp listen; this can fork() for us; it only returns on error or on
	 successful establishment of tcp connection */
      result = _xioopen_listen(xfd, xioflags,
			       (struct sockaddr *)us, uslen,
			       opts, pf, socktype, proto,
#if WITH_RETRY
			       (xfd->retry||xfd->forever)?E_INFO:E_ERROR
#else
			       E_ERROR
#endif /* WITH_RETRY */
			       );
	 /*! not sure if we should try again on retry/forever */
      switch (result) {
      case STAT_OK: break;
#if WITH_RETRY
      case STAT_RETRYLATER:
      case STAT_RETRYNOW:
	 if (xfd->forever || xfd->retry) {
	    dropopts(opts, PH_ALL); opts = copyopts(opts0, GROUP_ALL);
	    if (result == STAT_RETRYLATER) {
	       Nanosleep(&xfd->intervall, NULL);
	    }
	    dropopts(opts, PH_ALL); opts = copyopts(opts0, GROUP_ALL);
	    --xfd->retry;
	    continue;
	 }
	 return STAT_NORETRY;
#endif /* WITH_RETRY */
      default:
	 return result;
      }

      break;
   }	/* drop out on success */

   return result;
}


/* waits for incoming connection, checks its source address and port. Depending
   on fork option, it may fork a subprocess.
   Returns 0 if a connection was accepted; with fork option, this is always in
   a subprocess!
   Other return values indicate a problem; this can happen in the master
   process or in a subprocess.
   This function does not retry. If you need retries, handle this is a
   loop in the calling function.
   after fork, we set the forever/retry of the child process to 0
 */
int _xioopen_listen(struct single *xfd, int xioflags, struct sockaddr *us, socklen_t uslen,
		 struct opt *opts, int pf, int socktype, int proto, int level) {
   struct sockaddr sa;
   socklen_t salen;
   int backlog = 5;	/* why? 1 seems to cause problems under some load */
   char *rangename;
   bool dofork = false;
   pid_t pid;	/* mostly int; only used with fork */
   char infobuff[256];
   char lisname[256];
   int result;

   retropt_bool(opts, OPT_FORK, &dofork);

   if (dofork) {
      if (!(xioflags & XIO_MAYFORK)) {
	 Error("option fork not allowed here");
	 return STAT_NORETRY;
      }
      xfd->flags |= XIO_DOESFORK;
   }

   if (applyopts_single(xfd, opts, PH_INIT) < 0)  return -1;

   if (dofork) {
      xiosetchilddied();	/* set SIGCHLD handler */
   }

   if ((xfd->fd = Socket(pf, socktype, proto)) < 0) {
      Msg4(level,
	   "socket(%d, %d, %d): %s", pf, socktype, proto, strerror(errno));
      return STAT_RETRYLATER;
   }

   applyopts(xfd->fd, opts, PH_PASTSOCKET);

   applyopts_cloexec(xfd->fd, opts);

   applyopts(xfd->fd, opts, PH_PREBIND);
   applyopts(xfd->fd, opts, PH_BIND);
   if (Bind(xfd->fd, (struct sockaddr *)us, uslen) < 0) {
      Msg4(level, "bind(%d, {%s}, "F_Zd"): %s", xfd->fd,
	   sockaddr_info(us, uslen, infobuff, sizeof(infobuff)), uslen,
	   strerror(errno));
      Close(xfd->fd);
      return STAT_RETRYLATER;
   }

#if WITH_UNIX
   if (us->sa_family == AF_UNIX) {
      applyopts_named(((struct sockaddr_un *)us)->sun_path, opts, PH_FD);
   }
#endif
   /* under some circumstances (e.g., TCP listen on port 0) bind() fills empty
      fields that we want to know. */
   salen = sizeof(sa);
   if (Getsockname(xfd->fd, us, &uslen) < 0) {
      Warn4("getsockname(%d, %p, {%d}): %s",
	    xfd->fd, &us, uslen, strerror(errno));
   }

   applyopts(xfd->fd, opts, PH_PASTBIND);
#if WITH_UNIX
   if (us->sa_family == AF_UNIX) {
      /*applyopts_early(((struct sockaddr_un *)us)->sun_path, opts);*/
      applyopts_named(((struct sockaddr_un *)us)->sun_path, opts, PH_EARLY);
      applyopts_named(((struct sockaddr_un *)us)->sun_path, opts, PH_PREOPEN);
   }
#endif /* WITH_UNIX */

   retropt_int(opts, OPT_BACKLOG, &backlog);
   if (Listen(xfd->fd, backlog) < 0) {
      Error3("listen(%d, %d): %s", xfd->fd, backlog, strerror(errno));
      return STAT_RETRYLATER;
   }

#if WITH_IP4 /*|| WITH_IP6*/
   if (retropt_string(opts, OPT_RANGE, &rangename) >= 0) {
      if (parserange(rangename, us->sa_family, &xfd->para.socket.range) < 0) {
	 free(rangename);
	 return STAT_NORETRY;
      }
      free(rangename);
      xfd->para.socket.dorange = true;
   }
#endif

#if (WITH_TCP || WITH_UDP) && WITH_LIBWRAP
   xio_retropt_tcpwrap(xfd, opts);
#endif /* && (WITH_TCP || WITH_UDP) && WITH_LIBWRAP */

#if WITH_TCP || WITH_UDP
   if (retropt_ushort(opts, OPT_SOURCEPORT, &xfd->para.socket.ip.sourceport) >= 0) {
      xfd->para.socket.ip.dosourceport = true;
   }
   retropt_bool(opts, OPT_LOWPORT, &xfd->para.socket.ip.lowport);
#endif /* WITH_TCP || WITH_UDP */

   if (xioopts.logopt == 'm') {
      Info("starting accept loop, switching to syslog");
      diag_set('y', xioopts.syslogfac);  xioopts.logopt = 'y';
   } else {
      Info("starting accept loop");
   }
   while (true) {	/* but we only loop if fork option is set */
      char peername[256];
      char sockname[256];
      int ps;		/* peer socket */
      union sockaddr_union _peername;
      union sockaddr_union _sockname;
      union sockaddr_union *pa = &_peername;	/* peer address */
      union sockaddr_union *la = &_sockname;	/* local address */
      socklen_t pas = sizeof(_peername);	/* peer address size */
      socklen_t las = sizeof(_sockname);	/* local address size */
      salen = sizeof(struct sockaddr);

      do {
	 /*? int level = E_ERROR;*/
	 Notice1("listening on %s", sockaddr_info(us, uslen, lisname, sizeof(lisname)));
	 ps = Accept(xfd->fd, (struct sockaddr *)&sa, &salen);
	 if (ps >= 0) {
	    /*0 Info4("accept(%d, %p, {"F_Zu"}) -> %d", xfd->fd, &sa, salen, ps);*/
	    break;	/* success, break out of loop */
	 }
	 if (errno == EINTR) {
	    continue;
	 }
	 if (errno == ECONNABORTED) {
	    Notice4("accept(%d, %p, {"F_Zu"}): %s",
		    xfd->fd, &sa, salen, strerror(errno));
	    continue;
	 }
	 Msg4(level, "accept(%d, %p, {"F_Zu"}): %s",
	      xfd->fd, &sa, salen, strerror(errno));
	 Close(xfd->fd);
	 return STAT_RETRYLATER;
      } while (true);
      applyopts_cloexec(ps, opts);
      if (Getpeername(ps, &pa->soa, &pas) < 0) {
	 Warn4("getpeername(%d, %p, {"F_socklen"}): %s",
	       ps, pa, pas, strerror(errno));
      }
      if (Getsockname(ps, &la->soa, &las) < 0) {
	 Warn4("getsockname(%d, %p, {"F_socklen"}): %s",
	       ps, pa, pas, strerror(errno));
      }
      Notice2("accepting connection from %s on %s",
	      sockaddr_info(&pa->soa, pas, peername, sizeof(peername)),
	      sockaddr_info(&la->soa, las, sockname, sizeof(sockname)));

      if (xiocheckpeer(xfd, pa, la) < 0) {
	 if (Shutdown(ps, 2) < 0) {
	    Info2("shutdown(%d, 2): %s", ps, strerror(errno));
	 }
	 continue;
      }

      Info1("permitting connection from %s",
	    sockaddr_info((struct sockaddr *)pa, pas,
			  infobuff, sizeof(infobuff)));

      applyopts(xfd->fd, opts, PH_FD);

      applyopts(xfd->fd, opts, PH_CONNECTED);

      if (dofork) {
	 if ((pid = Fork()) < 0) {
	    Msg1(level, "fork(): %s", strerror(errno));
	    Close(xfd->fd);
	    return STAT_RETRYLATER;
	 }
	 if (pid == 0) {	/* child */
	    if (Close(xfd->fd) < 0) {
	       Info2("close(%d): %s", xfd->fd, strerror(errno));
	    }
	    xfd->fd = ps;

#if WITH_RETRY
	    /* !? */
	    xfd->retry = 0;
	    xfd->forever = 0;
	    level = E_ERROR;
#endif /* WITH_RETRY */

	    /* drop parents locks, reset FIPS... */
	    if (xio_forked_inchild() != 0) {
	       Exit(1);
	    }

#if WITH_UNIX
	    /* with UNIX sockets: only listening parent is allowed to remove
	       the socket file */
	    xfd->opt_unlink_close = false;
#endif /* WITH_UNIX */

	    break;
	 }

	 /* server: continue loop with listen */
	 /* shutdown() closes the socket even for the child process, but
	    close() does what we want */
	 if (Close(ps) < 0) {
	    Info2("close(%d): %s", ps, strerror(errno));
	 }
	 Notice1("forked off child process "F_pid, pid);
	 Info("still listening");
      } else {
	 if (Close(xfd->fd) < 0) {
	    Info2("close(%d): %s", xfd->fd, strerror(errno));
	 }
	 xfd->fd = ps;
	break;
      }
   }
   if ((result = _xio_openlate(xfd, opts)) < 0)
      return result;

   return 0;
}

#endif /* WITH_LISTEN */