/* Native-dependent code for FreeBSD.

   Copyright (C) 2002-2015 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "gdbcore.h"
#include "inferior.h"
#include "regcache.h"
#include "regset.h"
#include "gdbthread.h"
#include "gdb_wait.h"
#include <sys/types.h>
#include <sys/procfs.h>
#include <sys/ptrace.h>
#include <sys/sysctl.h>
#ifdef HAVE_KINFO_GETVMMAP
#include <sys/user.h>
#include <libutil.h>
#endif

#include "elf-bfd.h"
#include "fbsd-nat.h"

/* Return the name of a file that can be opened to get the symbols for
   the child process identified by PID.  */

static char *
fbsd_pid_to_exec_file (struct target_ops *self, int pid)
{
  ssize_t len = PATH_MAX;
  static char buf[PATH_MAX];
  char name[PATH_MAX];

#ifdef KERN_PROC_PATHNAME
  int mib[4];

  mib[0] = CTL_KERN;
  mib[1] = KERN_PROC;
  mib[2] = KERN_PROC_PATHNAME;
  mib[3] = pid;
  if (sysctl (mib, 4, buf, &len, NULL, 0) == 0)
    return buf;
#endif

  xsnprintf (name, PATH_MAX, "/proc/%d/exe", pid);
  len = readlink (name, buf, PATH_MAX - 1);
  if (len != -1)
    {
      buf[len] = '\0';
      return buf;
    }

  return NULL;
}

#ifdef HAVE_KINFO_GETVMMAP
/* Iterate over all the memory regions in the current inferior,
   calling FUNC for each memory region.  OBFD is passed as the last
   argument to FUNC.  */

static int
fbsd_find_memory_regions (struct target_ops *self,
			  find_memory_region_ftype func, void *obfd)
{
  pid_t pid = ptid_get_pid (inferior_ptid);
  struct kinfo_vmentry *vmentl, *kve;
  uint64_t size;
  struct cleanup *cleanup;
  int i, nitems;

  vmentl = kinfo_getvmmap (pid, &nitems);
  if (vmentl == NULL)
    perror_with_name (_("Couldn't fetch VM map entries."));
  cleanup = make_cleanup (free, vmentl);

  for (i = 0; i < nitems; i++)
    {
      kve = &vmentl[i];

      /* Skip unreadable segments and those where MAP_NOCORE has been set.  */
      if (!(kve->kve_protection & KVME_PROT_READ)
	  || kve->kve_flags & KVME_FLAG_NOCOREDUMP)
	continue;

      /* Skip segments with an invalid type.  */
      if (kve->kve_type != KVME_TYPE_DEFAULT
	  && kve->kve_type != KVME_TYPE_VNODE
	  && kve->kve_type != KVME_TYPE_SWAP
	  && kve->kve_type != KVME_TYPE_PHYS)
	continue;

      size = kve->kve_end - kve->kve_start;
      if (info_verbose)
	{
	  fprintf_filtered (gdb_stdout, 
			    "Save segment, %ld bytes at %s (%c%c%c)\n",
			    (long) size,
			    paddress (target_gdbarch (), kve->kve_start),
			    kve->kve_protection & KVME_PROT_READ ? 'r' : '-',
			    kve->kve_protection & KVME_PROT_WRITE ? 'w' : '-',
			    kve->kve_protection & KVME_PROT_EXEC ? 'x' : '-');
	}

      /* Invoke the callback function to create the corefile segment.
	 Pass MODIFIED as true, we do not know the real modification state.  */
      func (kve->kve_start, size, kve->kve_protection & KVME_PROT_READ,
	    kve->kve_protection & KVME_PROT_WRITE,
	    kve->kve_protection & KVME_PROT_EXEC, 1, obfd);
    }
  do_cleanups (cleanup);
  return 0;
}
#else
static int
fbsd_read_mapping (FILE *mapfile, unsigned long *start, unsigned long *end,
		   char *protection)
{
  /* FreeBSD 5.1-RELEASE uses a 256-byte buffer.  */
  char buf[256];
  int resident, privateresident;
  unsigned long obj;
  int ret = EOF;

  /* As of FreeBSD 5.0-RELEASE, the layout is described in
     /usr/src/sys/fs/procfs/procfs_map.c.  Somewhere in 5.1-CURRENT a
     new column was added to the procfs map.  Therefore we can't use
     fscanf since we need to support older releases too.  */
  if (fgets (buf, sizeof buf, mapfile) != NULL)
    ret = sscanf (buf, "%lx %lx %d %d %lx %s", start, end,
		  &resident, &privateresident, &obj, protection);

  return (ret != 0 && ret != EOF);
}

