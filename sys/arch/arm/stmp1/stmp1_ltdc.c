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

#include "opt_ddb.h"
#include "opt_genfb.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/systm.h>
#include <sys/time.h>

#include <dev/fdt/fdtvar.h>
#include <dev/fdt/fdt_port.h>

#if defined(DDB)
#include <machine/db_machdep.h>
#include <ddb/db_extern.h>
#endif

#include <dev/videomode/videomode.h>
#include <dev/wsfb/genfbvar.h>

// Timings
#define LTDC_SSCR	0x008
#define LTDC_BPCR	0x00C
#define LTDC_AWCR	0x010
#define LTDC_TWCR	0x014
#define LTDC_GCR	0x018
#define LTDC_SRCR	0x024
#define LTDC_IER	0x034
#define LTDC_ISR	0x038
#define LTDC_ICR	0x03C
#define LTDC_LIPCR	0x040

// Layer 1
#define LTDC_L1CR	0x084
#define LTDC_L1WHPCR	0x088
#define LTDC_L1WVPCR	0x08C
#define LTDC_L1PFCR	0x094
#define LTDC_L1CFBAR	0x0AC
#define LTDC_L1CFBLR	0x0B0
#define LTDC_L1CFBLNR	0x0B4
#define LTDC_L1CLUTWR	0x0C4

// Misc
#define LTDC_BCCR	0x02C	// Background Color Configuration
#define LTDC_ISR	0x038
#define LTDC_CPSR	0x044
#define LTDC_CDSR	0x048

static int stmp1ltdc_match(device_t, cfdata_t, void *);
static void stmp1ltdc_attach(device_t, device_t, void *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "st,stm32-ltdc" },
	DEVICE_COMPAT_EOL
};

static struct genfb_colormap_callback gfb_cb;

struct stmp1ltdc_softc {
	// This has to be first!
	struct genfb_softc	sc_gen;

	device_t		sc_dev;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	bus_dma_tag_t		sc_dmat;
	void			*sc_global_ih;

	kmutex_t		sc_lock;

	struct clk		*sc_clk;
	struct fdtbus_reset	*sc_rst;
	struct fdt_device_ports	sc_ports;
	struct fdt_endpoint	*sc_out_ep;

	bus_size_t		fb_buf_size;
	int			fb_nsegs;
	bus_dma_segment_t	fb_seg;
	bus_dmamap_t		fb_dmamap;
	void			*fb_va;

	bool			sc_vblank;

	bool	clut_modified[256];
	bool	clut_any_modified;
	uint8_t	clut_r[256];
	uint8_t	clut_g[256];
	uint8_t	clut_b[256];
};

#if defined(DDB)
static void	stm_genfb_ddb_trap_callback(int);
static device_t	stm_genfb_consoledev = NULL;
#endif

static int	stm_genfb_ioctl(void *, void *, u_long, void *, int, lwp_t *);
static paddr_t	stm_genfb_mmap(void *, void *, off_t, int);
static bool	stm_genfb_shutdown(device_t, int);

static void	stmp1ltdc_enable(struct stmp1ltdc_softc *, bool);

static int
stm_genfb_ioctl(void *v, void *vs, u_long cmd, void *data, int flag, lwp_t *l)
{
	struct stmp1ltdc_softc *sc = v;
	struct wsdisplayio_bus_id *busid;

	switch (cmd) {
	case WSDISPLAYIO_GTYPE:
		*(u_int *)data = WSDISPLAY_TYPE_STM32;
		return 0;
	case WSDISPLAYIO_GET_BUSID:
		busid = data;
		busid->bus_type = WSDISPLAYIO_BUS_SOC;
		return 0;
	case WSDISPLAYIO_GVIDEO:
		aprint_normal("%s: GVIDEO called\n", __func__);
		if (sc->sc_vblank)
			*(u_int *)data = WSDISPLAYIO_VIDEO_OFF;
		else
			*(u_int *)data = WSDISPLAYIO_VIDEO_ON;
		return 0;
	case WSDISPLAYIO_SVIDEO:
		if (*(u_int *)data == WSDISPLAYIO_VIDEO_ON) {
			if (sc->sc_vblank)
				stmp1ltdc_enable(sc, true);
			sc->sc_vblank = false;
		} else {
			if (!sc->sc_vblank)
				stmp1ltdc_enable(sc, false);
			sc->sc_vblank = true;
		}
		return 0;
	default:
		return EPASSTHROUGH;
	}
}

