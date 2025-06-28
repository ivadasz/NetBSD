/* $NetBSD$ */

/*-
 * Copyright (c) 2025 Imre Vadasz <imrevdsz@gmail.com>
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
#include <sys/systm.h>

#include <dev/fdt/fdtvar.h>

#define PUSH_PULL	0
#define OPEN_DRAIN	1

static int stmp1pinctrl_match(device_t, cfdata_t, void *);
static void stmp1pinctrl_attach(device_t, device_t, void *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "st,stm32mp157-pinctrl" },
	DEVICE_COMPAT_EOL
};

struct stmp1pinctrl_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
};

static void
configure_af_pin(struct stmp1pinctrl_softc *sc, u_int id, u_int line, u_int af,
    u_int speed, u_int output_type)
{
	bus_addr_t base = 0x1000*id;
	uint32_t mask = __BITS(2*line, 2*line+1);
	uint32_t af_off = line >= 8 ? 4 : 0;
	uint32_t af_mask = __BITS((line % 8)*4, (line % 8)*4+3);
	uint32_t val;

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, base+0x20+af_off);
	val &= ~af_mask;
	val |= __SHIFTIN(af, af_mask); // Alternate Function $af for line
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, base+0x20+af_off, val);
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, base+0x04);
	if (output_type == OPEN_DRAIN)
		val |= __BIT(line); // Open-Drain mode for line
	else
		val &= ~__BIT(line); // Push-Pull mode for line
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, base+0x04, val);
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, base+0x08);
	val &= ~mask;
	val |= __SHIFTIN(speed, mask); // Set Speed for line
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, base+0x08, val);
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, base+0x0C);
	val &= ~mask; // No Pull-up or Pull-down for line
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, base+0x0C, val);
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, base+0x00);
	val &= ~mask;
	val |= __SHIFTIN(0x2, mask); // Alternate Function mode for line
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, base+0x00, val);
}

static void
configure_sdmmc1(struct stmp1pinctrl_softc *sc)
{
	configure_af_pin(sc, 2, 8, 12, 1, PUSH_PULL);
	configure_af_pin(sc, 2, 9, 12, 1, PUSH_PULL);
	configure_af_pin(sc, 2, 10, 12, 1, PUSH_PULL);
	configure_af_pin(sc, 2, 11, 12, 1, PUSH_PULL);

	// Command line
	configure_af_pin(sc, 3, 2, 12, 1, PUSH_PULL);

	// Clock line
	configure_af_pin(sc, 2, 12, 12, 2, PUSH_PULL);
}

static void
configure_i2c1(struct stmp1pinctrl_softc *sc)
{
	// Set for STM32_PINMUX('F', 14, AF5) bias-disable, drive-open-drain, Low Speed
	configure_af_pin(sc, 5, 14, 5, 0, OPEN_DRAIN);

	// Set for STM32_PINMUX('B', 9, AF4) bias-disable, drive-open-drain, Low Speed
	configure_af_pin(sc, 1, 9, 4, 0, OPEN_DRAIN);
}

static void
configure_ltdc(struct stmp1pinctrl_softc *sc)
{
	configure_af_pin(sc, 8, 14, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 8, 12, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 8, 13, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 10, 7, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 8, 15, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 0, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 1, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 2, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 3, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 4, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 5, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 6, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 7, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 8, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 9, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 10, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 11, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 10, 0, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 10, 1, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 10, 2, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 12, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 13, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 14, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 9, 15, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 10, 3, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 10, 4, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 10, 5, 14, 1, PUSH_PULL);
	configure_af_pin(sc, 10, 6, 14, 1, PUSH_PULL);
}

static void
reset_it66121(struct stmp1pinctrl_softc *sc)
{
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x5018, __BIT(3));
	delay(1000);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x5018, __BIT(19));
}

CFATTACH_DECL_NEW(stmp1pinctrl, sizeof(struct stmp1pinctrl_softc),
	stmp1pinctrl_match, stmp1pinctrl_attach, NULL, NULL);

static int
stmp1pinctrl_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
stmp1pinctrl_attach(device_t parent, device_t self, void *aux)
{
	struct stmp1pinctrl_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	//const int phandle = faa->faa_phandle;
	bus_addr_t addr;
	bus_size_t size;

	sc->sc_dev = self;
	sc->sc_bst = faa->faa_bst;

// TODO: Need to solve this correctly
//	if (fdtbus_get_reg(phandle, 0, &addr, &size) != 0) {
//		aprint_error("%s: Failed to get registers address\n",
//		    device_xname(self));
//		return;
//	}
	addr = 0x50002000;
	size = 0xA400;
	if (bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_bsh) != 0) {
		aprint_error("%s: Failed to map registers\n",
		    device_xname(self));
		return;
	}

	aprint_naive("\n");
	aprint_normal(": STM32MP1 Pinctrl A-K\n");

	configure_i2c1(sc);
	configure_sdmmc1(sc);
	configure_ltdc(sc);
	reset_it66121(sc);
}
