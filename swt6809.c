/*
 *	SWTPC 6809
 *	MP-09 CPU card
 *	20bit DAT addressing
 *	ACIA at slot 1
 *	DCS4ish at slot 2
 *	PT SS30-IDE slot 6
 *
 *	TODO: sort out slot arrangement properly
 *	How to handle stuff like DS68 Vidoe (E7FE/E7FF 6845, E800-EFFF
 *	RAM)
 *
 *	Need to do MP-T timer board, MP-L parallel (plus PIA IDE),
 *	maybe DMAF tuff and FD2 ?
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "d6809.h"
#include "e6809.h"
#include "serialdevice.h"
#include "ttycon.h"
#include "acia.h"
#include "ide.h"
#include "wd17xx.h"
#include "6840.h"
#include "6821.h"

struct slot {
	const char *name;
	uint8_t(*read) (void *, unsigned);
	void (*write)(void *, unsigned, uint8_t);
	unsigned (*irq)(void *);
	void (*tick)(void *);
	void *private;
};

struct mps2 {
	struct acia *low;
	struct acia *high;
	uint8_t cli;
};

struct mpid {
	struct m6840 *timer;
	struct m6821 *pia;
};

static uint8_t rom[4096];
static uint16_t rombase = 0xFC00;
static uint16_t rommask = 0x03FF;
static uint8_t ram[1024 * 1024];
static uint8_t dat[16];
static struct slot slot[16];

static unsigned fast;

/* 1MHz */
static uint16_t clockrate = 100;

static uint8_t live_irq;

static volatile int done;

#define TRACE_MEM	1
#define TRACE_IO	2
#define TRACE_ROM	4
#define TRACE_UNK	8
#define TRACE_CPU	16
#define TRACE_IRQ	32
#define TRACE_ACIA	64
#define TRACE_FDC	128
#define TRACE_DAT	256

static int trace = 0;

/*
 *	The DAT doe a 4->8 expansion where the upper 4 bits are
 *	a bank code and the lower 4 are an inverted A15-A12 output
 *
 *	FFxx is hardwired to physical FFFxxx (bank reg F0)
 *	FFFxxx is the ROM space.
 */
static uint8_t *dat_xlate(unsigned addr, unsigned w)
{
	unsigned d = dat[addr >> 12];
	/* Upper bits are non inverted bank lower are inverted
	   top physical address nybble */
	/* Hardware hardwires 0xFFxx to be bank F address 0XXX which
	   is the ROM */
	if (addr >= 0xFF00 || d == 0xF0) {
		if (w)
			return NULL;
		return rom + (addr & rommask);
	}
	d ^= 0x0F;
	if (d >= 0x80)
		return NULL;
	return ram + (d << 12) + (addr & 0xFFF);
}

static uint8_t dat_page(unsigned addr)
{
	return dat[addr >> 12];
}

static uint8_t empty_read(void *info, unsigned addr)
{
	return 0xFF;
}

static void empty_write(void *info, unsigned addr, uint8_t val)
{
}

static unsigned no_slot_int(void *info)
{
	return 0;
}

static void no_slot_tick(void *info)
{
}

static struct slot empty_slot = { "empty", empty_read, empty_write, no_slot_int, no_slot_tick };

static uint8_t mps_read(void *info, unsigned addr)
{
	struct acia *a = info;
	return acia_read(a, addr & 1);
}

static void mps_write(void *info, unsigned addr, uint8_t val)
{
	struct acia *a = info;
	acia_write(a, addr & 1, val);
}

static unsigned mps_int(void *info)
{
	struct acia *a = info;
	return acia_irq_pending(a);
}

static void mps_tick(void *info)
{
	struct acia *a = info;
	acia_timer(a);
}

static struct slot mps_slot = { "MPS", mps_read, mps_write, mps_int, mps_tick };

