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
#include <sys/proc.h>

#include <dev/sysmon/sysmonvar.h>

#include <dev/fdt/fdtvar.h>

static int stmp1dts_match(device_t, cfdata_t, void *);
static void stmp1dts_attach(device_t, device_t, void *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "st,stm32-thermal" },
	DEVICE_COMPAT_EOL
};

struct stmp1dts_softc {
	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	struct clk		*sc_clk;

	u_int			ts1_ramp_coeff;
	u_int			ts1_smp_time;
	u_int			ref_clk;
	u_int			ts1_fmt0;
	u_int			ts1_t0;

	struct sysmon_envsys	*sc_sme;
	envsys_data_t		sc_sensor;
};

// Returns temperature in milliCelsius
static int
stmp1dts_compute(struct stmp1dts_softc *sc, u_int ts1_mfreq)
{
	uint32_t val;

	//aprint_normal_dev(sc->sc_dev, "ts1_mfreq: %d\n", ts1_mfreq);

	val = sc->ref_clk * sc->ts1_smp_time;
	val /= ts1_mfreq;
	val -= 100 * sc->ts1_fmt0;
	val = (val * 1000) / sc->ts1_ramp_coeff;
	return val + sc->ts1_t0 * 1000;
}

static void
stmp1dts_sensor_refresh(struct sysmon_envsys *sme, envsys_data_t *edata)
{
	struct stmp1dts_softc *sc = sme->sme_cookie;
	uint32_t val;

	clk_enable(sc->sc_clk);
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0, val | __BIT(4));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0, val);
	// delay with timer (i.e. yielding)
	// check for RDY flag
	int cnt = 0;
	while (!(bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x20) & __BIT(15))) {
		kpause("stmp1dts_refresh", false, 1, NULL);
		if (++cnt > 10)
			break;
	}
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x1c) & 0xffff;
	clk_disable(sc->sc_clk);
	//aprint_normal_dev(sc->sc_dev, "Got mfreq=%d after %d loops\n", val, cnt);

	val = stmp1dts_compute(sc, val);

	// update sensor
	sc->sc_sensor.state = ENVSYS_SVALID;
	sc->sc_sensor.value_cur = val * 1000 + 273150000;
}

CFATTACH_DECL_NEW(stmp1dts, sizeof(struct stmp1dts_softc),
	stmp1dts_match, stmp1dts_attach, NULL, NULL);

static int
stmp1dts_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
stmp1dts_attach(device_t parent, device_t self, void *aux)
{
	struct stmp1dts_softc * const sc = device_private(self);
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

	sc->sc_clk = fdtbus_clock_get(phandle, "pclk");
	if (sc->sc_clk == NULL) {
		aprint_error(": couldn't enable Peripheral\n");
		return;
	}
	clk_enable(sc->sc_clk);

	aprint_naive("\n");
	aprint_normal(": STM32MP1 Digital Temperature Sensor\n");

	uint32_t val;
	// Configure
	val = __BIT(0) | (0xf << 16) | (105 << 24);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0, val);

	sc->ts1_smp_time = 0xf;

	// Hardcode 104.438965MHz pclk3 frequency.
	sc->ref_clk = 104438965;

	// Read TS1_RAMP_COEFF value from DTS_RAMPVALR
	sc->ts1_ramp_coeff =
	    bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x10) & 0xffff;

	// Read TS1_FMT0 value from DTS_T0VALR1
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x08);
	sc->ts1_fmt0 = val & 0xffff;
	if (val & __BIT(16))
		sc->ts1_t0 = 130;
	else
		sc->ts1_t0 = 30;

	clk_disable(sc->sc_clk);

	aprint_normal_dev(self, "ts1_ramp_coeff: %d, ts1_fmt0: %d\n",
	    sc->ts1_ramp_coeff, sc->ts1_fmt0);

	sc->sc_sme = sysmon_envsys_create();
	sc->sc_sme->sme_name = device_xname(self);
	sc->sc_sme->sme_cookie = sc;
	sc->sc_sme->sme_refresh = stmp1dts_sensor_refresh;

	sc->sc_sensor.units = ENVSYS_STEMP;
	sc->sc_sensor.state = ENVSYS_SINVALID;
	sc->sc_sensor.value_cur = 0;
	snprintf(sc->sc_sensor.desc, sizeof(sc->sc_sensor.desc),
	    "SoC Temperature");
	sysmon_envsys_sensor_attach(sc->sc_sme, &sc->sc_sensor);

	sysmon_envsys_register(sc->sc_sme);
}
