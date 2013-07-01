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
 * The register at AK_VA_SYSCTRL + 0 reports a revision code, not a part
 * number, so a single expected value only matches the revision this SDK
 * release was cut against.
 */
static const unsigned long akcpu_known_ids[] __initconst = {
	0x20120100,
};



#define IODESC_ENT(x) 							\
{												\
	.virtual = (unsigned long)AK_VA_##x,		\
	.pfn	 = __phys_to_pfn(AK_PA_##x),		\
	.length	 = AK_SZ_##x,						\
	.type	 = MT_DEVICE						\
}

static struct map_desc ak39_iodesc[] __initdata = {
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

	if (i == ARRAY_SIZE(akcpu_known_ids))
		panic("ANYKA CPU ID 0x%lx is not supported\n", regval);

	/* need to change asic freq is here, Because higher asic freq  was affected usb function,
	 * I don't know essential reason of the problem 
	 */
	aisc_freq_set();
}

