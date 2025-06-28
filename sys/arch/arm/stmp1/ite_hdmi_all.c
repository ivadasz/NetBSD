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

#include <dev/fdt/fdtvar.h>
#include <dev/i2c/i2cvar.h>
#include <dev/videomode/edidvar.h>

#define ITEHDMI_I2C_DEFAULT_ADDR	0x4C
#define ITEHDMI_I2C_ALTERNATIVE_ADDR	0x4D

static int itehdmi_match(device_t, cfdata_t, void *);
static void itehdmi_attach(device_t, device_t, void *);

#if 0
static const struct device_compatible_entry compat_data[] = {
	{ .compat = "ite,it66121" },
	DEVICE_COMPAT_EOL
};
#endif

struct itehdmi_softc {
	device_t	sc_dev;
	i2c_tag_t	sc_i2c;
	i2c_addr_t	sc_addr;
	int		sc_phandle;
};

static int
itehdmi_do(struct itehdmi_softc *sc, uint8_t reg, uint8_t *val, size_t len,
	   i2c_op_t op)
{
	int ret;

	ret = iic_acquire_bus(sc->sc_i2c, 0);
	if (ret == 0) {
		ret = iic_exec(sc->sc_i2c, op, sc->sc_addr,
		    &reg, 1, val, len, 0);
		iic_release_bus(sc->sc_i2c, 0);
	}

	return ret;
}

static int
itehdmi_read(struct itehdmi_softc *sc, uint8_t reg, uint8_t *val, size_t len)
{
	return itehdmi_do(sc, reg, val, len, I2C_OP_READ_WITH_STOP);
}

static int
itehdmi_read_reg(struct itehdmi_softc *sc, uint8_t reg, uint8_t *val)
{
	return itehdmi_read(sc, reg, val, 1);
}

static int
itehdmi_write(struct itehdmi_softc *sc, uint8_t reg, uint8_t *val, size_t len)
{
	return itehdmi_do(sc, reg, val, len, I2C_OP_WRITE_WITH_STOP);
}

static int
itehdmi_write_reg(struct itehdmi_softc *sc, uint8_t reg, uint8_t val)
{
	return itehdmi_write(sc, reg, &val, 1);
}

static int
itehdmi_update_reg(struct itehdmi_softc *sc, uint8_t reg, uint8_t mask,
    uint8_t set)
{
	int error;
	uint8_t val;

	error = itehdmi_read_reg(sc, reg, &val);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to read Reg 0x%x\n", reg);
		goto done;;
	}
	val &= ~mask;
	val |= set;
	error = itehdmi_write_reg(sc, reg, val);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to write Reg 0x%x\n", reg);
		goto done;;
	}

done:
	return error;
}

static int
itehdmi_identify(struct itehdmi_softc *sc, uint8_t *buf)
{
	int error;
	int i;

	for (i = 0; i < 4; i++) {
		error = itehdmi_read_reg(sc, i, &buf[i]);
		if (error)
			break;
	}

	return error;
}

#if 0
static int
itehdmi_poweron(struct itehdmi_softc *sc)
{
	int error;

	// Power on RCLK (to enable I2C write accesses)
	//error = itehdmi_update_reg(sc, 0x0F, __BITS(3,6), __SHIFTIN(7, __BITS(3,6)));
	error = itehdmi_update_reg(sc, 0x0F, __BIT(6), 0);
	if (error != 0)
		goto done;;

	// Do a reset
	//error = itehdmi_update_reg(sc, 0x04, __BIT(5), __BIT(5));
	//if (error != 0)
	//	goto done;;
	//delay(100);
	//error = itehdmi_update_reg(sc, 0x04, __BIT(5), 0);
	//if (error != 0)
	//	goto done;;

	// Power on RCLK (to enable I2C write accesses)
	//error = itehdmi_update_reg(sc, 0x0F, __BITS(3,6), __SHIFTIN(7, __BITS(3,6)));
	//if (error != 0)
	//	goto done;;

	// Enable PCLK
	error = itehdmi_update_reg(sc, 0x05, __BIT(0), 0);
	if (error != 0)
		goto done;

	// Power on DRV
	error = itehdmi_update_reg(sc, 0x61, __BIT(5), 0);
	if (error != 0)
		goto done;

	// Power on XPLL
	error = itehdmi_update_reg(sc, 0x62, __BIT(2) | __BIT(6), 0);
	if (error != 0)
		goto done;

	// Power on IPLL
	error = itehdmi_update_reg(sc, 0x64, __BIT(6), 0);
	if (error != 0)
		goto done;

	// Enable DRV
	error = itehdmi_update_reg(sc, 0x61, __BIT(4), 0);
	if (error != 0)
		goto done;

	// ??
	error = itehdmi_update_reg(sc, 0x62, __BIT(3), __BIT(3));
	if (error != 0)
		goto done;

	// ??
	error = itehdmi_update_reg(sc, 0x64, __BIT(2), __BIT(2));
	if (error != 0)
		goto done;

	//error = itehdmi_update_reg(sc, 0x0F, __BITS(4,5), 0);
	//if (error != 0)
	//	goto done;;

done:
	return error;
}

