/***************************Copyright-DO-NOT-REMOVE-THIS-LINE**
 * CONDOR Copyright Notice
 *
 * See LICENSE.TXT for additional notices and disclaimers.
 *
 * Copyright (c)1990-1998 CONDOR Team, Computer Sciences Department, 
 * University of Wisconsin-Madison, Madison, WI.  All Rights Reserved.  
 * No use of the CONDOR Software Program Source Code is authorized 
 * without the express consent of the CONDOR Team.  For more information 
 * contact: CONDOR Team, Attention: Professor Miron Livny, 
 * 7367 Computer Sciences, 1210 W. Dayton St., Madison, WI 53706-1685, 
 * (608) 262-0856 or miron@cs.wisc.edu.
 *
 * U.S. Government Rights Restrictions: Use, duplication, or disclosure 
 * by the U.S. Government is subject to restrictions as set forth in 
 * subparagraph (c)(1)(ii) of The Rights in Technical Data and Computer 
 * Software clause at DFARS 252.227-7013 or subparagraphs (c)(1) and 
 * (2) of Commercial Computer Software-Restricted Rights at 48 CFR 
 * 52.227-19, as applicable, CONDOR Team, Attention: Professor Miron 
 * Livny, 7367 Computer Sciences, 1210 W. Dayton St., Madison, 
 * WI 53706-1685, (608) 262-0856 or miron@cs.wisc.edu.
****************************Copyright-DO-NOT-REMOVE-THIS-LINE**/

#include "condor_common.h"
#include "condor_debug.h"		/* for EXCEPT */
#include "condor_sys.h"
#include "errno.h"
#include "debug.h"

extern	char	*strerror();

#if defined(HPUX)
extern void _sigreturn();
#endif

#if defined(HPUX)
#define MASK_TYPE long
#else
#define MASK_TYPE int
#endif

#if defined(NSIG)
#define NUM_SIGS NSIG
#else
#error Must know NSIG -  see comment here in the code !
/* If you get the error message above, you need to devise a way so that
 * NSIG is set to be the maximum number of signals supported on this
 * platform.  Usually, NSIG is defined in <signal.h>, but you may need
 * to edit condor_includes/condor_fix_signal.h to define or undefine something
 * in order to fix this.  DO NOT JUST DEFINE NSIG SOMEPLACE YOURSELF.  Why?  
 * Say you define NSIG to be 25 for the Foo operating system.  A few months
 * later, Foo 2.0 comes out and has 5 new signals for a total of 30.  Your
 * own little define still says 25, and is now wrong.  Get it? By using a 
 * true value in the system header files you are ensured they will get updated
 * with new versions of the operating system.
 */
#endif

void display_sigstate( int line, const char * file );

#if defined(LINUX)
typedef int				SS_TYPE;
#elif defined(Solaris) || defined(IRIX)
typedef struct sigaltstack SS_TYPE;
#else
typedef struct sigstack SS_TYPE;
#endif

struct signal_states_struct {
	int nsigs;		/* the number of signals supported on this platform +1 */
	sigset_t user_mask;
	struct sigaction actions[NUM_SIGS];
	sigset_t pending;
	SS_TYPE sig_stack; 
} ;

static struct signal_states_struct signal_states;


/* Here we save the signal state of the user process.  This is called
 * when we are checkpointing.  */
void
condor_save_sigstates()
{
	sigset_t mask;
	int scm;
	int sig;

    scm = SetSyscalls( SYS_LOCAL | SYS_UNRECORDED );

	/* Save the user's signal mask */
	sigprocmask(SIG_SETMASK,(sigset_t *)0L,&(signal_states.user_mask));

	/* Save pending signal information */
	sigpending( &(signal_states.pending) );

	if ( signal_states.nsigs == 0 ) {
#if !SIGISMEMBER_IS_BROKEN
		/* This code only runs once; here we figure out how many signals
		 * this OS supports and verify our array (of size NUM_SIGS) is
		 * big enough to store all the state. */
		sigfillset(&mask);
		sig = 1;
		while ( sigismember(&mask,sig) > 0 ) sig++;
		signal_states.nsigs = sig;
		if ( sig > NUM_SIGS ) {
			dprintf( D_ALWAYS,
					 "must recompile to support %d signals; current max=%d\n",
					 sig, NUM_SIGS);
			Suicide();
		}
#else 
		signal_states.nsigs = NUM_SIGS;
#endif
	}

	/* Save handler information for each signal */
	for ( sig=1; sig < signal_states.nsigs; sig++ ) {
		sigaction(sig,NULL,&(signal_states.actions[sig]));
	}

	/* Save pointer to signal stack (not POSIX, but widely supported) */	
#if defined(Solaris) || defined(IRIX)
	sigaltstack( (SS_TYPE *) 0, &(signal_states.sig_stack) ); 
#elif !defined(LINUX)
	sigstack( (struct sigstack *) 0, &(signal_states.sig_stack) ); 
#endif

    (void) SetSyscalls( scm );
}