static paddr_t
stm_genfb_mmap(void *v, void *vs, off_t offset, int prot)
{
	struct stmp1ltdc_softc *sc = v;
	paddr_t r;

	KASSERT(offset >= 0);
	KASSERT(offset < sc->fb_seg.ds_len);

	r = bus_dmamem_mmap(sc->sc_dmat, &sc->fb_seg, sc->fb_nsegs,
	    offset, prot, BUS_DMA_PREFETCHABLE);

	return r;
}

static bool
stm_genfb_shutdown(device_t self, int flags)
{
	genfb_enable_polling(self);
	return true;
}
                
#if defined(DDB)
static void
stm_genfb_ddb_trap_callback(int where)
{
	if (stm_genfb_consoledev == NULL)
		return;

	if (where)
		genfb_enable_polling(stm_genfb_consoledev);
	else
		genfb_disable_polling(stm_genfb_consoledev);
}
#endif

static void
stmp1ltdc_hdmi_ep_connect(device_t dev, struct fdt_endpoint *ep, bool connect)
{
	struct stmp1ltdc_softc *sc = device_private(dev);

	sc->sc_out_ep = ep;
	// TODO(ivadasz): Schedule a task to actually activate the HDMI connector state.
}

static void
stmp1ltdc_enable(struct stmp1ltdc_softc *sc, bool enable)
{
	int val;

	if (enable) {
		clk_enable(sc->sc_clk);
	} else {
		clk_disable(sc->sc_clk);
	}

	// Enable Layer 1
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_L1CR);
	val &= ~__BIT(0);
	val &= ~__BIT(4);
	if (enable)
		val |= __BIT(0);
	val |= __BIT(4);	// Enable CLUT
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_L1CR, val);

	// Reload shadow registers immediately
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_SRCR, __BIT(0));

#if 0
	// Set Background Color
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_BCCR, 0x00ff0000);
#endif

	// Enable LCD Controller
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_GCR);
	val &= ~__BIT(0);
	if (enable)
		val |= __BIT(0);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_GCR, val);

	if (sc->sc_out_ep) {
		aprint_normal("%s: Updating HDMI chip state to %s\n", __func__, enable ? "ON" : "OFF");
		fdt_endpoint_activate(sc->sc_out_ep, enable);
	}
}

static void
stmp1ltdc_set_clut(struct stmp1ltdc_softc *sc,
		   uint8_t addr, uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t val;

	val = __SHIFTIN(addr, __BITS(24,31)) | __SHIFTIN(r, __BITS(16,23)) |
	      __SHIFTIN(g, __BITS(8,15)) | __SHIFTIN(b, __BITS(0,7));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_L1CLUTWR, val);
}