static int
itehdmi_do_ddc_command(struct itehdmi_softc *sc, uint8_t cmd)
{
	int error;
	uint8_t val;

	error = itehdmi_update_reg(sc, 0x15, __BITS(0,3),
	    __SHIFTIN(cmd, __BITS(0,3)));
	if (error != 0)
		goto done;

	int cnt = 0;
	while (true) {
		if (cnt < 3)
			delay(1000);
		else
			kpause("itehdmi", 0, 1, NULL);
		error = itehdmi_read_reg(sc, 0x16, &val);
		if (error != 0) {
			device_printf(sc->sc_dev, "Failed to read DDC cmd status cmd=%u\n", cmd);
			goto done;;
		}
		if (val & __BITS(3,5)) {
			error = EAGAIN;
			device_printf(sc->sc_dev, "DDC command error cmd=%u status=0x%02x\n", cmd, val);
			goto done;;
		}
		if (val & __BIT(7))
			break;
		cnt++;
		if (cnt > 10) {
			error = EAGAIN;
			device_printf(sc->sc_dev, "giving up wait for DDC command cmd=%u\n", cmd);
			goto done;;
		}
	}

done:
	return error;
}

// Requirements: offset < 128, len <= 32
static int
itehdmi_read_edid_bytes(struct itehdmi_softc *sc, uint8_t offset, uint8_t len,
    uint8_t *buf)
{
	int error;
	int i;

	error = itehdmi_do_ddc_command(sc, 9);
	if (error != 0)
		goto done;


	error = itehdmi_write_reg(sc, 0x12, offset);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to set EDID block offset\n");
		goto done;
	}
	error = itehdmi_write_reg(sc, 0x13, len);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to set EDID read length\n");
		goto done;
	}
	error = itehdmi_do_ddc_command(sc, 3);
	if (error != 0)
		goto done;

	for (i = 0; i < 32; i++) {
		error = itehdmi_read_reg(sc, 0x17, &buf[i]);
		if (error != 0) {
			device_printf(sc->sc_dev, "Failed to read DDC FIFO\n");
			goto done;
		}
	}

done:
	return error;
}

static int
itehdmi_read_edid(struct itehdmi_softc *sc, uint8_t *buf)
{
	int error;
	int i;

	error = itehdmi_update_reg(sc, 0x10, __BIT(0), __BIT(0));
	if (error != 0)
		goto done;

	error = itehdmi_write_reg(sc, 0x11, 0xA0);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to set EDID fetching\n");
		goto done;
	}
	error = itehdmi_write_reg(sc, 0x14, 0x00);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to set EDID segment\n");
		goto done;
	}

	for (i = 0; i < 4; i++) {
		error = itehdmi_read_edid_bytes(sc, i*32, 32, &buf[i*32]);
		if (error != 0)
			goto done;
	}

done:
	return error;
}
#endif