/* Iterate over all the memory regions in the current inferior,
   calling FUNC for each memory region.  OBFD is passed as the last
   argument to FUNC.  */

static int
fbsd_find_memory_regions (struct target_ops *self,
			  find_memory_region_ftype func, void *obfd)
{
  pid_t pid = ptid_get_pid (inferior_ptid);
  char *mapfilename;
  FILE *mapfile;
  unsigned long start, end, size;
  char protection[4];
  int read, write, exec;
  struct cleanup *cleanup;

  mapfilename = xstrprintf ("/proc/%ld/map", (long) pid);
  cleanup = make_cleanup (xfree, mapfilename);
  mapfile = fopen (mapfilename, "r");
  if (mapfile == NULL)
    error (_("Couldn't open %s."), mapfilename);
  make_cleanup_fclose (mapfile);

  if (info_verbose)
    fprintf_filtered (gdb_stdout, 
		      "Reading memory regions from %s\n", mapfilename);

  /* Now iterate until end-of-file.  */
  while (fbsd_read_mapping (mapfile, &start, &end, &protection[0]))
    {
      size = end - start;

      read = (strchr (protection, 'r') != 0);
      write = (strchr (protection, 'w') != 0);
      exec = (strchr (protection, 'x') != 0);

      if (info_verbose)
	{
	  fprintf_filtered (gdb_stdout, 
			    "Save segment, %ld bytes at %s (%c%c%c)\n",
			    size, paddress (target_gdbarch (), start),
			    read ? 'r' : '-',
			    write ? 'w' : '-',
			    exec ? 'x' : '-');
	}

      /* Invoke the callback function to create the corefile segment.
	 Pass MODIFIED as true, we do not know the real modification state.  */
      func (start, size, read, write, exec, 1, obfd);
    }

  do_cleanups (cleanup);
  return 0;
}
#endif

/*
 * Thread TODO:
 * + handle PL_FLAG_BORN and PL_FLAG_EXIT
 * + update ptid for "first" thread on first PL_FLAG_BORN event
 * + custom to_resume
 *   - suspend / resume individual LWPs
 * + to_thread_alive
 * + to_update_thread_list
 * + to_pid_to_str
 * - get_thread_address (this might be too hard)
 * - "tsd" and "signal" commands
 *   - "tsd" might be a bit of a pain as it doesn't map too well
 *     to a ptrace op
 * - core_pid_to_str gdbarch method? (use THRMISC)
 * - fix gcore to iterate over threads
 *   - might need to save thread names in private_info for fbsd-tdep.c to use?
 */
#ifdef PT_LWPINFO

/* XXX */
#define	HAVE_STRUCT_PTRACE_LWPINFO_PL_TDNAME

static ptid_t (*super_wait) (struct target_ops *,
			     ptid_t,
			     struct target_waitstatus *,
			     int);

#if defined(TDP_RFPPWAIT) || defined(HAVE_STRUCT_PTRACE_LWPINFO_PL_TDNAME)
/* Fetch the external variant of the kernel's internal process
   structure for the process PID into KP.  */

static void
fbsd_fetch_kinfo_proc (pid_t pid, struct kinfo_proc *kp)
{
  size_t len;
  int mib[4];

  len = sizeof *kp;
  mib[0] = CTL_KERN;
  mib[1] = KERN_PROC;
  mib[2] = KERN_PROC_PID;
  mib[3] = pid;
  if (sysctl (mib, 4, kp, &len, NULL, 0) == -1)
    perror_with_name (("sysctl"));
}
#endif

