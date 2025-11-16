/* $NetBSD$ */

/*-
 * Copyright (c) 2025 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Imre Vad<C3><A1>sz.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/device.h>
#include <sys/cpu.h>
#include <sys/module.h>
#include <sys/xcall.h>

#include <dev/sysmon/sysmonvar.h>

#include <machine/cpuvar.h>
#include <machine/cpufunc.h>
#include <machine/cputypes.h>
#include <machine/specialreg.h>

#define MSR_RAPL_POWER_UNIT_POWER	__BITS(0, 3)
#define MSR_RAPL_POWER_UNIT_ENERGY	__BITS(8, 12)
#define MSR_RAPL_POWER_UNIT_TIME	__BITS(16, 19)

static int	corepower_match(device_t, cfdata_t, void *);
static void	corepower_attach(device_t, device_t, void *);
static int	corepower_detach(device_t, int);
static void	corepower_refresh(struct sysmon_envsys *, envsys_data_t *);
static void	corepower_refresh_xcall(void *, void *);

struct corepower_sensor {
	uint64_t	energy;
	struct timeval	tv;
	uint		msr;
	envsys_data_t	sensor;
};

struct corepower_softc {
	device_t		 sc_dev;
	struct cpu_info		*sc_ci;
	struct sysmon_envsys	*sc_sme;
	struct corepower_sensor	 sc_sensors[5];
	int			 sc_nsensors;

	uint32_t		 sc_watt_unit;
	uint32_t		 sc_joule_unit;
	uint32_t		 sc_second_unit;

	int			 sc_have_sens;
	int			 sc_is_atom;
};

static uint32_t	corepower_energy_to_uwatts(struct corepower_softc *,
					   uint32_t, uint64_t);
static int	corepower_try(u_int, const char *);
static int	corepower_sens_init(struct corepower_softc *, const char *,
				    u_int);
static void	corepower_sens_update(struct corepower_softc *,
				      struct corepower_sensor *);

CFATTACH_DECL_NEW(corepower, sizeof(struct corepower_softc),
    corepower_match, corepower_attach, corepower_detach, NULL);

static int
corepower_match(device_t parent, cfdata_t cf, void *aux)
{
	struct cpufeature_attach_args *cfaa = aux;
	struct cpu_info *ci = cfaa->ci;
	int cpu_family, cpu_model;

	if (strcmp(cfaa->name, "power") != 0)
		return 0;

	if (cpu_vendor != CPUVENDOR_INTEL || cpuid_level < 0x06)
		return 0;

	/*
	 * Only attach on the first Package ID.
	 */
	if (ci->ci_core_id != 0)
		return 0;

	cpu_model = CPUID_TO_MODEL(ci->ci_signature);
	cpu_family = CPUID_TO_BASEFAMILY(ci->ci_signature);

	if (cpu_family == 0x06) {
		switch (cpu_model) {
		/* Core CPUs */
		case 0x2a:
		case 0x3a:
		/* Xeon CPUs */
		case 0x2d:
		case 0x3e:
		case 0x3f:
		case 0x4f:
		case 0x56:
		/* Haswell, Broadwell, Skylake, Kabylake */
		case 0x3c:
		case 0x3d:
		case 0x45:
		case 0x46:
		case 0x47:
		case 0x4e:
		case 0x5e:
		case 0x8e:	/* Kabylake */
		/* Atom CPUs */
		case 0x37:
		case 0x4a:
		case 0x4c:
		case 0x4d:
		case 0x5a:
		case 0x5d:
		// TODO: Update list of CPU models
			break;
		default:
			return 0;
		}
	}

	if (corepower_try(MSR_RAPL_POWER_UNIT, "MSR_RAPL_POWER_UNIT") == 0)
		return 0;

	return 1;
}

