// SPDX-License-Identifier: GPL-2.0-only
/* * Nexo OS - CPU Feature Validation
 * Optimized for modern x86_64 gaming hardware.
 * Removed support for i486, Transmeta, and legacy 32-bit specific workarounds.
 */

#ifdef _SETUP
# include "boot.h"
#endif
#include <linux/types.h>
#include <asm/cpufeaturemasks.h>
#include <asm/intel-family.h>
#include <asm/processor-flags.h>
#include <asm/msr-index.h>
#include <asm/shared/msr.h>

#include "string.h"

static u32 err_flags[NCAPINTS];
static const int req_level = 64; // NEXO: We accepteren alleen 64-bit (Long Mode)

static const u32 req_flags[NCAPINTS] =
{
	REQUIRED_MASK0,
	REQUIRED_MASK1,
	0, 0,
	REQUIRED_MASK4,
	0,
	REQUIRED_MASK6,
	0, 0, 0, 0, 0, 0, 0, 0, 0,
	REQUIRED_MASK16,
};

#define A32(a, b, c, d) (((d) << 24)+((c) << 16)+((b) << 8)+(a))

static int is_amd(void)
{
	return cpu_vendor[0] == A32('A', 'u', 't', 'h') &&
	       cpu_vendor[1] == A32('e', 'n', 't', 'i') &&
	       cpu_vendor[2] == A32('c', 'A', 'M', 'D');
}

static int is_intel(void)
{
	return cpu_vendor[0] == A32('G', 'e', 'n', 'u') &&
	       cpu_vendor[1] == A32('i', 'n', 'e', 'I') &&
	       cpu_vendor[2] == A32('n', 't', 'e', 'l');
}

static int check_cpuflags(void)
{
	u32 err = 0;
	int i;

	for (i = 0; i < NCAPINTS; i++) {
		err_flags[i] = req_flags[i] & ~cpu.flags[i];
		if (err_flags[i])
			err |= 1 << i;
	}
	return err;
}

/*
 * NEXO OS: Simplified CPU Check
 * We gaan direct naar level 64. Geen checks voor AC-flags (486) of 386-beperkingen.
 */
int check_cpu(int *cpu_level_ptr, int *req_level_ptr, u32 **err_flags_ptr)
{
	int err;

	memset(&cpu.flags, 0, sizeof(cpu.flags));
	
	/* NEXO: We slaan level 3 en 4 checks over. 
	 * Als CPUID niet werkt of Long Mode ontbreekt, stopt Nexo onmiddellijk. 
	 */
	get_cpuflags();
	err = check_cpuflags();

	if (test_bit(X86_FEATURE_LM, cpu.flags))
		cpu.level = 64;
	else
		cpu.level = 6; // Pentium Pro of hoger, maar geen 64-bit = FAIL

	/* NEXO: Verwijdering van alle legacy hacks (Transmeta, Centaur, Pentium M)
	 * Alleen moderne AMD/Intel features worden gevalideerd.
	 */
	if (err) {
		/* Eventuele AMD-specifieke activatie van SSE kan hier blijven 
		 * mits het moderne Zen-architecturen niet schaadt. */
		if (is_amd() && (err_flags[0] & (1 << X86_FEATURE_XMM))) {
			struct msr m;
			raw_rdmsr(MSR_K7_HWCR, &m);
			m.l &= ~(1 << 15);
			raw_wrmsr(MSR_K7_HWCR, &m);
			get_cpuflags();
			err = check_cpuflags();
		}
	}

	if (err_flags_ptr)
		*err_flags_ptr = err ? err_flags : NULL;
	if (cpu_level_ptr)
		*cpu_level_ptr = cpu.level;
	if (req_level_ptr)
		*req_level_ptr = req_level;

	return (cpu.level < req_level || err) ? -1 : 0;
}

/* * NEXO: We behouden Xeon Phi checks omdat dit 'recente' high-end hardware is,
 * maar we vereisen nu altijd 64-bit voor deze modellen.
 */
int check_knl_erratum(void)
{
	if (!is_intel() || cpu.family != 6 || cpu.model != 0x57)
		return 0;

	if (IS_ENABLED(CONFIG_X86_64))
		return 0;

	puts("Nexo OS vereist een 64-bit kernel voor deze processor.\n");
	return -1;
}
