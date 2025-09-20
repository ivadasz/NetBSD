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
//#include <sys/proc.h>

#include <dev/i2c/i2cvar.h>

#include <dev/fdt/fdtvar.h>

// Default Clock Frequency is usually HSI clock, i.e. 64MHz

#define I2C_CR1 0
#define I2C_PE	__BIT(0)
#define I2C_TXIE __BIT(1)
#define I2C_RXIE __BIT(2)
#define I2C_TCIE __BIT(6)
#define I2C_ERRIE __BIT(7)
#define I2C_DNF __BITS(8,11)
#define I2C_ANFOFF __BIT(12)

#define I2C_CR2 0x4
#define I2C_SADD7 __BITS(1,7)
#define I2C_RD_WRN __BIT(10)
#define I2C_START __BIT(13)
#define I2C_STOP __BIT(14)
#define I2C_NBYTES __BITS(16,23)
#define I2C_RELOAD __BIT(24)
#define I2C_AUTOEND __BIT(25)

#define I2C_OAR1 0x8

#define I2C_OAR2 0xC

#define I2C_TIMINGR 0x10
#define I2C_SCLL __BITS(0,7)
#define I2C_SCLH __BITS(8,15)
#define I2C_SDADEL __BITS(16,19)
#define I2C_SCLDEL __BITS(20,23)
#define I2C_PRESCL __BITS(28,31)

#define I2C_TIMEOUTR 0x14

#define I2C_ISR 0x18
#define I2C_TCR __BIT(7)
#define I2C_TC __BIT(6)
#define I2C_RXNE __BIT(2)
#define I2C_TXIS __BIT(1)

#define I2C_ICR 0x1C

#define I2C_RXDR 0x24

#define I2C_TXDR 0x28

static int stmp1iic_match(device_t, cfdata_t, void *);
static void stmp1iic_attach(device_t, device_t, void *);

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "st,stm32mp15-i2c" },
	DEVICE_COMPAT_EOL
};

struct stmp1iic_softc {
	device_t		sc_dev;
	int			sc_phandle;
	struct fdtbus_reset	*sc_rst;
	struct clk		*sc_clk;
	struct i2c_controller	sc_ic;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	void			*sc_ih_event;
	void			*sc_ih_error;

	kmutex_t		sc_lock;
	kcondvar_t		sc_cv;

	bool			sc_busy;
	int			sc_flags;

	// RX or TX Buffer
	u_char			*sc_buf;
	uint8_t			sc_pos;
	uint8_t			sc_len;
	bool			sc_done;

	u_char			*sc_restart_buf;
	uint8_t			sc_restart_len;
	bool			sc_restart_iswrite;

	int			sc_error;
};

static int
stmp1iic_event_intr(void *arg)
{
	struct stmp1iic_softc *sc = arg;
	uint32_t val;


	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_ISR);
	//aprint_normal_dev(sc->sc_dev, "Interrupt: I2C_ISR=0x%x\n", val);
	if (val & I2C_TCR) {
		// Not handled yet, and shouldn't occur if RELOAD=0
	}
	if (val & I2C_TC) {
		mutex_enter(&sc->sc_lock);
		sc->sc_buf = sc->sc_restart_buf;
		sc->sc_len = sc->sc_restart_len;
		sc->sc_pos = 0;
		sc->sc_restart_buf = NULL;
		// Implement Transmission (re)start part.
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR2);
		val &= ~I2C_RD_WRN;
		if (!sc->sc_restart_iswrite)
			val |= I2C_RD_WRN;
		val &= ~I2C_NBYTES;
		val |= __SHIFTIN(sc->sc_len, I2C_NBYTES);
		// Always auto-stop, whatever flags claim.
		val |= I2C_AUTOEND;
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_CR2, val);
		mutex_exit(&sc->sc_lock);
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR2);
		val |= I2C_START;
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_CR2, val);
		//aprint_normal_dev(sc->sc_dev, "Restarted transfer\n");
		return 0;
	}
	if (val & I2C_RXNE) {
		mutex_enter(&sc->sc_lock);
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_RXDR);
		if (sc->sc_buf != NULL)
			sc->sc_buf[sc->sc_pos] = val & __BITS(0,7);
		sc->sc_pos++;
		if (sc->sc_pos == sc->sc_len) {
			sc->sc_done = TRUE;
			mutex_exit(&sc->sc_lock);
			cv_signal(&sc->sc_cv);
		} else {
			mutex_exit(&sc->sc_lock);
		}
		return 0;
		
	}
	if (val & I2C_TXIS) {
		mutex_enter(&sc->sc_lock);
		val = 0;
		if (sc->sc_buf != NULL) {
			val = sc->sc_buf[sc->sc_pos];
		}
		sc->sc_pos++;
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_TXDR, val);
		if (sc->sc_pos == sc->sc_len && sc->sc_restart_buf == NULL) {
			sc->sc_done = TRUE;
			mutex_exit(&sc->sc_lock);
			cv_signal(&sc->sc_cv);
		} else {
			mutex_exit(&sc->sc_lock);
		}
		return 0;
	}

	aprint_normal_dev(sc->sc_dev, "Unhandled interrupt: 0x%x\n", val);

	return 0;
}

