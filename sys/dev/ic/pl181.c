/* $NetBSD: pl181.c,v 1.9 2021/08/07 16:19:12 thorpej Exp $ */

/*-
 * Copyright (c) 2015 Jared D. McNeill <jmcneill@invisible.ca>
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
__KERNEL_RCSID(0, "$NetBSD: pl181.c,v 1.9 2021/08/07 16:19:12 thorpej Exp $");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/kernel.h>

#include <dev/sdmmc/sdmmcvar.h>
#include <dev/sdmmc/sdmmcchip.h>
#include <dev/sdmmc/sdmmc_ioreg.h>

#include <dev/ic/pl181reg.h>
#include <dev/ic/pl181var.h>

//#define PLMMC_DEBUG

/*
 * Round maximum transfer size down to the nearest sector.
 */
#define	PLMMC_MAXXFER	rounddown(65536*2, SDMMC_SECTOR_SIZE)

/*
 * PL181 FIFO is 16 words deep (64 bytes)
 */
#define	PL181_FIFO_DEPTH	64

static uint32_t idma_memory[0x4000];

/*
 * Data transfer IRQ status bits
 */
#define	PLMMC_INT_DATA_MASK						\
	(MMCI_INT_DATA_TIMEOUT|MMCI_INT_DATA_CRC_FAIL|			\
	 MMCI_INT_IDMA_COMPLETE|					\
	 MMCI_INT_DATA_END|MMCI_INT_DATA_BLOCK_END)
#define	PLMMC_INT_CMD_MASK						\
	(MMCI_INT_CMD_TIMEOUT|MMCI_INT_CMD_RESP_END|			\
	MMCI_INT_ACKTIMEOUT|MMCI_INT_ACKFAIL|			\
	MMCI_INT_CMD_CRC_FAIL|MMCI_INT_CMD_SENT|MMCI_INT_BUSYD0END)

static int	plmmc_host_reset(sdmmc_chipset_handle_t);
static uint32_t	plmmc_host_ocr(sdmmc_chipset_handle_t);
static int	plmmc_host_maxblklen(sdmmc_chipset_handle_t);
static int	plmmc_card_detect(sdmmc_chipset_handle_t);
static int	plmmc_write_protect(sdmmc_chipset_handle_t);
static int	plmmc_bus_power(sdmmc_chipset_handle_t, uint32_t);
static int	plmmc_bus_clock(sdmmc_chipset_handle_t, int);
static int	plmmc_bus_width(sdmmc_chipset_handle_t, int);
static int	plmmc_bus_rod(sdmmc_chipset_handle_t, int);
static void	plmmc_exec_command(sdmmc_chipset_handle_t,
				     struct sdmmc_command *);
static void	plmmc_card_enable_intr(sdmmc_chipset_handle_t, int);
static void	plmmc_card_intr_ack(sdmmc_chipset_handle_t);

static int	plmmc_wait_cmd(struct plmmc_softc *);
static int	plmmc_pio_transfer(struct plmmc_softc *,
				     struct sdmmc_command *, int);

static struct sdmmc_chip_functions plmmc_chip_functions = {
	.host_reset = plmmc_host_reset,
	.host_ocr = plmmc_host_ocr,
	.host_maxblklen = plmmc_host_maxblklen,
	.card_detect = plmmc_card_detect,
	.write_protect = plmmc_write_protect,
	.bus_power = plmmc_bus_power,
	.bus_clock = plmmc_bus_clock,
	.bus_width = plmmc_bus_width,
	.bus_rod = plmmc_bus_rod,
	.exec_command = plmmc_exec_command,
	.card_enable_intr = plmmc_card_enable_intr,
	.card_intr_ack = plmmc_card_intr_ack,
};

#define MMCI_WRITE(sc, reg, val) \
	bus_space_write_4((sc)->sc_bst, (sc)->sc_bsh, (reg), (val))
