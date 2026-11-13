/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright © 2026 Joseph Telaak */

#ifndef PVRSGX_CORE_H
#define PVRSGX_CORE_H

#include <linux/bits.h>
#include <linux/types.h>

/*
 * Core features, as the SGX_FEATURE_* names of the GPL DDK's
 * sgxfeaturedefs.h. Only those the driver's design depends on are listed.
 */
/* One page directory per client: BIF directory lists beyond list 0. */
#define PVRSGX_FEATURE_MULTIPLE_MEM_CONTEXTS	BIT(0)
/* EUR_CR_EVENT_KICK1..3 next to EUR_CR_EVENT_KICK. */
#define PVRSGX_FEATURE_MULTI_EVENT_KICK		BIT(1)

/* The USSE instruction set, for the shader validator's tables. */
enum pvrsgx_isa {
	PVRSGX_ISA_SGX545,
};

/* What this driver needs: a directory per VM, and the second kick for the microkernel's start. */
#define PVRSGX_FEATURES_REQUIRED \
	(PVRSGX_FEATURE_MULTIPLE_MEM_CONTEXTS | PVRSGX_FEATURE_MULTI_EVENT_KICK)

/* The device-virtual layout (pvrsgx_fw.h) needs a 32-bit address space. */
#define PVRSGX_VA_BITS_REQUIRED	32

/*
 * SGX_CR_CORE_ID's core number: bits 15:0 are CONFIG, which the integrator
 * chose and which therefore differs from board to board. Only the number
 * above them is the core's.
 */
#define PVRSGX_CORE_ID_CORE_MASK	GENMASK(31, 16)

/* Part flags (&struct pvrsgx_part). */
/*
 * Nothing on this part can power the graphics island back once it is gated:
 * ACPI's GFX0 _PS0 only writes GDDS, the island belongs to the P-Unit's APM
 * block and the driver has no APM hand-over yet. It then keeps the device
 * awake for as long as it is bound.
 */
#define PVRSGX_PART_ISLAND_ALWAYS_ON	BIT(0)
/*
 * The SGX has no interrupt line of its own: it is bit 18 of the display
 * engine's interrupt registers in the same BAR 0, and that block signals the
 * CPU by MSI (Interrupt Pin reads 0). gma500 does this on Cedarview; on Clover
 * Trail no display driver exists, so powervr_sgx owns that one bit.
 */
#define PVRSGX_PART_VDC_IRQ		BIT(1)

/**
 * struct pvrsgx_part - What a board adds to the core it has
 *
 * Several parts carry the same SGX545 and differ in where its window sits and
 * in who manages its power. The bus glue picks the entry, and the core number
 * from it: the PCI id table's driver_data is the index.
 */
struct pvrsgx_part {
	/** @name: The part, as "Intel Clover Trail", for the log. */
	const char *name;
	/** @core: Its core's number, as the family table names it (545). */
	unsigned int core;
	/** @regs_off: Offset of the SGX's window inside BAR 0. */
	u32 regs_off;
	/** @flags: PVRSGX_PART_*. */
	u32 flags;
};

/*
 * struct pvrsgx_core_regs - Registers that move or change between SGX5xx cores
 *
 * The other registers in pvrsgx_regs.h sit at the same offsets with the same
 * fields on SGX535, 540, 544 and 545 (the GPL DDK's sgx5xxdefs.h). A register
 * a core lacks is 0.
 */
struct pvrsgx_core_regs {
	u32 core_id;
	u32 core_revision;
	u32 clkgatectl_units;		/* every gating field of CLKGATECTL */
	u32 clkgatectl_dpm;		/* CLKGATECTL's DPM field */
	u32 clkgatectl2;
	u32 clkgatectl2_units;
	u32 clkgatestatus;
	u32 clkgatestatus_dpm;
	u32 soft_reset_bif;
	u32 soft_reset_cores;		/* every unit's reset but the BIF's */
	u32 event_kick2;
	u32 event_kick2_now;
	u32 use_code_base0;
	u32 pds_exec_base;
	u32 bif_int_stat_fault_req;
	u32 bif_int_stat_fault_type;
	/* Not in the GPL header (pvrsgx_hw.h). */
	u32 event_pds_enable;
	u32 event_pds_enable_all;
	u32 event_pds_base;
	u32 event_pds_data0;
	u32 event_pds_data1;
	u32 edm_task_reg0;
	u32 edm_task_reg1;
};

/* One write of the vendor init script (docs/SRVINIT.md, "Init scripts"). */
struct pvrsgx_core_init_reg {
	u16 reg;
	u32 val;
};

/**
 * struct pvrsgx_core_family - What one SGX5xx core design has in common
 *
 * Every revision and part of the core shares these. The bus says which core
 * to expect, since even SGX_CR_CORE_ID moves between cores.
 */
struct pvrsgx_core_family {
	/** @name: The core, as "SGX545". */
	const char *name;
	/** @core: Its number, as the bus glue names it: 545 for SGX545. */
	unsigned int core;
	/** @core_id: Expected SGX_CR_CORE_ID; only its core number is compared. */
	u32 core_id;
	/** @regs: Where its registers and fields are. */
	const struct pvrsgx_core_regs *regs;
	/** @features: PVRSGX_FEATURE_*. */
	u32 features;
	/** @isa: Its USSE instruction set. */
	enum pvrsgx_isa isa;
	/** @num_dir_lists: BIF directory lists (SGX_FEATURE_BIF_NUM_DIRLISTS). */
	u32 num_dir_lists;
	/** @va_bits: Device-virtual address bits (SGX_FEATURE_ADDRESS_SPACE_SIZE). */
	u32 va_bits;
	/** @init_regs: Init script part 2's register writes, in order. */
	const struct pvrsgx_core_init_reg *init_regs;
	/** @num_init_regs: Entries in @init_regs. */
	unsigned int num_init_regs;
};

/**
 * struct pvrsgx_core_info - One SGX5xx part this driver supports
 *
 * A new part of a known core is a new entry; a new core is a new
 * &struct pvrsgx_core_family as well. The microkernel host interface and the
 * client heaps are the same across the family.
 */
struct pvrsgx_core_info {
	/** @family: The core design. */
	const struct pvrsgx_core_family *family;
	/** @name: What the part is called in the log. */
	const char *name;
	/** @core_rev: Expected SGX_CR_CORE_REVISION. */
	u32 core_rev;
	/** @clock_hz: Core clock, for the microkernel's timer. */
	u32 clock_hz;
	/** @fw_name: The open microkernel under /lib/firmware. */
	const char *fw_name;
};

const struct pvrsgx_core_family *pvrsgx_core_family_get(unsigned int core);
const struct pvrsgx_core_info *pvrsgx_core_lookup(const struct pvrsgx_core_family *family,
						  u32 core_id, u32 core_rev);

#endif /* PVRSGX_CORE_H */