/* Here we restore the signal state of the user process.  This is called
 * upon a restart of a checkpointed process.  The data space must be
 * restored _before_ calling this function (all signal state info is saved
 * in the static structure signal_states) */
void
condor_restore_sigstates()
{
	int scm;
	int sig;
	pid_t mypid;

	scm = SetSyscalls( SYS_LOCAL | SYS_UNRECORDED );

#if defined(HPUX)
     /* Tell the kernel to fill in our process's "trampoline" return
	  * code function with _sigreturn, so that signal handlers can
	  * return properly.  HPUX normally does this in _start().  But
	  * since we are restarting and thus not calling _start(), we must
	  * do it here.  (The constant is a magic cookie to prove we are
	  * post pa-risc 89 revision era) -Todd Tannenbaum, 12/94  
	  */
     _sigsetreturn(_sigreturn,0x06211988,sizeof(struct sigcontext));
#endif

	/* Restore signal actions */
	for ( sig=1; sig < signal_states.nsigs; sig++ ) {
		switch (sig) {
			/* Do not fiddle with the special Condor signals under
			 * any circumstance! */
			case SIGUSR1:
			case SIGUSR2:
			case SIGTSTP:
			case SIGKILL:
			case SIGSTOP:
			case SIGCONT:
				continue;
		}
		sigaction(sig, &(signal_states.actions[sig]), NULL);
	}

	/* Restore signal mask, making _certain_ that Condor & special
	   signals are not blocked */
	/* seems to me my logic here is flawed, since this is running as 
	 * a TSTP signal handler right now, and as soon as this signal
	 * handler returns, the previous mask will be replaced. So this
	 * probably need not be here....  -Todd 12/94 */
	sigdelset(&(signal_states.user_mask),SIGUSR1);
	sigdelset(&(signal_states.user_mask),SIGUSR2);
	sigdelset(&(signal_states.user_mask),SIGTSTP);
	sigdelset(&(signal_states.user_mask),SIGKILL);
	sigdelset(&(signal_states.user_mask),SIGSTOP);
	sigdelset(&(signal_states.user_mask),SIGCONT);
	sigprocmask(SIG_SETMASK,&(signal_states.user_mask),NULL);

	/* Restore signal stack pointer */
#if defined(Solaris) || defined(IRIX)
	sigaltstack( &(signal_states.sig_stack), (SS_TYPE *) 0 );  
#elif !defined(LINUX)
	sigstack( &(signal_states.sig_stack), (struct sigstack *) 0 );  
#endif

	/* Restore pending signals, again ignoring special Condor sigs */
	mypid = getpid();
	for ( sig=1; sig < signal_states.nsigs; sig++ ) {
		switch (sig) {
			/* Do not fiddle with the special Condor signals under
			 * any circumstance! */
			case SIGUSR1:
			case SIGUSR2:
			case SIGTSTP:
			case SIGKILL:
			case SIGSTOP:
			case SIGCONT:
				continue;
		}
		if ( sigismember(&(signal_states.pending),sig) == 1 ) {
			kill(mypid,sig);
		}
	}

	(void) SetSyscalls( scm );
}


#if defined (SYS_sigvec)
#if defined(HPUX)
sigvector( sig, vec, ovec )
#else
sigvec( sig, vec, ovec )
#endif
int sig;
const struct sigvec *vec;
struct sigvec *ovec;
{
	int rval;
	struct	sigvec	nvec;

	if( ! MappingFileDescriptors() ) {
#if defined(HPUX)
			rval = syscall( SYS_sigvector, sig, vec, ovec );
#elif defined(SUNOS41) || defined(ULTRIX43)
			rval = SIGVEC( sig, vec, ovec );
#else
			rval = syscall( SYS_sigvec, sig, vec, ovec );
#endif
	} else {
		switch (sig) {
				/* disallow the special Condor signals */
				/* this list could be shortened */
		case SIGUSR1:
		case SIGUSR2:
		case SIGTSTP:
		case SIGCONT:
			rval = -1;
			errno = EINVAL;
			break;
		default:
			if (vec) {
				memcpy( (char *) &nvec,(char *) vec, sizeof nvec );
				/* while in the handler, block checkpointing */
#if defined(LINUX)
				nvec.sv_mask |= __sigmask( SIGTSTP );
				nvec.sv_mask |= __sigmask( SIGUSR1 );
				nvec.sv_mask |= __sigmask( SIGUSR2 );
#else
				nvec.sv_mask |= sigmask( SIGTSTP );
				nvec.sv_mask |= sigmask( SIGUSR1 );
				nvec.sv_mask |= sigmask( SIGUSR2 );
#endif
			}

#if defined(HPUX)
			rval = syscall( SYS_sigvector, sig, vec ? &nvec : vec, ovec );
#elif defined(SUNOS41) || defined(ULTRIX43)
			rval = SIGVEC( sig, vec ? &nvec : vec, ovec );
#else
			rval = syscall( SYS_sigvec, sig, vec ? &nvec : vec, ovec );
#endif
			break;
		}
	}

