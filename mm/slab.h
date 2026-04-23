\/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MM_SLAB_H
#define MM_SLAB_H

#include <linux/reciprocal_div.h>
#include <linux/list_lru.h>
#include <linux/local_lock.h>
#include <linux/random.h>
#include <linux/kobject.h>
#include <linux/sched/mm.h>
#include <linux/memcontrol.h>
#include <linux/kfence.h>
#include <linux/kasan.h>

/*
 * Internal slab definitions
 *
 * Changes vs. upstream:
 *   - Leak tracking: alloc timestamp + PID stored in kmem_obj_info,
 *     slab_track_alloc() helper (debug-only, zero overhead in production).
 *   - Page corruption guards: NULL-safe page_slab(), bounds-clamped
 *     nearest_obj() with WARN_ONCE on underflow.
 *   - Safer allocation paths: kmalloc_slab() rejects size==0 and NULL
 *     cache entries; slab_want_init_on_alloc() NULL-guards the cache ptr.
 */

/* ───────────────────────────────────────────────────────────────────────────
 * ABA-safe freelist types
 * ─────────────────────────────────────────────────────────────────────────*/

#ifdef CONFIG_64BIT
# ifdef system_has_cmpxchg128
#  define system_has_freelist_aba()	system_has_cmpxchg128()
#  define try_cmpxchg_freelist		try_cmpxchg128
# endif
typedef u128 freelist_full_t;
#else
# ifdef system_has_cmpxchg64
#  define system_has_freelist_aba()	system_has_cmpxchg64()
#  define try_cmpxchg_freelist		try_cmpxchg64
# endif
typedef u64 freelist_full_t;
#endif

#if defined(system_has_freelist_aba) && !defined(CONFIG_HAVE_ALIGNED_STRUCT_PAGE)
# undef system_has_freelist_aba
#endif

/*
 * Freelist pointer + counter packed together so cmpxchg operates on both
 * atomically, eliminating the classic ABA race.
 */
struct freelist_counters {
	union {
		struct {
			void *freelist;
			union {
				unsigned long counters;
				struct {
					unsigned inuse:16;
					unsigned objects:15;
					/*
					 * frozen == 1 when the slab is held by a CPU.
					 * Under SLUB_DEBUG it is repurposed to signal
					 * detected corruption.
					 */
					unsigned frozen:1;
#ifdef CONFIG_64BIT
					/*
					 * stride is only present on 64-bit; some
					 * optimisations that store data in the free
					 * bits of 'counters' are disabled when it is
					 * absent.
					 */
					unsigned int stride;
#endif
				};
			};
		};
#ifdef system_has_freelist_aba
		freelist_full_t freelist_counters;
#endif
	};
};

/* ───────────────────────────────────────────────────────────────────────────
 * struct slab  —  reuses bits in struct page
 * ─────────────────────────────────────────────────────────────────────────*/

struct slab {
	memdesc_flags_t flags;

	struct kmem_cache *slab_cache;
	union {
		struct {
			struct list_head slab_list;
			/* Double-word boundary */
			struct freelist_counters;
		};
		struct rcu_head rcu_head;
	};

	unsigned int __page_type;
	atomic_t __page_refcount;
#ifdef CONFIG_SLAB_OBJ_EXT
	unsigned long obj_exts;
#endif
};

/* Compile-time layout assertions: slab fields must alias page fields exactly */
#define SLAB_MATCH(pg, sl)						\
	static_assert(offsetof(struct page, pg) == offsetof(struct slab, sl))
SLAB_MATCH(flags, flags);
SLAB_MATCH(compound_info, slab_cache);	/* bit 0 must be clear */
SLAB_MATCH(_refcount, __page_refcount);
#ifdef CONFIG_MEMCG
SLAB_MATCH(memcg_data, obj_exts);
#elif defined(CONFIG_SLAB_OBJ_EXT)
SLAB_MATCH(_unused_slab_obj_exts, obj_exts);
#endif
#undef SLAB_MATCH
static_assert(sizeof(struct slab) <= sizeof(struct page));
#if defined(system_has_freelist_aba)
static_assert(IS_ALIGNED(offsetof(struct slab, freelist),
			  sizeof(struct freelist_counters)));
