// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright(c) 2020 Intel Corporation. All rights reserved.
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 */

#include <sof/lib/alloc.h>
#include <sof/drivers/interrupt.h>
#include <sof/drivers/interrupt-map.h>
#include <sof/lib/dma.h>
#include <sof/schedule/schedule.h>
#include <platform/drivers/interrupt.h>
#include <sof/lib/notifier.h>
#include <sof/audio/pipeline.h>
#include <sof/audio/component_ext.h>
#include <sof/trace/trace.h>

/* Zephyr includes */
#include <soc.h>

/* Including following header
 * #include <kernel.h>
 * triggers include chain issue
 *
 * TODO: Figure out best way for include
 */
void *k_malloc(size_t size);
void *k_calloc(size_t nmemb, size_t size);
void k_free(void *ptr);

int arch_irq_connect_dynamic(unsigned int irq, unsigned int priority,
			     void (*routine)(void *parameter),
			     void *parameter, u32_t flags);

#define arch_irq_enable(irq)	z_soc_irq_enable(irq)
#define arch_irq_disable(irq)	z_soc_irq_disable(irq)

/* TODO: Use ASSERT */
#if !defined(CONFIG_DYNAMIC_INTERRUPTS)
#error Define CONFIG_DYNAMIC_INTERRUPTS
#endif

#if !defined(CONFIG_HEAP_MEM_POOL_SIZE)
#error Define CONFIG_HEAP_MEM_POOL_SIZE
#endif

/*
 * Memory
 */

/*  Single-linked alloc list for simple book keeping */
static sys_slist_t alloc_list;

/* Organized for alignment purposes */
struct __alloc_hdr {
	sys_snode_t snode;
	uint32_t size;
	char padding[20];
	void *orig_ptr;
} __packed;

#define DEBUG_ALLOC			1
#define ALWAYS_USE_ALIGNED_ALLOC	1

static struct k_spinlock lock;

BUILD_ASSERT((sizeof(struct __alloc_hdr) == 32), "Must be 32");

void *rmalloc(enum mem_zone zone, uint32_t flags, uint32_t caps, size_t bytes)
{
	if (IS_ENABLED(ALWAYS_USE_ALIGNED_ALLOC)) {
		return  rballoc_align(flags, caps, bytes,
				      PLATFORM_DCACHE_ALIGN);
	} else {
		struct __alloc_hdr *hdr;
		void *new_ptr;

		bytes += sizeof(struct __alloc_hdr);

		/* TODO: Use different memory areas - & cache line alignment*/

		/* Allocation header in the beginning of the memory block */
		hdr = k_malloc(bytes);
		if (!hdr) {
			trace_error(TRACE_CLASS_MEM, "Failed to malloc");
			return NULL;
		}

		/* New pointer starts right after sizeof(*hdr) */
		new_ptr = hdr + 1;

		hdr->orig_ptr = hdr;
		hdr->size = bytes;

		tracev_event(TRACE_CLASS_MEM, "rz: hdr %p new_ptr %p sz %u",
			     hdr, new_ptr, hdr->size);

		if (IS_ENABLED(DEBUG_ALLOC)) {
			k_spinlock_key_t k = k_spin_lock(&lock);
			sys_slist_append(&alloc_list, &hdr->snode);
			k_spin_unlock(&lock, k);
		}

		return new_ptr;
	}
}

/* Use SOF_MEM_ZONE_BUFFER at the moment */
void *rbrealloc_align(void *ptr, uint32_t flags, uint32_t caps, size_t bytes,
		      size_t old_bytes, uint32_t alignment)
{
	void *new_ptr;

	if (!ptr) {
		/* TODO: Use correct zone */
		return rmalloc(SOF_MEM_ZONE_BUFFER, flags, caps, bytes);
	}

	/* Original version returns NULL without freeing this memory */
	if (!bytes) {
		/* TODO: Should we call rfree(ptr); */
		trace_error(TRACE_CLASS_MEM, "bytes == 0");
		return NULL;
	}

	new_ptr = rmalloc(SOF_MEM_ZONE_BUFFER, flags, caps, bytes);
	if (!new_ptr) {
		return NULL;
	}

	if (!(flags & SOF_MEM_FLAG_NO_COPY)) {
		memcpy(new_ptr, ptr, MIN(bytes, old_bytes));
	}

	rfree(ptr);

	trace_event(TRACE_CLASS_MEM, "realloc: new ptr %p", new_ptr);

	return new_ptr;
}

/**
 * Similar to rmalloc(), guarantees that returned block is zeroed.
 *
 * @note Do not use  for buffers (SOF_MEM_ZONE_BUFFER zone).
 *       rballoc(), rballoc_align() to allocate memory for buffers.
 */
