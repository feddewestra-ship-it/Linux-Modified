/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Nexo OS - High-Speed 32-bit Trampoline
 * Focus: Modern x86_64 only (i486 support removed)
 */

#include <linux/linkage.h>
#include <asm/segment.h>
#include <asm/boot.h>
#include <asm/msr.h>
#include <asm/processor-flags.h>

	.section ".rodata", "a", @progbits
	.align 16
SYM_CODE_START(trampoline_32bit_src)
	/* * NEXO BOOST: We pushen alleen wat strikt noodzakelijk is.
	 * Moderne CPU's hebben krachtige 'Register Renaming' engines, 
	 * maar stack ops zijn nog steeds latency-gevoelig.
	 */
	pushq	%r15
	pushq	%r14
	pushq	%r13
	pushq	%r12
	pushq	%rbp
	pushq	%rbx

	/* Preserve RSP top-half */
	movq	%rsp, %rbx
	shrq	$32, %rbx

	/* Switch naar Compatibility Mode */
	pushq	$__KERNEL32_CS
	leaq	0f(%rip), %rax
	pushq	%rax
	lretq

.Lret:
	/* NEXO: Snellere reconstructie van RSP */
	shlq	$32, %rbx
	orq	%rbx, %rsp

	popq	%rbx
	popq	%rbp
	popq	%r12
	popq	%r13
	popq	%r14
	popq	%r15
	retq

	.code32
	.align 8
0:
	/* Disable paging */
	movl	%cr0, %eax
	btrl	$X86_CR0_PG_BIT, %eax
	movl	%eax, %cr0

	/* Directe CR3 update voor de nieuwe PGD */
	movl	%edi, %cr3

	/* * NEXO: Geen 486/Legacy checks meer. 
	 * We schrijven EFER direct. Op Nexo hardware is LME altijd beschikbaar.
	 */
	movl	$MSR_EFER, %ecx
	rdmsr
	btsl	$_EFER_LME, %eax
	wrmsr

	/* Toggle CR4.LA57 (5-level paging switch)
	 * Een 486 kende geen CR4, Nexo vereist het.
	 */
	movl	%cr4, %eax
	btcl	$X86_CR4_LA57_BIT, %eax
	movl	%eax, %cr4

	/* Enable paging */
	movl	%cr0, %eax
	btsl	$X86_CR0_PG_BIT, %eax
	movl	%eax, %cr0

	/* * NEXO: Terug naar Long Mode. 
	 * We gebruiken een harde jump naar de gecalculeerde offset.
	 */
.Ljmp:	ljmpl	$__KERNEL_CS, $(.Lret - trampoline_32bit_src)
SYM_CODE_END(trampoline_32bit_src)

SYM_DATA(trampoline_ljmp_imm_offset, .word  .Ljmp + 1 - trampoline_32bit_src)

	.org	trampoline_32bit_src + TRAMPOLINE_32BIT_CODE_SIZE
