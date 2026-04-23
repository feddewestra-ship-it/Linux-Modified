/* SPDX-License-Identifier: GPL-2.0 */
/*
 * xhci-dbc.h — xHCI Debug Capability (DbC) early driver
 *
 * Copyright (C) 2016 Intel Corporation
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 *
 * The xHCI Debug Capability (DbC) is a hardware feature that exposes a
 * USB device interface independently of the main xHCI host controller
 * software stack.  This header describes:
 *
 *   • MMIO register layout  (struct xdbc_regs)
 *   • DMA data structures   (TRBs, ERST, contexts, strings)
 *   • Software state        (struct xdbc_state, struct xdbc_ring, …)
 *   • Bit-field definitions and manifest constants
 *
 * References:
 *   xHCI specification §7.6  "xHCI Debug Capability"
 *   xHCI specification §4.5  "Device Context Index"
 *   xHCI specification §7.6.3.2  "Endpoint Contexts and Transfer Rings"
 */

#ifndef __LINUX_XHCI_DBC_H
#define __LINUX_XHCI_DBC_H

#include <linux/types.h>
#include <linux/usb/ch9.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * MMIO register layout
 *
 * All registers are memory-mapped at the DbC Extended Capability base.
 * Use readl()/writel() (or the xdbc_read64/xdbc_write64 wrappers below)
 * for all accesses — never dereference raw pointers.
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * struct xdbc_regs — Debug Capability register set (§7.6.1).
 *
 * Field widths and offsets must match the hardware exactly; the
 * __reserved_* members consume the padding mandated by the spec.
 */
struct xdbc_regs {
	__le32	capability;		/* 00h  DbC Capability register        */
	__le32	doorbell;		/* 04h  DbC Doorbell register          */
	__le32	ersts;			/* 08h  Event Ring Segment Table Size  */
	__le32	__reserved_0;		/* 0ch  reserved                       */
	__le64	erstba;			/* 10h  ERST Base Address              */
	__le64	erdp;			/* 18h  Event Ring Dequeue Pointer     */
	__le32	control;		/* 20h  DbC Control register           */
	__le32	status;			/* 24h  DbC Status register            */
	__le32	portsc;			/* 28h  Port Status and Control        */
	__le32	__reserved_1;		/* 2ch  reserved                       */
	__le64	dccp;			/* 30h  Debug Capability Context Ptr   */
	__le32	devinfo1;		/* 38h  Device Descriptor Info 1       */
	__le32	devinfo2;		/* 3ch  Device Descriptor Info 2       */
};

/* ───────────────────────────────────────────────────────────────────────────
 * capability register (offset 00h)
 * ─────────────────────────────────────────────────────────────────────────*/

/* Maximum burst size from the DbC Capability register bits [23:16]. */
#define DEBUG_MAX_BURST(p)		(((p) >> 16) & 0xff)

/* ───────────────────────────────────────────────────────────────────────────
 * control register (offset 20h)
 * ─────────────────────────────────────────────────────────────────────────*/

#define CTRL_DBC_RUN			BIT(0)	/* DbC run                  */
#define CTRL_PORT_ENABLE		BIT(1)	/* DbC port enable          */
#define CTRL_HALT_OUT_TR		BIT(2)	/* Halt OUT transfer ring   */
#define CTRL_HALT_IN_TR			BIT(3)	/* Halt IN transfer ring    */
#define CTRL_DBC_RUN_CHANGE		BIT(4)	/* DbC Run Change indicator */
#define CTRL_DBC_ENABLE			BIT(31)	/* Global DbC enable        */

/* ───────────────────────────────────────────────────────────────────────────
 * status register (offset 24h)
 * ─────────────────────────────────────────────────────────────────────────*/

/* USB debug port number from DbC Status register bits [31:24]. */
#define DCST_DEBUG_PORT(p)		(((p) >> 24) & 0xff)

/* ───────────────────────────────────────────────────────────────────────────
 * portsc register (offset 28h)
 * ─────────────────────────────────────────────────────────────────────────*/