static int
stmp1iic_error_intr(void *arg)
{
	struct stmp1iic_softc *sc = arg;
	uint32_t val;

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_ISR);
	aprint_normal_dev(sc->sc_dev, "I2C_ISR=0x%x\n", val);
	// TODO: print error type
	// TODO: clear interrupt.
	// TODO: Abort current transfer (signal stmp1iic_op to complete with error).
	mutex_enter(&sc->sc_lock);
	sc->sc_error = 1;
	sc->sc_done = TRUE;
	mutex_exit(&sc->sc_lock);
	cv_signal(&sc->sc_cv);
	aprint_normal_dev(sc->sc_dev, "Got error interrupt: 0x%x\n", val);
	// Clear any interrupts.
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_ICR, val);

	return 0;
}

// if restart is non-NULL, is_write applies to the restart transfer.
// TODO: Make sure that flags don't contradict this.
static int
stmp1iic_op(struct stmp1iic_softc *sc, i2c_addr_t addr, bool is_write,
    uint8_t *buf, size_t buflen, uint8_t *restart, size_t restart_buflen,
    int flags)
{
	uint32_t val;
	int err;
	int retries = 0;

#if 0
	aprint_normal_dev(sc->sc_dev,
	    "addr %x op %s buf %p buflen %d restart %p restart_buflen %d"
	    " flags %x\n",
	    addr, is_write ? "WRITE" : "READ", buf, (unsigned int) buflen,
	    restart, (unsigned int) restart_buflen,
	    flags);
	if (buflen == 2 && is_write)
		aprint_normal_dev(sc->sc_dev, "Writing command 0x%x data 0x%x\n", buf[0], buf[1]);
#endif

	// buflen > 255 needs special "reload" handling.
	if (buflen > 255 || restart_buflen > 255)
		return EINVAL;

	mutex_enter(&sc->sc_lock);
//retry:
	sc->sc_pos = 0;
	sc->sc_buf = buf;
	sc->sc_len = buflen;
	sc->sc_restart_buf = restart;
	sc->sc_restart_len = restart_buflen;
	sc->sc_restart_iswrite = is_write;
	sc->sc_flags = flags;
	sc->sc_error = 0;
	sc->sc_done = FALSE;
	// Implement Transmission start part.
	val = 0;
	val |= __SHIFTIN(addr, I2C_SADD7);
	if (!is_write && restart == NULL)
		val |= I2C_RD_WRN;
	val |= __SHIFTIN(buflen, I2C_NBYTES);
	if (restart == NULL)
		val |= I2C_AUTOEND;

	clk_enable(sc->sc_clk);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_CR2, val);
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR2);
	val |= I2C_START;
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_CR2, val);

	// TODO: Implement polling.

	while ((err = cv_timedwait(&sc->sc_cv, &sc->sc_lock, 10)) == 0 &&
	    !sc->sc_done)
		continue;
	if (err != 0) {
		aprint_normal_dev(sc->sc_dev, "timeout\n");
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR2);
		aprint_normal_dev(sc->sc_dev, "I2C_CR2=0x%x\n", val);
		val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_ISR);
		aprint_normal_dev(sc->sc_dev, "I2C_ISR=0x%x\n", val);
		if (val & __BIT(4)) {
			retries++;
			//if (retries < 10)
			//	goto retry;
		}
	}
	clk_disable(sc->sc_clk);
	sc->sc_buf = NULL;
	sc->sc_restart_buf = NULL;
	if (sc->sc_error != 0)
		err = EAGAIN; // ??
	mutex_exit(&sc->sc_lock);

	return err;
}

