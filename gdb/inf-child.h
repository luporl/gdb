/* Base/prototype target for default child (native) targets.

   Copyright (C) 2004-2018 Free Software Foundation, Inc.

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

#ifndef INF_CHILD_H
#define INF_CHILD_H

#include "target.h"

/* A prototype child target.  The client can override it with local
   methods.  */

class inf_child_target
  : public memory_breakpoint_target<target_ops>
{
public:
  inf_child_target ();
  ~inf_child_target () override = 0;

  const char *shortname () override
  { return "native"; }

  const char *longname () override
  { return _("Native process"); }

  const char *doc () override
  { return _("Native process (started by the \"run\" command)."); }

  void open (const char *arg, int from_tty) override;
  void close () override;

  void disconnect (const char *, int) override;

  void fetch_registers (struct regcache *, int) override = 0;
  void store_registers (struct regcache *, int) override = 0;

  void prepare_to_store (struct regcache *) override;

  bool supports_terminal_ours () override;
  void terminal_init () override;
  void terminal_inferior () override;
  void terminal_ours_for_output () override;
  void terminal_ours () override;
  void terminal_info (const char *, int) override;

  void interrupt () override;
  void pass_ctrlc () override;

  void post_startup_inferior (ptid_t) override;

  void mourn_inferior () override;

  int can_run () override;

  bool can_create_inferior () override;
  void create_inferior (const char *, const std::string &,
			char **, int) = 0;

  bool can_attach () override;
  void attach (const char *, int) override = 0;

  void post_attach (int);

  /* We must default these because they must be implemented by any
     target that can run.  */
  int can_async_p ()  override { return 0; }
  int supports_non_stop ()  override { return 0; }

  char *pid_to_exec_file (int pid) override;

  int has_all_memory () override;
  int has_memory () override;
  int has_stack () override;
  int has_registers () override;
  int has_execution (ptid_t) override;

  int fileio_open (struct inferior *inf, const char *filename,
		   int flags, int mode, int warn_if_slow,
		   int *target_errno) override;
  int fileio_pwrite (int fd, const gdb_byte *write_buf, int len,
		     ULONGEST offset, int *target_errno) override;
  int fileio_pread (int fd, gdb_byte *read_buf, int len,
		    ULONGEST offset, int *target_errno) override;
  int fileio_fstat (int fd, struct stat *sb, int *target_errno) override;
  int fileio_close (int fd, int *target_errno) override;
  int fileio_unlink (struct inferior *inf,
		     const char *filename,
		     int *target_errno) override;
  gdb::optional<std::string> fileio_readlink (struct inferior *inf,
					      const char *filename,
					      int *target_errno) override;
  int use_agent (int use) override;

  int can_use_agent () override;

protected:
  /* Unpush the target if it wasn't explicitly open with "target native"
     and there are no live inferiors left.  Note: if calling this as a
     result of a mourn or detach, the current inferior shall already
     have its PID cleared, so it isn't counted as live.  That's usually
     done by calling either generic_mourn_inferior or
     detach_inferior.  */
  void maybe_unpush_target ();
};

/* Functions for helping to write a native target.  */

/* This is for native targets which use a unix/POSIX-style waitstatus.  */
extern void store_waitstatus (struct target_waitstatus *, int);

#endif