#define PORTSC_CONN_STATUS		BIT(0)	/* Current connect status   */
#define PORTSC_CONN_CHANGE		BIT(17)	/* Connect status change    */
#define PORTSC_RESET_CHANGE		BIT(21)	/* Port reset change        */
#define PORTSC_LINK_CHANGE		BIT(22)	/* Port link state change   */
#define PORTSC_CONFIG_CHANGE		BIT(23)	/* Port config error change */

/* ═══════════════════════════════════════════════════════════════════════════
 * DMA data structures
 *
 * These structures are written to memory shared with the hardware.
 * All fields are little-endian; use cpu_to_le*() / le*_to_cpu() for access.
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * struct xdbc_trb — Transfer Request Block (§6.4).
 *
 * Generic 16-byte TRB laid out as four 32-bit words.  Interpretation of
 * individual bits depends on the TRB type encoded in field[3][15:10].
 */
struct xdbc_trb {
	__le32 field[4];
};

/**
 * struct xdbc_erst_entry — Event Ring Segment Table entry (§6.5).
 *
 * Each entry describes one contiguous ring segment in DMA space.
 */
struct xdbc_erst_entry {
	__le64	seg_addr;		/* Segment base address (64-bit DMA) */
	__le32	seg_size;		/* Number of TRBs in the segment     */
	__le32	__reserved_0;		/* Reserved, must be zero            */
};

/**
 * struct xdbc_info_context — DbC Info Context (§7.6.2).
 *
 * Points to the USB string descriptors exposed by the debug device.
 * Each string* field holds a 64-bit DMA address.
 */
struct xdbc_info_context {
	__le64	string0;		/* DMA addr of String0 descriptor    */
	__le64	manufacturer;		/* DMA addr of Manufacturer string   */
	__le64	product;		/* DMA addr of Product string        */
	__le64	serial;			/* DMA addr of Serial Number string  */
	__le32	length;			/* Total length of all string descs  */
	__le32	__reserved_0[7];	/* Reserved, must be zero            */
};

/**
 * struct xdbc_ep_context — DbC Endpoint Context (§7.6.3).
 *
 * Describes a single bulk endpoint (IN or OUT) used by the debug device.
 * ep_info2 encodes the Max Packet Size, Max Burst Size, and EP type.
 * deq is the 64-bit DMA address of the Transfer Ring dequeue pointer.
 */
struct xdbc_ep_context {
	__le32	ep_info1;		/* Endpoint state, interval, etc.    */
	__le32	ep_info2;		/* EP type, max packet/burst size    */
	__le64	deq;			/* Transfer Ring dequeue pointer     */
	__le32	tx_info;		/* Average TRB length, max ESIT      */
	__le32	__reserved_0[11];	/* Reserved, must be zero            */
};

/**
 * struct xdbc_context — Top-level DbC Context structure (§7.6.2).
 *
 * Layout: [ Info Context | OUT EP Context | IN EP Context ]
 * The hardware locates it via the DCCP register.
 */
struct xdbc_context {
	struct xdbc_info_context	info;
	struct xdbc_ep_context		out;
	struct xdbc_ep_context		in;
};

/* ───────────────────────────────────────────────────────────────────────────
 * String descriptor constants
 * ─────────────────────────────────────────────────────────────────────────*/

#define XDBC_INFO_CONTEXT_SIZE		48	/* bytes, per spec §7.6.2    */
#define XDBC_MAX_STRING_LENGTH		64	/* bytes incl. NUL           */

#define XDBC_STRING_MANUFACTURER	"Linux Foundation"
#define XDBC_STRING_PRODUCT		"Linux USB GDB Target"
#define XDBC_STRING_SERIAL		"0001"

/**
 * struct xdbc_strings — Pool of USB string descriptors for the debug device.
 *
 * All four strings are allocated in a single DMA-coherent page so that one
 * DMA mapping covers the entire pool.
 */
struct xdbc_strings {
	char	string0[XDBC_MAX_STRING_LENGTH];
	char	manufacturer[XDBC_MAX_STRING_LENGTH];
	char	product[XDBC_MAX_STRING_LENGTH];
	char	serial[XDBC_MAX_STRING_LENGTH];
};