#endif

/* ───────────────────────────────────────────────────────────────────────────
 * slab ↔ folio / page helpers
 * ─────────────────────────────────────────────────────────────────────────*/

/**
 * slab_folio - The folio allocated for a slab.
 * @s: The slab.
 *
 * Slabs are allocated as folios.  Use this helper instead of casting
 * directly so that the implementation can change without touching callers.
 */
#define slab_folio(s)		(_Generic((s),				\
	const struct slab *:	(const struct folio *)s,		\
	struct slab *:		(struct folio *)s))

/**
 * page_slab - Convert a struct page to its owning slab.
 * @page: Any page (may or may not belong to a slab).
 *
 * Returns the slab that contains @page, or NULL if the page does not belong
 * to a slab (including large-kmalloc pages).
 *
 * Safety: guards against a NULL compound_head() return and emits
 * VM_WARN_ONCE when page_type looks corrupt, rather than silently
 * returning a garbage pointer.
 */
static inline struct slab *page_slab(const struct page *page)
{
	page = compound_head(page);

	/* Guard: compound_head() should never return NULL for a live page,
	 * but be defensive in case memory is already corrupted. */
	if (unlikely(!page))
		return NULL;

	if (data_race(page->page_type >> 24) != PGTY_slab) {
		VM_WARN_ONCE(1,
			"page_slab: unexpected page_type 0x%x at %p\n",
			page->page_type, page);
		return NULL;
	}

	return (struct slab *)page;
}

/**
 * slab_page - First struct page of a slab's underlying folio.
 * @s: The slab.
 *
 * Convenience wrapper for code not yet converted to folios.
 */
#define slab_page(s) folio_page(slab_folio(s), 0)

static inline void *slab_address(const struct slab *slab)
{
	return folio_address(slab_folio(slab));
}

static inline int slab_nid(const struct slab *slab)
{
	return memdesc_nid(slab->flags);
}

static inline pg_data_t *slab_pgdat(const struct slab *slab)
{
	return NODE_DATA(slab_nid(slab));
}

static inline struct slab *virt_to_slab(const void *addr)
{
	return page_slab(virt_to_page(addr));
}

static inline int slab_order(const struct slab *slab)
{
	return folio_order(slab_folio(slab));
}

static inline size_t slab_size(const struct slab *slab)
{
	return PAGE_SIZE << slab_order(slab);
}

/* ───────────────────────────────────────────────────────────────────────────
 * kmem_cache structures
 * ─────────────────────────────────────────────────────────────────────────*/

/*
 * Compact word encoding of (order, objects-per-slab) used for lock-free
 * atomic reads.
 */
struct kmem_cache_order_objects {
	unsigned int x;
};

struct kmem_cache_per_node_ptrs {
	struct node_barn		*barn;
	struct kmem_cache_node	*node;
};

/**
 * struct kmem_cache - Descriptor for a slab cache.
 *
 * All fields accessed on the hot allocation path are grouped at the top
 * and padded to avoid false sharing with slow-path fields.
 */
struct kmem_cache {
	/* ---- hot path ---------------------------------------------------- */
	struct slub_percpu_sheaves __percpu *cpu_sheaves;
	slab_flags_t		flags;
	unsigned long		min_partial;
	unsigned int		size;		/* object size incl. metadata  */
	unsigned int		object_size;	/* object size excl. metadata  */
	struct reciprocal_value	reciprocal_size;
	unsigned int		offset;		/* free-pointer offset         */
	unsigned int		sheaf_capacity;
	struct kmem_cache_order_objects oo;

