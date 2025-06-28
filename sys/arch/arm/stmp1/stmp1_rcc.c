/* $NetBSD$ */

/*-
 * Copyright (c) 2025 Imre Vadasz <imrevdsz@gmail.com>
 * Copyright (c) 2018 Jared McNeill <jmcneill@invisible.ca>
 * All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/cpu.h>
#include <sys/device.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/kmem.h>
#include <sys/gpio.h>

#include <dev/clk/clk_backend.h>
#include <dev/fdt/fdtvar.h>
#include <dev/fdt/syscon.h>

#define RCC_MPCKSELR	0x020
#define RCC_MPUSRC	__BITS(0,1)
#define RCC_MPUSRCRDY	__BIT(31)

#define RCC_MPCKDIVR	0x02C	// RCC MPU clock divider register
#define RCC_MPUDIV	__BITS(0,2)

#define RCC_RCK12SELR	0x028
#define RCC_PLL12SRC	__BITS(0,1)

#define RCC_PLL1CFGR1	0x084
#define RCC_DIVM1	__BITS(16,21)
#define RCC_DIVN	__BITS(0,8)

#define RCC_PLL1CFGR2	0x088

#define RCC_PLL1FRACR	0x08C

#define RCC_PLL4SRC	0x824
#define RCC_PLL4CR	0x894
#define RCC_PLL4CFGR1	0x898
#define RCC_PLL4CFGR2	0x89C
#define RCC_PLL4FRACR	0x8A0

#define RCC_SDMMC12CKSELR	0x8F4
#define RCC_SDMMC12SRC	__BITS(0,2)

#define	RESET_REG(index)	(((index) / 32) * 4)
#define	RESET_MASK(index)	__BIT((index) % 32)

#define	RESET_READ(sc, reg)		\
	bus_space_read_4((sc)->sc_bst, (sc)->sc_bsh, (reg))
#define	RESET_WRITE(sc, reg, val)	\
	bus_space_write_4((sc)->sc_bst, (sc)->sc_bsh, (reg), (val))

struct stmp1rcc_clk {
	struct clk base;
	u_int rate;
};

struct stmp1rcc_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	kmutex_t		sc_lock;
	struct clk_domain	sc_clkdom;

	struct stmp1rcc_clk	mpuclk;

	struct syscon		sc_syscon;
};

static int	stmp1rcc_match(device_t, cfdata_t, void *);
static void	stmp1rcc_attach(device_t, device_t, void *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "st,stm32mp1-rcc" },
	DEVICE_COMPAT_EOL
};

CFATTACH_DECL_NEW(stmp1rcc, sizeof(struct stmp1rcc_softc),
	stmp1rcc_match, stmp1rcc_attach, NULL, NULL);

static void
stmp1rcc_generic_lock(void *priv)
{
	struct stmp1rcc_softc * const sc = priv;

	mutex_enter(&sc->sc_lock);
}

static void
stmp1rcc_generic_unlock(void *priv)
{
	struct stmp1rcc_softc * const sc = priv;

	mutex_exit(&sc->sc_lock);
}

static uint32_t
stmp1rcc_generic_read_4(void *priv, bus_size_t reg)
{
	struct stmp1rcc_softc * const sc = priv;

	KASSERT(mutex_owned(&sc->sc_lock));

	aprint_normal("%s: reading reg: 0x%lx\n", device_xname(sc->sc_dev), reg);
	return bus_space_read_4(sc->sc_bst, sc->sc_bsh, reg);
}

static void
stmp1rcc_generic_write_4(void *priv, bus_size_t reg, uint32_t val)
{
	struct stmp1rcc_softc * const sc = priv;

	KASSERT(mutex_owned(&sc->sc_lock));

	aprint_normal("%s: writing reg: 0x%lx val: 0x%x\n", device_xname(sc->sc_dev), reg, val);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, reg, val);
}

static void *
stmp1rcc_resets_acquire(device_t dev, const void *data, size_t len)
{
	if (len != 4)
		return NULL;

	/* Specifier is an index. Just return it. */
	return (void *)(uintptr_t)be32dec(data);
}

static void
stmp1rcc_resets_release(device_t dev, void *priv)
{
}

static int
stmp1rcc_resets_assert(device_t dev, void *priv)
{
	struct stmp1rcc_softc * const sc = device_private(dev);
	const uintptr_t index = (uintptr_t)priv;

	const bus_size_t reset_reg = RESET_REG(index);
	const uint32_t reset_mask = RESET_MASK(index);

	RESET_WRITE(sc, reset_reg, reset_mask);

	return 0;
}