/* ───────────────────────────────────────────────────────────────────────────
 * USB device identity constants
 * ─────────────────────────────────────────────────────────────────────────*/

#define XDBC_PROTOCOL		1	/* GNU Remote Debug Command Set      */
#define XDBC_VENDOR_ID		0x1d6b	/* Linux Foundation VID              */
#define XDBC_PRODUCT_ID		0x0011	/* DbC debug gadget PID              */
#define XDBC_DEVICE_REV		0x0010	/* bcdDevice = 0.10                  */

/* ═══════════════════════════════════════════════════════════════════════════
 * Transfer ring software state
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * struct xdbc_segment — One contiguous block of TRBs backing a ring.
 *
 * @trbs:  Virtual address of the TRB array (kernel-mapped).
 * @dma:   Bus address of the TRB array (passed to hardware).
 */
struct xdbc_segment {
	struct xdbc_trb		*trbs;
	dma_addr_t		 dma;
};

/** Number of TRBs in each ring segment (must be a power of two). */
#define XDBC_TRBS_PER_SEGMENT		256

/**
 * struct xdbc_ring — Circular transfer ring.
 *
 * @segment:    Backing segment (single-segment rings only for DbC).
 * @enqueue:    Next TRB the producer will write.
 * @dequeue:    Next TRB the consumer will read.
 * @cycle_state: Producer cycle bit, toggled on each ring wrap.
 */
struct xdbc_ring {
	struct xdbc_segment	*segment;
	struct xdbc_trb		*enqueue;
	struct xdbc_trb		*dequeue;
	u32			 cycle_state;
};

/* ═══════════════════════════════════════════════════════════════════════════
 * Endpoint ID (Context Index) values
 *
 * The spec §7.6.3.2 assigns endpoint IDs 0 and 1 for the OUT and IN
 * Transfer Rings of the DbC Context.  AMD hardware follows this directly.
 * Intel hardware applies the Device Context Index formula from §4.5.1,
 * yielding IDs 2 and 3.  Both sets are defined here; drivers probe for
 * whichever pair the hardware reports.
 * ═════════════════════════════════════════════════════════════════════════*/

#define XDBC_EPID_OUT			0	/* OUT ep, AMD / spec-literal  */
#define XDBC_EPID_IN			1	/* IN  ep, AMD / spec-literal  */
#define XDBC_EPID_OUT_INTEL		2	/* OUT ep, Intel §4.5.1        */
#define XDBC_EPID_IN_INTEL		3	/* IN  ep, Intel §4.5.1        */

/* ═══════════════════════════════════════════════════════════════════════════
 * Top-level DbC software state
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * struct xdbc_state — Complete software state of one DbC instance.
 *
 * This structure is the single source of truth for an active DbC session.
 * Fields are grouped by function; do not reorder without updating all
 * initialisers.
 */
struct xdbc_state {
	/* ── PCI identity ─────────────────────────────────────────────── */
	u16			vendor;
	u16			device;
	u32			bus;
	u32			dev;
	u32			func;

	/* ── xHCI controller mapping ──────────────────────────────────── */
	void __iomem		*xhci_base;	/* ioremap base              */
	u64			 xhci_start;	/* PCI BAR0 physical address */
	size_t			 xhci_length;	/* BAR0 mapped size          */
	int			 port_number;	/* DbC USB port number       */

	/* ── DbC register base ────────────────────────────────────────── */
	struct xdbc_regs __iomem *xdbc_reg;

	/* ── DMA table page (one coherent page holding all DMA structs) ─ */
	dma_addr_t		 table_dma;
	void			*table_base;

	/* ── Event Ring Segment Table ─────────────────────────────────── */
	dma_addr_t		 erst_dma;
	size_t			 erst_size;
	void			*erst_base;

	/* ── Event ring ───────────────────────────────────────────────── */
	struct xdbc_ring	 evt_ring;
	struct xdbc_segment	 evt_seg;

	/* ── DbC context (info + ep contexts) ────────────────────────── */
	dma_addr_t		 dbcc_dma;
	size_t			 dbcc_size;
	void			*dbcc_base;

