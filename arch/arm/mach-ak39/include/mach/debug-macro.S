/*
 * Low-level debug output for AK39xx UART0.
 *
 * This UART does not have a data register.  A byte is transmitted by placing
 * it in the UART's L2 buffer and then telling the UART to send one byte from
 * there.  The sequence below is the one the decompressor already uses in
 * mach/uncompress.h, which is the only implementation proven on this SoC.
 *
 * Two register windows are therefore needed, 638 MiB apart:
 *
 *   0x20130000  UART0 registers      (+0x10000 reaches the L2 controller)
 *   0x48001000  UART0 L2 TX buffer
 *
 * They are mapped so the virtual gap equals the physical gap, which lets the
 * macros work unchanged before and after the MMU is enabled.
 *
 * The port is left as U-Boot and the decompressor configured it; nothing here
 * reprograms the baud rate.
 *
 * senduart uses r4/r5 as scratch and therefore needs a stack.  That is true
 * from __mmap_switched onwards, which covers every caller that matters.  The
 * stackless error paths in head.S are not supported.
 */

#define AK39_DEBUG_UART_PHYS	0x20130000
#define AK39_DEBUG_UART_VIRT	0xd0130000
#define AK39_DEBUG_L2_GAP	0x27ed1000	/* 0x48001000 - 0x20130000 */
#define AK39_DEBUG_L2CTRL_GAP	0x00010000	/* 0x20140000 - 0x20130000 */

#define AK39_UART_CONF1		0x00
#define AK39_UART_CONF2		0x04
#define AK39_L2_CONF2		0x8c

#define AK39_TX_BUF_CLR		(1 << 16)	/* L2 CONF2: UART0 TX buffer */
#define AK39_TX_STATUS_CLR	(1 << 28)	/* UART CONF1 */
#define AK39_TX_COUNT_ONE	(1 << 4)	/* UART CONF2: send 1 byte */
#define AK39_TX_COUNT_VALID	(1 << 16)	/* UART CONF2 */
#define AK39_TX_END		(1 << 19)	/* UART CONF2 */
#define AK39_TX_ENDING_OFFSET	60		/* buffer terminator slot */

		.macro	addruart, rp, rv, tmp
		ldr	\rp, =AK39_DEBUG_UART_PHYS
		ldr	\rv, =AK39_DEBUG_UART_VIRT
		.endm

		/*
		 * rd must survive: printascii inspects it after senduart to
		 * decide whether to follow a newline with a carriage return.
		 * Use r4/r5 for everything and leave rd and rx untouched.
		 */
		.macro	senduart, rd, rx
		stmfd	sp!, {r4, r5}
		add	r4, \rx, #AK39_DEBUG_L2CTRL_GAP

		ldr	r5, [r4, #AK39_L2_CONF2]	@ clear the TX buffer
		orr	r5, r5, #AK39_TX_BUF_CLR
		str	r5, [r4, #AK39_L2_CONF2]

		ldr	r4, =AK39_DEBUG_L2_GAP
		add	r4, \rx, r4			@ L2 TX buffer
		str	\rd, [r4]
		mov	r5, #0
		str	r5, [r4, #AK39_TX_ENDING_OFFSET]

		ldr	r5, [\rx, #AK39_UART_CONF1]	@ clear the TX count
		orr	r5, r5, #AK39_TX_STATUS_CLR
		str	r5, [\rx, #AK39_UART_CONF1]

		ldr	r5, [\rx, #AK39_UART_CONF2]	@ send one byte
		orr	r5, r5, #AK39_TX_COUNT_ONE
		orr	r5, r5, #AK39_TX_COUNT_VALID
		str	r5, [\rx, #AK39_UART_CONF2]
		ldmfd	sp!, {r4, r5}
		.endm

		.macro	waituart, rd, rx
		.endm

		.macro	busyuart, rd, rx
1001:		ldr	\rd, [\rx, #AK39_UART_CONF2]
		tst	\rd, #AK39_TX_END
		beq	1001b
		.endm