static int
stmp1iic_acquire_bus(void *opaque, int flags)
{
	struct stmp1iic_softc *sc = opaque;

	mutex_enter(&sc->sc_lock);
	while (sc->sc_busy)
		cv_wait(&sc->sc_cv, &sc->sc_lock);
	sc->sc_busy = true;
	mutex_exit(&sc->sc_lock);

	return 0;
}

static void
stmp1iic_release_bus(void *opaque, int flags)
{
	struct stmp1iic_softc *sc = opaque;

	mutex_enter(&sc->sc_lock);
	sc->sc_busy = false;
	cv_broadcast(&sc->sc_cv);
	mutex_exit(&sc->sc_lock);
}

static int
stmp1iic_exec(void *opaque, i2c_op_t op, i2c_addr_t addr,
    const void *cmdbuf, size_t cmdlen, void *buf, size_t len, int flags)
{
	struct stmp1iic_softc *sc = opaque;
	uint8_t mybuf[255];
	int err;

#if 0
	aprint_normal_dev(sc->sc_dev,
	    "op 0x%x cmdlen %zd len %zd flags 0x%x\n",
	    op, cmdlen, len, flags);
#endif

	/*
	 * I2C controller doesn't allow for zero-byte transfers.
	 */
	if (len == 0) {
		err = EINVAL;
		goto done;
	}

	if (cmdlen > 0 && I2C_OP_WRITE_P(op)) {
		if (cmdlen + len > 255) {
			err = EINVAL;
			goto done;
		}
		memcpy(mybuf, cmdbuf, cmdlen);
		memcpy(&mybuf[cmdlen], buf, len);
		err = stmp1iic_op(sc, addr, TRUE, mybuf, cmdlen + len,
		    NULL, 0, flags);
		goto done;
	}

	if (cmdlen > 0) {
		err = stmp1iic_op(sc, addr, I2C_OP_WRITE_P(op),
		    __UNCONST(cmdbuf), cmdlen, buf, len, flags);
	} else {
		if (I2C_OP_READ_P(op)) {
			err = stmp1iic_op(sc, addr, FALSE, buf, len, NULL, 0, flags);
		} else {
			err = stmp1iic_op(sc, addr, TRUE, buf, len, NULL, 0, flags);
		}
	}

done:
	//if (err)
	//	ti_iic_reset(sc);

	//ti_iic_flush(sc);

	//aprint_normal_dev(sc->sc_dev, "done %d\n", err);
	return err;
}

static void
stmp1iic_attach_late(device_t self)
{
	struct stmp1iic_softc * const sc = device_private(self);

	// Register i2cbus
	iicbus_attach(self, &sc->sc_ic);

	clk_disable(sc->sc_clk);
}

CFATTACH_DECL_NEW(stmp1iic, sizeof(struct stmp1iic_softc),
	stmp1iic_match, stmp1iic_attach, NULL, NULL);

static int
stmp1iic_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;

	return of_compatible_match(faa->faa_phandle, compat_data);
}