static void
stmp1ltdc_configure_timing(struct stmp1ltdc_softc *sc)
{
	uint32_t val;
#if 0
	u_int width = 1024;
	u_int hfrontporch = 24;
	u_int hsync = 136;
	u_int hbackporch = 160;
	u_int height = 768;
	u_int vfrontporch = 3;
	u_int vsync = 6;
	u_int vbackporch = 29;
#else
	u_int width = 1280;
	u_int hbackporch = 128;
	u_int hfrontporch = 88;
	u_int hsync = 120;
	u_int height = 720;
	u_int vbackporch = 1;
	u_int vsync = 3;
	u_int vfrontporch = 9;
#endif

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_SSCR);
	val &= ~__BITS(0,11);
	val &= ~__BITS(16,27);
	val |= __SHIFTIN(hsync-1, __BITS(16,27));
	val |= __SHIFTIN(vsync-1, __BITS(0,11));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_SSCR, val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_BPCR);
	val &= ~__BITS(0,11);
	val &= ~__BITS(16,27);
	val |= __SHIFTIN(hsync+hbackporch-1, __BITS(16,27));
	val |= __SHIFTIN(vsync+vbackporch-1, __BITS(0,11));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_BPCR, val);
	
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_AWCR);
	val &= ~__BITS(0,11);
	val &= ~__BITS(16,27);
	val |= __SHIFTIN(hsync+hbackporch+width-1, __BITS(16,27));
	val |= __SHIFTIN(vsync+vbackporch+height-1, __BITS(0,11));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_AWCR, val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_TWCR);
	val &= ~__BITS(0,11);
	val &= ~__BITS(16,27);
	val |= __SHIFTIN(hsync+hbackporch+width+hfrontporch-1, __BITS(16,27));
	val |= __SHIFTIN(vsync+vbackporch+height+vfrontporch-1, __BITS(0,11));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_TWCR, val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_GCR);
	// Configure H/V synchronization polarity
	val &= ~__BITS(28,31);
	//val |= __BIT(31); // VSYNC polarity
	val |= __BIT(30); // HSYNC polarity
	//val |= __BIT(29); // DE polarity
	//val |= __BIT(28); // Pixel Clock polarity
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_GCR, val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_L1WHPCR);
	val &= ~__BITS(0,11);
	val &= ~__BITS(16,27);
	val |= __SHIFTIN(hsync+hbackporch+width-1, __BITS(16,27));
	val |= __SHIFTIN(hsync+hbackporch, __BITS(0,11));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_L1WHPCR, val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_L1WVPCR);
	val &= ~__BITS(0,11);
	val &= ~__BITS(16,27);
	val |= __SHIFTIN(vsync+vbackporch+height-1, __BITS(16,27));
	val |= __SHIFTIN(vsync+vbackporch, __BITS(0,11));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_L1WVPCR, val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_L1PFCR);
	val &= ~__BITS(0,2);
	//val |= __SHIFTIN(2, __BITS(0,2)); // Set to RGB565 pixel mode.
	val |= __SHIFTIN(5, __BITS(0,2)); // Set to L8 pixel mode.
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_L1PFCR, val);

	// Set Framebuffer Physical Address
	//bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_L1CFBAR, 0xC2000000);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_L1CFBAR,
	    sc->fb_seg.ds_addr);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_L1CFBLR);
	val &= ~__BITS(0,13);
	val &= ~__BITS(16,29);
	//val |= __SHIFTIN(2*width, __BITS(16,29));
	//val |= __SHIFTIN(2*width+7, __BITS(0,13));
	val |= __SHIFTIN(width, __BITS(16,29));
	val |= __SHIFTIN(width+7, __BITS(0,13));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_L1CFBLR, val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_L1CFBLNR);
	val &= ~__BITS(0,11);
	val |= __SHIFTIN(height, __BITS(0,11));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_L1CFBLNR, val);

	// Fill in CLUT with some default/dummy data
	for (val = 0; val <= 0xff; val++) {
		stmp1ltdc_set_clut(sc, val, val, val, val);
	}

	// Mask all LTDC interrupts
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_IER);
	val &= ~__BITS(0,3);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_IER, val);

	// Configure Line interrupt
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_LIPCR);
	val &= ~__BITS(0,11);
	val |= __SHIFTIN(vsync+vbackporch+height , __BITS(0,11));
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_LIPCR, val);

	stmp1ltdc_enable(sc, true);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_SSCR);
	device_printf(sc->sc_dev, "LTDC_SSCR: 0x%08x\n", val);
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_BPCR);
	device_printf(sc->sc_dev, "LTDC_BPCR: 0x%08x\n", val);
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_AWCR);
	device_printf(sc->sc_dev, "LTDC_AWCR: 0x%08x\n", val);
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_TWCR);
	device_printf(sc->sc_dev, "LTDC_TWCR: 0x%08x\n", val);
}

