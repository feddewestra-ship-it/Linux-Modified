// SPDX-License-Identifier: GPL-2.0
/*
 * Nexo OS - GPU Buddy Allocator Performance Layer
 * Focus: Low-latency, Zero-bottleneck allocation for Gaming.
 */

use core::ops::Range;
use core::sync::atomic::{AtomicU64, Ordering}; // NEXO: Voor lockless 'avail' checks

use crate::{
    bindings,
    clist_create,
    error::to_result,
    interop::list::CListHead,
    new_mutex,
    prelude::*,
    ptr::Alignment,
    sync::{
        lock::mutex::MutexGuard,
        Arc,
        Mutex,
    },
    types::Opaque,
};

// ... (GpuBuddyAllocMode en GpuBuddyAllocFlags blijven gelijk) ...

/// Nexo Optimized Inner Structure
#[pin_data(PinnedDrop)]
struct GpuBuddyInner {
    #[pin]
    inner: Opaque<bindings::gpu_buddy>,

    #[pin]
    lock: Mutex<()>,
    
    /// NEXO: Lockless tracker voor beschikbare bytes om 'hot-path' queries
    /// zoals buddy.avail() te versnellen zonder mutex-contention.
    avail_atomic: AtomicU64,
    
    params: GpuBuddyParams,
}

impl GpuBuddyInner {
    fn new(params: GpuBuddyParams) -> impl PinInit<Self, Error> {
        let size = params.size;
        let chunk_size = params.chunk_size;

        try_pin_init!(Self {
            inner <- Opaque::try_ffi_init(|ptr| {
                to_result(unsafe {
                    bindings::gpu_buddy_init(ptr, size, chunk_size.as_usize() as u64)
                })
            }),
            lock <- new_mutex!(()),
            avail_atomic: AtomicU64::new(size), // Start met volledige grootte
            params,
        })
    }

    fn lock(&self) -> GpuBuddyGuard<'_> {
        GpuBuddyGuard {
            inner: self,
            _guard: self.lock.lock(),
        }
    }
}

impl GpuBuddy {
    /// NEXO BOOST: Lockless availability check.
    /// Games gebruiken dit vaak om te checken of een texture geladen kan worden.
    /// Voorheen moest dit wachten op een lopende allocatie (mutex). Nu is het O(1).
    pub fn avail(&self) -> u64 {
        self.0.avail_atomic.load(Ordering::Relaxed)
    }

    /// Geoptimaliseerde allocatie
    pub fn alloc_blocks(
        &self,
        mode: GpuBuddyAllocMode,
        size: u64,
        min_block_size: Alignment,
        flags: impl Into<GpuBuddyAllocFlags>,
    ) -> impl PinInit<AllocatedBlocks, Error> {
        let buddy_arc = Arc::clone(&self.0);
        let (start, end) = mode.range();
        let mode_flags = mode.as_flags();
        let modifier_flags = flags.into();

        try_pin_init!(AllocatedBlocks {
            buddy: buddy_arc,
            list <- CListHead::new(),
            _: {
                if let GpuBuddyAllocMode::Range(range) = &mode {
                    if range.is_empty() { return Err(EINVAL); }
                }

                // Hot-path lock
                let guard = buddy.lock();

                let res = to_result(unsafe {
                    bindings::gpu_buddy_alloc_blocks(
                        guard.as_raw(),
                        start,
                        end,
                        size,
                        min_block_size.as_usize() as u64,
                        list.as_raw(),
                        mode_flags | usize::from(modifier_flags),
                    )
                });

                if res.is_ok() {
                    // NEXO: Update de atomic tracker direct na succesvolle allocatie
                    // We gebruiken 'fetch_sub' voor thread-safety.
                    buddy.avail_atomic.fetch_sub(size, Ordering::SeqCst);
                }
                res?
            }
        })
    }
}

#[pinned_drop]
impl PinnedDrop for AllocatedBlocks {
    fn drop(self: Pin<&mut Self>) {
        let guard = self.buddy.lock();
        
        // NEXO PERFORMANCE: Bereken de grootte van de vrij te geven lijst
        // voordat we de C-call doen om de atomic tracker te herstellen.
        let mut freed_size: u64 = 0;
        for block in self.iter() {
            freed_size += block.size();
        }

        unsafe {
            bindings::gpu_buddy_free_list(guard.as_raw(), self.list.as_raw(), 0);
        }
        
        // Herstel de beschikbare ruimte in de lockless tracker
        self.buddy.avail_atomic.fetch_add(freed_size, Ordering::SeqCst);
    }
}

impl AllocatedBlock<'_> {
    /// NEXO BOOST: Gebruik bit-shifting voor ultrasnelle size-berekening.
    /// In gaming loops wordt dit duizenden keren per seconde aangeroepen.
    #[inline(always)]
    pub fn size(&self) -> u64 {
        let shift = self.this.order();
        let base = self.blocks.buddy.params.chunk_size.as_usize() as u64;
        base << shift
    }
}
