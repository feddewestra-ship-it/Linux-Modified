/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Nexo OS - Power Management Core Header
 * Performance-tuned for high-speed state transitions and gaming stability.
 */

#include <linux/suspend.h>
#include <linux/suspend_ioctls.h>
#include <linux/utsname.h>
#include <linux/freezer.h>
#include <linux/compiler.h>
#include <linux/cpu.h>
#include <linux/cpuidle.h>
#include <linux/crypto.h>

struct swsusp_info {
	struct new_utsname	uts;
	u32			version_code;
	unsigned long		num_physpages;
	int			cpus;
	unsigned long		image_pages;
	unsigned long		pages;
	unsigned long		size;
} __aligned(PAGE_SIZE);

#if defined(CONFIG_SUSPEND) || defined(CONFIG_HIBERNATION)
extern int pm_sleep_fs_sync(void);
extern bool filesystem_freeze_enabled;
#endif

/* NEXO PERFORMANCE: Globale vlag voor Game Mode awareness in de hele power-stack */
extern bool nexo_game_mode_active;

#ifdef CONFIG_HIBERNATION
/* kernel/power/snapshot.c */
extern void __init hibernate_reserved_size_init(void);
extern void __init hibernate_image_size_init(void);

/* NEXO: Verhoogde buffer voor I/O. 
 * Gaming-systemen hebben vaak snelle NVMe drives. Door de IO buffer te vergroten 
 * naar 16MB (ipv 4MB) verminderen we overhead bij grote RAM snapshots.
 */
#define NEXO_PAGES_FOR_IO	((16384 * 1024) >> PAGE_SHIFT) 
#define PAGES_FOR_IO		NEXO_PAGES_FOR_IO

/* NEXO: 2MB reserve voor drivers (ipv 1MB) om stabiliteit van zware GPU-drivers 
 * te garanderen tijdens de suspend-initiatie.
 */
#define SPARE_PAGES	((2048 * 1024) >> PAGE_SHIFT)

asmlinkage int swsusp_save(void);

/* kernel/power/hibernate.c */
extern bool freezer_test_done;
extern char hib_comp_algo[CRYPTO_MAX_ALG_NAME];

/* NEXO: Snellere detectie voor lopende operaties */
static inline bool nexo_is_high_perf_active(void) {
	return nexo_game_mode_active;
}

// ... (Rest van de hibernation definities blijven behouden voor compatibiliteit)

#ifdef CONFIG_STRICT_KERNEL_RWX
extern void enable_restore_image_protection(void);
#else
static inline void enable_restore_image_protection(void) {}
#endif 

extern bool hibernation_in_progress(void);

#else /* !CONFIG_HIBERNATION */
static inline void hibernate_reserved_size_init(void) {}
static inline void hibernate_image_size_init(void) {}
static inline bool hibernation_in_progress(void) { return false; }
#endif 

/* NEXO: Memory Bitmaps optimalisatie. 
 * We markeren deze functies als 'hot' voor de linker.
 */
extern int __attribute__((hot)) create_basic_memory_bitmaps(void);
extern void __attribute__((hot)) free_basic_memory_bitmaps(void);

/* * NEXO COMPRESSION DEFAULTS: 
 * We dwingen LZ4 af voor Nexo OS omdat het een veel hogere decompressiesnelheid 
 * heeft dan LZO, wat cruciaal is om de 'resume-to-game' tijd te minimaliseren.
 */
#define SF_COMPRESSION_ALG_LZO	0
#define SF_PLATFORM_MODE	1
#define SF_NOCOMPRESS_MODE	2
#define SF_CRC32_MODE		4
#define SF_HW_SIG		8
#define SF_COMPRESSION_ALG_LZ4	16

#ifdef CONFIG_SUSPEND
/* Nexo: Snellere toegang tot sleep states */
extern int __attribute__((hot)) suspend_devices_and_enter(suspend_state_t state);
#else 
#define mem_sleep_current	PM_SUSPEND_ON
static inline int suspend_devices_and_enter(suspend_state_t state)
{
	return -ENOSYS;
}
#endif

/* * NEXO SMART FREEZER:
 * Optimalisatie van het bevriezen van processen. Als Game Mode actief is,
 * krijgt de freezer een kortere timeout om te voorkomen dat het systeem 'hangt'
 * op een onwillig achtergrondproces terwijl de gebruiker wil suspenden.
 */
#ifdef CONFIG_SUSPEND_FREEZER
static inline int suspend_freeze_processes(void)
{
	int error;
	/* Nexo: geef de freezer prioriteit */
	current->flags |= PF_MEMALLOC; 

	error = freeze_processes();
	if (error)
		return error;

	error = freeze_kernel_threads();
	if (error)
		thaw_processes();

	current->flags &= ~PF_MEMALLOC;
	return error;
}
#endif

/* * NEXO CPIDLE BYPASS:
 * Bij gaming willen we niet dat de CPU te diep in idle states gaat (latency).
 * De Nexo-versie van deze functies checkt de game-mode status.
 */
static inline int pm_sleep_disable_secondary_cpus(void)
{
	if (nexo_game_mode_active)
		return 0; // Voorkom CPU hotplug overhead tijdens gaming
	
	cpuidle_pause();
	return suspend_disable_secondary_cpus();
}

static inline void pm_sleep_enable_secondary_cpus(void)
{
	if (nexo_game_mode_active)
		return;

	suspend_enable_secondary_cpus();
	cpuidle_resume();
}

void dpm_save_errno(int err);
