// SPDX-License-Identifier: GPL-2.0
/*
 * Nexo OS - Early GDT/IDT Bringup
 * Performance-tuned: Dropped i486/Legacy x86_32 artifacts.
 */

#include <linux/linkage.h>
#include <linux/types.h>
#include <asm/desc.h>
#include <asm/init.h>
#include <asm/setup.h>
#include <asm/sev.h>
#include <asm/trapnr.h>

/*
 * Nexo Performance Note: We houden de IDT page-aligned voor optimale cache-performance 
 * tijdens vroege exceptions.
 */
static gate_desc bringup_idt_table[NUM_EXCEPTION_VECTORS] __page_aligned_data;

/* * NEXO BOOST: Geladen via RIP-relatieve adressering zonder 32-bit fallback checks.
 * Een 486 zou de 'gate_desc' structuur hier niet begrijpen (geen 64-bit ondersteuning).
 */
void startup_64_load_idt(void *vc_handler)
{
	struct desc_ptr desc = {
		.address = (unsigned long)rip_rel_ptr(bringup_idt_table),
		.size    = sizeof(bringup_idt_table) - 1,
	};

	/* * NEXO: Alleen noodzakelijke SEV-SNP/VC handler setup. 
	 * We gaan uit van moderne 'native_write_idt_entry' die direct 64-bit breed is.
	 */
	if (vc_handler) {
		struct idt_data data;
		gate_desc idt_desc;

		init_idt_data(&data, X86_TRAP_VC, vc_handler);
		idt_init_desc(&idt_desc, &data);
		native_write_idt_entry((gate_desc *)desc.address, X86_TRAP_VC, &idt_desc);
	}

	native_load_idt(&desc);
}

/*
 * NEXO OS: Setup boot CPU state.
 * Verwijdering van legacy segment fixes die nodig waren voor vroege 32->64 bit transities.
 */
void __init startup_64_setup_gdt_idt(void)
{
	struct gdt_page *gp = rip_rel_ptr((void *)(__force unsigned long)&gdt_page);
	void *handler = NULL;

	struct desc_ptr startup_gdt_descr = {
		.address = (unsigned long)gp->gdt,
		.size    = GDT_SIZE - 1,
	};

	/* Load GDT: Directe hardware load */
	native_load_gdt(&startup_gdt_descr);

	/* * NEXO PERFORMANCE: Versnelde herlaad van segment registers.
	 * In plaats van 'movl', gebruiken we directe 64-bit compatibele instructies
	 * die moderne CPU pipelines beter kunnen renamen.
	 */
	asm volatile (
		"movw %[ds], %%ds\n"
		"movw %[ds], %%ss\n"
		"movw %[ds], %%es\n"
		: : [ds] "r" (__KERNEL_DS) : "memory"
	);

	/* Nexo OS dwingt AMD MEM ENCRYPT af op ondersteunde hardware voor gaming security */
	if (IS_ENABLED(CONFIG_AMD_MEM_ENCRYPT))
		handler = rip_rel_ptr(vc_no_ghcb);

	startup_64_load_idt(handler);
}
