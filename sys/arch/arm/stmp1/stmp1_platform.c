/*	$NetBSD$	*/

/*-
 * Copyright (c) 2025 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Imre Vadasz.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include "opt_arm_debug.h"
#include "opt_console.h"
#include "opt_evbarm_boardtype.h"
//#include "opt_stmp1.h"
#include "opt_kgdb.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/atomic.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/reboot.h>
#include <sys/termios.h>

#include <dev/cons.h>

#include <uvm/uvm_extern.h>

#include <arm/db_machdep.h>
#include <arm/undefined.h>
#include <arm/arm32/machdep.h>

#include <machine/autoconf.h>
#include <machine/bootconfig.h>

#include <arm/cortex/gtmr_var.h>
#include <arm/cortex/scu_reg.h>

#include <dev/fdt/fdtvar.h>
#include <arm/fdt/arm_fdtvar.h>

//#include <evbarm/fdt/machdep.h>
#include <evbarm/fdt/platform.h>

#define STMP1_L4_APB1_VBASE	KERNEL_IO_VBASE
#define STMP1_L4_APB1_PBASE	0x40000000
#define STMP1_L4_APB1_SIZE	0x00100000

#define STMP1_L4_AHB4_VBASE	(STMP1_L4_APB1_VBASE + STMP1_L4_APB1_SIZE)
#define STMP1_L4_AHB4_PBASE	0x50000000
#define STMP1_L4_AHB4_SIZE	0x00100000

#define STMP1_L4_APB4_VBASE	(STMP1_L4_AHB4_VBASE + STMP1_L4_AHB4_SIZE)
#define STMP1_L4_APB4_PBASE	0x5A000000
#define STMP1_L4_APB4_SIZE	0x00100000

#define STMP1_L4_GIC_VBASE	(STMP1_L4_APB4_VBASE + STMP1_L4_APB4_SIZE)
#define STMP1_L4_GIC_PBASE	0xA0000000
#define STMP1_L4_GIC_SIZE	0x00100000

#define STMP1_RCC_BASE	0x50000000
#define RCC_MPSYSRST	(STMP1_RCC_BASE + 0x114)
#define RCC_MPSYSRST_RST	__BIT(0)

#define STMP1_DDRCTRL_BASE	0x5A003000
#define DDRCTRL_PWRCTL	(STMP1_DDRCTRL_BASE + 0x030)
#define DDRCTRL_EN_DFI_DRAM_CLK_DISABLE	__BIT(3)
#define DDRCTRL_POWERDOWN_EN	__BIT(1)
#define DDRCTRL_SELFREF_EN	__BIT(0)
#define DDRCTRL_PWRTMG	(STMP1_DDRCTRL_BASE + 0x034)

extern struct arm32_bus_dma_tag arm_generic_dma_tag;
extern struct bus_space arm_generic_bs_tag;

void stmp1_platform_early_putchar(char);

static inline vaddr_t
stmp1_phystovirt(paddr_t pa)
{
	if (pa >= STMP1_L4_APB1_PBASE &&
	    pa < STMP1_L4_APB1_PBASE + STMP1_L4_APB1_SIZE)
		return (pa - STMP1_L4_APB1_PBASE) + STMP1_L4_APB1_VBASE;

	if (pa >= STMP1_L4_AHB4_PBASE &&
	    pa < STMP1_L4_AHB4_PBASE + STMP1_L4_AHB4_SIZE)
		return (pa - STMP1_L4_AHB4_PBASE) + STMP1_L4_AHB4_VBASE;

	if (pa >= STMP1_L4_APB4_PBASE &&
	    pa < STMP1_L4_APB4_PBASE + STMP1_L4_APB4_SIZE)
		return (pa - STMP1_L4_APB4_PBASE) + STMP1_L4_APB4_VBASE;

//	if (pa >= STMP1_L4_GIC_PBASE &&
//	    pa < STMP1_L4_GIC_PBASE + STMP1_L4_GIC_SIZE)
//		return (pa - STMP1_L4_GIC_PBASE) + STMP1_L4_GIC_VBASE;

	panic("%s: pa %#x not in devmap", __func__, (uint32_t)pa);
}

void __noasan
stmp1_platform_early_putchar(char c) {
#ifdef CONSADDR
	volatile uint32_t *uartaddr = cpu_earlydevice_va_p() ?
	    (volatile uint32_t *)stmp1_phystovirt(CONSADDR):
	    (volatile uint32_t *)CONSADDR;

	while (!(le32toh(uartaddr[7]) & (1 << 7)))
		continue;
	uartaddr[10] = htole32(c);
#endif
}

static const struct pmap_devmap *
stmp1_platform_devmap(void)
{
	static const struct pmap_devmap devmap[] = {
		DEVMAP_ENTRY(STMP1_L4_APB1_VBASE,
			     STMP1_L4_APB1_PBASE,
			     STMP1_L4_APB1_SIZE),
		DEVMAP_ENTRY(STMP1_L4_AHB4_VBASE,
			     STMP1_L4_AHB4_PBASE,
			     STMP1_L4_AHB4_SIZE),
		DEVMAP_ENTRY(STMP1_L4_APB4_VBASE,
			     STMP1_L4_APB4_PBASE,
			     STMP1_L4_APB4_SIZE),
	//	DEVMAP_ENTRY(STMP1_L4_GIC_VBASE,
	//		     STMP1_L4_GIC_PBASE,
	//		     STMP1_L4_GIC_SIZE),
		DEVMAP_ENTRY_END
	};

	return devmap;
}

static void
stmp1_platform_init_attach_args(struct fdt_attach_args *faa)
{
	faa->faa_bst = &arm_generic_bs_tag;
	faa->faa_dmat = &arm_generic_dma_tag;
}

static void
stmp1_platform_device_register(device_t self, void *aux)
{
}

static u_int
stmp1_platform_uart_freq(void)
{
	return 4000000U;
}

static void
stmp1_platform_reset(void)
{
	volatile uint32_t *rstctrl =
	    (volatile uint32_t *)stmp1_phystovirt(RCC_MPSYSRST);

	*rstctrl |= RCC_MPSYSRST_RST;
	for (;;)
		__asm("wfi");
}

static void
stmp1_platform_bootstrap(void)
{
        arm_fdt_cpu_bootstrap();

	volatile uint32_t *ddrctrl;

	ddrctrl = (volatile uint32_t *)stmp1_phystovirt(DDRCTRL_PWRTMG);
	printf("%s: DDRCTRL_PWRTMG=0x%08x\n", __func__, *ddrctrl);
	ddrctrl = (volatile uint32_t *)stmp1_phystovirt(DDRCTRL_PWRCTL);
	printf("%s: DDRCTRL_PWRCTL=0x%08x\n", __func__, *ddrctrl);
	//*ddrctrl |= DDRCTRL_EN_DFI_DRAM_CLK_DISABLE | DDRCTRL_SELFREF_EN;
	//*ddrctrl |= DDRCTRL_SELFREF_EN;
	//*ddrctrl |= DDRCTRL_POWERDOWN_EN;
}

static const struct arm_platform stmp1_platform = {
	.ap_devmap = stmp1_platform_devmap,
	.ap_bootstrap = stmp1_platform_bootstrap,
	.ap_init_attach_args = stmp1_platform_init_attach_args,
	.ap_device_register = stmp1_platform_device_register,
	.ap_reset = stmp1_platform_reset,
	.ap_delay = gtmr_delay,
	.ap_uart_freq = stmp1_platform_uart_freq,
	.ap_mpstart = arm_fdt_cpu_mpstart,
};

ARM_PLATFORM(stmp1, "st,stm32mp153", &stmp1_platform);