	return( rval );
}
#endif

#if defined (SYS_sigvec)
#if defined(HPUX)
_sigvector( int sig, const struct sigvec vec, struct sigvec ovec )
{
	sigvector( sig, vec, ovec );
}
#else
_sigvec( int sig, const struct sigvec vec, struct sigvec ovec )
{
	sigvec( sig, vec, ovec );
}
#endif
#endif


#if defined(SYS_sigblock)
MASK_TYPE 
sigblock( mask )
MASK_TYPE mask;
{
	MASK_TYPE condor_sig_mask;
	MASK_TYPE rval;

	if ( ! MappingFileDescriptors() ) {
		condor_sig_mask = ~0;
	} else {
		/* mask out the special Condor signals */
#if defined(LINUX)
		condor_sig_mask = ~(__sigmask( SIGUSR1 ) | __sigmask( SIGUSR2 ) |
			 __sigmask( SIGTSTP ) | __sigmask(SIGCONT));
#else
		condor_sig_mask = ~(sigmask( SIGUSR1 ) | sigmask( SIGUSR2 ) |
			 sigmask( SIGTSTP ) | sigmask(SIGCONT));
#endif
		mask &= condor_sig_mask;
	}
	rval =  syscall( SYS_sigblock, mask ) & condor_sig_mask;
	return rval;
}
#endif

#if defined(SYS_sigpause)
#ifdef HPUX10
int
sigpause( mask )
int mask;
#else   /* HPUX10 */
MASK_TYPE 
sigpause( mask )
MASK_TYPE mask;
#endif  /* else HPUX10 */
{
	MASK_TYPE condor_sig_mask;
	MASK_TYPE rval;

	if ( MappingFileDescriptors() ) {
		/* mask out the special Condor signals */
#if defined(LINUX)
		condor_sig_mask = ~(__sigmask( SIGUSR1 ) | __sigmask( SIGUSR2 ) |
			 __sigmask( SIGTSTP ) | __sigmask(SIGCONT));
#else
		condor_sig_mask = ~(sigmask( SIGUSR1 ) | sigmask( SIGUSR2 ) |
			 sigmask( SIGTSTP ) | sigmask(SIGCONT));
#endif
		mask &= condor_sig_mask;
	}
	rval =  syscall( SYS_sigpause, mask );
	return rval;
}
#endif

#if defined(SYS_sigsetmask)
MASK_TYPE 
sigsetmask( mask )
MASK_TYPE mask;
{
	MASK_TYPE condor_sig_mask;
	MASK_TYPE rval;

	if ( ! MappingFileDescriptors() ) {
		condor_sig_mask = ~0;
	} else {
		/* mask out the special Condor signals */
#if defined(LINUX)
		condor_sig_mask = ~(__sigmask( SIGUSR1 ) | __sigmask( SIGUSR2 ) |
			 __sigmask( SIGTSTP ) | __sigmask(SIGCONT));
#else
		condor_sig_mask = ~(sigmask( SIGUSR1 ) | sigmask( SIGUSR2 ) |
			 sigmask( SIGTSTP ) | sigmask(SIGCONT));
#endif
		mask &= condor_sig_mask;
	}
	rval =  syscall( SYS_sigsetmask, mask ) & condor_sig_mask;
	return rval;
}
#endif

/* fork() and sigaction() are not in fork.o or sigaction.o on Solaris 2.5
   but instead are only in the threads libraries.  We access the old
   versions through their new names. */

#if defined(Solaris)
#include <signal.h>

int 
SIGACTION(int sig, const struct sigaction *act, struct sigaction *oact)
{
	return _libc_SIGACTION(sig, act, oact);
}
#endif

#if defined(SYS_sigaction)
#if defined(LINUX) && !defined(GLIBC)
int
sigaction( int sig, struct sigaction *act, struct sigaction *oact )
#else
int
sigaction( int sig, const struct sigaction *act, struct sigaction *oact )
#endif
{
	struct sigaction tmp, *my_act = &tmp;

	if( act ) {
		*my_act = *act;
	} else {
		my_act = NULL;
	}

	if ( MappingFileDescriptors() && act ) {		/* called by User code */
		switch (sig) {
			case SIGUSR1:
			case SIGUSR2:
			case SIGTSTP:
			case SIGCONT: /* don't let user code mess with these signals */
				errno = EINVAL;
				return -1;
			default: /* block checkpointing inside users signal handler */
				sigaddset( &(my_act->sa_mask), SIGTSTP );
				sigaddset( &(my_act->sa_mask), SIGUSR2 );
		}
	}

#if defined(OSF1) || defined(ULTRIX43) || defined(Solaris)
	return SIGACTION( sig, my_act, oact);
#elif defined(IRIX)
	return syscall(SYS_ksigaction, sig, my_act, oact);
#else
	return syscall(SYS_sigaction, sig, my_act, oact);
#endif
}
#endif