#if 1
static int
itehdmi_patterngen(struct itehdmi_softc *sc)
{
	int error;

#if 0
	u_int htotal = 858;
	u_int hdew = 720;
	u_int hfp = 16;
	u_int hsw = 62;
	//u_int hbp = 60;
	u_int vtotal = 525;
	u_int vdew = 480;
	u_int vfp = 9;
	u_int vsw = 6;
	//u_int vbp = 30;
#else
	u_int htotal = 1664;
	u_int hdew = 1280;
	u_int hfp = 56;
	u_int hsw = 136;
	//u_int hbp = 192;
	u_int vtotal = 726;
	u_int vdew = 720;
	u_int vfp = 1;
	u_int vsw = 3;
	//u_int vbp = 2;
#endif

	u_int hrs = hfp;
	u_int hre = hrs + hsw - 1;
	u_int hdes = hrs + hsw;
	u_int hdee = hdes + hdew - 1;

	u_int vrs = vfp;
	u_int vre = (vrs + vsw) % 16;
	u_int vrs2nd = 0xfff;
	u_int vre2nd = 0xfff;

	u_int vdes = vrs + vsw + vfp;
	u_int vdee = vdes + vdew;
	u_int vdes2 = 0xfff;
	u_int vdee2 = 0xfff;

	// XXX Consider enabling VSYNC/HSYNC/DE Generation

	error = itehdmi_update_reg(sc, 0x90, 0xF0, (htotal&0x0F) << 4);
	if (error != 0)
		goto done;

	error = itehdmi_write_reg(sc, 0x91, (htotal&0x0FF0) >> 4);
	if (error != 0)
		goto done;


	error = itehdmi_write_reg(sc, 0x95, hrs & 0xFF);
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0x96, hre & 0xFF);
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0x97, ((hre & 0x0F00)>>4) + ((hrs & 0x0F00) >> 8));
	if (error != 0)
		goto done;

	error = itehdmi_write_reg(sc, 0x92, hdes & 0xFF);
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0x93, hdee & 0xFF);
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0x94, ((hdee & 0x0F00)>>4) + ((hdes & 0x0F00) >> 8));
	if (error != 0)
		goto done;

	error = itehdmi_write_reg(sc, 0x98, vtotal & 0xFF);
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0x99, (vtotal & 0x0F00) >> 8);
	if (error != 0)
		goto done;

	error = itehdmi_write_reg(sc, 0xa0, vrs & 0xFF);
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0xa1, ((vre & 0x0F) << 4) + ((vrs & 0x0F00) >> 8));
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0xa2, vrs2nd & 0xFF);
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0xa6, (vre2nd & 0xF0) + ((vre & 0xF0) >> 4));
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0xa3, ((vre2nd & 0x0F)<<4) + ((vrs2nd & 0xF00) >> 8));
	if (error != 0)
		goto done;

	error = itehdmi_write_reg(sc, 0x9a, vdes & 0xFF);
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0x9b, vdee & 0xFF);
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0x9c, ((vdee & 0x0F00)>>4) + ((vdes&0xF00)>>8));
	if (error != 0)
		goto done;

	error = itehdmi_write_reg(sc, 0x9D, vdes2 & 0xFF);
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0x9E, vdee2 & 0xFF);
	if (error != 0)
		goto done;
	error = itehdmi_write_reg(sc, 0x9F, ((vdee2 & 0x0F00)>>4) + ((vdes2&0xF00)>>8));
	if (error != 0)
		goto done;

	error = itehdmi_update_reg(sc, 0xb1, 0x55, ((hre&0x1000)>>6)+((hrs&0x1000)>>8)+((hdee&0x1000)>>10)+((hdes&0x1000)>>12));
	if (error != 0)
		goto done;
	error = itehdmi_update_reg(sc, 0xB2, 0x05, 0);
	if (error != 0)
		goto done;

	// Enable H/V-SYNC and DE generation
	error = itehdmi_update_reg(sc, 0x90, 0x09, 0x00);
	if (error != 0)
		goto done;

#if 0
	error = itehdmi_update_reg(sc, 0xA9, 0xFF, 0);
	if (error != 0)
		goto done;
	error = itehdmi_update_reg(sc, 0xAF, 0xFF, 0xA0);
	if (error != 0)
		goto done;
	error = itehdmi_update_reg(sc, 0xB0, 0xFF, 0);
	if (error != 0)
		goto done;

	// Enable pattern generator
	error = itehdmi_update_reg(sc, 0xA8, 0x01, 0x01);
	if (error != 0)
		goto done;
#else
	// Disable pattern generator
	error = itehdmi_update_reg(sc, 0xA8, 0x01, 0x00);
	if (error != 0)
		goto done;
#endif

done:
	return error;
}
#endif

#if 1
struct regtable {
	uint8_t reg;
	uint8_t invmask;
	uint8_t ormask;
};

#if 1
static struct regtable power_on_table[] = {
	{0x0F, 0x40, 0x00},
	{0x05, 0x01, 0x00},
	{0x61, 0x20, 0x00},
	{0x62, 0x44, 0x00},
	{0x64, 0x40, 0x00},
	{0x61, 0x10, 0x00},
	{0x62, 0x08, 0x08},
	{0x64, 0x04, 0x04},
	{0,0,0},
};

