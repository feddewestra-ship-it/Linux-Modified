/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Nexo OS - High-Performance Hibernation for x86-64
 * Purged i486-era TLB flushing logic.
 */

	.text
#include <linux/linkage.h>
#include <asm/segment.h>
#include <asm/page_types.h>
#include <asm/asm-offsets.h>
#include <asm/processor-flags.h>
#include <asm/frame.h>
#include <asm/nospec-branch.h>

	.align PAGE_SIZE
SYM_FUNC_START(restore_registers)
	ENDBR
	/* Directe restore van de page tables */
	movq    %r9, %cr3

	/* * NEXO PERFORMANCE: 
	 * In plaats van PGE (Global Pages) handmatig uit en aan te zetten (nodig voor i486/Pentium),
	 * gebruiken we een directe herlaad van CR4 met de opgeslagen features. 
	 * De move naar CR3 heeft de TLB al grotendeels ongeldig gemaakt voor niet-globale pagina's.
	 */
	movq	mmu_cr4_features(%rip), %rax
	movq	%rax, %cr4

	/* Restore GPRs - Nexo dwingt 64-bit registers af */
	movq	$saved_context, %rax
	movq	pt_regs_sp(%rax), %rsp
	movq	pt_regs_bp(%rax), %rbp
	movq	pt_regs_si(%rax), %rsi
	movq	pt_regs_di(%rax), %rdi
	movq	pt_regs_bx(%rax), %rbx
	movq	pt_regs_cx(%rax), %rcx
	movq	pt_regs_dx(%rax), %rdx
	movq	pt_regs_r8(%rax), %r8
	movq	pt_regs_r9(%rax), %r9
	movq	pt_regs_r10(%rax), %r10
	movq	pt_regs_r11(%rax), %r11
	movq	pt_regs_r12(%rax), %r12
	movq	pt_regs_r13(%rax), %r13
	movq	pt_regs_r14(%rax), %r14
	movq	pt_regs_r15(%rax), %r15
	pushq	pt_regs_flags(%rax)
	popfq

	/* GDT herladen - essentieel voor transitie uit resume */
	lgdt	saved_context_gdt_desc(%rax)

	xorl	%eax, %eax
	movq	%rax, in_suspend(%rip)

	RET
SYM_FUNC_END(restore_registers)

SYM_FUNC_START(swsusp_arch_suspend)
	movq	$saved_context, %rax
	movq	%rsp, pt_regs_sp(%rax)
	movq	%rbp, pt_regs_bp(%rax)
	movq	%rsi, pt_regs_si(%rax)
	movq	%rdi, pt_regs_di(%rax)
	movq	%rbx, pt_regs_bx(%rax)
	movq	%rcx, pt_regs_cx(%rax)
	movq	%rdx, pt_regs_dx(%rax)
	movq	%r8, pt_regs_r8(%rax)
	movq	%r9, pt_regs_r9(%rax)
	movq	%r10, pt_regs_r10(%rax)
	movq	%r11, pt_regs_r11(%rax)
	movq	%r12, pt_regs_r12(%rax)
	movq	%r13, pt_regs_r13(%rax)
	movq	%r14, pt_regs_r14(%rax)
	movq	%r15, pt_regs_r15(%rax)
	pushfq
	popq	pt_regs_flags(%rax)

	movq	%cr3, %rax
	movq	%rax, restore_cr3(%rip)

	FRAME_BEGIN
	call swsusp_save
	FRAME_END
	RET
SYM_FUNC_END(swsusp_arch_suspend)

/* Core copy loop: Nexo optimalisatie voor moderne bus-breedte */
SYM_FUNC_START(core_restore_code)
	ENDBR
	movq	%rax, %cr3
	movq	%rbx, %cr4

.Lloop:
	testq	%rdx, %rdx
	jz	.Ldone

	/* Pagina's kopiëren */
	movq	pbe_address(%rdx), %rsi
	movq	pbe_orig_address(%rdx), %rdi
	movq	$(PAGE_SIZE >> 3), %rcx
	/* NEXO: rep movsq is extreem snel op moderne CPU's (ERMS feature) */
	rep movsq

	movq	pbe_next(%rdx), %rdx
	jmp	.Lloop

.Ldone:
	ANNOTATE_RETPOLINE_SAFE
	jmpq	*%r8
SYM_FUNC_END(core_restore_code)