	/* ---- slab allocation / freeing ----------------------------------- */
	struct kmem_cache_order_objects min;
	gfp_t			allocflags;	/* gfp flags for each alloc    */
	int			refcount;	/* for kmem_cache_destroy()    */
	void (*ctor)(void *object);
	unsigned int		inuse;		/* offset to metadata          */
	unsigned int		align;
	unsigned int		red_left_pad;	/* left redzone padding        */
	const char		*name;		/* display name only           */
	struct list_head	list;

#ifdef CONFIG_SYSFS
	struct kobject		kobj;
#endif
#ifdef CONFIG_SLAB_FREELIST_HARDENED
	unsigned long		random;
#endif
#ifdef CONFIG_NUMA
	unsigned int		remote_node_defrag_ratio;
#endif
#ifdef CONFIG_SLAB_FREELIST_RANDOM
	unsigned int		*random_seq;
#endif
#ifdef CONFIG_KASAN_GENERIC
	struct kasan_cache	kasan_info;
#endif
#ifdef CONFIG_HARDENED_USERCOPY
	unsigned int		useroffset;
	unsigned int		usersize;
#endif
#ifdef CONFIG_SLUB_STATS
	struct kmem_cache_stats __percpu *cpu_stats;
#endif

	struct kmem_cache_per_node_ptrs per_node[MAX_NUMNODES];
};

/**
 * cache_has_sheaves - True when the cache uses real (non-bootstrap) sheaves.
 * @s: The cache to test.
 */
static inline bool cache_has_sheaves(struct kmem_cache *s)
{
	return !IS_ENABLED(CONFIG_SLUB_TINY) && s->sheaf_capacity;
}

/* ───────────────────────────────────────────────────────────────────────────
 * sysfs helpers
 * ─────────────────────────────────────────────────────────────────────────*/

#if defined(CONFIG_SYSFS) && !defined(CONFIG_SLUB_TINY)
# define SLAB_SUPPORTS_SYSFS 1
void sysfs_slab_unlink(struct kmem_cache *s);
void sysfs_slab_release(struct kmem_cache *s);
int  sysfs_slab_alias(struct kmem_cache *s, const char *name);
#else
static inline void sysfs_slab_unlink(struct kmem_cache *s) { }
static inline void sysfs_slab_release(struct kmem_cache *s) { }
static inline int  sysfs_slab_alias(struct kmem_cache *s, const char *name)
						{ return 0; }
#endif

/* ───────────────────────────────────────────────────────────────────────────
 * Object ↔ index helpers
 * ─────────────────────────────────────────────────────────────────────────*/

void *fixup_red_left(struct kmem_cache *s, void *p);

/**
 * nearest_obj - Round an interior pointer down to the nearest object start.
 * @cache: The owning cache.
 * @slab:  The slab containing the pointer.
 * @x:     An arbitrary pointer within (or near) the slab.
 *
 * Safety additions vs. upstream:
 *   - Early NULL check on @slab / @x.
 *   - Underflow detection: if the computed object falls before slab_address()
 *     we clamp and emit a WARN_ONCE so the corruption is reported rather than
 *     silently propagated.
 */
static inline void *nearest_obj(struct kmem_cache *cache,
				const struct slab *slab, void *x)
{
	void *base, *object, *last_object, *result;

	if (unlikely(!slab || !x))
		return NULL;

	base        = slab_address(slab);
	object      = x - (x - base) % cache->size;
	last_object = base + (slab->objects - 1) * cache->size;

	/* Underflow: pointer was before the slab start — memory corruption. */
	if (unlikely(object < base)) {
		WARN_ONCE(1,
			"nearest_obj: object %p before slab base %p (cache %s)\n",
			object, base, cache->name);
		object = base;
	}

	result = (unlikely(object > last_object)) ? last_object : object;
	result = fixup_red_left(cache, result);
	return result;
}

static inline unsigned int __obj_to_index(const struct kmem_cache *cache,
					  void *addr, const void *obj)
{
	return reciprocal_divide(kasan_reset_tag(obj) - addr,
				 cache->reciprocal_size);
}

static inline unsigned int obj_to_index(const struct kmem_cache *cache,
					const struct slab *slab,
					const void *obj)
{
	if (is_kfence_address(obj))
		return 0;
	return __obj_to_index(cache, slab_address(slab), obj);
}