/*
  FreeBSD's first thread support was via a "reentrant" version of libc
  (libc_r) that first shipped in 2.2.7.  This library multiplexed all
  of the threads in a process onto a single kernel thread.  This
  library is supported via the bsd-uthread target.

  FreeBSD 5.1 introduced two new threading libraries that made use of
  multiple kernel threads.  The first (libkse) scheduled M user
  threads onto N (<= M) kernel threads (LWPs).  The second (libthr)
  bound each user thread to a dedicated kernel thread.  libkse shipped
  as the default threading library (libpthread).

  FreeBSD 5.3 added a libthread_db to abstract the interface across
  the various thread libraries (libc_r, libkse, and libthr).

  FreeBSD 7.0 switched the default threading library from from libkse
  to libpthread and removed libc_r.

  FreeBSD 8.0 removed libkse and the in-kernel support for it.  The
  only threading library supported by 8.0 and later is libthr which
  ties each user thread directly to an LWP.  To simplify the
  implementation, this target only supports LWP-backed threads using
  ptrace directly rather than libthread_db.
*/

/* Return true if PTID is still active in the inferior.  */

static int
fbsd_thread_alive (struct target_ops *ops, ptid_t ptid)
{
  if (ptid_lwp_p (ptid))
    {
      struct ptrace_lwpinfo pl;

      if (ptrace (PT_LWPINFO, ptid_get_lwp (ptid), (caddr_t)&pl, sizeof pl)
	  == -1)
	return 0;
#ifdef PL_FLAG_EXITED
      if (pl.pl_flags & PL_FLAG_EXITED)
	return 0;
#endif
    }

  return 1;
}

/* Convert PTID to a string.  Returns the string in a static
   buffer.  */

static char *
fbsd_pid_to_str (struct target_ops *ops, ptid_t ptid)
{
  lwpid_t lwp;

  lwp = ptid_get_lwp (ptid);
  if (lwp != 0)
    {
#ifdef HAVE_STRUCT_PTRACE_LWPINFO_PL_TDNAME
      struct ptrace_lwpinfo pl;
      struct kinfo_proc kp;
#endif
      static char buf[64];
      int pid = ptid_get_pid (ptid);

#ifdef HAVE_STRUCT_PTRACE_LWPINFO_PL_TDNAME
      fbsd_fetch_kinfo_proc (pid, &kp);
      if (ptrace (PT_LWPINFO, lwp, (caddr_t)&pl, sizeof pl) == -1)
	perror_with_name (("ptrace"));
      if (strcmp (kp.ki_comm, pl.pl_tdname) == 0)
	xsnprintf (buf, sizeof buf, "process %d, LWP %d", pid, lwp);
      else
	xsnprintf (buf, sizeof buf, "process %d, LWP %d %s", pid, lwp,
		   pl.pl_tdname);
#else
      xsnprintf (buf, sizeof buf, "process %d, LWP %d", pid, lwp);
#endif
      return buf;
    }

  return normal_pid_to_str (ptid);
}