static void
stm_genfb_set_mapreg(void *opaque, int index, int r, int g, int b)
{
	struct stmp1ltdc_softc *sc = opaque;
	uint32_t val;

	//aprint_normal("%s: CLUT request [%d]: r:%x g:%x b:%x\n",
	//    __func__, index, r, g, b);
	sc->clut_r[index] = r;
	sc->clut_g[index] = g;
	sc->clut_b[index] = b;
	// TODO: Maybe add memory write fence
	sc->clut_modified[index] = TRUE;
	if (!sc->clut_any_modified) {
		mutex_enter(&sc->sc_lock);
		sc->clut_any_modified = TRUE;
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_IER);
		val |= __BIT(0);
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_IER, val);
		mutex_exit(&sc->sc_lock);
	}
}

static int
stmp1ltdc_global_intr(void *arg)
{
	struct stmp1ltdc_softc *sc = arg;
	uint32_t val;
	int i;

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_ISR);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_ICR, val);
	mutex_enter(&sc->sc_lock);
	if (sc->clut_any_modified) {
		for (i = 0; i < 256; i++) {
			if (sc->clut_modified[i]) {
				stmp1ltdc_set_clut(sc, i, sc->clut_r[i],
				    sc->clut_g[i], sc->clut_b[i]);
				sc->clut_modified[i] = FALSE;
			}
		}
		sc->clut_any_modified = FALSE;
	}
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, LTDC_IER);
	val &= ~__BIT(0);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, LTDC_IER, val);
	mutex_exit(&sc->sc_lock);

	return 0;
}

CFATTACH_DECL_NEW(stmp1ltdc, sizeof(struct stmp1ltdc_softc),
	stmp1ltdc_match, stmp1ltdc_attach, NULL, NULL);