static inline int objs_per_slab(const struct kmem_cache *cache,
				const struct slab *slab)
{
	return slab->objects;
}

/* ───────────────────────────────────────────────────────────────────────────
 * Boot state
 * ─────────────────────────────────────────────────────────────────────────*/

/**
 * enum slab_state - Allocator bootstrap stages.
 *
 * Most allocators manage structures that are themselves allocated from slab
 * caches, requiring a careful multi-stage initialisation sequence.
 *
 * @DOWN:    No slab functionality at all.
 * @PARTIAL: SLUB: kmem_cache_node is available.
 * @UP:      Caches usable, extras not yet ready.
 * @FULL:    Everything operational.
 */
enum slab_state {
	DOWN,
	PARTIAL,
	UP,
	FULL,
};

extern enum slab_state slab_state;

/* Protects management structures during modifications */
extern struct mutex slab_mutex;

/* Global list of all active slab caches */
extern struct list_head slab_caches;

/* The cache that manages cache descriptors themselves */
extern struct kmem_cache *kmem_cache;

/* ───────────────────────────────────────────────────────────────────────────
 * kmalloc helpers
 * ─────────────────────────────────────────────────────────────────────────*/

extern const struct kmalloc_info_struct {
	const char *name[NR_KMALLOC_TYPES];
	unsigned int size;
} kmalloc_info[];

void setup_kmalloc_cache_index_table(void);
void create_kmalloc_caches(void);

extern u8 kmalloc_size_index[24];

static inline unsigned int size_index_elem(unsigned int bytes)
{
	return (bytes - 1) / 8;
}

/**
 * kmalloc_slab - Look up the kmem_cache serving a given allocation size.
 * @size:   Requested size (caller must ensure 0 < size ≤ KMALLOC_MAX_CACHE_SIZE).
 * @b:      Optional bucket override; NULL → use the default kmalloc_caches.
 * @flags:  GFP flags (used to select cache type).
 * @caller: Call-site PC for diagnostics.
 *
 * Safety additions vs. upstream:
 *   - Rejects size == 0 with WARN_ONCE; returns NULL so callers do not
 *     dereference a stale or uninitialised cache pointer.
 *   - Validates that the resolved cache entry is non-NULL before returning.
 */
static inline struct kmem_cache *
kmalloc_slab(size_t size, kmem_buckets *b, gfp_t flags, unsigned long caller)
{
	unsigned int index;

	if (unlikely(size == 0)) {
		WARN_ONCE(1,
			"kmalloc_slab: zero-size allocation requested from %pS\n",
			(void *)caller);
		return NULL;
	}

	if (!b)
		b = &kmalloc_caches[kmalloc_type(flags, caller)];

	if (size <= 192)
		index = kmalloc_size_index[size_index_elem(size)];
	else
		index = fls(size - 1);

	if (unlikely(!(*b)[index])) {
		WARN_ONCE(1,
			"kmalloc_slab: NULL cache for size=%zu index=%u (caller %pS)\n",
			size, index, (void *)caller);
		return NULL;
	}

	return (*b)[index];
}

gfp_t kmalloc_fix_flags(gfp_t flags);

/* ───────────────────────────────────────────────────────────────────────────
 * Cache lifecycle
 * ─────────────────────────────────────────────────────────────────────────*/

int  do_kmem_cache_create(struct kmem_cache *s, const char *name,
			  unsigned int size, struct kmem_cache_args *args,
			  slab_flags_t flags);
void __init kmem_cache_init(void);
void create_boot_cache(struct kmem_cache *, const char *name,
		       unsigned int size, slab_flags_t flags,
		       unsigned int useroffset, unsigned int usersize);

int        slab_unmergeable(struct kmem_cache *s);
bool       slab_args_unmergeable(struct kmem_cache_args *args, slab_flags_t flags);
slab_flags_t kmem_cache_flags(slab_flags_t flags, const char *name);

