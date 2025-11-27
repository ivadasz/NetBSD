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

/*
 * Device driver for Intel's C-state residency counters.
 * Supports Nehalem and later Core CPUs.
 * ATOM CPUs have at least some support.
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

#define ATOM_MSR_PKG_CSTATE_COUNTER_HZ	(1000 * 1000)
#define ATOM_MSR_PKG_C2_RESIDENCY	MSR_PKG_C3_RESIDENCY
#define ATOM_MSR_PKG_C4_RESIDENCY	MSR_PKG_C6_RESIDENCY
#define ATOM_MSR_PKG_C6_RESIDENCY	MSR_PKG_C7_RESIDENCY

#define ATOM_MSR_CORE_C1_RESIDENCY	0x660
#define ATOM_MSR_CORE_C6_RESIDENCY	MSR_CORE_C6_RESIDENCY

static int	corecstat_match(device_t, cfdata_t, void *);
static void	corecstat_attach(device_t, device_t, void *);
static int	corecstat_detach(device_t, int);
static void	corecstat_refresh(struct sysmon_envsys *, envsys_data_t *);
static void	corecstat_refresh_xcall(void *, void *);

struct corecstat_sensor {
	uint64_t	tsc_count;
	uint64_t	cst_count;
	u_int		msr;
	u_int		bits;
	uint64_t	mask;
	envsys_data_t	sensor;
};

struct corecstat_softc {
	device_t		 sc_dev;
	struct cpu_info		*sc_ci;
	struct sysmon_envsys	*sc_sme;
	struct corecstat_sensor	 sc_pkg_sensors[7];
	int			 sc_pkg_nsensors;

	int			 sc_is_atom;
	/*
	 * sc_is_atom == 0
	 *   0 ==> Nehalem/Westmere
	 *   1 ==> Sandy Bridge / Ivy Bridge / Haswell / Broadwell / Skylake
	 *   2 ==> Haswell/Broadwell - low-power
	 *
	 * sc_is_atom == 1
	 *   0 ==> Original Atom
	 *   1 ==> Airmont Atom
	 */
	int			 sc_version;
};

static void	corecstat_sens_init(struct corecstat_softc *, const char *,
				    u_int, u_int);
static void	corecstat_sens_update(struct corecstat_softc *,
				      struct corecstat_sensor *);
static void	corecstat_init_atom_pkg(struct corecstat_softc *);
static void	corecstat_init_core_pkg(struct corecstat_softc *);

CFATTACH_DECL_NEW(corecstat, sizeof(struct corecstat_softc),
    corecstat_match, corecstat_attach, corecstat_detach, NULL);

static int
corecstat_match(device_t parent, cfdata_t cf, void *aux)
{
	struct cpufeature_attach_args *cfaa = aux;
	struct cpu_info *ci = cfaa->ci;
	int cpu_family, cpu_model;

	if (strcmp(cfaa->name, "cstat") != 0)
		return 0;

	if (cpu_vendor != CPUVENDOR_INTEL || cpuid_level < 0x06)
		return 0;

	/*
	 * Only attach on the first Package ID for now (only Package C-States).
	 */
	if (ci->ci_core_id != 0 || ci->ci_smt_id != 0)
		return 0;

	cpu_model = CPUID_TO_MODEL(ci->ci_signature);
	cpu_family = CPUID_TO_BASEFAMILY(ci->ci_signature);

	if (cpu_family == 0x06) {
		switch (cpu_model) {
		/* ATOM */
		case 0x1c:
		case 0x26:
		case 0x27:
		case 0x35:
		case 0x36:
		/* Silvermont Atom */
		case 0x37:
		case 0x4a:
		case 0x4c: /* Airmont Atom */
		case 0x4d:
		case 0x5a:
		case 0x5d:
		/* Nehalem */
		case 0x1a:
		case 0x1e:
		case 0x1f:
		case 0x25: /* Westmere */
		case 0x2c: /* Westmere */
		case 0x2e:
		case 0x2f: /* Westmere */
		/* Sandy Bridge*/
		case 0x2a:
		case 0x2d:
		case 0x3a: /* Ivy Bridge */
		case 0x3e: /* Ivy Bridge-E */
		/* Haswell */
		case 0x3c:
		case 0x3f: /* Haswell-E */
		case 0x45:
		case 0x46:
		/* Broadwell */
		case 0x3d:
		case 0x47:
		case 0x4f:
		case 0x56:
		/* Skylake */
		case 0x4e:
		case 0x5e:
		/* Kabylake/Coffeelake */
		case 0x8e:
		// TODO: Update list of CPU models
			break;
		default:
			return 0;
		}
	}

	return 1;
}

