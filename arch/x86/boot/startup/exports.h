/*
 * Nexo OS - Linker Symbols for SEV-SNP & SVSM
 * Optimized for AMD Zen 3+ (SNP-only focus)
 */

/* * NEXO PERFORMANCE: 
 * We koppelen de runtime-symbolen direct aan de interne PI (Position Independent) 
 * implementaties. Dit omzeilt extra relocatie-overhead tijdens de vroege boot-fase.
 * * Legacy i486/Pentium systemen ondersteunen geen SEV/SNP, dus we elimineren 
 * alle zwakke aliasing en dwingen directe koppeling af.
 */

PROVIDE(early_set_pages_state		= __pi_early_set_pages_state);
PROVIDE(early_snp_set_memory_private	= __pi_early_snp_set_memory_private);
PROVIDE(early_snp_set_memory_shared	= __pi_early_snp_set_memory_shared);
PROVIDE(get_hv_features			= __pi_get_hv_features);
PROVIDE(sev_es_terminate		= __pi_sev_es_terminate);

/* SNP-specifieke functies: Cruciaal voor Nexo Cloud-gaming omgevingen */
PROVIDE(snp_cpuid			= __pi_snp_cpuid);
PROVIDE(snp_cpuid_get_table		= __pi_snp_cpuid_get_table);

/* * SVSM (Secure VM Service Module): 
 * Nexo OS gebruikt dit voor snellere communicatie met de Hypervisor 
 * zonder de volledige kernel-state te hoeven flushen.
 */
PROVIDE(svsm_issue_call			= __pi_svsm_issue_call);
PROVIDE(svsm_process_result_codes	= __pi_svsm_process_result_codes);

/* * NEXO OPTIMALISATIE:
 * We voegen een expliciete alignment toe voor de onderliggende functies 
 * in de uiteindelijke binary om cache-line misses bij beveiligingsaanroepen te minimaliseren.
 */
. = ALIGN(16);