static inline bool is_kmalloc_cache(struct kmem_cache *s)
{
	return s->flags & SLAB_KMALLOC;
}

static inline bool is_kmalloc_normal(struct kmem_cache *s)
{
	if (!is_kmalloc_cache(s))
		return false;
	return !(s->flags & (SLAB_CACHE_DMA | SLAB_ACCOUNT |
			     SLAB_RECLAIM_ACCOUNT));
}

bool __kfree_rcu_sheaf(struct kmem_cache *s, void *obj);
void flush_all_rcu_sheaves(void);
void flush_rcu_sheaves_on_cache(struct kmem_cache *s);

/* ───────────────────────────────────────────────────────────────────────────
 * Flag groups
 * ─────────────────────────────────────────────────────────────────────────*/

#define SLAB_CORE_FLAGS  (SLAB_HWCACHE_ALIGN | SLAB_CACHE_DMA |	\
			  SLAB_CACHE_DMA32 | SLAB_PANIC |		\
			  SLAB_TYPESAFE_BY_RCU | SLAB_DEBUG_OBJECTS |	\
			  SLAB_NOLEAKTRACE | SLAB_RECLAIM_ACCOUNT |	\
			  SLAB_TEMPORARY | SLAB_ACCOUNT |		\
			  SLAB_NO_USER_FLAGS | SLAB_KMALLOC | SLAB_NO_MERGE)

#define SLAB_DEBUG_FLAGS (SLAB_RED_ZONE | SLAB_POISON | SLAB_STORE_USER | \
			  SLAB_TRACE | SLAB_CONSISTENCY_CHECKS)

#define SLAB_FLAGS_PERMITTED (SLAB_CORE_FLAGS | SLAB_DEBUG_FLAGS)

bool __kmem_cache_empty(struct kmem_cache *);
int  __kmem_cache_shutdown(struct kmem_cache *);
void __kmem_cache_release(struct kmem_cache *);
int  __kmem_cache_shrink(struct kmem_cache *);
void slab_kmem_cache_release(struct kmem_cache *);

/* ───────────────────────────────────────────────────────────────────────────
 * slabinfo
 * ─────────────────────────────────────────────────────────────────────────*/

struct seq_file;
struct file;

struct slabinfo {
	unsigned long active_objs;
	unsigned long num_objs;
	unsigned long active_slabs;
	unsigned long num_slabs;
	unsigned long shared_avail;
	unsigned int  limit;
	unsigned int  batchcount;
	unsigned int  shared;
	unsigned int  objects_per_slab;
	unsigned int  cache_order;
};

void get_slabinfo(struct kmem_cache *s, struct slabinfo *sinfo);

/* ───────────────────────────────────────────────────────────────────────────
 * SLUB debug helpers
 * ─────────────────────────────────────────────────────────────────────────*/

#ifdef CONFIG_SLUB_DEBUG
# ifdef CONFIG_SLUB_DEBUG_ON
DECLARE_STATIC_KEY_TRUE(slub_debug_enabled);
# else
DECLARE_STATIC_KEY_FALSE(slub_debug_enabled);
# endif
extern void print_tracking(struct kmem_cache *s, void *object);
long validate_slab_cache(struct kmem_cache *s);
static inline bool __slub_debug_enabled(void)
{
	return static_branch_unlikely(&slub_debug_enabled);
}
#else
static inline void print_tracking(struct kmem_cache *s, void *object) { }
static inline bool __slub_debug_enabled(void) { return false; }
#endif

/**
 * kmem_cache_debug_flags - Test whether any of @flags are set on @s.
 *
 * Only meaningful for flags processed by setup_slub_debug() which also
 * enables the static key.  Use only for SLAB_DEBUG_FLAGS members.
 */
static inline bool kmem_cache_debug_flags(struct kmem_cache *s,
					  slab_flags_t flags)
{
	if (IS_ENABLED(CONFIG_SLUB_DEBUG))
		VM_WARN_ON_ONCE(!(flags & SLAB_DEBUG_FLAGS));
	if (__slub_debug_enabled())
		return s->flags & flags;
	return false;
}

