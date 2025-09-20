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
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/intr.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/kauth.h>
#include <sys/lwp.h>
#include <sys/tty.h>

#include <dev/cons.h>

#include <dev/fdt/fdtvar.h>
#include <dev/fdt/fdt_console.h>

static int stmp1uart_match(device_t, cfdata_t, void *);
static void stmp1uart_attach(device_t, device_t, void *);

static void	stmp1_uart_start(struct tty *);
static int	stmp1_uart_param(struct tty *, struct termios *);

extern struct cfdriver stmp1uart_cd;

static struct cnm_state stmp1_uart_cnm_state;

static dev_type_open(stmp1_uart_open);
static dev_type_open(stmp1_uart_close);
static dev_type_read(stmp1_uart_read);
static dev_type_write(stmp1_uart_write);
static dev_type_ioctl(stmp1_uart_ioctl);
static dev_type_tty(stmp1_uart_tty);
static dev_type_poll(stmp1_uart_poll);
static dev_type_stop(stmp1_uart_stop);

const struct cdevsw stmp1_uart_cdevsw = {
	.d_open = stmp1_uart_open,
	.d_close = stmp1_uart_close,
	.d_read = stmp1_uart_read,
	.d_write = stmp1_uart_write,
	.d_ioctl = stmp1_uart_ioctl,
	.d_stop = stmp1_uart_stop,
	.d_tty = stmp1_uart_tty,
	.d_poll = stmp1_uart_poll,
	.d_mmap = nommap,
	.d_kqfilter = ttykqfilter,
	.d_discard = nodiscard,
	.d_flag = D_TTY
};

static int stmp1_uart_cmajor = -1;

static const struct device_compatible_entry compat_data[] = {
	{ .compat = "st,stm32h7-uart" },
	DEVICE_COMPAT_EOL
};

struct stmp1uart_softc {
	device_t		sc_dev;
	int			phandle;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	void			*sc_ih;
	void			*sc_sih;
	struct tty		*sc_tty;
	uint8_t			rxring[32];
	uint8_t			rxring_len;
	uint8_t			rxring_pos;
	uint8_t			rxring_cnt;
	u_char			buf[1024];
};

static struct stmp1uart_softc console_sc_alloc;
static struct stmp1uart_softc *console_sc = NULL;

static void
stmp1uart_rxsoft(void *priv)
{
	struct stmp1uart_softc *sc = priv;
	struct tty *tp = sc->sc_tty;
	int c;

	while (sc->rxring_cnt > 0) {
		sc->rxring_pos = (sc->rxring_pos - 1) % sc->rxring_len;
		sc->rxring_cnt--;
		c = sc->rxring[sc->rxring_pos];
		if (tp->t_linesw->l_rint(c & 0xff, tp) == -1)
			break;
	}
}

static int
stmp1_uart_open(dev_t dev, int flag, int mode, lwp_t *l)
{
	struct stmp1uart_softc *sc =
	    device_lookup_private(&stmp1uart_cd, minor(dev));
	struct tty *tp = sc->sc_tty;
#if 0
	uint32_t control;
#endif

	if (kauth_authorize_device_tty(l->l_cred,
	    KAUTH_DEVICE_TTY_OPEN, tp) != 0) {
		return EBUSY;
	}

	if ((tp->t_state & TS_ISOPEN) == 0 && tp->t_wopen == 0) {
		tp->t_dev = dev;
		ttychars(tp);
		tp->t_iflag = TTYDEF_IFLAG;
		tp->t_oflag = TTYDEF_OFLAG;
		tp->t_cflag = TTYDEF_CFLAG;
		tp->t_lflag = TTYDEF_LFLAG;
		tp->t_ispeed = tp->t_ospeed = TTYDEF_SPEED;
		ttsetwater(tp);

#if 0
		// Meson Uart starts interrupts when tty opened.
		control = bus_space_read_4(sc->sc_bst, sc->sc_bsh, UART_CONTROL_REG);
		control |= UART_CONTROL_RX_INT_EN;
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, UART_CONTROL_REG, control);
#endif
	}
	tp->t_state |= TS_CARR_ON;

	return tp->t_linesw->l_open(dev, tp);
}

static int
stmp1_uart_close(dev_t dev, int flag, int mode, lwp_t *l)
{
	struct stmp1uart_softc *sc =
	    device_lookup_private(&stmp1uart_cd, minor(dev));
	struct tty *tp = sc->sc_tty;
#if 0
	uint32_t control;
#endif

	tp->t_linesw->l_close(tp, flag);
	ttyclose(tp);

#if 0
	// Meson Uart stops interrupts when tty closed.
	if (!ISSET(tp->t_state, TS_ISOPEN) && tp->t_wopen == 0) {
		control = bus_space_read_4(sc->sc_bst, sc->sc_bsh, UART_CONTROL_REG);
		control &= ~UART_CONTROL_RX_INT_EN;
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, UART_CONTROL_REG, control);
	}
#endif

	return 0;
}

