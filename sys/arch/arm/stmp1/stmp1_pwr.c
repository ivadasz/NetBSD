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

static int stmp1pwr_match(device_t, cfdata_t, void *);
static void stmp1pwr_attach(device_t, device_t, void *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "st,stm32mp1,pwr-reg" },
	DEVICE_COMPAT_EOL
};

struct stmp1pwr_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
};

CFATTACH_DECL_NEW(stmp1pwr, sizeof(struct stmp1pwr_softc),
	stmp1pwr_match, stmp1pwr_attach, NULL, NULL);

static int
stmp1pwr_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
stmp1pwr_attach(device_t parent, device_t self, void *aux)
{
	struct stmp1pwr_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	bus_addr_t addr;
	bus_size_t size;

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

	aprint_naive("\n");
	aprint_normal(": STM32MP1 Power Regulator\n");

	uint32_t val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0xc);
	aprint_normal("%s: PWR control register 3: 0x%x\n", device_xname(self), val);
//	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0xc, val | __BIT(24));
//	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0xc);
//	aprint_normal("%s: PWR control register 3 after: 0x%x\n", device_xname(self), val);
}