#if 0
static uint8_t mps2_read(void *info, unsigned addr)
{
	struct mps2 *m = info;
	switch(addr & 0x0E) {
	case 0x0:
		return acia_read(m->low, addr & 1);
	case 0x4:
		return acia_read(m->high, addr & 1);
	case 0xE:
		return m->cli;	/* Not really emulated */
	default:
		return 0xFF;
	}
}

static void mps2_write(void *info, unsigned addr, uint8_t val)
{
	struct mps2 *m = info;
	switch(addr & 0x0E) {
	case 0x0:
		acia_write(m->low, addr & 1, val);
		break;
	case 0x4:
		acia_write(m->high, addr & 1, val);
		break;
	case 0xE:
	default:
		break;
	}
}

static void mps2_int(void *info)
{
	struct mps2 *m = info;
	return acia_irq_pending(m->low) | acia_irq_pending(m->high);
}

static void mps2_tick(void *info)
{
	struct mps2 *m = info;
	acia_timer(m->low);
	acia_timer(m->high);
}

static struct slot mps2_slot = { "MPS2", mps2_read, mps2_write };
#endif

/* TODO: PL-L2 - 6821  0/1 A data/control 2/3 b data/control E/F latch */

static uint8_t dcs34_read(void *info, unsigned addr)
{
	struct wd17xx *fdc = info;
	switch (addr) {
	case 0x08:
		return wd17xx_status(fdc);
	case 0x09:
		return wd17xx_read_track(fdc);
	case 0x0A:
		return wd17xx_read_sector(fdc);
	case 0x0B:
		return wd17xx_read_data(fdc);
	case 0x04:
		/* TODO verify which pin gets intrq */
		return wd17xx_intrq(fdc) | (wd17xx_status_noclear(fdc) & 0x80);
	default:
		return 0xFF;	/* TODO: check schematic and decodes */
	}
}

#if 0
static uint8_t dcs2_read(void *info, unsigned addr)
{
	struct wd17xx *fdc = info;
	switch (addr) {
	case 0x08:
		return wd17xx_status(fdc);
	case 0x09:
		return wd17xx_read_track(fdc);
	case 0x0A:
		return wd17xx_read_sector(fdc);
	case 0x0B:
		return wd17xx_read_data(fdc);
	default:
		return 0xFF;	/* TODO: check schematic and decodes */
	}
}
#endif

static void dcs34_write(void *info, unsigned addr, uint8_t val)
{
	struct wd17xx *fdc = info;
	switch (addr) {
	case 0x08:
		wd17xx_command(fdc, val);
		break;
	case 0x09:
		wd17xx_write_track(fdc, val);
		break;
	case 0x0A:
		wd17xx_write_sector(fdc, val);
		break;
	case 0x0B:
		wd17xx_write_data(fdc, val);
		break;
	case 0x04:		/* TODO sort all the bits out */
		wd17xx_set_drive(fdc, val & 1);
		break;
	}
}

//static struct slot dcs2_slot = { "DCS2", dcs2_read, dcs34_write, no_slot_int, no_slot_tick };
/* TODO: review IRQ */
static struct slot dcs34_slot = { "DCS3/4", dcs34_read, dcs34_write, no_slot_int, no_slot_tick };

static uint8_t pt_ss30_read(void *info, unsigned addr)
{
	struct ide_controller *i = info;
	return ide_read8(i, addr & 7);
}

static void pt_ss30_write(void *info, unsigned addr, uint8_t val)
{
	struct ide_controller *i = info;
	ide_write8(i, addr & 7, val);
}

static struct slot pt_ss30_slot = { "PT_SS30", pt_ss30_read, pt_ss30_write, no_slot_int, no_slot_tick };

/*
 *	Internal MPID glue
 */

/* Two x4 counters chained top bit unused with the input bit as bit 0 */
static uint8_t mpid_counter;
static struct mpid mpid;

static void mpid_clock_counters(unsigned n)
{
	/* input edge */
	if (!(mpid_counter & 1) && n == 1)
		mpid_counter += 2;
	else {
		/* Keep in sync */
		mpid_counter &= 0xFE;
		mpid_counter |= n;
	}
	m6821_set_control(mpid.pia, M6821_CA1, mpid_counter & 0x80);
}