static int
stmp1rcc_resets_deassert(device_t dev, void *priv)
{
	struct stmp1rcc_softc * const sc = device_private(dev);
	const uintptr_t index = (uintptr_t)priv;

	const bus_size_t reset_reg = RESET_REG(index) + 4;
	const uint32_t reset_mask = RESET_MASK(index);

	RESET_WRITE(sc, reset_reg, reset_mask);

	return 0;
}

static const struct fdtbus_reset_controller_func stmp1rcc_fdtreset_funcs = {
	.acquire = stmp1rcc_resets_acquire,
	.release = stmp1rcc_resets_release,
	.reset_assert = stmp1rcc_resets_assert,
	.reset_deassert = stmp1rcc_resets_deassert,
};

static struct clk *
stmp1rcc_clk_get(void *priv, const char *name)
{
	struct stmp1rcc_softc * const sc = priv;

	aprint_normal_dev(sc->sc_dev, "Got asked for clock \"%s\"\n", name);

	if (strcmp(name, sc->mpuclk.base.name) != 0)
		return NULL;

	return &sc->mpuclk.base;
}

static void
stmp1rcc_clk_put(void *priv, struct clk *clk)
{
}

static u_int
stmp1rcc_clk_get_rate(void *priv, struct clk *clk)
{
	struct stmp1rcc_softc * const sc = priv;
	uint32_t val;

	if (clk == &sc->mpuclk.base) {
		// Get MPUSRC multiplexer
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_MPCKSELR);
		val = __SHIFTOUT(val, RCC_MPUSRC);
		if (val != 0x3) {
			aprint_normal_dev(sc->sc_dev, "MPUSRC is %d, and not 3 :(\n", val);
			// Can't deal with this yet
			return 0;
		}
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_MPCKDIVR);
		val = __SHIFTOUT(val, RCC_MPUDIV);
		if (val == 0)
			return 0;
		if (val >= 4)
			val = 4;
		val = 1 << val;
		return 800000000 / val;
	}

	return 0;
}

static int
stmp1rcc_clk_set_rate(void *priv, struct clk *clk, u_int rate)
{
	struct stmp1rcc_softc * const sc = priv;
	uint32_t val;
	uint32_t pll_ck = 800000000;
	uint32_t new_div, old_div;

	if (rate * 2 == pll_ck)
		new_div = 1;
	else if (rate * 4 == pll_ck)
		new_div = 2;
	else if (rate * 8 == pll_ck)
		new_div = 3;
	else if (rate * 16 == pll_ck)
		new_div = 4;
	else
		return EINVAL;

	if (clk == &sc->mpuclk.base) {
		// Get MPUSRC multiplexer
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_MPCKSELR);
		val = __SHIFTOUT(val, RCC_MPUSRC);
		if (val != 0x3) {
			// Can't deal with this yet
			return EINVAL;
		}

		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_MPCKDIVR);
		old_div = __SHIFTOUT(val, RCC_MPUDIV);
		if (old_div == new_div) {
			return 0;
		} else {
			val &= ~RCC_MPUDIV;
			val |= __SHIFTIN(new_div, RCC_MPUDIV);
			bus_space_write_4(sc->sc_bst, sc->sc_bsh, RCC_MPCKDIVR, val);
			return 0;
		}
	}

	return EINVAL;
}

static const struct clk_funcs stmp1rcc_clk_funcs = {
	.get = stmp1rcc_clk_get,
	.put = stmp1rcc_clk_put,
	.get_rate = stmp1rcc_clk_get_rate,
	.set_rate = stmp1rcc_clk_set_rate,
};

static struct clk *
stmp1rcc_mpuclk_decode(device_t dev, int cc_phandle, const void *data, size_t len)
{
	struct stmp1rcc_softc * const sc = device_private(dev);

	if (len == 4) {
		uint32_t val = *(const uint32_t *)data;
		aprint_normal_dev(sc->sc_dev, "Got asked to decode clock %d\n", val);
		return NULL;
	}

	if (len != 0)
		return NULL;


	return &sc->mpuclk.base;
}

static const struct fdtbus_clock_controller_func stmp1rcc_mpuclk_fdt_funcs = {
	.decode = stmp1rcc_mpuclk_decode
};