#ifdef PT_LWP_EVENTS
static void
fbsd_switch_to_threaded (pid_t pid)
{
  struct cleanup *cleanup;
  lwpid_t *lwps;
  struct ptrace_lwpinfo *pl;
  int i, nlwps;

  nlwps = ptrace (PT_GETNUMLWPS, pid, NULL, 0);
  if (nlwps == -1)
    perror_with_name (("ptrace"));

  lwps = xcalloc (nlwps, sizeof *lwps);
  cleanup = make_cleanup (xfree, lwps);

  nlwps = ptrace (PT_GETLWPLIST, pid, (caddr_t)lwps, nlwps);
  if (nlwps == -1)
    perror_with_name (("ptrace"));
  pl = xcalloc (nwlps, sizeof *pl);
  make_cleanup (xfree, pl);
  for (i = 0; i < nlwps; i++)
    {
      if (ptrace (PT_LWPINFO, lwps[i], (caddr_t)&pl[i], sizeof pl[i]) == -1)
	perror_with_name (("ptrace"));
    }

  /* Choose a candidate thread for the main thread.  Prefer the first
     non-BORN and non-EXITED thread.  If all threads are newborns, use
     the first non-EXITED thread.  */
  for (i = 0; i < nlwps; i++)
    {
      ptid_t ptid = ptid_build (pid, lwps[i], 0);

      if (pl[i].pl_flags & (PL_FLAG_BORN | PL_FLAG_EXITED) != 0)
	continue;

      gdb_assert (!in_thread_list (ptid));
      thread_change_ptid (inferior_ptid, ptid);
      break;
    }
  for (i = 0; i < nlwps; i++)
    {
      ptid_t ptid = ptid_build (pid, lwps[i], 0);

      if (pl[i].pl_flags & (PL_FLAG_EXITED) != 0)
	continue;

      if (!ptid_lwp_p (inferior_ptid))
	{
	  gdb_assert (!in_thread_list (ptid));
	  thread_change_ptid (inferior_ptid, ptid);
	}
      else if (!in_thread_list (ptid))
	add_thread (ptid);
    }

  do_cleanups (cleanup);
}

static void
fbsd_enable_lwp_events (pid_t pid)
{
  if (ptrace (PT_LWP_EVENTS, pid, (PTRACE_TYPE_ARG3)0, 1) == -1)
    perror_with_name (("ptrace"));
}
#else
static void
fbsd_add_threads (pid_t pid, int nlwps)
{
  struct cleanup *cleanup;
  lwpid_t *lwps;
  int i;

  lwps = xcalloc (nlwps, sizeof *lwps);
  cleanup = make_cleanup (xfree, lwps);

  nlwps = ptrace (PT_GETLWPLIST, pid, (caddr_t)lwps, nlwps);
  if (nlwps == -1)
    perror_with_name (("ptrace"));

  for (i = 0; i < nlwps; i++)
    {
      ptid_t ptid = ptid_build (pid, lwps[i], 0);

      /* If this inferior is not using LWP ptids, use the first LWP as
	 the main thread.  */
      if (!ptid_lwp_p (inferior_ptid))
	{
	  gdb_assert (!in_thread_list (ptid));
	  thread_change_ptid (inferior_ptid, ptid);
	}
      else if (!in_thread_list (ptid))
	{
	  gdb_assert (ptid_lwp_p (inferior_ptid));
	  add_thread (ptid);
	}
    }
  do_cleanups (cleanup);
}
#endif

/* Implement the to_update_thread_list target method for this
   target.  */

static void
fbsd_update_thread_list (struct target_ops *ops)
{
#ifdef PT_LWP_EVENTS
  /* With support for thread events, threads are added/deleted from the
     list as events are reported, so just try deleting exited threads.  */
  delete_exited_threads ();
#else
  int nlwps;
  int pid = ptid_get_pid (inferior_ptid);

  /* XXX: Not sure this is needed. */
  gdb_assert (!target_has_execution);

  prune_threads();

  nlwps = ptrace (PT_GETNUMLWPS, pid, NULL, 0);
  if (nlwps == -1)
    perror_with_name (("ptrace"));

  /* Leave single-threaded processes with a non-threaded ptid alone.  */
  if (nlwps == 1 && !ptid_lwp_p (inferior_ptid))
    return;

  fbsd_add_threads (pid, nlwps);
#endif
}

static void (*super_resume) (struct target_ops *,
			     ptid_t,
			     int,
			     enum gdb_signal);

static int
resume_one_thread_cb(struct thread_info *tp, void *data)
{
  ptid_t *ptid = data;
  int request;

  if (ptid_get_pid (tp->ptid) != ptid_get_pid (*ptid))
    return 0;

  if (ptid_get_lwp (tp->ptid) == ptid_get_lwp (*ptid))
    request = PT_RESUME;
  else
    request = PT_SUSPEND;
  
  if (ptrace (PT_SUSPEND, ptid_get_lwp (tp->ptid), (caddr_t)0, 0) == -1)
    perror_with_name (("ptrace"));
  return 0;
}