#define	MMCI_WRITE_MULTI(sc, reg, datap, cnt) \
	bus_space_write_multi_4((sc)->sc_bst, (sc)->sc_bsh, (reg), (datap), (cnt))
#define MMCI_READ(sc, reg) \
	bus_space_read_4((sc)->sc_bst, (sc)->sc_bsh, (reg))
#define	MMCI_READ_MULTI(sc, reg, datap, cnt) \
	bus_space_read_multi_4((sc)->sc_bst, (sc)->sc_bsh, (reg), (datap), (cnt))

void
plmmc_init(struct plmmc_softc *sc)
{
	struct sdmmcbus_attach_args saa;
	int error;

	error = bus_dmamap_create(sc->sc_dmat, 0x10000, 1, 0x10000, 0, BUS_DMA_NOWAIT, &sc->sc_dmamap);
	if (error != 0) {
		device_printf(sc->sc_dev, "Could not create IDMA map\n");
		return;
	}
#if 0
	int rsegs;
	error = bus_dmamem_alloc(sc->sc_dmat, 0x10000, 0, 0,
	    &sc->sc_dmaseg, 1, &rsegs, BUS_DMA_NOWAIT);
	if (error != 0 || rsegs != 1) {
		device_printf(sc->sc_dev, "Could not allocate IDMA memory\n");
		return;
	}
	error = bus_dmamem_map(sc->sc_dmat, &sc->sc_dmaseg, 1, 0x10000,
	    &sc->sc_dmamem_va, BUS_DMA_NOWAIT);
	if (error != 0) {
		device_printf(sc->sc_dev, "Could not map IDMA memory\n");
		return;
	}
	error = bus_dmamap_load_raw(sc->sc_dmat, sc->sc_dmamap, &sc->sc_dmaseg,
	    1, PLMMC_MAXXFER, BUS_DMA_NOWAIT);
	if (error != 0) {
		device_printf(sc->sc_dev, "Could not load IDMA memory\n");
		return;
	}
#endif
	error = bus_dmamap_load(sc->sc_dmat, sc->sc_dmamap,
	    &idma_memory[0], 0x10000, NULL, BUS_DMA_WAITOK);
	if (error != 0) {
		device_printf(sc->sc_dev,
		    "Failed to load DMA buffer: %d\n", error);
		return;
	}

	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_BIO);
	cv_init(&sc->sc_intr_cv, "plmmcirq");

#ifdef PLMMC_DEBUG
	device_printf(sc->sc_dev, "PeriphID %#x %#x %#x %#x\n",
	    MMCI_READ(sc, MMCI_PERIPH_ID0_REG),
	    MMCI_READ(sc, MMCI_PERIPH_ID1_REG),
	    MMCI_READ(sc, MMCI_PERIPH_ID2_REG),
	    MMCI_READ(sc, MMCI_PERIPH_ID3_REG));
	device_printf(sc->sc_dev, "PCellID %#x %#x %#x %#x\n",
	    MMCI_READ(sc, MMCI_PCELL_ID0_REG),
	    MMCI_READ(sc, MMCI_PCELL_ID1_REG),
	    MMCI_READ(sc, MMCI_PCELL_ID2_REG),
	    MMCI_READ(sc, MMCI_PCELL_ID3_REG));
#endif

	MMCI_WRITE(sc, MMCI_POWER_REG, 0x2);
	MMCI_WRITE(sc, MMCI_POWER_REG, 0);
	delay(1000);

	plmmc_bus_clock(sc, 400);
	MMCI_WRITE(sc, MMCI_POWER_REG, __SHIFTIN(3, MMCI_POWER_CTRL) | 0x10);
	//MMCI_WRITE(sc, MMCI_POWER_REG, 0xBF);
	delay(10000);
	//plmmc_host_reset(sc);

	memset(&saa, 0, sizeof(saa));
	saa.saa_busname = "sdmmc";
	saa.saa_sct = &plmmc_chip_functions;
	saa.saa_sch = sc;
	saa.saa_clkmin = 400;
	saa.saa_clkmax = sc->sc_max_freq > 0 ?
	    sc->sc_max_freq / 1000 : sc->sc_clock_freq / 1000;