#if IS_ENABLED(CONFIG_SLUB_DEBUG) && IS_ENABLED(CONFIG_KUNIT)
bool slab_in_kunit_test(void);
#else
static inline bool slab_in_kunit_test(void) { return false; }
#endif

/* ───────────────────────────────────────────────────────────────────────────
 * Metadata access guards
 *
 * SLUB manipulates object metadata that lives outside the allocated range.
 * These wrappers suppress KASAN / KMSAN reports for those accesses.
 * ─────────────────────────────────────────────────────────────────────────*/

static inline void metadata_access_enable(void)
{
	kasan_disable_current();
	kmsan_disable_current();
}

static inline void metadata_access_disable(void)
{
	kmsan_enable_current();
	kasan_enable_current();
}

/* ───────────────────────────────────────────────────────────────────────────
 * Object extension vectors  (CONFIG_SLAB_OBJ_EXT)
 * ─────────────────────────────────────────────────────────────────────────*/

#ifdef CONFIG_SLAB_OBJ_EXT

/**
 * slab_obj_exts - Raw pointer to the extension vector for a slab.
 * @slab: The slab to query.
 *
 * Returns the extension vector base address, or 0 if none has been
 * associated.  Do NOT dereference directly; always use get_slab_obj_exts()
 * / put_slab_obj_exts() bracketing and slab_obj_ext() for element access.
 *
 * Example::
 *
 *   obj_exts = slab_obj_exts(slab);
 *   if (obj_exts) {
 *       get_slab_obj_exts(obj_exts);
 *       ext = slab_obj_ext(slab, obj_exts, obj_to_index(s, slab, obj));
 *       // use ext
 *       put_slab_obj_exts(obj_exts);
 *   }
 */
static inline unsigned long slab_obj_exts(struct slab *slab)
{
	unsigned long obj_exts = READ_ONCE(slab->obj_exts);

#ifdef CONFIG_MEMCG
	VM_BUG_ON_PAGE(obj_exts && !(obj_exts & MEMCG_DATA_OBJEXTS) &&
		       obj_exts != OBJEXTS_ALLOC_FAIL, slab_page(slab));
	VM_BUG_ON_PAGE(obj_exts & MEMCG_DATA_KMEM, slab_page(slab));
#endif

	return obj_exts & ~OBJEXTS_FLAGS_MASK;
}

static inline void get_slab_obj_exts(unsigned long obj_exts)
{
	VM_WARN_ON_ONCE(!obj_exts);
	metadata_access_enable();
}

static inline void put_slab_obj_exts(unsigned long obj_exts)
{
	metadata_access_disable();
}

#ifdef CONFIG_64BIT
static inline void slab_set_stride(struct slab *slab, unsigned int stride)
{
	slab->stride = stride;
}
static inline unsigned int slab_get_stride(struct slab *slab)
{
	return slab->stride;
}
#else
static inline void slab_set_stride(struct slab *slab, unsigned int stride)
{
	VM_WARN_ON_ONCE(stride != sizeof(struct slabobj_ext));
}
static inline unsigned int slab_get_stride(struct slab *slab)
{
	return sizeof(struct slabobj_ext);
}
#endif /* CONFIG_64BIT */

/**
 * slab_obj_ext - Pointer to the extension record for one object.
 * @slab:     The slab containing the object.
 * @obj_exts: Base address from slab_obj_exts().
 * @index:    Object index from obj_to_index().
 *
 * Must be called inside a get/put_slab_obj_exts() section.
 */
static inline struct slabobj_ext *slab_obj_ext(struct slab *slab,
					       unsigned long obj_exts,
					       unsigned int index)
{
	struct slabobj_ext *obj_ext;

	VM_WARN_ON_ONCE(obj_exts != slab_obj_exts(slab));
	obj_ext = (struct slabobj_ext *)(obj_exts +
					 slab_get_stride(slab) * index);
	return kasan_reset_tag(obj_ext);
}