static int
stmp1rcc_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
program_pll4(struct stmp1rcc_softc *sc, uint64_t clk)
{
	uint32_t val;

	// LTDC Pixel Clock Programming:
	// Reference Clock: 24MHz (hse)
	// Prescaler Divider: 4
	uint64_t refclk = 6000000;

	// Fix the multiplicator to give a value between 400MHz and 800MHz.
	u_int64_t mult = 120;

	// Now we need to find the divisor to produce pll4_q_ck from VCO/div.
	// This method gives a quite reasonable result already.
	uint64_t a = (mult*refclk*8192)/clk;
	u_int div = a / 8192;
	u_int frac = a % 8192;

	uint64_t result = (mult*refclk*8192)/(div*8192+frac);
	device_printf(sc->sc_dev,
	    "Selecting mult=%llu frac=%u div=%u for clk=%llu\n",
	    mult, frac, div, clk);
	device_printf(sc->sc_dev, "Resulting Pixel Clock=%llu\n", result);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4CFGR1);
	val &= ~__BITS(0,8);
	// Set multiplication factor
	// This intermediate frequency needs to be between 400MHz and 800MHz.
	val |= __SHIFTIN(mult-1, __BITS(0,8));
	// We should set IFRGE to x0
	val &= ~__BIT(24);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4CFGR1, val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4CFGR2);
	val &= ~__BITS(8,14);
	// Set divider
	val |= __SHIFTIN(div-1, __BITS(8,14));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4CFGR2, val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4FRACR);
	val &= ~__BIT(16);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4FRACR, val);
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4FRACR);
	val &= ~__BITS(3,15);
	val |= __SHIFTIN(frac, __BITS(3,15));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4FRACR, val);
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4FRACR);
	val |= __BIT(16);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4FRACR, val);

	// Wait for PLL4 clock ready flag
	while (1) {
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4CR);
		if (val & __BIT(1))
			break;
	}
}

static void
stmp1rcc_attach(device_t parent, device_t self, void *aux)
{
	struct stmp1rcc_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	bus_addr_t addr;
	bus_size_t size;
	int child;

	if (fdtbus_get_reg(phandle, 0, &addr, &size) != 0) {
		aprint_error(": couldn't get registers\n");
		return;
	}

	sc->sc_dev = self;
	sc->sc_bst = faa->faa_bst;
	if (bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_bsh) != 0) {
		aprint_error(": couldn't map registers\n");
		return;
	}
	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_VM);
	sc->sc_syscon.priv = sc;
	sc->sc_syscon.lock = stmp1rcc_generic_lock;
	sc->sc_syscon.unlock = stmp1rcc_generic_unlock;
	sc->sc_syscon.read_4 = stmp1rcc_generic_read_4;
	sc->sc_syscon.write_4 = stmp1rcc_generic_write_4;

	aprint_naive("\n");
	aprint_normal(": STM32MP1 RCC\n");

	aprint_normal("%s: RCC USB kernel clock selection register: 0x%x\n",
	    device_xname(self), bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x91C));
	aprint_normal("%s: RCC APB4 peripheral enable for MPU set register: 0x%x\n",
	    device_xname(self), bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x200));
	// Enable USB Phy
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x200, __BIT(16));
	aprint_normal("%s: RCC APB4 peripheral enable for MPU clear register: 0x%x\n",
	    device_xname(self), bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x204));

	aprint_normal("%s: RCC AHB6 peripheral enable for MUP set register: 0x%x\n",
	    device_xname(self), bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x218));
	// Disable SDMMC2 on RCC AHB6
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x21C, __BIT(17));

	// Enable USB Host controller
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x218, __BIT(24));

	// Enable DTS Temperature Sensor clocks
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0xA10, __BIT(16));