static int
resume_all_threads_cb(struct thread_info *tp, void *data)
{
  ptid_t *filter = data;

  if (!ptid_match (tp->ptid, *filter))
    return 0;

  /* Ignore single-threaded processes.  */
  if (!ptid_lwp_p (tp->ptid))
    return 0;

  if (ptrace (PT_RESUME, ptid_get_lwp (tp->ptid), (caddr_t)0, 0) == -1)
    perror_with_name (("ptrace"));
  return 0;
}

static void
fbsd_resume (struct target_ops *ops,
	     ptid_t ptid, int step, enum gdb_signal signo)
{
  if (ptid_lwp_p (ptid))
    {
      /* If ptid is a specific LWP, suspend all other LWPs in the process.  */
      iterate_over_threads (resume_one_thread_cb, &ptid);
    }
  else
    {
      /* If ptid is a wildcard, resume all matching threads (they won't run
	 until the process is continued however).  */
      iterate_over_threads (resume_all_threads_cb, &ptid);
      ptid = inferior_ptid;
    }
  super_resume (ops, ptid, step, signo);
}

#ifdef TDP_RFPPWAIT
/*
  To catch fork events, PT_FOLLOW_FORK is set on every traced process
  to enable stops on returns from fork or vfork.  Note that both the
  parent and child will always stop, even if system call stops are not
  enabled.

  After a fork, both the child and parent process will stop and report
  an event.  However, there is no guarantee of order.  If the parent
  reports its stop first, then fbsd_wait explicitly waits for the new
  child before returning.  If the child reports its stop first, then
  the event is saved on a list and ignored until the parent's stop is
  reported.  fbsd_wait could have been changed to fetch the parent PID
  of the new child and used that to wait for the parent explicitly.
  However, if two threads in the parent fork at the same time, then
  the wait on the parent might return the "wrong" fork event.

  The initial version of PT_FOLLOW_FORK did not set PL_FLAG_CHILD for
  the new child process.  This flag could be inferred by treating any
  events for an unknown pid as a new child.

  In addition, the initial version of PT_FOLLOW_FORK did not report a
  stop event for the parent process of a vfork until after the child
  process executed a new program or exited.  The kernel was changed to
  defer the wait for exit or exec of the child until after posting the
  stop event shortly after the change to introduce PL_FLAG_CHILD.
  This could be worked around by reporting a vfork event when the
  child event posted and ignoring the subsequent event from the
  parent.

  This implementation requires both of these fixes for simplicity's
  sake.  FreeBSD versions newer than 9.1 contain both fixes.
*/

struct fbsd_fork_child_info
{
  struct fbsd_fork_child_info *next;
  pid_t child;			/* Pid of new child.  */
};

static struct fbsd_fork_child_info *fbsd_pending_children;

/* Record a new child process event that is reported before the
   corresponding fork event in the parent.  */

static void
fbsd_remember_child (pid_t pid)
{
  struct fbsd_fork_child_info *info;

  info = xcalloc (1, sizeof *info);

  info->child = pid;
  info->next = fbsd_pending_children;
  fbsd_pending_children = info;
}

/* Check for a previously-recorded new child process event for PID.
   If one is found, remove it from the list.  */

static int
fbsd_is_child_pending (pid_t pid)
{
  struct fbsd_fork_child_info *info, *prev;

  prev = NULL;
  for (info = fbsd_pending_children; info; prev = info, info = info->next)
    {
      if (info->child == pid)
	{
	  if (prev == NULL)
	    fbsd_pending_children = info->next;
	  else
	    prev->next = info->next;
	  xfree (info);
	  return 1;
	}
    }
  return 0;
}
#endif

/* Wait for the child specified by PTID to do something.  Return the
   process ID of the child, or MINUS_ONE_PTID in case of error; store
   the status in *OURSTATUS.  */