//	saa.saa_caps = SMC_CAPS_4BIT_MODE | SMC_CAPS_SD_HIGHSPEED | SMC_CAPS_SINGLE_ONLY;
	saa.saa_caps = SMC_CAPS_4BIT_MODE | SMC_CAPS_SD_HIGHSPEED;

	sc->sc_sdmmc_dev = config_found(sc->sc_dev, &saa, NULL, CFARGS_NONE);
}

#if 0
static int
plmmc_intr_xfer(struct plmmc_softc *sc, struct sdmmc_command *cmd)
{
	uint32_t len;

	if (cmd == NULL) {
		device_printf(sc->sc_dev, "TX/RX interrupt with no active transfer\n");
		return EINVAL;
	}

	if (cmd->c_buf == NULL) {
		return EINVAL;
	}

	const uint32_t fifo_cnt =
	    __SHIFTOUT(MMCI_READ(sc, MMCI_FIFO_CNT_REG), MMCI_FIFO_CNT) * 4;
	if (fifo_cnt > sc->sc_fifo_resid) {
		device_printf(sc->sc_dev, "FIFO counter is out of sync with active transfer\n");
		return EIO;
	}

	if (cmd->c_flags & SCF_CMD_READ)
		len = sc->sc_fifo_resid - fifo_cnt;
	else
		len = uimin(sc->sc_fifo_resid, PL181_FIFO_DEPTH);

	if (len == 0)
		return 0;

	if (cmd->c_flags & SCF_CMD_READ)
		MMCI_READ_MULTI(sc, MMCI_FIFO_REG, (uint32_t *)cmd->c_buf, len / 4);
	else
		MMCI_WRITE_MULTI(sc, MMCI_FIFO_REG, (uint32_t *)cmd->c_buf, len / 4);

	sc->sc_fifo_resid -= len;
	cmd->c_resid -= len;
	cmd->c_buf += len;

	return 0;
}
#endif

int
plmmc_intr(void *priv)
{
	struct plmmc_softc *sc = priv;
	uint32_t status = 0;
	//uint32_t mask;
	int retry = 10;
	int done = 0;

	mutex_enter(&sc->sc_lock);

	if (sc->assume_transfer_complete) {
		device_printf(sc->sc_dev, "Got interrupt while we already think transfer has finished. Last cmd=%u\n", sc->last_opcode);
		status = MMCI_READ(sc, MMCI_STATUS_REG);
		printf("%s: MMCI_STATUS_REG = %#x\n", __func__, status);
	}

	if (sc->sc_cmd == NULL) {
		device_printf(sc->sc_dev, "Got interrupt while no transfer should be running. Last cmd=%u\n", sc->last_opcode);
		status = MMCI_READ(sc, MMCI_STATUS_REG);
		printf("%s: MMCI_STATUS_REG = %#x\n", __func__, status);
	}

	while (--retry > 0) {
		status = MMCI_READ(sc, MMCI_STATUS_REG);
		//if (sc->sc_cmd != NULL && sc->sc_cmd->c_opcode == 12)
		//	printf("%s: CMD12: MMCI_STATUS_REG = %#x\n", __func__, status);
#ifdef PLMMC_DEBUG
		printf("%s: MMCI_STATUS_REG = %#x\n", __func__, status);
#endif
		if ((status & sc->sc_status_mask) == 0)
			break;
		MMCI_WRITE(sc, MMCI_CLEAR_REG, status & ~__BITS(12,20));
		sc->sc_intr_status |= status;

		if (status & MMCI_INT_CMD_TIMEOUT) {
			done = 1;
			break;
		}

		if (status & (MMCI_INT_DATA_TIMEOUT|MMCI_INT_DATA_CRC_FAIL)) {
			device_printf(sc->sc_dev,
			    "data xfer error, status 0x%08x\n", status);
			if (sc->sc_cmd != NULL)
				sc->sc_cmd->c_resid = 0;
			sc->sc_fifo_resid = 0;
			done = 1;
			break;
		}

		if (status & MMCI_INT_IDMA_COMPLETE) {
			sc->sc_cmd->c_resid = 0;
			sc->sc_fifo_resid = 0;
			done = 1;
			break;
		}
		if (status & MMCI_INT_IDMA_ERROR) {
			device_printf(sc->sc_dev, "IDMA error: 0x%08x\n", status);
			done = 1;
			break;
		}

		if (status & MMCI_INT_DATA_END) {
			sc->sc_cmd->c_resid = 0;
			sc->sc_fifo_resid = 0;
			done = 1;
			break;
		}

		if (status & MMCI_INT_CMD_SENT) {
			done = 1;
			break;
		}
		if (status & MMCI_INT_CMD_RESP_END) {
#ifdef PLMMC_DEBUG
			uint32_t resp = MMCI_READ(sc, MMCI_RESP0_REG);
			device_printf(sc->sc_dev, "Got response R1: 0x%x\n", resp);
#endif
			done = 1;
			break;
		}
		if (status & MMCI_INT_BUSYD0END) {
			done = 1;
			break;
		}
	}
	if (retry == 0) {
		device_printf(sc->sc_dev, "intr handler stuck, fifo resid %d, status %08x\n",
		    sc->sc_fifo_resid, MMCI_READ(sc, MMCI_STATUS_REG));
	}

	if (done || 1) {
		if (sc->sc_cmd == NULL || sc->sc_cmd->c_opcode != 12 ||
		    !(status & MMCI_INT_BUSYD0)) {
			cv_broadcast(&sc->sc_intr_cv);
		}
	}
	mutex_exit(&sc->sc_lock);

	return 1;
}