void *rzalloc(enum mem_zone zone, uint32_t flags, uint32_t caps, size_t bytes)
{
	if (IS_ENABLED(ALWAYS_USE_ALIGNED_ALLOC)) {
		void *ptr = rmalloc(zone, flags, caps, bytes);

		memset(ptr, 0, bytes);

		return ptr;
	} else {
		struct __alloc_hdr *hdr;
		void *new_ptr;

		bytes += sizeof(struct __alloc_hdr);

		/* TODO: Use different memory areas & cache line alignment */

		/* Allocation header in the beginning of the memory block */
		hdr = k_calloc(bytes, 1);
		if (!hdr) {
			trace_error(TRACE_CLASS_MEM, "Failed to rzalloc");
			k_panic();
			return NULL;
		}

		/* New pointer starts right after sizeof(*hdr) */
		new_ptr = hdr + 1;

		hdr->orig_ptr = hdr;
		hdr->size = bytes;

		tracev_event(TRACE_CLASS_MEM, "rz: hdr %p new %p sz %u",
			     hdr, new_ptr, hdr->size);

		if (IS_ENABLED(DEBUG_ALLOC)) {
			k_spinlock_key_t k = k_spin_lock(&lock);
			sys_slist_append(&alloc_list, &hdr->snode);
			k_spin_unlock(&lock, k);
		}

		return new_ptr;
	}
}

/**
 * Allocates memory block from SOF_MEM_ZONE_BUFFER.
 * @param flags Flags, see SOF_MEM_FLAG_...
 * @param caps Capabilities, see SOF_MEM_CAPS_...
 * @param bytes Size in bytes.
 * @param alignment Alignment in bytes.
 * @return Pointer to the allocated memory or NULL if failed.
 */
void *rballoc_align(uint32_t flags, uint32_t caps, size_t bytes,
		    uint32_t alignment)
{
	struct __alloc_hdr *hdr;
	void *ptr, *new_ptr;

	bytes += PLATFORM_DCACHE_ALIGN - 1 + sizeof(struct __alloc_hdr);

	/* TODO: Rewrite with alignment, mem areas, caps */
	ptr = k_malloc(bytes);
	if (!ptr) {
		trace_error(TRACE_CLASS_MEM, "Failed to rballoc_align");
		return NULL;
	}

	new_ptr = (void *)ROUND_UP((unsigned long)ptr +
				   sizeof(struct __alloc_hdr), PLATFORM_DCACHE_ALIGN);

	hdr = (struct __alloc_hdr *)new_ptr - 1;

	hdr->orig_ptr = ptr;
	hdr->size = bytes;

	if (IS_ENABLED(DEBUG_ALLOC)) {
		k_spinlock_key_t k = k_spin_lock(&lock);
		sys_slist_append(&alloc_list, &hdr->snode);
		k_spin_unlock(&lock, k);
	}

	tracev_event(TRACE_CLASS_MEM, "ra: hdr %p new %p sz %u",
		     hdr, new_ptr, hdr->size);

	return new_ptr;
}

/*
 * Free's memory allocated by above alloc calls.
 */
void rfree(void *ptr)
{
	if (!ptr) {
		/* Should this be warning? */
		trace_error(TRACE_CLASS_MEM, "Trying to free NULL");
		return;
	} else {
		struct __alloc_hdr *hdr = (struct __alloc_hdr *)ptr - 1;
		void *orig_ptr = hdr->orig_ptr;

		tracev_event(TRACE_CLASS_MEM, "rm: ptr %p orig %p",
			     ptr, orig_ptr);

		if (IS_ENABLED(DEBUG_ALLOC)) {
			k_spinlock_key_t k;
			bool found;

			k = k_spin_lock(&lock);
			found = sys_slist_find_and_remove(&alloc_list, &hdr->snode);
			k_spin_unlock(&lock, k);

			if (!found) {
				trace_error(TRACE_CLASS_MEM,
					    "Remove unknown %p, size %u",
					    ptr, hdr->size);
			}

			return;
		}

		k_free(orig_ptr);
	}
}

/* debug only - only needed for linking */
void heap_trace_all(int force)
{
}

/* needed for linkage only */
const char irq_name_level2[] = "level2";
const char irq_name_level5[] = "level5";

int interrupt_get_irq(unsigned int irq, const char *cascade)
{
	if (cascade == irq_name_level2)
		return SOC_AGGREGATE_IRQ(irq, IRQ_NUM_EXT_LEVEL2);
	if (cascade == irq_name_level5)
		return SOC_AGGREGATE_IRQ(irq, IRQ_NUM_EXT_LEVEL5);
	return SOC_AGGREGATE_IRQ(0, irq);
}

int interrupt_register(uint32_t irq, void(*handler)(void *arg), void *arg)
{
	return arch_irq_connect_dynamic(irq, 0, handler, arg, 0);
}

/* unregister an IRQ handler - matches on IRQ number and data ptr */
void interrupt_unregister(uint32_t irq, const void *arg)
{
	/*
	 * There is no "unregister" (or "disconnect") for
         * interrupts in Zephyr.
         */
	arch_irq_disable(irq);
}