static ptid_t
fbsd_wait (struct target_ops *ops,
	   ptid_t ptid, struct target_waitstatus *ourstatus,
	   int target_options)
{
  ptid_t wptid;

  while (1)
    {
      wptid = super_wait (ops, ptid, ourstatus, target_options);
      if (ourstatus->kind == TARGET_WAITKIND_STOPPED)
	{
	  struct ptrace_lwpinfo pl;
	  pid_t pid;
	  int status;

	  pid = ptid_get_pid (wptid);
	  if (ptrace (PT_LWPINFO, pid, (caddr_t)&pl, sizeof pl) == -1)
	    perror_with_name (("ptrace"));

#ifdef TDP_RFPPWAIT
	  if (pl.pl_flags & PL_FLAG_FORKED)
	    {
	      struct kinfo_proc kp;
	      pid_t child;

	      child = pl.pl_child_pid;
	      ourstatus->kind = TARGET_WAITKIND_FORKED;
	      ourstatus->value.related_pid = pid_to_ptid (child);

	      /* Make sure the other end of the fork is stopped too.  */
	      if (!fbsd_is_child_pending (child))
		{
		  pid = waitpid (child, &status, 0);
		  if (pid == -1)
		    perror_with_name (("waitpid"));

		  gdb_assert (pid == child);

		  if (ptrace (PT_LWPINFO, child, (caddr_t)&pl, sizeof pl) == -1)
		    perror_with_name (("ptrace"));

		  gdb_assert (pl.pl_flags & PL_FLAG_CHILD);
		}

	      /* For vfork, the child process will have the P_PPWAIT
		 flag set.  */
	      fbsd_fetch_kinfo_proc (child, &kp);
	      if (kp.ki_flag & P_PPWAIT)
		ourstatus->kind = TARGET_WAITKIND_VFORKED;

	      return wptid;
	    }

	  if (pl.pl_flags & PL_FLAG_CHILD)
	    {
	      /* Remember that this child forked, but do not report it
		 until the parent reports its corresponding fork
		 event.  */
	      fbsd_remember_child (ptid_get_pid (wptid));
	      continue;
	    }
#endif

#ifdef PL_FLAG_EXEC
	  if (pl.pl_flags & PL_FLAG_EXEC)
	    {
	      ourstatus->kind = TARGET_WAITKIND_EXECD;
	      ourstatus->value.execd_pathname
		= xstrdup (fbsd_pid_to_exec_file (NULL, pid));
	      return wptid;
	    }
#endif

#ifdef PT_LWP_EVENTS
	  if (pl.pl_flags & PL_FLAG_BORN)
	    {
	      if (ptid_lwp_p (inferior_ptid))
		{
		  ptid_t ptid = ptid_build (pid, pl.pl_lwpid, 0);
		  gdb_assert(!in_thread_list (ptid));
		  add_thread (ptid);
		}
	      else
		fbsd_switch_to_threaded (pid);
	      ourstatus->kind = TARGET_WAITKIND_IGNORE;
	      return wptid;
	    }
	  if (pl.pl_flags & PL_FLAG_EXITED)
	    {
	      ptid_t ptid = ptid_build (pid, pl.pl_lwpid, 0);

	      gdb_assert (in_thread_list (ptid));
	      delete_thread (ptid);
	      ourstatus->kind = TARGET_WAITKIND_IGNORE;
	      return wptid;
	    }
#endif
	}
      return wptid;
    }
}

#ifdef TDP_RFPPWAIT
/* Target hook for follow_fork.  On entry and at return inferior_ptid is
   the ptid of the followed inferior.  */

static int
fbsd_follow_fork (struct target_ops *ops, int follow_child,
			int detach_fork)
{
  if (!follow_child)
    {
      struct thread_info *tp = inferior_thread ();
      pid_t child_pid = ptid_get_pid (tp->pending_follow.value.related_pid);

      /* Breakpoints have already been detached from the child by
	 infrun.c.  */

      if (ptrace (PT_DETACH, child_pid, (PTRACE_TYPE_ARG3)1, 0) == -1)
	perror_with_name (("ptrace"));
    }

  return 0;
}