static void
stmp1iic_attach(device_t parent, device_t self, void *aux)
{
	struct stmp1iic_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	sc->sc_phandle = faa->faa_phandle;
	bus_addr_t addr;
	bus_size_t size;

	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NET);
	cv_init(&sc->sc_cv, "stmp1iic");
	iic_tag_init(&sc->sc_ic);
	sc->sc_ic.ic_cookie = sc;
	sc->sc_ic.ic_acquire_bus = stmp1iic_acquire_bus;
	sc->sc_ic.ic_release_bus = stmp1iic_release_bus;
	sc->sc_ic.ic_exec = stmp1iic_exec;

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

	sc->sc_clk = fdtbus_clock_get_index(phandle, 0);
	if (sc->sc_clk == NULL) {
		aprint_error(": couldn't enable Peripheral\n");
		return;
	}
	clk_enable(sc->sc_clk);

	char intrstr[128];
	if (!fdtbus_intr_str(phandle, 0, intrstr, sizeof(intrstr))) {
		aprint_error_dev(self, "failed to decode event interrupt\n");
		return;
	}

	sc->sc_ih_event = fdtbus_intr_establish_xname(phandle, 0, IPL_NET, 0,
	    stmp1iic_event_intr, sc, device_xname(self));
	if (sc->sc_ih_event == NULL) {
		aprint_error_dev(self,
		    "couldn't establish event interrupt on %s\n", intrstr);
		return;
	}

	if (!fdtbus_intr_str(phandle, 1, intrstr, sizeof(intrstr))) {
		aprint_error_dev(self, "failed to decode event interrupt\n");
		return;
	}

	sc->sc_ih_error = fdtbus_intr_establish_xname(phandle, 1, IPL_NET, 0,
	    stmp1iic_error_intr, sc, device_xname(self));
	if (sc->sc_ih_error == NULL) {
		aprint_error_dev(self,
		    "couldn't establish event interrupt on %s\n", intrstr);
		return;
	}

	aprint_naive("\n");
	aprint_normal(": STM32MP1 I2C Controller\n");

#if 1
	sc->sc_rst = fdtbus_reset_get_index(phandle, 0);

	/* Apply Reset */
	if (sc->sc_rst != NULL) {
		int error;
		aprint_normal_dev(self, "Applying reset\n");
		error = fdtbus_reset_assert(sc->sc_rst);
		if (error != 0) {
			aprint_error(": couldn't assert reset\n");
			//return error;
		}
		delay(20000);
		error = fdtbus_reset_deassert(sc->sc_rst);
		if (error != 0) {
			aprint_error(": couldn't de-assert reset\n");
			//return error;
		}
		delay(20000);
	}
#endif

	aprint_normal_dev(self, "Initial I2C_CR1: 0x%x I2C_CR2: 0x%x I2C_TIMINGR: 0x%x I2C_ISR: 0x%x\n",
	    bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR1),
	    bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR2),
	    bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_TIMINGR),
	    bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_ISR));

	uint32_t val;
	// Disable peripheral (Start reset)
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR1);
	val &= ~__BITS(0,23);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_CR1, val);
	// Read back once to make sure we're holding the reset long enough
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR1);
	// TODO: Consider configuring digital filtering in I2C_CR1
	// Analog filtering is on by default
	// Configure I2C timings for 10KHz according to example in manual
#if 1
	// Example from ST reference documentation
	val = __SHIFTIN(0xF, I2C_PRESCL) |
	      __SHIFTIN(0x13, I2C_SCLL) |
	      __SHIFTIN(0xF, I2C_SCLH) |
	      __SHIFTIN(0x2, I2C_SDADEL) |
	      __SHIFTIN(0x4, I2C_SCLDEL);
#else
	// Possibly wrong value from CubeMX tool
	val = 0x10D07DB5;
#endif
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_TIMINGR, val);
	// Enable peripheral
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR1);
	val |= I2C_PE;
#if 1
	val |= I2C_ERRIE;
	val |= I2C_TXIE;
	val |= I2C_RXIE;
	val |= I2C_TCIE;
#endif
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_CR1, val);
	delay(20000);

#if 0
	// Enable relevant interrupts
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR1);
	val |= I2C_ERRIE;
	val |= I2C_TXIE;
	val |= I2C_RXIE;
	val |= I2C_TCIE;
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_CR1, val);
#endif
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_TIMEOUTR, 0);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, I2C_CR2, 0);

	aprint_normal_dev(self, "After attach I2C_CR1: 0x%x I2C_CR2: 0x%x I2C_TIMINGR: 0x%x I2C_ISR: 0x%x\n",
	    bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR1),
	    bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_CR2),
	    bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_TIMINGR),
	    bus_space_read_4(sc->sc_bst, sc->sc_bsh, I2C_ISR));

	config_interrupts(self, stmp1iic_attach_late);
}