static int
stmp1_uart_read(dev_t dev, struct uio *uio, int flag)
{
	struct stmp1uart_softc *sc =
	    device_lookup_private(&stmp1uart_cd, minor(dev));
	struct tty *tp = sc->sc_tty;

	return tp->t_linesw->l_read(tp, uio, flag);
}

static int
stmp1_uart_write(dev_t dev, struct uio *uio, int flag)
{
	struct stmp1uart_softc *sc =
	    device_lookup_private(&stmp1uart_cd, minor(dev));
	struct tty *tp = sc->sc_tty;

	return tp->t_linesw->l_write(tp, uio, flag);
}

static int
stmp1_uart_poll(dev_t dev, int events, lwp_t *l)
{
	struct stmp1uart_softc *sc =
	    device_lookup_private(&stmp1uart_cd, minor(dev));
	struct tty *tp = sc->sc_tty;

	return tp->t_linesw->l_poll(tp, events, l);
}

static int
stmp1_uart_ioctl(dev_t dev, u_long cmd, void *data, int flag, lwp_t *l)
{
	struct stmp1uart_softc *sc =
	    device_lookup_private(&stmp1uart_cd, minor(dev));
	struct tty *tp = sc->sc_tty;
	int error;

	error = tp->t_linesw->l_ioctl(tp, cmd, data, flag, l);
	if (error != EPASSTHROUGH)
		return error;

	return ttioctl(tp, cmd, data, flag, l);
}

static struct tty *
stmp1_uart_tty(dev_t dev)
{
	struct stmp1uart_softc *sc =
	    device_lookup_private(&stmp1uart_cd, minor(dev));

	return sc->sc_tty;
}

static void
stmp1_uart_stop(struct tty *tp, int flag)
{
}

static void
stmp1_uart_start(struct tty *tp)
{
	struct stmp1uart_softc *sc = tp->t_sc;
	u_char *p = sc->buf;
	int s, brem;

	s = spltty();

	if (tp->t_state & (TS_TTSTOP | TS_BUSY | TS_TIMEOUT)) {
		splx(s);
		return;
	}
	tp->t_state |= TS_BUSY;

	splx(s);

	for (brem = q_to_b(&tp->t_outq, sc->buf, sizeof(sc->buf));
	    brem > 0; brem--, p++) {
		while (!(bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x1c) & __BIT(7)))
			continue;
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x28, *p);
	}

	s = spltty();
	tp->t_state &= ~TS_BUSY;
	if (ttypull(tp)) {
		tp->t_state |= TS_TIMEOUT;
		callout_schedule(&tp->t_rstrt_ch, 1);
	}
	splx(s);
}

static int
stmp1_uart_param(struct tty *tp, struct termios *t)
{

	tp->t_ispeed = t->c_ispeed;
	tp->t_ospeed = t->c_ospeed;
	tp->t_cflag = t->c_cflag;

	return 0;
}

static int
stmp1uart_intr(void *arg)
{
	struct stmp1uart_softc *sc = arg;
	uint32_t val;

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x1C);
	if (val & (__BIT(3) | __BIT(5))) {
		bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x20, __BIT(3) | __BIT(5));
		while (sc->rxring_cnt < sc->rxring_len) {
			if (!(bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x1c) & __BIT(5)))
				goto rxdone;
			uint8_t c = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x24) & 0xff;
			sc->rxring[sc->rxring_pos] = c;
			sc->rxring_cnt++;
			sc->rxring_pos = (sc->rxring_pos + 1) % sc->rxring_len;
		}
		// Discard the rest.
		while (bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x1c) & __BIT(5))
			bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x24);
	}
rxdone:
#if 0
	if (val & __BIT(23)) {
		// Allow sending
	}
#endif

	if (sc->rxring_cnt > 0)
		softint_schedule(sc->sc_sih);

	return 0;
}

static int
stmp1ucngetc(dev_t dev)
{
	struct stmp1uart_softc *sc = console_sc;
	int c;
	int s = splserial();

	if (sc->rxring_cnt > 0) {
		sc->rxring_cnt--;
		sc->rxring_pos = (sc->rxring_pos - 1) % sc->rxring_len;
		c = sc->rxring[sc->rxring_pos];
		goto done;
	}
	if (!(bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x1c) & __BIT(5)))
		c = -1;
	else
		c = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x24) & 0xff;
done:
	splx(s);
	return c;
}

static void
stmp1ucnputc(dev_t dev, int c)
{
	struct stmp1uart_softc *sc = console_sc;
	int s = splserial();

	while (!(bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x1c) & __BIT(7)))
		continue;
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x28, c);
	splx(s);
	return;
}

static void
stmp1ucnpollc(dev_t dev, int on)
{
#if 0
	struct stmp1uart_softc *sc = console_sc;

	sc->rxring_len = 0;
#endif
}

static struct consdev stmp1ucons = {
	NULL, NULL, stmp1ucngetc, stmp1ucnputc, stmp1ucnpollc, NULL, NULL, NULL,
	NODEV, CN_NORMAL
};

CFATTACH_DECL_NEW(stmp1uart, sizeof(struct stmp1uart_softc),
	stmp1uart_match, stmp1uart_attach, NULL, NULL);