static int
plmmc_wait_cmd(struct plmmc_softc *sc)
{
	int error = 0;

	KASSERT(mutex_owned(&sc->sc_lock));

	while (error == 0) {
		if (sc->sc_intr_status & MMCI_INT_CMD_TIMEOUT) {
			error = ETIMEDOUT;
			break;
		} else if (sc->sc_intr_status & MMCI_INT_CMD_RESP_END) {
			break;
		}

		error = cv_timedwait(&sc->sc_intr_cv, &sc->sc_lock, hz * 2);
		if (error != 0)
			break;
	}
	sc->assume_transfer_complete = true;

	return error;
}

static int
plmmc_pio_transfer(struct plmmc_softc *sc, struct sdmmc_command *cmd,
    int xferlen)
{
	int error = 0;

	while (sc->sc_fifo_resid > 0 && error == 0) {
#ifdef PLMMC_DEBUG
		//device_printf(sc->sc_dev,
		 //   "%s: MMCI_STATUS_REG = %08x\n", __func__, MMCI_READ(sc, MMCI_STATUS_REG));
#endif
		error = cv_timedwait(&sc->sc_intr_cv,
		    &sc->sc_lock, hz * 5);
		if (error != 0)
			break;

		if (sc->sc_intr_status & MMCI_INT_DATA_TIMEOUT)
			error = ETIMEDOUT;
		else if (sc->sc_intr_status & MMCI_INT_DATA_CRC_FAIL) {
			error = EIO;
		}
	}
	sc->assume_transfer_complete = true;

	return error;
}

static int
plmmc_host_reset(sdmmc_chipset_handle_t sch)
{
	struct plmmc_softc *sc = sch;

	MMCI_WRITE(sc, MMCI_MASK0_REG, 0);
	MMCI_WRITE(sc, MMCI_MASK1_REG, 0);
	MMCI_WRITE(sc, MMCI_CLEAR_REG, 0xffffffff);

	return 0;
}

static uint32_t
plmmc_host_ocr(sdmmc_chipset_handle_t sch)
{
	return MMC_OCR_3_2V_3_3V | MMC_OCR_3_3V_3_4V;
}

