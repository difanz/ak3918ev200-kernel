/*
 * init cpu freq, clock
 *
 * report cpu id
 */
#include <asm/mach/irq.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/io.h>

#include <asm/sizes.h>
#include <mach/map.h>
#include <mach/clock.h>

#define AK_CPU_ID			(AK_VA_SYSCTRL + 0x00)

#if defined(CONFIG_CPU_AK3910)
#define AKCPU_TYPE			"AK3910"
#elif defined(CONFIG_CPU_AK3916)
#define AKCPU_TYPE			"AK3916"
#elif defined(CONFIG_CPU_AK3918)
#define AKCPU_TYPE			"AK3918"
#else
#error AK39xx Board NOT supported
#endif

/*
 * Silicon revisions this port has been seen on.  The register at
 * AK_VA_SYSCTRL + 0 reports a revision code, not a part number, so a single
 * expected value only ever matches the revisions that existed when the SDK
 * release was cut.
 *
 * 0x20120100  the value this SDK release shipped with
 * 0x20150200  AK3918EV200, the revision this port targets
 */
static const unsigned long akcpu_known_ids[] __initconst = {
	0x20120100,
	0x20150200,
};


/*
 * Memory layout invariant for this SoC family.
 *
 * arch/arm/mach-ak39/Makefile.boot derives both the decompressed kernel load
 * address and the boot-parameter address from CONFIG_RAM_BASE plus
 * CONFIG_VIDEO_RESERVED_MEM_SIZE:
 *
 *	zreladdr    = CONFIG_RAM_BASE + CONFIG_VIDEO_RESERVED_MEM_SIZE + 0x8000
 *	params_phys = CONFIG_RAM_BASE + CONFIG_VIDEO_RESERVED_MEM_SIZE + 0x100
 *
 * CONFIG_PHYS_OFFSET is an independent Kconfig hex; Kconfig cannot derive it.
 * With CONFIG_ARM_PATCH_PHYS_VIRT disabled (which this SoC requires, because
 * arch/arm/kernel/head.S expects a 16 MiB aligned physical base and these
 * layouts are only 8 MiB aligned) __phys_to_virt() and __virt_to_phys() are
 * compile-time constant folds over CONFIG_PHYS_OFFSET.
 *
 * arch/arm/kernel/head.S is position independent: it derives the real physical
 * base with adr and builds both the kernel direct mapping and the boot-params
 * mapping from it.  So a disagreement between the two values does not stop
 * early assembly - it silently displaces every C-level address conversion.
 * The first casualty is phys_to_virt(__atags_pointer) in setup_machine_tags(),
 * which faults before trap_init() has installed any vectors and hangs the CPU
 * with no console output.
 *
 * Fail the build instead.
 */
#if defined(CONFIG_PHYS_OFFSET) && defined(CONFIG_RAM_BASE) && \
	defined(CONFIG_VIDEO_RESERVED_MEM_SIZE)
#if CONFIG_PHYS_OFFSET != (CONFIG_RAM_BASE + CONFIG_VIDEO_RESERVED_MEM_SIZE)
#error CONFIG_PHYS_OFFSET must equal CONFIG_RAM_BASE + CONFIG_VIDEO_RESERVED_MEM_SIZE
#endif
#endif


#define IODESC_ENT(x) 							\
{												\
	.virtual = (unsigned long)AK_VA_##x,		\
	.pfn	 = __phys_to_pfn(AK_PA_##x),		\
	.length	 = AK_SZ_##x,						\
	.type	 = MT_DEVICE						\
}

#ifdef CONFIG_DEBUG_AK39_UART0
/*
 * Keep the low-level debug windows alive past paging_init.  The virtual gap
 * between them must stay 0x27ed1000, which is what mach/debug-macro.S adds to
 * the UART base to reach the L2 transmit buffer.
 */
#define AK39_DEBUG_UART_VA	0xd0130000
#define AK39_DEBUG_L2CTRL_VA	0xd0140000	/* UART_VA + AK39_DEBUG_L2CTRL_GAP */
#define AK39_DEBUG_L2_VA	0xf8001000
#endif

static struct map_desc ak39_iodesc[] __initdata = {
#ifdef CONFIG_DEBUG_AK39_UART0
	{
		.virtual = AK39_DEBUG_UART_VA,
		.pfn	 = __phys_to_pfn(0x20130000),
		.length	 = SZ_4K,
		.type	 = MT_DEVICE,
	},
	{
		/*
		 * senduart clears the UART TX buffer through the L2 controller,
		 * 64 KiB above the UART registers.  head.S maps a 1 MiB section
		 * that happens to cover it, but devicemaps_init() clears every
		 * pmd from VMALLOC_START before calling map_io() and flushes the
		 * TLB straight after, so without this entry the console dies on
		 * the first character emitted after that flush - and the fault
		 * handler's own printk re-enters printascii and faults again.
		 */
		.virtual = AK39_DEBUG_L2CTRL_VA,
		.pfn	 = __phys_to_pfn(0x20140000),
		.length	 = SZ_4K,
		.type	 = MT_DEVICE,
	},
	{
		.virtual = AK39_DEBUG_L2_VA,
		.pfn	 = __phys_to_pfn(0x48001000),
		.length	 = SZ_4K,
		.type	 = MT_DEVICE,
	},
#endif
	IODESC_ENT(SYSCTRL),
	IODESC_ENT(CAMERA),
	IODESC_ENT(VENCODE),
	IODESC_ENT(SUBCTRL),
	IODESC_ENT(MAC),
	IODESC_ENT(REGRAM),
	IODESC_ENT(L2MEM),
};

void __init ak39_map_io(void)
{
	unsigned long regval = 0x0;
	unsigned int i;

	/* initialise the io descriptors we need for initialisation */
	iotable_init(ak39_iodesc, ARRAY_SIZE(ak39_iodesc));

	regval = __raw_readl(AK_CPU_ID);
	for (i = 0; i < ARRAY_SIZE(akcpu_known_ids); i++)
		if (regval == akcpu_known_ids[i])
			break;

	printk("ANYKA CPU %s (ID 0x%lx)\n", AKCPU_TYPE, regval);

	/*
	 * An unrecognised revision is worth reporting, but it is not worth
	 * killing the boot for: this runs from paging_init(), long before any
	 * console is registered, so the panic() that used to be here produced a
	 * silent hang with no way to tell it apart from a lockup.
	 */
	if (i == ARRAY_SIZE(akcpu_known_ids))
		pr_warn("ANYKA CPU ID 0x%lx is not a revision this port has been tested on\n",
			regval);

	/* need to change asic freq is here, Because higher asic freq  was affected usb function,
	 * I don't know essential reason of the problem 
	 */
	aisc_freq_set();
}