static int
fbsd_insert_fork_catchpoint (struct target_ops *self, int pid)
{
  return 0;
}

static int
fbsd_remove_fork_catchpoint (struct target_ops *self, int pid)
{
  return 0;
}

static int
fbsd_insert_vfork_catchpoint (struct target_ops *self, int pid)
{
  return 0;
}

static int
fbsd_remove_vfork_catchpoint (struct target_ops *self, int pid)
{
  return 0;
}

/* Enable fork tracing for a specific process.
   
   To catch fork events, PT_FOLLOW_FORK is set on every traced process
   to enable stops on returns from fork or vfork.  Note that both the
   parent and child will always stop, even if system call stops are
   not enabled.  */

static void
fbsd_enable_follow_fork (pid_t pid)
{
  if (ptrace (PT_FOLLOW_FORK, pid, (PTRACE_TYPE_ARG3)0, 1) == -1)
    perror_with_name (("ptrace"));
}
#endif

/* Implement the "to_post_startup_inferior" target_ops method.  */

static void
fbsd_post_startup_inferior (struct target_ops *self, ptid_t pid)
{
#ifdef TDP_RFPPWAIT
  fbsd_enable_follow_fork (ptid_get_pid (pid));
#endif
#ifdef PT_LWP_EVENTS
  fbsd_enable_lwp_events (ptid_get_pid (pid));
#endif
}

/* Implement the "to_post_attach" target_ops method.  */

static void
fbsd_post_attach (struct target_ops *self, int pid)
{
  int nlwps;

#ifdef TDP_RFPPWAIT
  fbsd_enable_follow_fork (pid);
#endif
#ifdef PT_LWP_EVENTS
  fbsd_enable_lwp_events (pid);
#endif

  /* Add threads for other LWPs when attaching to a threaded process.  */
  nlwps = ptrace (PT_GETNUMLWPS, pid, NULL, 0);
  if (nlwps == -1)
    perror_with_name (("ptrace"));
  if (nlwps > 1)
    {
#ifdef PT_LWP_EVENTS
      fbsd_switch_to_threaded (pid);
#else
      fbsd_add_threads (pid, nlwps);
#endif
    }
}

#ifdef PL_FLAG_EXEC
/* If the FreeBSD kernel supports PL_FLAG_EXEC, then traced processes
   will always stop after exec.  */

static int
fbsd_insert_exec_catchpoint (struct target_ops *self, int pid)
{
  return 0;
}

static int
fbsd_remove_exec_catchpoint (struct target_ops *self, int pid)
{
  return 0;
}
#endif
#endif

void
fbsd_nat_add_target (struct target_ops *t)
{
  t->to_pid_to_exec_file = fbsd_pid_to_exec_file;
  t->to_find_memory_regions = fbsd_find_memory_regions;
#ifdef PT_LWPINFO
  t->to_thread_alive = fbsd_thread_alive;
  t->to_pid_to_str = fbsd_pid_to_str;
  t->to_update_thread_list = fbsd_update_thread_list;
  super_resume = t->to_resume;
  t->to_resume = fbsd_resume;
  super_wait = t->to_wait;
  t->to_wait = fbsd_wait;
  t->to_post_startup_inferior = fbsd_post_startup_inferior;
  t->to_post_attach = fbsd_post_attach;
#ifdef TDP_RFPPWAIT
  t->to_follow_fork = fbsd_follow_fork;
  t->to_insert_fork_catchpoint = fbsd_insert_fork_catchpoint;
  t->to_remove_fork_catchpoint = fbsd_remove_fork_catchpoint;
  t->to_insert_vfork_catchpoint = fbsd_insert_vfork_catchpoint;
  t->to_remove_vfork_catchpoint = fbsd_remove_vfork_catchpoint;
#endif
#ifdef PL_FLAG_EXEC
  t->to_insert_exec_catchpoint = fbsd_insert_exec_catchpoint;
  t->to_remove_exec_catchpoint = fbsd_remove_exec_catchpoint;
#endif
#endif
  add_target (t);
}