	/* ── USB string descriptors ───────────────────────────────────── */
	dma_addr_t		 string_dma;
	size_t			 string_size;
	void			*string_base;

	/* ── Bulk OUT endpoint ────────────────────────────────────────── */
	struct xdbc_ring	 out_ring;
	struct xdbc_segment	 out_seg;
	void			*out_buf;	/* bounce buffer, virtual    */
	dma_addr_t		 out_dma;	/* bounce buffer, bus addr   */

	/* ── Bulk IN endpoint ─────────────────────────────────────────── */
	struct xdbc_ring	 in_ring;
	struct xdbc_segment	 in_seg;
	void			*in_buf;	/* bounce buffer, virtual    */
	dma_addr_t		 in_dma;	/* bounce buffer, bus addr   */

	/* ── Driver flags (see XDBC_FLAGS_* below) ───────────────────── */
	u32			 flags;

	/* ── Serialises early_xdbc_write() against re-entrancy ───────── */
	raw_spinlock_t		 lock;
};

/* ───────────────────────────────────────────────────────────────────────────
 * xdbc_state.flags bit definitions
 * ─────────────────────────────────────────────────────────────────────────*/

#define XDBC_FLAGS_INITIALIZED		BIT(0)	/* Hardware initialised      */
#define XDBC_FLAGS_IN_STALL		BIT(1)	/* IN endpoint stalled       */
#define XDBC_FLAGS_OUT_STALL		BIT(2)	/* OUT endpoint stalled      */
#define XDBC_FLAGS_IN_PROCESS		BIT(3)	/* IN transfer in progress   */
#define XDBC_FLAGS_OUT_PROCESS		BIT(4)	/* OUT transfer in progress  */
#define XDBC_FLAGS_CONFIGURED		BIT(5)	/* DbC fully configured      */

/* ═══════════════════════════════════════════════════════════════════════════
 * Sizing constants
 * ═════════════════════════════════════════════════════════════════════════*/

/* PCI topology limits for the early DbC bus scan. */
#define XDBC_PCI_MAX_BUSES		256
#define XDBC_PCI_MAX_DEVICES		32
#define XDBC_PCI_MAX_FUNCTION		8

/*
 * Layout of the single DMA table page:
 *   [ ERST entry × 1  |  DbC context × 3  |  String entries × 4  ]
 * Each entry is XDBC_TABLE_ENTRY_SIZE bytes (one cache line).
 */
#define XDBC_TABLE_ENTRY_SIZE		64
#define XDBC_ERST_ENTRY_NUM		1
#define XDBC_DBCC_ENTRY_NUM		3
#define XDBC_STRING_ENTRY_NUM		4

/* Maximum USB packet size for the bulk endpoints (SuperSpeed = 1024 bytes). */
#define XDBC_MAX_PACKET			1024

/* ═══════════════════════════════════════════════════════════════════════════
 * Doorbell register helpers
 * ═════════════════════════════════════════════════════════════════════════*/

#define OUT_EP_DOORBELL			0
#define IN_EP_DOORBELL			1

/**
 * DOOR_BELL_TARGET - Encode an endpoint index into the Doorbell register.
 * @p: Endpoint index (OUT_EP_DOORBELL or IN_EP_DOORBELL).
 *
 * The DB Target field lives in bits [15:8] of the Doorbell register.
 */
#define DOOR_BELL_TARGET(p)		(((p) & 0xff) << 8)

/* ═══════════════════════════════════════════════════════════════════════════
 * 64-bit MMIO accessors
 *
 * xhci_read_64() / xhci_write_64() handle the split 32-bit read-modify-write
 * sequence required on platforms that do not support 64-bit MMIO natively.
 * Passing NULL for the xhci_hcd pointer is intentional for the early DbC
 * path, which runs before the main xHCI driver is initialised.
 * ═════════════════════════════════════════════════════════════════════════*/

#define xdbc_read64(regs)		xhci_read_64(NULL, (regs))
#define xdbc_write64(val, regs)		xhci_write_64(NULL, (val), (regs))

#endif /* __LINUX_XHCI_DBC_H */