static void
corecstat_attach(device_t parent, device_t self, void *aux)
{
	struct corecstat_softc *sc = device_private(self);
	struct cpufeature_attach_args *cfaa = aux;
	struct cpu_info *ci = cfaa->ci;
	int cpu_family, cpu_model;

	sc->sc_ci = ci;
	sc->sc_dev = self;

	cpu_model = CPUID_TO_MODEL(ci->ci_signature);
	cpu_family = CPUID_TO_BASEFAMILY(ci->ci_signature);

	if (cpu_family == 0x06) {
		switch (cpu_model) {
		/* ATOM */
		case 0x1c:
		case 0x26:
		case 0x27:
		case 0x35:
		case 0x36:
			/* Only Package C-States available, no Core C-states */
			sc->sc_is_atom = 1;
			sc->sc_version = 0;
			break;
		/* Silvermont Atom */
		case 0x37:
		case 0x4a:
		case 0x4c: /* Airmont Atom */
		case 0x4d:
		case 0x5a:
		case 0x5d:
			sc->sc_is_atom = 1;
			sc->sc_version = 1;
			break;
		/* Nehalem */
		case 0x1a:
		case 0x1e:
		case 0x1f:
		case 0x25: /* Westmere */
		case 0x2c: /* Westmere */
		case 0x2e:
		case 0x2f: /* Westmere */
			/* PKG_C3, PKG_C6, PKG_C7 */
			/* CORE_C3, CORE_C6 */
			sc->sc_version = 0;
			break;
		/* Sandy Bridge*/
		case 0x2a:
		case 0x2d:
		case 0x3a: /* Ivy Bridge */
		case 0x3e: /* Ivy Bridge-E */
		/* Haswell */
		case 0x3c:
		case 0x3f: /* Haswell-E */
		case 0x45:
		case 0x46:
		/* Broadwell */
		case 0x3d:
		case 0x47:
		case 0x4f:
		case 0x56:
		/* Skylake */
		case 0x4e:
		case 0x5e:
			/* PKG_C2, PKG_C3, PKG_C6, PKG_C7 */
			/* CORE_C3, CORE_C6, CORE_C7 */
			sc->sc_version = 1;
			break;
		/* Kabylake/Coffeelake */
		case 0x8e:
			/* PKG_C2, PKG_C3, PKG_C6, PKG_C7, PKG_C8, PKG_C9, PKG_C10 */
			/* CORE_C3, CORE_C6, CORE_C7 */
			sc->sc_version = 2;
			break;
		// TODO: Update list of CPU models
			break;
		default:
			return;
		}
	}

	aprint_naive("\n");

	sc->sc_sme = sysmon_envsys_create();

	if (sc->sc_is_atom)
		corecstat_init_atom_pkg(sc);
	else
		corecstat_init_core_pkg(sc);

#if 0
	if (sc->sc_is_atom)
		corecstat_init_atom_cpu(sc);
	else
		corecstat_init_core_cpu(sc);
#endif

	if (sc->sc_pkg_nsensors == 0)
		goto fail;

	(void)pmf_device_register(self, NULL, NULL);

	sc->sc_sme->sme_cookie = sc;
	sc->sc_sme->sme_name = device_xname(self);
	sc->sc_sme->sme_refresh = corecstat_refresh;

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
corecstat_detach(device_t self, int flags)
{
	struct corecstat_softc *sc = device_private(self);

	if (sc->sc_sme != NULL)
		sysmon_envsys_unregister(sc->sc_sme);

	pmf_device_deregister(self);

	return 0;
}

static void
corecstat_init_atom_pkg(struct corecstat_softc *sc)
{
	if (sc->sc_version == 0) {
		corecstat_sens_init(sc, "PC2 residency",
		    ATOM_MSR_PKG_C2_RESIDENCY, 64);
		corecstat_sens_init(sc, "PC4 residency",
		    ATOM_MSR_PKG_C4_RESIDENCY, 64);
		corecstat_sens_init(sc, "PC6 residency",
		    ATOM_MSR_PKG_C6_RESIDENCY, 64);
	} else {
		corecstat_sens_init(sc, "PC6 residency",
		    ATOM_MSR_PKG_C6_RESIDENCY, 64);
	}
}

#if 0
static void
corecstat_init_atom_cpu(struct corecstat_softc *sc)
{
	/* No Core C-state residency counters on old Atoms */
	KKASSERT(sc->sc_version != 0);

	corecstat_sens_init(sc, "C1 residency", ATOM_MSR_CORE_C1_RESIDENCY, 64);
	corecstat_sens_init(sc, "C6 residency", ATOM_MSR_CORE_C6_RESIDENCY, 64);
}
#endif

static void
corecstat_init_core_pkg(struct corecstat_softc *sc)
{
	if (sc->sc_version == 1 || sc->sc_version == 2) {
		corecstat_sens_init(sc, "PC2 residency",
		    MSR_PKG_C2_RESIDENCY, 64);
	}

	corecstat_sens_init(sc, "PC3 residency", MSR_PKG_C3_RESIDENCY, 64);
	corecstat_sens_init(sc, "PC6 residency", MSR_PKG_C6_RESIDENCY, 64);
	corecstat_sens_init(sc, "PC7 residency", MSR_PKG_C7_RESIDENCY, 64);

	if (sc->sc_version == 2) {
		corecstat_sens_init(sc, "PC8 residency",
		    MSR_PKG_C8_RESIDENCY, 60);
		corecstat_sens_init(sc, "PC9 residency",
		    MSR_PKG_C9_RESIDENCY, 60);
		corecstat_sens_init(sc, "PC10 residency",
		    MSR_PKG_C10_RESIDENCY, 60);
	}
}

#if 0
static void
corecstat_init_core_cpu(struct corecstat_softc *sc)
{
	corecstat_sens_init(sc, "C3 residency", MSR_CORE_C3_RESIDENCY, 64);
	corecstat_sens_init(sc, "C6 residency", MSR_CORE_C6_RESIDENCY, 64);

	if (sc->sc_version == 1 || sc->sc_version == 2) {
		corecstat_sens_init(sc, "C7 residency",
		    MSR_CORE_C7_RESIDENCY, 64);
	}
}
#endif

static void
corecstat_sens_init(struct corecstat_softc *sc, const char *desc, u_int msr,
		    u_int bits)
{
	struct corecstat_sensor *sens =
	    &sc->sc_pkg_sensors[sc->sc_pkg_nsensors];
	uint64_t a, b;
	int error;

	KASSERT(bits > 0 && bits <= 64);
	(void)snprintf(sens->sensor.desc, sizeof(sens->sensor.desc),
	    "%s %s", device_xname(sc->sc_ci->ci_dev), desc);
	sens->sensor.units = ENVSYS_INTEGER;
	sens->sensor.state = ENVSYS_SINVALID;
	sens->sensor.flags = ENVSYS_FHAS_ENTROPY;
	//sens->sensor.flags = ENVSYS_FHAS_ENTROPY | ENVSYS_FPERCENT |
	//		     ENVSYS_FVALID_MAX;
	//sens->sensor.value_max = 1000*1000;
	sens->msr = msr;
	sens->bits = bits;
	sens->mask = __BITS(0, bits-1);
	a = rdtsc();
	b = rdmsr(msr);
	sens->tsc_count = a;
	sens->cst_count = b & sens->mask;
	error = sysmon_envsys_sensor_attach(sc->sc_sme, &sens->sensor);
	if (error) {
		aprint_error("Failed to attach %s sensor error=%d",
		    desc, error);
	} else {
		sc->sc_pkg_nsensors++;
	}
}

static void
corecstat_refresh(struct sysmon_envsys *sme, envsys_data_t *edata)
{
	struct corecstat_softc *sc = sme->sme_cookie;
	uint64_t xc;

	xc = xc_unicast(0, corecstat_refresh_xcall, sc, edata, sc->sc_ci);
	xc_wait(xc);
}

static void
corecstat_refresh_xcall(void *arg0, void *arg1)
{
	struct corecstat_softc *sc = arg0;
	envsys_data_t *edata = arg1;
	struct corecstat_sensor *sens = &sc->sc_pkg_sensors[edata->sensor];

	corecstat_sens_update(sc, sens);
}

static void
corecstat_sens_update(struct corecstat_softc *sc, struct corecstat_sensor *sens)
{
	uint64_t a, b, msr;
	uint64_t tscdiff, cstdiff;
	extern uint64_t tsc_freq;

	msr = sens->msr;
	a = rdtsc();
	b = rdmsr(msr);

	b &= sens->mask;

	if (a < sens->tsc_count)
		tscdiff = a + (~sens->tsc_count);
	else
		tscdiff = a - sens->tsc_count;

	if (b < sens->cst_count)
		cstdiff = b + ((~sens->cst_count) & sens->mask);
	else
		cstdiff = b - sens->cst_count;

	sens->tsc_count = a;
	sens->cst_count = b;

	/*
	 * On old Atom CPUs (before Silvermont) the Package-cstate counters
	 * run at 1 MHz.
	 */
	if (sc->sc_is_atom && sc->sc_version == 0) {
		if (cstdiff < (1ULL << 32)) {
			cstdiff *= tsc_freq;
			cstdiff /= ATOM_MSR_PKG_CSTATE_COUNTER_HZ;
		} else {
			cstdiff /= ATOM_MSR_PKG_CSTATE_COUNTER_HZ;
			cstdiff *= tsc_freq;
		}
	}

	if (tscdiff > 0) {
		/* Make sure we don't calculate total garbage here */
		if (cstdiff < (1ULL << 44)) {
			sens->sensor.value_cur = (cstdiff * 1000) / tscdiff;
			//sens->sensor.value_cur = (cstdiff * 1000000) / tscdiff;
			sens->sensor.state = ENVSYS_SVALID;
		} else {
			sens->sensor.state = ENVSYS_SINVALID;
		}
	} else {
		sens->sensor.state = ENVSYS_SINVALID;
	}
}

MODULE(MODULE_CLASS_DRIVER, corecstat, "sysmon_envsys");

#ifdef _MODULE
#include "ioconf.c"
#endif

static int
corecstat_modcmd(modcmd_t cmd, void *aux)
{
	int error = 0;

	switch (cmd) {
	case MODULE_CMD_INIT:
#ifdef _MODULE
		error = config_init_component(cfdriver_ioconf_corecstat,
		    cfattach_ioconf_corecstat, cfdata_ioconf_corecstat);
#endif
		return error;
	case MODULE_CMD_FINI:
#ifdef _MODULE
		error = config_fini_component(cfdriver_ioconf_corecstat,
		    cfattach_ioconf_corecstat, cfdata_ioconf_corecstat);
#endif
		return error;
	default:
		return ENOTTY;
	}
}