void m6840_output_change(struct m6840 *ptm, uint8_t output)
{
	/* Output 0 drives the counter, 2 drives input 1 */
	if (output & 1)
		mpid_clock_counters(output & 1);
	if (output & 4)
		m6840_external_clock(ptm, 2);
}

void m6821_ctrl_change(struct m6821 *pia, uint8_t ctrl)
{
	/* See strobe */
}

uint8_t m6821_input(struct m6821 *pin, int port)
{
	if (port == 0)
		return mpid_counter;
	/* Printer */
	return 0xFF;
}

void m6821_output(struct m6821 *pia, int port, uint8_t data)
{
	if (port == 0)
		return;	/* Error */
	/* Printer data */
}

void m6821_strobe(struct m6821 *pia, int pin)
{
	/* CB2 is the output strobe CA2 is the speaker */
}

static uint8_t mpid_pia_read(void *info, unsigned addr)
{
	struct mpid *mpid = info;
	return m6821_read(mpid->pia, addr & 3);
}

static void mpid_pia_write(void *info, unsigned addr, uint8_t val)
{
	struct mpid *mpid = info;
	m6821_write(mpid->pia, addr & 3, val);
}

static uint8_t mpid_timer_read(void *info, unsigned addr)
{
	struct mpid *mpid = info;
	return m6840_read(mpid->timer, addr & 7);
}

static void mpid_timer_write(void *info, unsigned addr, uint8_t val)
{
	struct mpid *mpid = info;
	m6840_write(mpid->timer, addr & 7, val);
}

static unsigned mpid_pia_irq(void *info)
{
	struct mpid *mpid = info;
	return m6821_irq_pending(mpid->pia);
}

static unsigned mpid_timer_irq(void *info)
{
	struct mpid *mpid = info;
	return m6840_irq_pending(mpid->timer);
}

static void mpid_timer_tick(void *info)
{
	struct mpid *mpid = info;
	m6840_tick(mpid->timer, clockrate * 100);
	m6840_external_clock(mpid->timer, 1);
}

static struct slot mpid_pia_slot = { "MPID", mpid_pia_read, mpid_pia_write, mpid_pia_irq, no_slot_tick };
static struct slot mpid_timer_slot = { "MPID", mpid_timer_read, mpid_timer_write, mpid_timer_irq, mpid_timer_tick };

static void mpid_create(void)
{
	mpid.pia = m6821_create();
	mpid.timer = m6840_create();
}

static void slot_attach(unsigned snum, struct slot *s, void *private)
{
	memcpy(slot + snum, s, sizeof(struct slot));
	slot[snum].private = private;
}

static unsigned is_slot(unsigned addr)
{
	/* Hack for now */
	if (addr >= 0xE000 && addr <= 0xF800)
		return 1;
	return 0;
}

void recalc_interrupts(void)
{
/*	static unsigned prev; */
	unsigned irq = 0;
	unsigned i;
	for (i = 0; i < 16; i++)
		irq |= (slot[i].irq(slot[i].private) << i);
#if 0
	if (irq != prev) {
		fprintf(stderr, "INT: %x\n", live_irq);
		prev = irq;
	}
#endif
	live_irq = !!irq;
}

unsigned char do_e6809_read8(unsigned addr, unsigned debug)
{
	unsigned char r = 0xFF;
	uint8_t *ap;
	/* Stop the debugger causing side effects in the I/O window */
	if (debug && is_slot(addr))
		return 0xA5;
	if (is_slot(addr)) {
		struct slot *s = &slot[(addr & 0xF0) >> 4];
		r = s->read(s->private, addr & 0x0F);
	} else {
		ap = dat_xlate(addr, 0);
		if (ap)
			r = *ap;
	}
	if (!debug && (trace & TRACE_MEM))
		fprintf(stderr, "R [%02X]%04X = %02X\n", dat_page(addr), addr, r);
	return r;
}