static int
stmp1uart_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;
	bus_addr_t addr;
	bus_size_t size;

	if (of_compatible_match(faa->faa_phandle, compat_data)) {
		if (fdtbus_get_reg(faa->faa_phandle, 0, &addr, &size) != 0) {
			return 0;
		}
		aprint_normal("stmp1uart matching for addr=0x%lx\n", addr);
		if (addr == 0x40010000)
			return 1;
	}
	return 0;
}

static void
stmp1uart_attach(device_t parent, device_t self, void *aux)
{
	struct stmp1uart_softc * const sc = device_private(self);
	struct fdt_attach_args * const faa = aux;
	const int phandle = faa->faa_phandle;
	struct tty *tp;
	int minor, major, error;
	char intrstr[128];
	bus_addr_t addr;
	bus_size_t size;

	if (console_sc->phandle == phandle) {
		console_sc = sc;
	}
	sc->phandle = phandle;

	sc->rxring_len = sizeof(sc->rxring);
	sc->rxring_pos = 0;
	sc->rxring_cnt = 0;

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

	if (!fdtbus_intr_str(phandle, 0, intrstr, sizeof(intrstr))) {
		aprint_error_dev(self, "failed to decode interrupt\n");
		return;
	}

	sc->sc_ih = fdtbus_intr_establish_xname(phandle, 0, IPL_SERIAL, 0,
	    stmp1uart_intr, sc, device_xname(self));
	if (sc->sc_ih == NULL) {
		aprint_error_dev(self, "couldn't establish interrupt on %s\n",
		    intrstr);
		return;
	}

	sc->sc_sih = softint_establish(SOFTINT_SERIAL, stmp1uart_rxsoft, sc);
	if (sc->sc_sih == NULL) {
		aprint_error(": failed to establish softint\n");
		return;
	}

	if (stmp1_uart_cmajor == -1) {
		/* allocate a major number */
		int bmajor = -1, cmajor = -1;
		error = devsw_attach("stmp1uart", NULL, &bmajor,
		    &stmp1_uart_cdevsw, &cmajor);
		if (error) {
			aprint_error(": couldn't allocate major number\n");
			return;
		}
		stmp1_uart_cmajor = cmajor;
	}

	major = cdevsw_lookup_major(&stmp1_uart_cdevsw);
	minor = device_unit(self);

	tp = sc->sc_tty = tty_alloc();
	tp->t_oproc = stmp1_uart_start;
	tp->t_param = stmp1_uart_param;
	tp->t_dev = makedev(major, minor);
	tp->t_sc = sc;
	tty_attach(tp);

	aprint_naive("\n");
	if (console_sc == sc) {
		aprint_normal(": console");
		cn_tab->cn_dev = tp->t_dev;
	}
	aprint_normal(": STM32MP1 UART\n");

	uint32_t val;
	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x0);
	aprint_normal_dev(self, "USART_CR1: %0x\n", val);
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x0, val | __BIT(29));
#if 0
	val |= __BIT(29) | __BIT(30) | __BIT(5);
#else
	val |= __BIT(29) | __BIT(5);
#endif
	bus_space_write_4(sc->sc_bst, sc->sc_bsh, 0x0, val);

	val = bus_space_read_4(sc->sc_bst, sc->sc_bsh, 0x8);
	aprint_normal_dev(self, "USART_CR3: %0x\n", val);

	console_sc = sc;
}

static int
stmp1_uart_console_match(int phandle)
{
	bus_addr_t addr;
	bus_size_t size;

	if (of_compatible_match(phandle, compat_data)) {
		if (fdtbus_get_reg(phandle, 0, &addr, &size) != 0) {
			return 0;
		}
		aprint_normal("stmp1uart matching for addr=0x%lx\n", addr);
		if (addr == 0x40010000)
			return 1;
	}
	return 0;
}

static void
stmp1_uart_console_consinit(struct fdt_attach_args *faa, u_int uart_freq)
{
	struct stmp1uart_softc *sc;
	const int phandle = faa->faa_phandle;
	bus_addr_t addr;
	bus_size_t size;
	int error;

	console_sc = &console_sc_alloc;
	sc = console_sc;
	sc->phandle = phandle;

	fdtbus_get_reg(phandle, 0, &addr, &size);

	sc->sc_bst = faa->faa_bst;
#if 0
	sc->sc_ospeed = fdtbus_get_stdout_speed();
	if (sc->sc_ospeed < 0)
		sc->sc_ospeed = 115200;
	sc->sc_cflag = fdtbus_get_stdout_flags();
#endif

	error = bus_space_map(sc->sc_bst, addr, size, 0, &sc->sc_bsh);
	if (error != 0)
		panic("failed to map console, error = %d", error);

	cn_tab = &stmp1ucons;
	cn_init_magic(&stmp1_uart_cnm_state);
	cn_set_magic("\047\001");
}

static const struct fdt_console stmp1_uart_console = {
	.match = stmp1_uart_console_match,
	.consinit = stmp1_uart_console_consinit,
};

FDT_CONSOLE(stmp1_uart, &stmp1_uart_console);