/* enable an interrupt source - IRQ needs mapped to Zephyr,
 * arg is used to match.
 */
uint32_t interrupt_enable(uint32_t irq, void *arg)
{
	arch_irq_enable(irq);

	return 0;
}

/* disable interrupt */
uint32_t interrupt_disable(uint32_t irq, void *arg)
{
	arch_irq_disable(irq);

	return 0;
}

/* TODO; zephyr should do this. */
void platform_interrupt_init(void)
{
	int core = 0;

	/* mask all external IRQs by default */
	irq_write(REG_IRQ_IL2MSD(core), REG_IRQ_IL2MD_ALL);
	irq_write(REG_IRQ_IL3MSD(core), REG_IRQ_IL3MD_ALL);
	irq_write(REG_IRQ_IL4MSD(core), REG_IRQ_IL4MD_ALL);
	irq_write(REG_IRQ_IL5MSD(core), REG_IRQ_IL5MD_ALL);

}

/*
 * Timers
 */

uint64_t arch_timer_get_system(struct timer *timer)
{
	return platform_timer_get(timer);
}

/*
 * Notifier
 */

static struct notify *host_notify;

struct notify **arch_notify_get(void)
{
	if (!host_notify)
		host_notify = k_calloc(sizeof(*host_notify), 1);
	return &host_notify;
}

/*
 * Debug
 */
void arch_dump_regs_a(void *dump_buf)
{
	/* needed for linkage only */
}

/* used by panic code only - should not use this as zephyr register handlers */
volatile void *task_context_get(void)
{
	return NULL;
}

/*
 * Xtensa
 */
unsigned int _xtos_ints_off( unsigned int mask )
{
	/* turn all local IRQs OFF */
	irq_lock();
	return 0;
}

/*
 * init audio components.
 */

/* TODO: this is not yet working with Zephyr - section hase been created but
 *  no symbols are being loaded into ELF file.
 */
extern intptr_t _module_init_start;
extern intptr_t _module_init_end;

static void sys_module_init(void)
{
	intptr_t *module_init = (intptr_t *)(&_module_init_start);

	for (; module_init < (intptr_t *)&_module_init_end; ++module_init)
		((void(*)(void))(*module_init))();
}

char *get_trace_class(uint32_t trace_class)
{
#define CASE(x) case TRACE_CLASS_##x: return #x
	switch (trace_class) {
		CASE(IRQ);
		CASE(IPC);
		CASE(PIPE);
		CASE(DAI);
		CASE(DMA);
		CASE(COMP);
		CASE(WAIT);
		CASE(LOCK);
		CASE(MEM);
		CASE(BUFFER);
		CASE(SA);
		CASE(POWER);
		CASE(IDC);
		CASE(CPU);
		CASE(CLK);
		CASE(EDF);
		CASE(SCHEDULE);
		CASE(SCHEDULE_LL);
		CASE(CHMAP);
		CASE(NOTIFIER);
		CASE(MN);
		CASE(PROBE);
	default: return "unknown";
	}
}
/*
 * TODO: all the audio processing components/modules constructor should be
 * linked to the module_init section, but this is not happening. Just call
 * constructors directly atm.
 */

void sys_comp_volume_init(void);
void sys_comp_host_init(void);
void sys_comp_mixer_init(void);
void sys_comp_dai_init(void);
void sys_comp_src_init(void);
void sys_comp_mux_init(void);
void sys_comp_selector_init(void);
void sys_comp_switch_init(void);
void sys_comp_tone_init(void);
void sys_comp_eq_fir_init(void);
void sys_comp_keyword_init(void);
void sys_comp_asrc_init(void);
void sys_comp_dcblock_init(void);
void sys_comp_eq_iir_init(void);

int task_main_start(void)
{
	struct sof *sof = sof_get();

	/* init default audio components */
	sys_comp_init(sof);

	/* init self-registered modules */
	sys_module_init();
	sys_comp_volume_init();
	sys_comp_host_init();
	sys_comp_mixer_init();
	sys_comp_dai_init();
	sys_comp_src_init();

	/* only CAVS18+ have enough memory for these */
#if defined CONFIG_SOC_SERIES_INTEL_CAVS_V18 ||\
	defined CONFIG_SOC_SERIES_INTEL_CAVS_V20 ||\
	defined CONFIG_SOC_SERIES_INTEL_CAVS_V25
	//sys_comp_mux_init();   // needs more symbols.
	sys_comp_selector_init();
	sys_comp_switch_init();
	sys_comp_tone_init();
	sys_comp_eq_fir_init();
	sys_comp_keyword_init();
	sys_comp_asrc_init();
	sys_comp_dcblock_init();
	sys_comp_eq_iir_init();
#endif

	/* init pipeline position offsets */
	pipeline_posn_init(sof);

	return 0;
}