unsigned char e6809_read8(unsigned addr)
{
	return do_e6809_read8(addr, 0);
}

unsigned char e6809_read8_debug(unsigned addr)
{
	return do_e6809_read8(addr, 1);
}

/* FIXME: the actual hardware DAT maps everything but forces FFxx to
the top 1K of ROM (0xF0) */
void e6809_write8(unsigned addr, unsigned char val)
{
	if (trace & TRACE_MEM)
		fprintf(stderr, "W [%02X]%04X = %02X\n", dat_page(addr), addr, val);
	if (is_slot(addr)) {
		struct slot *s = &slot[(addr & 0xF0) >> 4];
		s->write(s->private, addr & 0x0F, val);
	} else if (addr >= 0xFFF0) {
		if (trace & TRACE_DAT)
			fprintf(stderr, "DAT %1X: %2X\n", addr & 0x0F, val);
		dat[addr & 0x0F] = val;
	} else {
		uint8_t *ap = dat_xlate(addr, 1);
		if (ap)
			*ap = val;
	}
}

static const char *make_flags(uint8_t cc)
{
	static char buf[9];
	char *p = "EFHINZVC";
	char *d = buf;

	while (*p) {
		if (cc & 0x80)
			*d++ = *p;
		else
			*d++ = '-';
		cc <<= 1;
		p++;
	}
	*d = 0;
	return buf;
}

/* Called each new instruction issue */
void e6809_instruction(unsigned pc)
{
	char buf[80];
	struct reg6809 *r = e6809_get_regs();
	if (trace & TRACE_CPU) {
		d6809_disassemble(buf, pc);
		fprintf(stderr, "%04X: %-16.16s | ", pc, buf);
		fprintf(stderr, "%s %02X:%02X %04X %04X %04X %04X\n", make_flags(r->cc), r->a, r->b, r->x, r->y, r->u, r->s);
	}
}

static struct termios saved_term, term;

static void cleanup(int sig)
{
	tcsetattr(0, TCSADRAIN, &saved_term);
	done = 1;
}

static void exit_cleanup(void)
{
	tcsetattr(0, TCSADRAIN, &saved_term);
}

static void usage(void)
{
	fprintf(stderr, "swt6809: [-f] [-i idepath] [-r rompath] [-d debug]\n");
	exit(EXIT_FAILURE);
}

struct diskgeom {
	const char *name;
	unsigned int size;
	unsigned int sides;
	unsigned int tracks;
	unsigned int spt;
	unsigned int secsize;
};

struct diskgeom disktypes[] = {
	{ "35 Track SS", 89600, 1, 35, 10, 256 },
	{ "40 Track SS", 102400, 1, 40, 10, 256 },
	{ "40 Track DS", 204800, 2, 40, 10, 256 },
	{ "CP/M 77 track DSDD", 788480, 2, 77, 10, 512 },
	{ "CP/M 77 track SSDD", 394240, 1, 77, 10, 512 },
	{ "3.5\" HD DD", 1474560, 80, 18, 10, 512 },
	{ NULL, }
};

static struct diskgeom *guess_format(const char *path)
{
	struct diskgeom *d = disktypes;
	struct stat s;
	off_t size;
	if (stat(path, &s) == -1) {
		perror(path);
		exit(1);
	}
	size = s.st_size;
	while (d->name) {
		if (d->size == size)
			return d;
		d++;
	}
	fprintf(stderr, "swt6809: unknown disk format size %ld.\n", (long) size);
	exit(1);
}