static int
plmmc_host_maxblklen(sdmmc_chipset_handle_t sch)
{
	return 16384;
}

static int
plmmc_card_detect(sdmmc_chipset_handle_t sch)
{
	return 1;
}

static int
plmmc_write_protect(sdmmc_chipset_handle_t sch)
{
	return 0;
}

static int
plmmc_bus_power(sdmmc_chipset_handle_t sch, uint32_t ocr)
{
	return 0;
}

static int
plmmc_bus_clock(sdmmc_chipset_handle_t sch, int freq)
{
	struct plmmc_softc *sc = sch;
	u_int pll_freq, clk_div;
	uint32_t clock;

	device_printf(sc->sc_dev,
	    "Device bus clock requested to %dKHz refclock: %dHz\n",
	    freq, sc->sc_clock_freq);
	//if (freq > 800)
	//	freq /= 2;

	clock = MMCI_CLOCK_PWRSAVE;
	clock |= MMCI_CLOCK_NEGEDGE;
	//clock |= MMCI_CLOCK_HWFLOW_EN;
	if (freq) {
		pll_freq = sc->sc_clock_freq / 1000;
		// TODO: Still not quite right
		pll_freq = roundup(pll_freq, freq);
		clk_div = (pll_freq/freq) >> 1;
		device_printf(sc->sc_dev, "Programming clock divisor %d\n",
		    clk_div == 0 ? 1 : clk_div << 1);
		clock |= __SHIFTIN(clk_div, MMCI_CLOCK_CLKDIV);
	}
	MMCI_WRITE(sc, MMCI_CLOCK_REG, clock);

	return 0;
}

static int
plmmc_bus_width(sdmmc_chipset_handle_t sch, int width)
{
	struct plmmc_softc *sc = sch;
	uint32_t val;

	device_printf(sc->sc_dev, "Device bus width requested to %d\n", width);

	if (width != 1 && width != 4)
		return EINVAL;

	val = MMCI_READ(sc, MMCI_CLOCK_REG);
	val &= MMCI_CLOCK_WIDBUS;
	if (width == 1)
		val |= __SHIFTIN(0, MMCI_CLOCK_WIDBUS);
	else if (width == 4)
		val |= __SHIFTIN(1, MMCI_CLOCK_WIDBUS);
	MMCI_WRITE(sc, MMCI_CLOCK_REG, val);
	return 0;
}

static int
plmmc_bus_rod(sdmmc_chipset_handle_t sch, int on)
{
	if (on != 0)
		return EINVAL;

	return 0;
}

