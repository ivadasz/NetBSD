/* $NetBSD$ */

/*-
 * Copyright (c) 2025 Imre Vadasz <imrevdsz@gmail.com>
 * Copyright (c) 2019 Jared McNeill <jmcneill@invisible.ca>
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

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/systm.h>
#include <sys/time.h>

#include <dev/fdt/fdtvar.h>

/* PHY control registers */
#define	USBPHYC_PLL	0
#define USBPHYC_PLLEN	__BIT(26)

#define USBPHYC_MISC		8
#define USBPHYC_SWITHOST	__BIT(0)	// 2nd port: 0: OTG, 1: Host

static int stmp1usbphy_match(device_t, cfdata_t, void *);
static void stmp1usbphy_attach(device_t, device_t, void *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "st,stm32mp1-usbphyc" },
	DEVICE_COMPAT_EOL
};

struct stmp1usbphy_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;

	struct clk		*sc_clk;
	struct fdtbus_regulator *sc_reg;
	struct fdtbus_reset	*sc_rst;
	struct fdtbus_gpio_pin	*sc_pin_vbus_det;
};

CFATTACH_DECL_NEW(stmp1usbphy, sizeof(struct stmp1usbphy_softc),
	stmp1usbphy_match, stmp1usbphy_attach, NULL, NULL);

static void *
stmp1usbphy_acquire(device_t dev, const void *data, size_t len)
{
	struct stmp1usbphy_softc * const sc = device_private(dev);

	aprint_normal("%s: acquiring len=0x%lx\n", device_xname(dev), len);

	return sc;
}

static void
stmp1usbphy_release(device_t dev, void *priv)
{
	aprint_normal("%s: releasing\n", device_xname(dev));
}

static int
stmp1usbphy_enable(device_t dev, void *priv, bool enable)
{
	struct stmp1usbphy_softc * const sc = device_private(dev);
	uint32_t val;
	int error;

	aprint_normal("%s: enabling\n", device_xname(dev));

	if (enable) {
		if (sc->sc_reg != NULL) {
			aprint_normal("%s: enabling regulator\n", device_xname(dev));
			error = fdtbus_regulator_enable(sc->sc_reg);
			if (error != 0)
				return error;
		}
		if (sc->sc_clk != NULL) {
			aprint_normal("%s: enabling clock\n", device_xname(dev));
			error = clk_enable(sc->sc_clk);
			if (error != 0)
				return error;
		}
		///* Apply Reset */
		//if (sc->sc_rst != NULL) {
		//	aprint_normal("%s Applying reset\n", device_xname(dev));
		//	error = fdtbus_reset_assert(sc->sc_rst);
		//	if (error != 0) {
		//		aprint_error(": couldn't assert reset\n");
		//		return error;
		//	}
		//	delay(20000);
		//	error = fdtbus_reset_deassert(sc->sc_rst);
		//	if (error != 0) {
		//		aprint_error(": couldn't de-assert reset\n");
		//		return error;
		//	}
		//}
		aprint_normal("%s: setting SWITHOST bit\n", device_xname(dev));
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, USBPHYC_MISC);
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, USBPHYC_MISC,
		    val | USBPHYC_SWITHOST);
		aprint_normal("%s: setting PLLEN bit\n", device_xname(dev));
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, USBPHYC_PLL);
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, USBPHYC_PLL,
		    val | USBPHYC_PLLEN);
		delay(100);
	} else {
		//if (sc->sc_pin_reset != NULL)
		//	fdtbus_gpio_write(sc->sc_pin_reset, 1);
		if (sc->sc_reg != NULL)
			fdtbus_regulator_disable(sc->sc_reg);
		if (sc->sc_clk != NULL)
			clk_disable(sc->sc_clk);
	}

	return 0;
}

const struct fdtbus_phy_controller_func stmp1usbphy_funcs = {
	.acquire = stmp1usbphy_acquire,
	.release = stmp1usbphy_release,
	.enable = stmp1usbphy_enable,
};

static int
stmp1usbphy_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
stmp1usbphy_attach(device_t parent, device_t self, void *aux)
{
	struct stmp1usbphy_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	bus_addr_t addr;
	bus_size_t size;
	int child;

	sc->sc_dev = self;
	sc->sc_bst = faa->faa_bst;

	if (fdtbus_get_reg(phandle, 0, &addr, &size) != 0) {
		aprint_error("%s: Failed to get registers address\n",
		    device_xname(self));
		return;
	}
	if (bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_bsh) != 0) {
		aprint_error("%s: Failed to map registers\n",
		    device_xname(self));
		return;
	}

	sc->sc_reg = fdtbus_regulator_acquire(phandle, "vbus-regulator");
	sc->sc_clk = fdtbus_clock_get(phandle, "main_clk");
	sc->sc_rst = fdtbus_reset_get_index(phandle, 0);

	aprint_naive("\n");
	aprint_normal(": USB PHY\n");

	// Register the Ports as well, which are listed as child devices.
	for (child = OF_child(phandle); child; child = OF_peer(child)) {
		if (!fdtbus_status_okay(child))
			continue;
		fdtbus_register_phy_controller(self, child, &stmp1usbphy_funcs);
	}
	fdtbus_register_phy_controller(self, phandle, &stmp1usbphy_funcs);
}