static void
corepower_attach(device_t parent, device_t self, void *aux)
{
	struct corepower_softc *sc = device_private(self);
	struct cpufeature_attach_args *cfaa = aux;
	struct cpu_info *ci = cfaa->ci;
	uint64_t val;
	uint32_t power_units, energy_units, time_units;
	int cpu_family, cpu_model;

	sc->sc_ci = ci;
	sc->sc_dev = self;

	cpu_model = CPUID_TO_MODEL(ci->ci_signature);
	cpu_family = CPUID_TO_BASEFAMILY(ci->ci_signature);

	if (cpu_family == 0x06) {
		switch (cpu_model) {
		/* Core CPUs */
		case 0x2a:
		case 0x3a:
			sc->sc_have_sens = 0xd;
			break;
		/* Xeon CPUs */
		case 0x2d: /* Only Xeon branded, Core i version should probably be 0x5 */
		case 0x3e:
		case 0x3f:
		case 0x4f:
		case 0x56:
			sc->sc_have_sens = 0x7;
			break;
		/* Haswell, Broadwell, Skylake, Kabylake */
		case 0x3c:
		case 0x3d:
		case 0x45:
		case 0x46:
		case 0x47:
			/* Check if Core or Xeon (Xeon CPUs might be 0x7) */
         		sc->sc_have_sens = 0xf;
			break;
		case 0x4e:
		case 0x5e:
		case 0x8e:	/* Kabylake */
			sc->sc_have_sens = 0x1f;
			break;
		/* Atom CPUs */
		case 0x37:
		case 0x4a:
		case 0x4c:
		case 0x4d:
		case 0x5a:
		case 0x5d:
			sc->sc_have_sens = 0x5;
			/* use quirk for Valleyview Atom CPUs */
			sc->sc_is_atom = 1;
			break;
		// TODO: Update list of CPU models
		default:
			aprint_normal("\n");
			return;
		}
	}

	val = rdmsr(MSR_RAPL_POWER_UNIT);

	power_units = __SHIFTOUT(val, MSR_RAPL_POWER_UNIT_POWER);
	energy_units = __SHIFTOUT(val, MSR_RAPL_POWER_UNIT_ENERGY);
	time_units = __SHIFTOUT(val, MSR_RAPL_POWER_UNIT_TIME);

	sc->sc_watt_unit = __BIT(power_units);
	sc->sc_joule_unit = __BIT(energy_units);
	sc->sc_second_unit = __BIT(time_units);

	aprint_naive("\n");

	sc->sc_sme = sysmon_envsys_create();

	if ((sc->sc_have_sens & 1) &&
	    corepower_try(MSR_PKG_ENERGY_STATUS, "MSR_PKG_ENERGY_STATUS")) {
		if (corepower_sens_init(sc, "Package Power",
		    MSR_PKG_ENERGY_STATUS) != 0)
			goto fail;
	} else {
		sc->sc_have_sens &= ~1;
	}
	if ((sc->sc_have_sens & 2) &&
	    corepower_try(MSR_DRAM_ENERGY_STATUS, "MSR_DRAM_ENERGY_STATUS")) {
		if (corepower_sens_init(sc, "DRAM Power",
		    MSR_DRAM_ENERGY_STATUS) != 0)
			goto fail;
	} else {
		sc->sc_have_sens &= ~2;
	}
	if ((sc->sc_have_sens & 4) &&
	    corepower_try(MSR_PP0_ENERGY_STATUS, "MSR_PP0_ENERGY_STATUS")) {
		if (corepower_sens_init(sc, "Cores Power",
		    MSR_PP0_ENERGY_STATUS) != 0)
			goto fail;
	} else {
		sc->sc_have_sens &= ~4;
	}
	if ((sc->sc_have_sens & 8) &&
	    corepower_try(MSR_PP1_ENERGY_STATUS, "MSR_PP1_ENERGY_STATUS")) {
		if (corepower_sens_init(sc, "Graphics Power",
		    MSR_PP1_ENERGY_STATUS) != 0)
			goto fail;
	} else {
		sc->sc_have_sens &= ~8;
	}
	if ((sc->sc_have_sens & 0x10) &&
	    corepower_try(MSR_PLATFORM_ENERGY_COUNTER, "MSR_PLATFORM_ENERGY_COUNTER") &&
	    (rdmsr(MSR_PLATFORM_ENERGY_COUNTER) & 0xffffffffU) != 0) {
		if (corepower_sens_init(sc, "Platform Power",
		    MSR_PLATFORM_ENERGY_COUNTER) != 0)
			goto fail;
	} else {
		sc->sc_have_sens &= ~0x10;
	}

	if (sc->sc_have_sens == 0)
		goto fail;

	(void)pmf_device_register(self, NULL, NULL);

	sc->sc_sme->sme_cookie = sc;
	sc->sc_sme->sme_name = device_xname(self);
	sc->sc_sme->sme_refresh = corepower_refresh;

	if (sysmon_envsys_register(sc->sc_sme) != 0)
		goto fail;

	aprint_normal("\n");
	return;

fail:
	sysmon_envsys_destroy(sc->sc_sme);
	sc->sc_sme = NULL;
	aprint_normal("\n");
}