static void
plmmc_do_command(sdmmc_chipset_handle_t sch, struct sdmmc_command *cmd)
{
	struct plmmc_softc *sc = sch;
	uint32_t cmdval = MMCI_COMMAND_ENABLE;

	KASSERT(mutex_owned(&sc->sc_lock));

	uint32_t status = MMCI_READ(sc, MMCI_STATUS_REG);
	//uint32_t datacount = MMCI_READ(sc, MMCI_DATA_CNT_REG);
	if ((status & 0x3000) && cmd->c_opcode != 12) {
		int retries = 0;
		device_printf(sc->sc_dev, "Controller still active: 0x%x\n",
		  status);
		while (MMCI_READ(sc, MMCI_STATUS_REG) & 0x3000) {
			//cv_timedwait(&sc->sc_intr_cv, &sc->sc_lock, 1);
			retries++;
			if (retries > 10) {
				status = MMCI_READ(sc, MMCI_STATUS_REG);
				device_printf(sc->sc_dev, "Controller still active: 0x%x, giving up\n",
				    status);
				cmd->c_error = 50;
				return;
			}
		}
	}

	const int xferlen = uimin(cmd->c_resid, PLMMC_MAXXFER);

	sc->sc_cmd = cmd;
	sc->sc_fifo_resid = xferlen;
	sc->sc_status_mask = ~0U;
	sc->sc_intr_status = 0;
	sc->last_opcode = cmd->c_opcode;

#if 0
	if (cmd->c_opcode == 55 || cmd->c_opcode == 6) {
		device_printf(sc->sc_dev,
		    "opcode %d flags %#x arg %#x datalen %d resid %d xferlen %d\n",
		    cmd->c_opcode, cmd->c_flags, cmd->c_arg, cmd->c_datalen, cmd->c_resid, xferlen);
	}
#endif

#ifdef PLMMC_DEBUG
	device_printf(sc->sc_dev,
	    "opcode %d flags %#x datalen %d resid %d xferlen %d\n",
	    cmd->c_opcode, cmd->c_flags, cmd->c_datalen, cmd->c_resid, xferlen);
	device_printf(sc->sc_dev,
	    "Command register before starting command: 0x%x\n",
	    MMCI_READ(sc, MMCI_COMMAND_REG));
	if (xferlen > 0)
		device_printf(sc->sc_dev, "Data %s transfer\n",
		    (cmd->c_flags & SCF_CMD_READ) ? "READ" : "WRITE");
#endif

	//MMCI_WRITE(sc, MMCI_COMMAND_REG, 0);
	//MMCI_WRITE(sc, MMCI_MASK0_REG, 0);
	//MMCI_WRITE(sc, MMCI_CLEAR_REG, 0xffffffff);
	MMCI_WRITE(sc, MMCI_MASK0_REG, PLMMC_INT_DATA_MASK | PLMMC_INT_CMD_MASK);

	if (cmd->c_flags & SCF_RSP_PRESENT) {
#ifdef PLMMC_DEBUG
		device_printf(sc->sc_dev, "Expecting a response\n");
#endif
		if (cmd->c_flags & SCF_RSP_136) {
			cmdval |= __SHIFTIN(3, MMCI_COMMAND_WAITRESP);
		} else {
			// Disable CRC check for ACMD41
			//if (cmd->c_opcode == 41)
				cmdval |= __SHIFTIN(2, MMCI_COMMAND_WAITRESP);
			//else
			//	cmdval |= __SHIFTIN(1, MMCI_COMMAND_WAITRESP);
		}
	}

	if (cmd->c_opcode == 12)
		cmdval |= MMCI_COMMAND_STOP;

	uint32_t arg = cmd->c_arg;

	if (xferlen > 0) {
		unsigned int nblks = xferlen / cmd->c_blklen;
		if (nblks == 0 || (xferlen % cmd->c_blklen) != 0)
			++nblks;

		const uint32_t dir = (cmd->c_flags & SCF_CMD_READ) ? 1 : 0;
		const uint32_t blksize = ffs(cmd->c_blklen) - 1;

		MMCI_WRITE(sc, MMCI_DATA_TIMER_REG, 0xffffffff);
		MMCI_WRITE(sc, MMCI_DATA_LENGTH_REG, nblks * cmd->c_blklen);
		uint32_t datactrl = 
		    __SHIFTIN(dir, MMCI_DATA_CTRL_DIRECTION) |
		    __SHIFTIN(blksize, MMCI_DATA_CTRL_BLOCKSIZE);
		if (cmd->c_opcode == 18 || cmd->c_opcode == 25)
			datactrl |= __SHIFTIN(3, MMCI_DATA_CTRL_DTMODE);
		MMCI_WRITE(sc, MMCI_DATA_CTRL_REG, datactrl);

		/* Adjust blkno if necessary */
		u_int blkoff =
		    (cmd->c_datalen - cmd->c_resid) / SDMMC_SECTOR_SIZE;
		if (!ISSET(cmd->c_flags, SCF_XFER_SDHC))
			blkoff <<= SDMMC_SECTOR_SIZE_SB;
		arg += blkoff;
		cmdval |= MMCI_COMMAND_DATA;

		// Use IDMA
		MMCI_WRITE(sc, MMCI_DMA_CTRL, __BIT(0));
		//uint32_t val;
		//int error;
		//val = roundup(xferlen, 32) / 32;
		//MMCI_WRITE(sc, MMCI_IDMA_BUFSIZE, __SHIFTIN(val, MMCI_IDMA_BNDT));
		if (!dir)
			memcpy((void *)&idma_memory[0], cmd->c_buf, xferlen);
		else
			memset((void *)&idma_memory[0], 0, xferlen);
		//bus_dmamap_sync(sc->sc_dmat, sc->sc_dmamap, 0, xferlen,
		bus_dmamap_sync(sc->sc_dmat, sc->sc_dmamap, 0, xferlen,
		    dir ? BUS_DMASYNC_PREREAD : BUS_DMASYNC_PREWRITE);
		//MMCI_WRITE(sc, MMCI_IDMA_BUFADDR, sc->sc_dmaseg.ds_addr);
#ifdef PLMMC_DEBUG
		device_printf(sc->sc_dev, "Programming IDMA buffer address: 0x%lx\n",
		    sc->sc_dmamap->dm_segs[0].ds_addr);
#endif
		MMCI_WRITE(sc, MMCI_IDMA_BUFADDR, sc->sc_dmamap->dm_segs[0].ds_addr);
	} else {
		MMCI_WRITE(sc, MMCI_DATA_CTRL_REG, 0);
	}

	sc->assume_transfer_complete = false;
	MMCI_WRITE(sc, MMCI_ARGUMENT_REG, arg);
	MMCI_WRITE(sc, MMCI_COMMAND_REG, cmdval | cmd->c_opcode);

	// XXX Test whether it's a timing issue
	//kpause("plmmc", 0, 1, &sc->sc_lock);

	if (xferlen > 0) {
		cmd->c_error = plmmc_pio_transfer(sc, cmd, xferlen);
		if (cmd->c_error) {
#ifdef PLMMC_DEBUG
			device_printf(sc->sc_dev,
			    "MMCI_STATUS_REG = %08x\n", MMCI_READ(sc, MMCI_STATUS_REG));
			device_printf(sc->sc_dev,
			    "MMCI_DATA_CNT = %08x\n", MMCI_READ(sc, MMCI_DATA_CNT_REG) & 0x1ffffff);
			device_printf(sc->sc_dev,
			    "error (%d) waiting for xfer\n", cmd->c_error);
#endif
			goto done;
		}
	}

	if ((cmd->c_flags & SCF_RSP_PRESENT) && cmd->c_resid == 0) {
		cmd->c_error = plmmc_wait_cmd(sc);
		if (cmd->c_error) {
#ifdef PLMMC_DEBUG
			device_printf(sc->sc_dev,
			    "error (%d) waiting for resp\n", cmd->c_error);
#endif
			goto done;
		}

		if (cmd->c_flags & SCF_RSP_136) {
			cmd->c_resp[3] = MMCI_READ(sc, MMCI_RESP0_REG);
			cmd->c_resp[2] = MMCI_READ(sc, MMCI_RESP1_REG);
			cmd->c_resp[1] = MMCI_READ(sc, MMCI_RESP2_REG);
			cmd->c_resp[0] = MMCI_READ(sc, MMCI_RESP3_REG);
			if (cmd->c_flags & SCF_RSP_CRC) {
				cmd->c_resp[0] = (cmd->c_resp[0] >> 8) |
				    (cmd->c_resp[1] << 24);
				cmd->c_resp[1] = (cmd->c_resp[1] >> 8) |
				    (cmd->c_resp[2] << 24);
				cmd->c_resp[2] = (cmd->c_resp[2] >> 8) |
				    (cmd->c_resp[3] << 24);
				cmd->c_resp[3] = (cmd->c_resp[3] >> 8);
			}
		} else {
			cmd->c_resp[0] = MMCI_READ(sc, MMCI_RESP0_REG);
		}
	}
	if (xferlen > 0) {
		const uint32_t dir = (cmd->c_flags & SCF_CMD_READ) ? 1 : 0;
		bus_dmamap_sync(sc->sc_dmat, sc->sc_dmamap, 0, xferlen,
		    dir ? BUS_DMASYNC_POSTREAD : BUS_DMASYNC_POSTWRITE);
		if (dir)
			memcpy(cmd->c_buf, (void *)&idma_memory[0], xferlen);
	}

done:
	sc->sc_cmd = NULL;
#if 0
	if (MMCI_READ(sc, MMCI_STATUS_REG) & 0x3000) {
		device_printf(sc->sc_dev,
		    "State Machine still active: 0x%08x\n",
		    MMCI_READ(sc, MMCI_STATUS_REG));
	}
#endif

	//MMCI_WRITE(sc, MMCI_COMMAND_REG, 0);
	//MMCI_WRITE(sc, MMCI_MASK0_REG, 0);
	//MMCI_WRITE(sc, MMCI_CLEAR_REG, 0xffffffff);
	//MMCI_WRITE(sc, MMCI_DATA_CNT_REG, 0);

#ifdef PLMMC_DEBUG
	device_printf(sc->sc_dev, "status = %#x\n", sc->sc_intr_status);
	device_printf(sc->sc_dev, "status = %#x\n", MMCI_READ(sc, MMCI_STATUS_REG));
#endif
}