static struct regtable program_video_mode_table[] = {
	{0xC1, 0x01, 0x01},
	{0x70, 0xDC, 0x04},
	{0x72, 0x03, 0x00},
	// Set AFE for Pixel Clock < 80MHz. Copied from Programming Guide.
	{0x62, 0x90, 0x10},
	{0x64, 0x89, 0x09},
	{0x68, 0x10, 0x10},
	{0x61, 0xFF, 0x00},
	// DVI/HDMI Mode
	{0xC0, 0x01, 0x01},
//	{0xC0, 0x01, 0x00},
	{0xC1, 0x01, 0x00},
	{0xC6, 0x03, 0x03},
//	{0xC6, 0x03, 0x00},
	{0,0,0},
};

#else

static struct regtable hdmitx_init_table[] = {
	{0x0F, 0x40, 0x00},

	{0x62, 0x08, 0x00},
	{0x64, 0x04, 0x00},
	{0x01, 0x00, 0x00},

	{0x04, 0x20, 0x20},
	{0x04, 0x1D, 0x1D},
	{0x01, 0x00, 0x00},

	{0x0F, 0x01, 0x00},

	{0x62, 0x90, 0x10},
	{0x64, 0x89, 0x09},
	{0x68, 0x10, 0x10},
	//{0x61, 0xFF, 0x00}, // ?? Trying out for debugging
	{0,0,0},
};

static struct regtable hdmitx_defaultvideo_table[] = {
	{0x72, 0xFF, 0x00},
	//{0x70, 0xFF, 0x04}, // Dual Edge pixel clock
	{0x70, 0xFF, 0x00},

	{0x04, 0x08, 0x00},
	{0,0,0},
};

static struct regtable hdmitx_sethdmi_table[] = {
	{0xC0, 0x01, 0x01},

	{0xC1, 0x03, 0x00},
	{0xC6, 0x03, 0x03},
	{0,0,0},
};

static struct regtable hdmitx_pwron_table[] = {
	{0xCF, 0x78, 0x38},
	{0x05, 0x01, 0x00},

	{0x61, 0x20, 0x00},
	{0x62, 0x44, 0x00},
	{0x64, 0x40, 0x00},

	{0x61, 0x10, 0x00},
	{0x62, 0x08, 0x08},
	{0x64, 0x04, 0x04},
	{0x0F, 0x78, 0x18},
	{0,0,0},
};
#endif

static int
loadregs(struct itehdmi_softc *sc, struct regtable *t)
{
	int error = 0;
	int i;

	for (i = 0;; i++) {
		if (t[i].reg == 0)
			goto done;
		if (t[i].reg == 1) {
			delay(100);
		} else {
			error = itehdmi_update_reg(sc,
			    t[i].reg, t[i].invmask, t[i].ormask);
			if (error != 0)
				goto done;
		}
	}

done:
	return error;
}
#endif

CFATTACH_DECL_NEW(itehdmi, sizeof(struct itehdmi_softc),
	itehdmi_match, itehdmi_attach, NULL, NULL);

static int
itehdmi_match(device_t parent, cfdata_t cf, void *aux)
{
	struct i2c_attach_args *ia = aux;
	//int match_result;

	aprint_normal("%s: got called for matching, addr=0x%x\n",
	    __func__, ia->ia_addr);

	//if (iic_use_direct_match(ia, cf, compat_data, &match_result))
	//	return match_result;

	switch (ia->ia_addr) {
	case ITEHDMI_I2C_DEFAULT_ADDR:
	case ITEHDMI_I2C_ALTERNATIVE_ADDR:
		return I2C_MATCH_ADDRESS_ONLY+1;
	}

	return 0;
}

static void
itehdmi_attach(device_t parent, device_t self, void *aux)
{
	struct itehdmi_softc * const sc = device_private(self);
	struct i2c_attach_args *ia = aux;
	uint8_t id[4];
	int error;

	sc->sc_dev = self;
	sc->sc_i2c = ia->ia_tag;
	sc->sc_addr = ia->ia_addr;
	sc->sc_phandle = ia->ia_cookie;

	error = itehdmi_identify(sc, id);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to read IT66121 chip identity\n");
		return;
	}
	int i;
	for (i = 0; i < 4; i++) {
		device_printf(sc->sc_dev, "Identity reg %d: 0x%02x\n",
		    i, id[i]);
	}
	if (id[0] != 0x54 || id[1] != 0x49 || id[2] != 0x12) {
		device_printf(sc->sc_dev, "Identity registers look wrong\n");
		return;
	}

	aprint_naive("\n");
	aprint_normal(": ITE IT66121 HDMI Controller\n");