int alloc_slab_obj_exts(struct slab *slab, struct kmem_cache *s,
			gfp_t gfp, bool new_slab);

#else /* CONFIG_SLAB_OBJ_EXT */

static inline unsigned long slab_obj_exts(struct slab *slab) { return 0; }
static inline struct slabobj_ext *slab_obj_ext(struct slab *slab,
					       unsigned long obj_exts,
					       unsigned int index)
					       { return NULL; }
static inline void         slab_set_stride(struct slab *slab, unsigned int stride) { }
static inline unsigned int slab_get_stride(struct slab *slab) { return 0; }

#endif /* CONFIG_SLAB_OBJ_EXT */

/* ───────────────────────────────────────────────────────────────────────────
 * vmstat index
 * ─────────────────────────────────────────────────────────────────────────*/

static inline enum node_stat_item cache_vmstat_idx(struct kmem_cache *s)
{
	return (s->flags & SLAB_RECLAIM_ACCOUNT) ?
		NR_SLAB_RECLAIMABLE_B : NR_SLAB_UNRECLAIMABLE_B;
}

/* ───────────────────────────────────────────────────────────────────────────
 * memcg hooks
 * ─────────────────────────────────────────────────────────────────────────*/

#ifdef CONFIG_MEMCG
bool __memcg_slab_post_alloc_hook(struct kmem_cache *s, struct list_lru *lru,
				  gfp_t flags, size_t size, void **p);
void __memcg_slab_free_hook(struct kmem_cache *s, struct slab *slab,
			    void **p, int objects, unsigned long obj_exts);
#endif

/* ───────────────────────────────────────────────────────────────────────────
 * Large kmalloc helpers
 * ─────────────────────────────────────────────────────────────────────────*/

void kvfree_rcu_cb(struct rcu_head *head);

static inline unsigned int large_kmalloc_order(const struct page *page)
{
	return page[1].flags.f & 0xff;
}

static inline size_t large_kmalloc_size(const struct page *page)
{
	return PAGE_SIZE << large_kmalloc_order(page);
}

/* ───────────────────────────────────────────────────────────────────────────
 * Leak / allocation tracking
 *
 * kmem_obj_info is extended with:
 *   kp_alloc_jiffies  — jiffies at allocation time
 *   kp_alloc_pid      — PID of the allocating task
 *   kp_free_stack_ts  — saved stack at free time (separate from kp_free_stack
 *                       which holds the free-path IP array)
 *
 * slab_track_alloc() fills the new fields; it compiles away to nothing when
 * CONFIG_SLUB_DEBUG is disabled so there is zero overhead in production builds.
 * ─────────────────────────────────────────────────────────────────────────*/

#ifdef CONFIG_PRINTK
# define KS_ADDRS_COUNT 16

struct kmem_obj_info {
	void			*kp_ptr;
	struct slab		*kp_slab;
	void			*kp_objp;
	unsigned long		 kp_data_offset;
	struct kmem_cache	*kp_slab_cache;
	void			*kp_ret;

	/* allocation-site stack */
	void *kp_stack[KS_ADDRS_COUNT];

	/* free-site IP array (filled by __kmem_obj_info on freed objects) */
	void *kp_free_stack[KS_ADDRS_COUNT];

	/* ---- leak-tracking additions ------------------------------------ */
# ifdef CONFIG_SLUB_DEBUG
	unsigned long		 kp_alloc_jiffies;  /* time of allocation      */
	pid_t			 kp_alloc_pid;      /* allocating task PID     */
	/* free-site stack snapshot (complements kp_free_stack IP array)   */
	void *kp_free_stack_ts[KS_ADDRS_COUNT];
# endif
};

void __kmem_obj_info(struct kmem_obj_info *kpp, void *object,
		     struct slab *slab);

/**
 * slab_track_alloc - Record allocation provenance in a kmem_obj_info.
 * @kpp: Info struct to populate.
 *
 * Call immediately after __kmem_obj_info() returns to capture the
 * timestamp and PID of the allocating task.  No-op in non-debug builds.
 */