static int
stmp1ltdc_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
stmp1ltdc_attach(device_t parent, device_t self, void *aux)
{
	struct stmp1ltdc_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	bus_addr_t addr;
	bus_size_t size;
	int error;

	sc->sc_dev = self;
	sc->sc_bst = faa->faa_bst;
	sc->sc_dmat = faa->faa_dmat;

	sc->sc_out_ep = NULL;

	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NET);

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

	char intrstr[128];
	if (!fdtbus_intr_str(phandle, 0, intrstr, sizeof(intrstr))) {
		aprint_error_dev(self, "failed to decode event interrupt\n");
		return;
	}

	sc->sc_global_ih = fdtbus_intr_establish_xname(phandle, 0, IPL_NET, 0,
	    stmp1ltdc_global_intr, sc, device_xname(self));
	if (sc->sc_global_ih == NULL) {
		aprint_error_dev(self,
		    "couldn't establish display interrupt on %s\n", intrstr);
		return;
	}

	sc->sc_clk = fdtbus_clock_get(phandle, "lcd");
	if (sc->sc_clk == NULL) {
		aprint_error(": couldn't enable Pixel clock\n");
		// return;
	}
	clk_enable(sc->sc_clk);
	sc->sc_rst = fdtbus_reset_get_index(phandle, 0);

	/* Apply Reset */
	if (sc->sc_rst != NULL) {
		device_printf(self, "Applying reset\n");
		error = fdtbus_reset_assert(sc->sc_rst);
		if (error != 0) {
			aprint_error(": couldn't assert reset\n");
			return;
		}
		delay(20000);
		error = fdtbus_reset_deassert(sc->sc_rst);
		if (error != 0) {
			aprint_error(": couldn't de-assert reset\n");
			return;
		}
		delay(20000);
	}

	aprint_naive("\n");
	aprint_normal(": STM32 LCD controller\n");

	//sc->fb_buf_size = 1280*720*2;
	sc->fb_buf_size = 1280*720;
	error = bus_dmamem_alloc(sc->sc_dmat, sc->fb_buf_size, PAGE_SIZE, 0,
	    &sc->fb_seg, 1, &sc->fb_nsegs, BUS_DMA_WAITOK);
	if (error) {
		aprint_error_dev(self, "Failed at bus_dmamem_alloc\n");
		return;
	}
	error = bus_dmamem_map(sc->sc_dmat, &sc->fb_seg, sc->fb_nsegs,
	    sc->fb_buf_size,
	    &sc->fb_va, BUS_DMA_WAITOK | BUS_DMA_PREFETCHABLE);
	if (error) {
		//goto free;
		aprint_error_dev(self, "Failed at bus_dmamem_map\n");
		return;
	}
	/* map memory for DMA */
	error = bus_dmamap_create(sc->sc_dmat, sc->fb_buf_size, 1,
	    sc->fb_buf_size, 0,
	    BUS_DMA_WAITOK, &sc->fb_dmamap);
	if (error) {
		//goto unmap;
		aprint_error_dev(self, "Failed at bus_dmamap_create\n");
		return;
	}
	error = bus_dmamap_load(sc->sc_dmat, sc->fb_dmamap, sc->fb_va,
	    sc->fb_buf_size, NULL, BUS_DMA_WAITOK);
	if (error) {
		//goto destroy;
		aprint_error_dev(self, "Failed at bus_dmamap_load\n");
		return;
	}
	//memset(sc->fb_va, 0x80, sc->fb_buf_size);
	//memset(sc->fb_va, 0xff, sc->fb_buf_size/2);

	stmp1ltdc_configure_timing(sc);

	prop_dictionary_t cfg = device_properties(self);
	struct genfb_ops ops;

	prop_dictionary_set_uint32(cfg, "width", 1280);
	prop_dictionary_set_uint32(cfg, "height", 720);
	//prop_dictionary_set_uint8(cfg, "depth", 16);
	prop_dictionary_set_uint8(cfg, "depth", 8);
	//prop_dictionary_set_uint16(cfg, "linebytes", 1280*2);
	prop_dictionary_set_uint16(cfg, "linebytes", 1280);
	prop_dictionary_set_uint32(cfg, "address", sc->fb_seg.ds_addr);
	prop_dictionary_set_uint32(cfg, "virtual_address",
	    (uintptr_t)sc->fb_va);

	aprint_normal_dev(self, "physical FB address: 0x%08lx, va: 0x%08lx\n",
	    sc->fb_seg.ds_addr, (uintptr_t)sc->fb_va);

	memset(&ops, 0, sizeof(ops));
	ops.genfb_ioctl = stm_genfb_ioctl; 
	ops.genfb_mmap = stm_genfb_mmap;

	bool is_console = false;
	prop_dictionary_set_bool(cfg, "is_console", true);
	prop_dictionary_get_bool(cfg, "is_console", &is_console);
	// Register CLUT update method
	gfb_cb.gcc_cookie = sc;
	gfb_cb.gcc_set_mapreg = stm_genfb_set_mapreg;
	prop_dictionary_set_uint64(cfg, "cmap_callback",
	    (uint64_t)(uintptr_t)&gfb_cb);

#if 1
	if (is_console)
		aprint_normal(": switching to framebuffer console\n");
	else
		aprint_normal("\n");

#endif

	sc->sc_gen.sc_dev = self;
	genfb_init(&sc->sc_gen);
	pmf_device_register1(self, NULL, NULL, stm_genfb_shutdown);
	genfb_attach(&sc->sc_gen, &ops);

	if (is_console)
		aprint_normal("STM32 LTDC is console\n");

#if defined(DDB)
	if (is_console) {
		stm_genfb_consoledev = self;
		db_trap_callback = stm_genfb_ddb_trap_callback;
	}
#endif

	// TODO(ivadasz): Move genfb attaching to enabling the out-endpoint.
	//	Unless the video mode is explicitly configured via the device-tree.
	sc->sc_ports.dp_ep_connect = stmp1ltdc_hdmi_ep_connect;
	fdt_ports_register(&sc->sc_ports, self, phandle, EP_CONNECTOR);
}