static void
plmmc_exec_command(sdmmc_chipset_handle_t sch, struct sdmmc_command *cmd)
{
	struct plmmc_softc *sc = sch;

#ifdef PLMMC_DEBUG
	device_printf(sc->sc_dev, "opcode %d flags %#x data %p datalen %d\n",
	    cmd->c_opcode, cmd->c_flags, cmd->c_data, cmd->c_datalen);
#endif

#if 0
	uint32_t status = MMCI_READ(sc, MMCI_STATUS_REG);
	uint32_t datacount = MMCI_READ(sc, MMCI_DATA_CNT_REG);
	if ((status & 0x3000) && cmd->c_opcode != 12) {
		device_printf(sc->sc_dev, "Controller still active: 0x%x datacount: 0x%x lastcmd: %u\n",
		  status, datacount, sc->last_opcode);
		int retries = 0;
		while (MMCI_READ(sc, MMCI_STATUS_REG) & 0x3000) {
			retries++;
			if (retries > 100) {
				//device_printf(sc->sc_dev, "Controller still active: 0x%x giving up\n",
				//  status);
				break;
			}
			delay(1);
		}
		while (MMCI_READ(sc, MMCI_STATUS_REG) & 0x3000) {
			retries++;
			if (retries > 110) {
				device_printf(sc->sc_dev, "Controller still active: 0x%x giving up\n",
				  status);
				break;
			}
			if (kpause("plmmc", true, 1, NULL) != EWOULDBLOCK)
				break;
		}
	}
#endif

	mutex_enter(&sc->sc_lock);
	cmd->c_resid = cmd->c_datalen;
	cmd->c_buf = cmd->c_data;
//	do {
		plmmc_do_command(sch, cmd);
#if 0
		if (cmd->c_resid > 0 && cmd->c_error == 0) {
			/*
			 * Multi block transfer and there is still data
			 * remaining. Send a stop cmd between transfers.
			 */
			struct sdmmc_command stop_cmd;
			memset(&stop_cmd, 0, sizeof(stop_cmd));
			stop_cmd.c_opcode = MMC_STOP_TRANSMISSION;
			stop_cmd.c_flags = SCF_CMD_AC | SCF_RSP_R1B |
			    SCF_RSP_SPI_R1B;
			plmmc_do_command(sch, &stop_cmd);
		}
	} while (cmd->c_resid > 0 && cmd->c_error == 0);
#endif
	cmd->c_flags |= SCF_ITSDONE;
	mutex_exit(&sc->sc_lock);
}

static void
plmmc_card_enable_intr(sdmmc_chipset_handle_t sch, int enable)
{
}

static void
plmmc_card_intr_ack(sdmmc_chipset_handle_t sch)
{
}