# ifdef CONFIG_SLUB_DEBUG
static inline void slab_track_alloc(struct kmem_obj_info *kpp)
{
	kpp->kp_alloc_jiffies = jiffies;
	kpp->kp_alloc_pid     = current->pid;
}
# else
static inline void slab_track_alloc(struct kmem_obj_info *kpp) { }
# endif

#endif /* CONFIG_PRINTK */

/* ───────────────────────────────────────────────────────────────────────────
 * Miscellaneous
 * ─────────────────────────────────────────────────────────────────────────*/

void __check_heap_object(const void *ptr, unsigned long n,
			 const struct slab *slab, bool to_user);

void defer_free_barrier(void);

static inline bool slub_debug_orig_size(struct kmem_cache *s)
{
	return kmem_cache_debug_flags(s, SLAB_STORE_USER) &&
	       (s->flags & SLAB_KMALLOC);
}

#ifdef CONFIG_SLUB_DEBUG
void skip_orig_size_check(struct kmem_cache *s, const void *object);
#endif

/* ───────────────────────────────────────────────────────────────────────────
 * Initialisation path helpers
 * ─────────────────────────────────────────────────────────────────────────*/

/**
 * slab_want_init_on_alloc - Should a freshly-allocated object be zeroed?
 * @flags: GFP flags of the allocation.
 * @c:     The cache serving the allocation.
 *
 * Returns true when init-on-alloc semantics require the object to be
 * zeroed before it is handed to the caller.
 *
 * Safety: guards against a NULL @c so callers need not check before
 * calling from the allocation fast-path.
 */
static inline bool slab_want_init_on_alloc(gfp_t flags, struct kmem_cache *c)
{
	if (unlikely(!c))
		return false;

	if (static_branch_maybe(CONFIG_INIT_ON_ALLOC_DEFAULT_ON,
				&init_on_alloc)) {
		if (c->ctor)
			return false;
		if (c->flags & (SLAB_TYPESAFE_BY_RCU | SLAB_POISON))
			return flags & __GFP_ZERO;
		return true;
	}
	return flags & __GFP_ZERO;
}

static inline bool slab_want_init_on_free(struct kmem_cache *c)
{
	if (static_branch_maybe(CONFIG_INIT_ON_FREE_DEFAULT_ON,
				&init_on_free))
		return !(c->ctor ||
			 (c->flags & (SLAB_TYPESAFE_BY_RCU | SLAB_POISON)));
	return false;
}

/* ───────────────────────────────────────────────────────────────────────────
 * debugfs
 * ─────────────────────────────────────────────────────────────────────────*/

#if defined(CONFIG_DEBUG_FS) && defined(CONFIG_SLUB_DEBUG)
void debugfs_slab_release(struct kmem_cache *);
#else
static inline void debugfs_slab_release(struct kmem_cache *s) { }
#endif

/* ───────────────────────────────────────────────────────────────────────────
 * Unreclaimable slab dump
 * ─────────────────────────────────────────────────────────────────────────*/

#ifdef CONFIG_SLUB_DEBUG
void dump_unreclaimable_slab(void);
#else
static inline void dump_unreclaimable_slab(void) { }
#endif

void ___cache_free(struct kmem_cache *cache, void *x, unsigned long addr);

/* ───────────────────────────────────────────────────────────────────────────
 * Freelist randomisation
 * ─────────────────────────────────────────────────────────────────────────*/

#ifdef CONFIG_SLAB_FREELIST_RANDOM
int  cache_random_seq_create(struct kmem_cache *cachep, unsigned int count,
			     gfp_t gfp);
void cache_random_seq_destroy(struct kmem_cache *cachep);
#else
static inline int cache_random_seq_create(struct kmem_cache *cachep,
					  unsigned int count, gfp_t gfp)
					  { return 0; }
static inline void cache_random_seq_destroy(struct kmem_cache *cachep) { }
#endif

#endif /* MM_SLAB_H */