/* On OSF1 there is a sigprocmask.o and a _sigprocmsk.o.  We extract the
   former and upcase SIGPROCMASK, but we can no longer extract the
   latter, because it results in a redefinition of _user_signal_mask.
   sigprocmask (in sigprocmask.o) calls _sigprocmask (in _sigprocmsk.o).
   In the upcase versions, SIGPROCMASK wants to call _SIGPROCMASK.  Since
   we do not trap _sigprocmask in remote syscalls, we just define
   _SIGPROCMASK in terms of _sigprocmask, and the _user_signal_mask
   data structure is kept in synch whether SIGPROCMASK or sigprocmask is
   called.  -Jim B. */
#if defined(OSF1)
int
_SIGPROCMASK( int how, const sigset_t *set, sigset_t *oset)
{
	_sigprocmask( how, set, oset );
}
#endif

#if defined(SYS_sigprocmask)
sigprocmask( int how, const sigset_t *set, sigset_t *oset)
{
	sigset_t tmp, *my_set;

	if( set ) {
		tmp = *set;
		my_set = &tmp;
	} else {
		my_set = NULL;
	}

	if ( MappingFileDescriptors() && set ) {
		if ( (how == SIG_BLOCK) || (how == SIG_SETMASK) ) {
			sigdelset(my_set,SIGUSR1);
			sigdelset(my_set,SIGUSR2);
			sigdelset(my_set,SIGTSTP);
			sigdelset(my_set,SIGCONT);
		}
	}
#if defined(OSF1) || defined(ULTRIX43)
	SIGPROCMASK(how,my_set,oset);
#else
	return syscall(SYS_sigprocmask,how,my_set,oset);
#endif
}
#endif

#if defined (SYS_sigsuspend)
int
sigsuspend(set)
#if defined(SUNOS41)
sigset_t *set;
#else
const sigset_t *set;
#endif
{
	sigset_t	my_set = *set;
	if ( MappingFileDescriptors() ) {
		sigdelset(&my_set,SIGUSR1);
		sigdelset(&my_set,SIGUSR2);
		sigdelset(&my_set,SIGTSTP);
		sigdelset(&my_set,SIGCONT);
	}
#if defined(OSF1) || defined(ULTRIX43) || defined(LINUX)
	SIGSUSPEND(&my_set);
#else
	return syscall(SYS_sigsuspend,&my_set);
#endif
}
#endif

#if defined(SYS_signal)
#if defined(VOID_SIGNAL_RETURN)
void (*
#else
int (*
#endif
signal(sig,func))(int)
int sig;
void (*func)(int);
{
	if ( MappingFileDescriptors() ) {	
		switch (sig) {
			/* don't let user code mess with these signals */
			case SIGUSR1:
			case SIGUSR2:
			case SIGTSTP:
			case SIGCONT:
				errno = EINVAL;
				return SIG_ERR; 
		}
	}

#if defined(OSF1)
	SIGNAL(sig, func);
#elif defined(VOID_SIGNAL_RETURN)
	return ( (void (*) (int)) syscall(SYS_signal,sig,func) );
#else
	return ( (int (*) (int)) syscall(SYS_signal,sig,func) );
#endif

}
#endif

#if 0
void
display_sigstate( int line, const char * file )
{
	int			scm;
	int			i;
	sigset_t	pending_sigs;
	sigset_t	blocked_sigs;
	struct sigaction	act;
	int			pending, blocked;

	scm = SetSyscalls( SYS_LOCAL | SYS_UNRECORDED );
	sigpending( &pending_sigs );
	sigprocmask(SIG_SETMASK,NULL,&blocked_sigs);

	dprintf( D_ALWAYS, "At line %d in file %s ==============\n", line, file );
	for( i=1; i<NSIG; i++ ) {
		sigaction( i, NULL, &act );

		pending = sigismember( &pending_sigs, i );
		blocked = sigismember( &blocked_sigs, i );
		dprintf( D_ALWAYS, "%d %s pending %s blocked handler = 0x%p\n",
			i,
			pending ? "IS " : "NOT",
			blocked ? "IS " : "NOT",
			act.sa_handler
		);
	}
	dprintf( D_ALWAYS, "====================================\n" );
	SetSyscalls( scm );
}
#endif