static int
corepower_detach(device_t self, int flags)
{
	struct corepower_softc *sc = device_private(self);

	if (sc->sc_sme != NULL)
		sysmon_envsys_unregister(sc->sc_sme);

	pmf_device_deregister(self);

	return 0;
}

static uint32_t
corepower_energy_to_uwatts(struct corepower_softc *sc, uint32_t units,
    uint64_t usecs)
{
	uint64_t val;

	if (sc->sc_is_atom) {
		val = ((uint64_t)units) * sc->sc_joule_unit;
	} else {
		val = ((uint64_t)units) * 1000ULL * 1000ULL;
		val /= sc->sc_joule_unit;
	}

	return (val * 1000ULL * 1000ULL) / usecs;
}

static int
corepower_sens_init(struct corepower_softc *sc, const char *desc, u_int msr)
{
	struct corepower_sensor *sens = &sc->sc_sensors[sc->sc_nsensors];
	int error;

	(void)snprintf(sens->sensor.desc, sizeof(sens->sensor.desc),
	    "%s %s", device_xname(sc->sc_ci->ci_dev), desc);
	sens->sensor.units = ENVSYS_SWATTS;
	sens->sensor.state = ENVSYS_SINVALID;
	sens->sensor.flags = ENVSYS_FHAS_ENTROPY;
	sens->msr = msr;
	sens->energy = rdmsr(sens->msr) & 0xffffffffU;
	microuptime(&sens->tv);
	error = sysmon_envsys_sensor_attach(sc->sc_sme, &sens->sensor);
	if (!error)
		sc->sc_nsensors++;
	return error;
}

static int
corepower_try(u_int msr, const char *name)
{
	uint64_t val;

	if (rdmsr_safe(msr, &val) != 0) {
		aprint_normal("msr %s (0x%08x) not available\n", name, msr);
		return 0;
	}
	return 1;
}

static void
corepower_refresh(struct sysmon_envsys *sme, envsys_data_t *edata)
{
	struct corepower_softc *sc = sme->sme_cookie;
	uint64_t xc;

	xc = xc_unicast(0, corepower_refresh_xcall, sc, edata, sc->sc_ci);
	xc_wait(xc);
}

static void
corepower_refresh_xcall(void *arg0, void *arg1)
{
	struct corepower_softc *sc = arg0;
	envsys_data_t *edata = arg1;
	struct corepower_sensor *sens = &sc->sc_sensors[edata->sensor];

	corepower_sens_update(sc, sens);
}

static void
corepower_sens_update(struct corepower_softc *sc,
    struct corepower_sensor *sens)
{
	struct timeval tv, diff;
	uint64_t a, res;

	a = rdmsr(sens->msr) & 0xffffffffU;
	microuptime(&tv);
	if (sens->energy > a) {
		res = (0x100000000ULL - sens->energy) + a;
	} else {
		res = a - sens->energy;
	}
	sens->energy = a;
	timersub(&tv, &sens->tv, &diff);
	sens->sensor.value_cur = corepower_energy_to_uwatts(sc, res,
	    diff.tv_sec * 1000ULL * 1000ULL + diff.tv_usec);
	sens->sensor.state = ENVSYS_SVALID;
	sens->tv = tv;
}

MODULE(MODULE_CLASS_DRIVER, corepower, "sysmon_envsys");

#ifdef _MODULE
#include "ioconf.c"
#endif

static int
corepower_modcmd(modcmd_t cmd, void *aux)
{
	int error = 0;

	switch (cmd) {
	case MODULE_CMD_INIT:
#ifdef _MODULE
		error = config_init_component(cfdriver_ioconf_corepower,
		    cfattach_ioconf_corepower, cfdata_ioconf_corepower);
#endif
		return error;
	case MODULE_CMD_FINI:
#ifdef _MODULE
		error = config_fini_component(cfdriver_ioconf_corepower,
		    cfattach_ioconf_corepower, cfdata_ioconf_corepower);
#endif
		return error;
	default:
		return ENOTTY;
	}
}