int main(int argc, char *argv[])
{
	static struct timespec tc;
	int opt;
	int fd;
	char *rompath = "swt6809.rom";
	char *idepath = NULL;
	char *fdc_path[2] = { NULL, NULL };
	unsigned int cycles = 0;
	unsigned need_fdc = 0;
	unsigned i;

	while ((opt = getopt(argc, argv, "A:B:d:fi:r:")) != -1) {
		switch (opt) {
		case 'A':
			fdc_path[0] = optarg;
			need_fdc = 1;
			break;
		case 'B':
			fdc_path[1] = optarg;
			need_fdc = 1;
			break;
		case 'd':
			trace = atoi(optarg);
			break;
		case 'f':
			fast = 1;
			break;
		case 'i':
			idepath = optarg;
			break;
		case 'r':
			rompath = optarg;
			break;
		default:
			usage();
		}
	}
	if (optind < argc)
		usage();

	fd = open(rompath, O_RDONLY);
	if (fd == -1) {
		perror(rompath);
		exit(EXIT_FAILURE);
	}
	i = read(fd, rom, sizeof(rom));
	if (i == 4096) {
		rombase = 0xF000;
		rommask = 0x0FFF;
	} else if (i == 2048) {
		rombase = 0xF800;
		rommask = 0x07FF;
	} else {
		fprintf(stderr, "swt6809: unknown rom size '%d'.\n", i);
		exit(EXIT_FAILURE);
	}
	close(fd);

	for (i = 0; i < 16; i++)
		slot_attach(i, &empty_slot, NULL);

	/* Our slot array is effectively 'off by one' as the real slots
	   are numbered 1 for 0x 2 for 1x etc */
	if (idepath) {
		struct ide_controller *ide = ide_allocate("cf");
		if (ide) {
			int ide_fd = open(idepath, O_RDWR);
			if (ide_fd == -1) {
				perror(idepath);
			} else if (ide_attach(ide, 0, ide_fd) == 0) {
				ide_reset_begin(ide);
			}
			slot_attach(5, &pt_ss30_slot, ide);
		}
	}

	if (need_fdc) {
		struct wd17xx *fdc = wd17xx_create(1797);
		slot_attach(1, &dcs34_slot, fdc);
		for (i = 0; i < 2; i++) {
			if (fdc_path[i]) {
				struct diskgeom *d = guess_format(fdc_path[i]);
				printf("[Drive %c, %s.]\n", 'A' + i, d->name);
				wd17xx_attach(fdc, i, fdc_path[i], d->sides, d->tracks, d->spt, d->secsize);
			}
		}
		wd17xx_trace(fdc, trace & TRACE_FDC);
	}

	{
		struct acia *acia = acia_create();
		slot_attach(0, &mps_slot, acia);
		acia_attach(acia, &console);
		acia_trace(acia, trace & TRACE_ACIA);
	}

	mpid_create();
	slot_attach(8, &mpid_pia_slot, &mpid);
	slot_attach(9, &mpid_timer_slot, &mpid);

	/* 5ms - it's a balance between nice behaviour and simulation
	   smoothness */
	tc.tv_sec = 0;
	tc.tv_nsec = 10000000L;
	/* 20ms a cycle, so we get 100Hz for the MP-ID */

	if (tcgetattr(0, &term) == 0) {
		saved_term = term;
		atexit(exit_cleanup);
		signal(SIGINT, cleanup);
		signal(SIGQUIT, cleanup);
		signal(SIGPIPE, cleanup);
		term.c_lflag &= ~(ICANON | ECHO);
		term.c_cc[VMIN] = 0;
		term.c_cc[VTIME] = 1;
		term.c_cc[VINTR] = 0;
		term.c_cc[VSUSP] = 0;
		term.c_cc[VSTOP] = 0;
		tcsetattr(0, TCSADRAIN, &term);
	}

	e6809_reset(trace & TRACE_CPU);

	/* This is the wrong way to do it but it's easier for the moment. We
	   should track how much real time has occurred and try to keep cycle
	   matched with that. The scheme here works fine except when the host
	   is loaded though */

	while (!done) {
		unsigned int i;
		for (i = 0; i < 100; i++) {
			while (cycles < clockrate)
				cycles += e6809_sstep(live_irq, 0);
			cycles -= clockrate;
			recalc_interrupts();
		}
		for (i = 0; i < 16; i++)
			slot[i].tick(slot[i].private);
		/* Do 10ms of I/O and delays */
		if (!fast)
			nanosleep(&tc, NULL);
	}
	exit(0);
}
