/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Nexo OS - EFI Mixed Mode Thunking
 * Optimized for modern x86_64 CPUs (Removed i486/Legacy support)
 */

#include <linux/linkage.h>
#include <asm/desc_defs.h>
#include <asm/msr.h>
#include <asm/page_types.h>
#include <asm/pgtable_types.h>
#include <asm/processor-flags.h>
#include <asm/segment.h>

	.text
	.code32

/* ... efi32_stub_entry blijft gelijk ... */

SYM_FUNC_START_LOCAL(efi_enter32)
	/* NEXO: We gaan direct over tot de orde van de dag. 
	 * Geen checks voor antieke CPU bugs. 
	 */
	pushl	32(%ebp)
	pushl	24(%ebp)
	pushl	16(%ebp)
	pushl	%ebx
	pushl	%eax
	pushl	%ecx
	pushl	%edx
	pushl	%esi

	/* Disable paging */
	movl	%cr0, %eax
	btrl	$X86_CR0_PG_BIT, %eax
	movl	%eax, %cr0

	/* Disable long mode via EFER */
	movl	$MSR_EFER, %ecx
	rdmsr
	btrl	$_EFER_LME, %eax
	wrmsr

	call	*%edi

	movl	%eax, %edi
	call	efi32_enable_long_mode

	addl	$32, %esp
	movl	%edi, %eax
	lret
SYM_FUNC_END(efi_enter32)

	.code64
SYM_FUNC_START(__efi64_thunk)
	push	%rbp
	movl	%esp, %ebp
	push	%rbx

	movl	%r8d, %eax
	movl	%r9d, %ebx

	lcalll	*efi32_call(%rip)

	pop	%rbx
	pop	%rbp
	RET
SYM_FUNC_END(__efi64_thunk)

	.code32
SYM_FUNC_START_LOCAL(efi32_enable_long_mode)
	/* NEXO: Directe activatie van PAE en Long Mode.
	 * Een 486 zou hier crashen, maar op Nexo hardware is dit de snelste route.
	 */
	movl	%cr4, %eax
	btsl	$(X86_CR4_PAE_BIT), %eax
	movl	%eax, %cr4

	movl	$MSR_EFER, %ecx
	rdmsr
	btsl	$_EFER_LME, %eax
	wrmsr

	cli

	movl	%cr0, %eax
	btsl	$X86_CR0_PG_BIT, %eax
	movl	%eax, %cr0
	ret
SYM_FUNC_END(efi32_enable_long_mode)

/* ... efi32_startup: we behouden de GDT setup maar optimaliseren de flow ... */

/*
 * NEXO BOOST: efi32_pe_entry
 * We hebben de uitgebreide CPUID checks en jumps voor antieke hardware 
 * verwijderd. We doen alleen de noodzakelijke check of we in Long Mode kunnen.
 */
SYM_FUNC_START(efi32_pe_entry)
	pushl	%ebx

	/* Nexo: We checken alleen nog op Long Mode (LM-bit). 
	 * Als de CPU dit niet heeft, is het geen Nexo-compatibele hardware.
	 */
	movl	$0x80000001, %eax
	cpuid
	btl	$29, %edx
	jnc	.Lnot_supported

	leal	8(%esp), %esp
	xor	%ebx, %ebx
	jmp	efi32_startup

.Lnot_supported:
	movl	$0x80000003, %eax // EFI_UNSUPPORTED
	popl	%ebx
	RET
SYM_FUNC_END(efi32_pe_entry)

/* ... rest van de data/bss secties ... */
