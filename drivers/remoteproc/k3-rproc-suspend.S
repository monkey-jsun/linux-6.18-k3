/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <linux/cfi_types.h>
#include <asm/asm.h>
#include <asm/asm-offsets.h>
#include <asm/assembler.h>
#include <asm/csr.h>
#include <asm/xip_fixup.h>

#ifdef CONFIG_HIBERNATION

/*
 * VA of the suspend_context written by rproc_system_suspend() before the
 * SBI ecall.  spacemit_cpu_resume_enter() reads this instead of a1 so the
 * resume path does not depend on OpenSBI replaying a stale snapshot value.
 * Must be in .data (not .bss) so it is included in the hibernation image and
 * restored by restore_image() before syscore_resume rewrites it.
 */
	.section ".data"
	.global spacemit_suspend_ctx_va
	.align	3
spacemit_suspend_ctx_va:
	.dword	0
	.previous

/*
 * int spacemit_cpu_resume_enter(unsigned long hartid, unsigned long context)
 *
 * Drop-in replacement for __cpu_resume_enter that loads the suspend_context
 * pointer from spacemit_suspend_ctx_va (set by rproc_system_suspend before
 * the SBI ecall) instead of from a1.  This makes resume independent of the
 * stale next_arg1 that OpenSBI replays from its snapshot.
 */
SYM_TYPED_FUNC_START(spacemit_cpu_resume_enter)
	load_global_pointer

#ifdef CONFIG_MMU
	add	t0, a0, zero
	add	t1, a1, zero

	la	a0, swapper_pg_dir
	XIP_FIXUP_OFFSET a0
	call	relocate_enable_mmu

	add	a0, t0, zero
	add	a1, t1, zero
#endif
	/* Load context pointer from global — ignore a1 from OpenSBI */
	la	a0, spacemit_suspend_ctx_va
	REG_L	a0, 0(a0)

	suspend_restore_csrs
	suspend_restore_regs

	add	a0, zero, zero
	ret
SYM_FUNC_END(spacemit_cpu_resume_enter)

#endif /* CONFIG_HIBERNATION */
