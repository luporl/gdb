/* Native-dependent code for BSD Unix running on ARM's, for GDB.

   Copyright (C) 1988-2015 Free Software Foundation, Inc.

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
#include "target.h"
#include "gregset.h"
#include <sys/types.h>
#include <sys/ptrace.h>
#include <machine/reg.h>
#include <machine/frame.h>

#include "fbsd-nat.h"
#include "arm-tdep.h"
#include "inf-ptrace.h"

extern int arm_apcs_32;

static pid_t
ptrace_pid (ptid_t ptid)
{
  pid_t pid;

#ifdef __FreeBSD__
  pid = ptid_get_lwp (ptid);
  if (pid == 0)
#endif
    pid = ptid_get_pid (ptid);
  return pid;
}

static void
arm_supply_gregset (struct regcache *regcache, const gregset_t *gregset, int regnum)
{
  int r;
  CORE_ADDR r_pc;

  /* Integer registers.  */
  for (r = ARM_A1_REGNUM; r < ARM_SP_REGNUM; r++)
    if ((r == regnum) || (regnum == -1))
      regcache_raw_supply (regcache, r, (char *) &gregset->r[r]);

  if ((regnum == ARM_SP_REGNUM) || (regnum == -1))
    regcache_raw_supply (regcache, ARM_SP_REGNUM,
		       (char *) &gregset->r_sp);
  if ((regnum == ARM_LR_REGNUM) || (regnum == -1))
    regcache_raw_supply (regcache, ARM_LR_REGNUM,
		       (char *) &gregset->r_lr);
  /* This is ok: we're running native...  */
  if ((regnum == ARM_PC_REGNUM) || (regnum == -1))
    {
      r_pc = gdbarch_addr_bits_remove (get_regcache_arch (regcache), gregset->r_pc);
      regcache_raw_supply (regcache, ARM_PC_REGNUM, (char *) &r_pc);
    }

  if ((regnum == ARM_PS_REGNUM) || (regnum == -1))
    {
      if (arm_apcs_32)
        regcache_raw_supply (regcache, ARM_PS_REGNUM,
			 (char *) &gregset->r_cpsr);
      else
        regcache_raw_supply (regcache, ARM_PS_REGNUM,
			 (char *) &gregset->r_pc);
    }
}

static void
armbsd_collect_gregset (const struct regcache *regcache, gregset_t *gregset, int regnum)
{
  int ret;
  int r;

  for (r = ARM_A1_REGNUM; r < ARM_SP_REGNUM; r++)
    if ((regnum == r) || (regnum == -1))
      regcache_raw_collect (regcache, r,
			  (char *) &gregset->r[r]);

  if ((regnum == ARM_SP_REGNUM) || (regnum == -1))
    regcache_raw_collect (regcache, ARM_SP_REGNUM,
			(char *) &gregset->r_sp);
  if ((regnum == ARM_LR_REGNUM) || (regnum == -1))
    regcache_raw_collect (regcache, ARM_LR_REGNUM,
			(char *) &gregset->r_lr);


  if ((regnum == ARM_PC_REGNUM) || (regnum == -1))
    regcache_raw_collect (regcache, ARM_PC_REGNUM,
			(char *) &gregset->r_pc);
  if ((regnum == ARM_PS_REGNUM) || (regnum == -1))
    {
      if (arm_apcs_32)
        {
          regcache_raw_collect (regcache, ARM_PS_REGNUM,
			    (char *) &gregset->r_cpsr);
        }
      else
        {
          unsigned psr_val;

          regcache_raw_collect (regcache, ARM_PS_REGNUM,
			   (char *) &psr_val);

          psr_val ^= gdbarch_addr_bits_remove (get_regcache_arch (regcache), psr_val);
          gregset->r_pc = gdbarch_addr_bits_remove
    			   (get_regcache_arch (regcache), gregset->r_pc);
          gregset->r_pc |= psr_val;
        }
    }
}

/* Fill GDB's register array with the general-purpose register values
   in *GREGSETP.  */

void
supply_gregset (struct regcache *regcache, const gregset_t *gregsetp)
{
  arm_supply_gregset (regcache, gregsetp, -1);
}

/* Fill register REGNUM (if it is a general-purpose register) in
   *GREGSETPS with the value in GDB's register array.  If REGNUM is -1,
   do this for all registers.  */

void
fill_gregset (const struct regcache *regcache, gdb_gregset_t *gregsetp, int regnum)
{
  armbsd_collect_gregset (regcache, gregsetp, regnum);
}

/* Fill GDB's register array with the floating-point register values
   in *FPREGSETP.  */

void
supply_fpregset (struct regcache *regcache, const fpregset_t *fpregsetp)
{
}

/* Fill register REGNUM (if it is a floating-point register) in
   *FPREGSETP with the value in GDB's register array.  If REGNUM is -1,
   do this for all registers.  */

void
fill_fpregset (const struct regcache *regcache, gdb_fpregset_t *fpregsetp, int regnum)
{
}

/* Fetch register REGNO from the child process. If REGNO is -1, do it
   for all registers.  */

static void
armfbsd_fetch_inferior_registers (struct target_ops *ops,
				  struct regcache *regcache, int regno)
{
  gdb_gregset_t regs;

  if (ptrace (PT_GETREGS, ptrace_pid (inferior_ptid),
	      (PTRACE_TYPE_ARG3) &regs, 0) == -1)
    perror_with_name (_("Couldn't get registers"));

  arm_supply_gregset (regcache, &regs, regno);
  /* TODO: fpregs */
}

/* Store register REGNO back into the child process. If REGNO is -1,
   do this for all registers.  */

static void
armfbsd_store_inferior_registers (struct target_ops *ops,
				  struct regcache *regcache, int regno)
{
  gdb_gregset_t regs;

  if (ptrace (PT_GETREGS, ptrace_pid (inferior_ptid),
	      (PTRACE_TYPE_ARG3) &regs, 0) == -1)
    perror_with_name (_("Couldn't get registers"));

  fill_gregset (regcache, &regs, regno);

  if (ptrace (PT_SETREGS, ptrace_pid (inferior_ptid),
	      (PTRACE_TYPE_ARG3) &regs, 0) == -1)
    perror_with_name (_("Couldn't write registers"));
  /* TODO: FP regs */
}

void _initialize_armfbsd_nat (void);

void
_initialize_armfbsd_nat (void)
{
  struct target_ops *t;

  /* Add in local overrides.  */
  t = inf_ptrace_target ();
  t->to_fetch_registers = armfbsd_fetch_inferior_registers;
  t->to_store_registers = armfbsd_store_inferior_registers;
  fbsd_nat_add_target (t);
}