#if 0
#if 1
	uint8_t val;
	error = itehdmi_poweron(sc);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed Power on sequence\n");
		return;
	}

	if (itehdmi_update_reg(sc, 0x09, __BITS(0,1), 0) != 0) {
		device_printf(sc->sc_dev, "Failed to enable events reg 0x09\n");
	}
	if (itehdmi_update_reg(sc, 0x0A, __BIT(6), 0) != 0) {
		device_printf(sc->sc_dev, "Failed to enable events reg 0x0A\n");
	}
	if (itehdmi_update_reg(sc, 0x0B, __BIT(3), 0) != 0) {
		device_printf(sc->sc_dev, "Failed to enable events reg 0x0B\n");
	}
	int cnt = 0;
	while (true) {
		kpause("itehdmi", 0, hz/2, NULL);
		if (cnt > 3)
			break;
		cnt++;
		if (itehdmi_read_reg(sc, 0x0E, &val) == 0) {
			device_printf(sc->sc_dev, "SenDetect Reg: 0x%02x\n",
			    val);
		}
		// Check and clear Interrupt registers
		if (itehdmi_read_reg(sc, 0x06, &val) == 0) {
			device_printf(sc->sc_dev, "Interrupt Status 0x06: 0x%02x\n",
			    val);
			if (val & __BIT(0)) {
				itehdmi_write_reg(sc, 0x0C, __BIT(0));
			}
			if (val & __BIT(1)) {
				itehdmi_write_reg(sc, 0x0C, __BIT(1));
				//done++;
			}
			if (val & __BITS(0,1)) {
				if (itehdmi_update_reg(sc, 0x0E, __BIT(0), __BIT(0)) != 0) {
					device_printf(sc->sc_dev, "Failed to set Interrupt Clearing\n");
				}
			}
		}
	}
	if (itehdmi_read_reg(sc, 0x07, &val) == 0) {
		device_printf(sc->sc_dev, "Interrupt Status 0x07: 0x%02x\n",
		    val);
	}
	if (itehdmi_read_reg(sc, 0x08, &val) == 0) {
		device_printf(sc->sc_dev, "Interrupt Status 0x08: 0x%02x\n",
		    val);
	}

	uint8_t edid[128];
	struct edid_info info;
	memset(edid, 0, sizeof(edid));
	error = itehdmi_read_edid(sc, edid);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to read EDID data\n");
		return;
	}
#if 0
	device_printf(sc->sc_dev, "EDID data:");
	for (i = 0; i < 128; i++) {
		printf(" 0x%02x", edid[i]);
	}
	printf("\n");
#endif

	error = edid_is_valid(edid);
	if (error != 0) {
		device_printf(sc->sc_dev, "EDID data is not valid\n");
		return;
	}
	error = edid_parse(edid, &info);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to parse EDID data\n");
		return;
	}
	edid_print(&info);

	if (itehdmi_read_reg(sc, 0x0E, &val) == 0) {
		device_printf(sc->sc_dev, "TxVidStable Reg: 0x%02x\n",
		    val);
	}
	// Check and clear Interrupt registers
	if (itehdmi_read_reg(sc, 0x06, &val) == 0) {
		device_printf(sc->sc_dev, "Interrupt Status 0x06: 0x%02x\n",
		    val);
		if (val & __BIT(0)) {
			itehdmi_write_reg(sc, 0x0C, __BIT(0));
		}
		if (val & __BIT(1)) {
			itehdmi_write_reg(sc, 0x0C, __BIT(1));
		}
		if (val & __BITS(0,1)) {
			if (itehdmi_update_reg(sc, 0x0E, __BIT(0), __BIT(0)) != 0) {
				device_printf(sc->sc_dev, "Failed to set Interrupt Clearing\n");
			}
		}
	}
	delay(2000);
	// Check and clear Interrupt registers
	if (itehdmi_read_reg(sc, 0x06, &val) == 0) {
		device_printf(sc->sc_dev, "Interrupt Status 0x06: 0x%02x\n",
		    val);
		if (val & __BIT(0)) {
			itehdmi_write_reg(sc, 0x0C, __BIT(0));
		}
		if (val & __BIT(1)) {
			itehdmi_write_reg(sc, 0x0C, __BIT(1));
		}
		if (val & __BITS(0,1)) {
			if (itehdmi_update_reg(sc, 0x0E, __BIT(0), __BIT(0)) != 0) {
				device_printf(sc->sc_dev, "Failed to set Interrupt Clearing\n");
			}
		}
	}
	if (itehdmi_read_reg(sc, 0x0E, &val) == 0) {
		device_printf(sc->sc_dev, "TxVidStable Reg: 0x%02x\n",
		    val);
	}
	if (itehdmi_read_reg(sc, 0x06, &val) == 0) {
		device_printf(sc->sc_dev, "1st Int Status Reg: 0x%02x\n",
		    val);
	}
	if (itehdmi_read_reg(sc, 0x07, &val) == 0) {
		device_printf(sc->sc_dev, "2nd Int Status Reg: 0x%02x\n",
		    val);
	}
	if (itehdmi_read_reg(sc, 0x08, &val) == 0) {
		device_printf(sc->sc_dev, "3rd Int Status Reg: 0x%02x\n",
		    val);
	}

	// Program videomode
	error = itehdmi_update_reg(sc, 0x04, 0x1D, 0x1D);
	if (error != 0)
		return;
	delay(1000);

	error = itehdmi_update_reg(sc, 0x04, __BIT(3), 0);
	if (error != 0)
		return;

	error = itehdmi_update_reg(sc, 0xC1, __BITS(0,1), __BITS(0,1));
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to set AV mute mode\n");
		return;
	}

	// Set input mode
	error = itehdmi_update_reg(sc, 0x70, __BITS(6,7), 0);
	if (error != 0)
		return;
	error = itehdmi_update_reg(sc, 0x70, __BIT(4), 0);
	if (error != 0)
		return;
	error = itehdmi_update_reg(sc, 0x70, __BIT(3), 0);
	if (error != 0)
		return;
	error = itehdmi_update_reg(sc, 0x90, __BIT(3), 0);
	if (error != 0)
		return;
	error = itehdmi_update_reg(sc, 0x90, __BIT(0), 0);
	if (error != 0)
		return;