#if 0
	// Enable GPIOB Peripheral Clock
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0xA28, __BIT(1));

	// Enable GPIOF Peripheral Clock
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0xA28, __BIT(5));

	// Enable GPIOZ Peripheral Clock
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x210, __BIT(0));
#else
	// Enable GPIOA-K Peripheral Clocks
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0xA28, __BITS(0,10));
#endif

	// Enable I2C1
	//bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0xA00, __BIT(21));

	// Enable I2C4
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x208, __BIT(2));

	// Enable LTDC
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x200, __BIT(0));

	uint32_t val, val2;
	// hsi clock is 64MHz, hse clock is 24MHz, csi clock is 4MHz
	// PLL3SRC mux selects hsi, hse or csi clock
	// DIVM3 divider produces ref3 clock
	// PLL3 DIVP divider produces pll3_p clock
	// pll3_p clock is selected for mcuss clock by MCUSSRC
	// MCUDIV divider produces mlhck clock
	// APB3DIV divider produces pclk 1/2/3
	// Read PLL3SRC multiplexer
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x820);
	val &= 0x3;
	aprint_normal_dev(self, "PLL3 clock selection: %d\n", val);
	// Read DIVM3 divider.
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x884);
	val &= 0x3f0000;
	val >>= 16;
	aprint_normal_dev(self, "PLL3 prescaler: %d\n", val+1);
	// Read PLL3 VCO multiplier. First Multiplication factor, then fractional part
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x884);
	val &= 0x1ff;
	val += 1;
	val2 = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x88C);
	val2 &= 0xfff8;
	val2 >>= 3;
	aprint_normal_dev(self, "VCO multiplier: %d.%03d\n", val, 1000*val2/8192);
	// Read RCC_PLL3CFGR2
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x888);
	val &= 0x7f;
	aprint_normal_dev(self, "Divider from vco3 to pll3_p: %d\n", val+1);
	// Read RCC_MSSCKSELR
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x048);
	aprint_normal_dev(self, "Selector for mcuss clock: %d\n", val & 0x3);
	// Read RCC_MCUDIVR
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x830);
	val &= 0x7;
	if (val > 4)
		val = 4;
	val = (1 << val);
	aprint_normal_dev(self, "Divider from mcuss to mlhck: %d\n", val);
	// Read RCC_APB3DIVR
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x83C);
	val &= 0x7;
	if (val > 4)
		val = 4;
	val = (1 << val);
	aprint_normal_dev(self, "Divider from mlhck to pclk3: %d\n", val);

	// Read RCC_I2C46CKSELR
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0xC0);
	aprint_normal_dev(self, "Selector for I2C46 clock: %d\n", val & 0x3);

	// Read RCC_SDMMC12CKSELR
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_SDMMC12CKSELR);
	aprint_normal_dev(self, "Selector for SDMMC1/2 clock: %llu\n", __SHIFTOUT(val,RCC_SDMMC12SRC));
	if (__SHIFTOUT(val,RCC_SDMMC12SRC) != 3) {
		aprint_normal_dev(self, "Changing SDMMC1/2 clock source back to default\n");
		val &= ~RCC_SDMMC12SRC;
		val |= __SHIFTIN(3,RCC_SDMMC12SRC);
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, RCC_SDMMC12CKSELR, val);
	}

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_RCK12SELR);
	aprint_normal_dev(self, "Selector for PLL1/2 Reference Clock: 0x%x\n", val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL1CFGR1);
	aprint_normal_dev(self, "PLL1 Configuration Register 1: 0x%x\n", val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL1CFGR2);
	aprint_normal_dev(self, "PLL1 Configuration Register 2: 0x%x\n", val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL1FRACR);
	aprint_normal_dev(self, "PLL1 Fractional Register: 0x%x\n", val);

	// PLL4 values (needed for LCD pixel clock)
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4SRC);
	aprint_normal_dev(self, "PLL4 Reference Source Register: 0x%x\n", val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4CR);
	aprint_normal_dev(self, "PLL4 Control Register: 0x%x\n", val);
	// Disable pll4_r_ck and pll4_p_ck
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4CR,
	    __BIT(5) | __BIT(0));

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4CFGR1);
	aprint_normal_dev(self, "PLL4 Configuration Register 1: 0x%x\n", val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4CFGR2);
	aprint_normal_dev(self, "PLL4 Configuration Register 2: 0x%x\n", val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_PLL4FRACR);
	aprint_normal_dev(self, "PLL4 Fractional Register: 0x%x\n", val);

	// XXX The Linux driver for IT66121 limits pixel clock to 74250000Hz.
	// 1024x768@60
	//program_pll4(sc, 65000000);
	// 1280x720@30
	program_pll4(sc, 33780000);

	// Make sure MPUSRC MUX is set to 3 (Using MPUDIV)
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_MPCKSELR);
	if (__SHIFTOUT(val, RCC_MPUSRC) == 2) {
		uint32_t w;
		aprint_normal_dev(self, "Changing MPU clock source to use equivalent MPUDIV\n");
		w = bus_space_read_4(sc->sc_bst, sc->sc_bsh, RCC_MPCKDIVR);
		if (__SHIFTOUT(w, RCC_MPUDIV) != 1) {
			w &= ~RCC_MPUDIV;
			w |= __SHIFTIN(1, RCC_MPUDIV);
			bus_space_write_4(sc->sc_bst, sc->sc_bsh, RCC_MPCKDIVR, w);
		}
		val &= ~RCC_MPUSRC;
		val |= __SHIFTIN(3, RCC_MPUSRC);
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, RCC_MPCKSELR, val);
	}

	fdtbus_register_syscon(self, phandle, &sc->sc_syscon);

	fdt_add_bus(self, phandle, faa);

	child = of_find_firstchild_byname(phandle, "clocks");
	if (child > 0)
		fdt_add_bus(self, child, faa);

	fdtbus_register_reset_controller(sc->sc_dev, phandle,
	    &stmp1rcc_fdtreset_funcs);

	sc->sc_clkdom.name = device_xname(self);
	sc->sc_clkdom.priv = sc;
	sc->sc_clkdom.funcs = &stmp1rcc_clk_funcs;

	sc->mpuclk.base.name = "mpuclk";
	sc->mpuclk.base.domain = &sc->sc_clkdom;
	clk_attach(&sc->mpuclk.base);

	fdtbus_register_clock_controller_byname(self, phandle, &stmp1rcc_mpuclk_fdt_funcs, sc->mpuclk.base.name);
}