#if 0
	error = itehdmi_update_reg(sc, 0x70, __BIT(2), __BIT(2)); // Dual edge
	if (error != 0)
		return;
#else
	error = itehdmi_update_reg(sc, 0x70, __BIT(2), 0);
	if (error != 0)
		return;
#endif

#if 1
	error = itehdmi_patterngen(sc);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed Patterngen sequence\n");
		return;
	}
#endif

	
	// Set AFE for Pixel Clock < 80MHz. Copied from Programming Guide.
	error = itehdmi_update_reg(sc, 0x62, 0x90, 0x10);
	if (error != 0)
		return;
	error = itehdmi_update_reg(sc, 0x64, 0x89, 0x09);
	if (error != 0)
		return;
	error = itehdmi_update_reg(sc, 0x68, 0x10, 0x10);
	if (error != 0)
		return;
	error = itehdmi_write_reg(sc, 0x61, 0);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to Fire AFE\n");
		return;
	}

	// Set HDMI mode
	error = itehdmi_update_reg(sc, 0xC0, __BIT(0), __BIT(0));
	if (error != 0)
		return;
	// Clear AVMUTE
	error = itehdmi_update_reg(sc, 0xC1, __BIT(0), 0);
	if (error != 0)
		return;
	// Enable General Control Packet
	error = itehdmi_update_reg(sc, 0xC6, 0x03, 0x03);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to set register 0xC6\n");
		return;
	}

#if 0
	// Test: Set Blue AV Mute
	error = itehdmi_update_reg(sc, 0xC1, __BITS(0,1), __BITS(0,1));
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed to set AV mute mode\n");
		return;
	}
#endif

#else

	error = loadregs(sc, hdmitx_init_table);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed init sequence\n");
		return;
	}
	error = loadregs(sc, hdmitx_defaultvideo_table);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed defaultvideo sequence\n");
		return;
	}
#if 1
	error = itehdmi_patterngen(sc);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed Patterngen sequence\n");
		return;
	}
#endif
	error = loadregs(sc, hdmitx_sethdmi_table);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed sethdmi sequence\n");
		return;
	}
	error = loadregs(sc, hdmitx_pwron_table);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed pwron sequence\n");
		return;
	}
#endif
#endif

#if 1
	error = loadregs(sc, power_on_table);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed power on sequence\n");
		return;
	}
#if 0
	error = itehdmi_patterngen(sc);
	if (error != 0) {
		device_printf(sc->sc_dev, "Failed Patterngen sequence\n");
		return;
	}
#endif
	error = loadregs(sc, program_video_mode_table);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "Failed video mode sequence\n");
		return;
	}
#endif

	device_printf(sc->sc_dev, "Finished programming IT66121\n");
}
