/*
 * w737-fix10 ? keep the SN65DSI86 lane/mode register (0x10) at the value the
 * W737 AMOLED panel actually needs.
 *
 * Title: SM-W737 UEFI leaves 0x10 = 0x80 (DSI_CHANNEL_MODE = dual,
 * LEFT_RIGHT_PIXELS = 1). The mainline ti-sn65dsi86 driver only updates bits
 * 4:3 (CHA_DSI_LANES), so the dual-channel bits survive and the panel stays
 * dark. With a single DSI channel driven, 0x10 must be:
 *
 *      0x26 = 0010 0110b
 *        bit 7    0   LEFT_RIGHT_PIXELS off
 *        bits 6:5 01  DSI_CHANNEL_MODE = single channel A
 *        bits 4:3 00  CHA_DSI_LANES = 4 lanes
 *        bits 2:1 11  CHB lane selector (unused in single mode, kept as-is)
 *
 * The bridge driver re-runs its init on every enable, so this has to be a
 * watcher, not a one-shot poke.
 *
 * Plain i2c-dev, no libi2c dependency.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#define DEFAULT_DEV	"/dev/i2c-3"
#define DEFAULT_ADDR	0x2c
#define REG_LANES	0x10
#define VAL_WANTED	0x26
#define	REG_DPPLL	0x0A		/* bit7 = DP_PLL_LOCK */
#define POLL_MS		400
#define SETTLE_MS	1500		/* don't rewrite more often than this */

static volatile sig_atomic_t stop;
static int g_addr = DEFAULT_ADDR;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int reg_read(int fd, uint8_t reg, uint8_t *out)
{
	struct i2c_msg msg[2];
	struct i2c_rdwr_ioctl_data xfer;
	uint8_t buf = reg;

	msg[0].addr = g_addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = &buf;
	msg[1].addr = g_addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = 1;
	msg[1].buf = out;

	xfer.msgs = msg;
	xfer.nmsgs = 2;

	if (ioctl(fd, I2C_RDWR, &xfer) < 0)
		return -1;
	return 0;
}

static int reg_write(int fd, uint8_t reg, uint8_t val)
{
	struct i2c_msg msg;
	struct i2c_rdwr_ioctl_data xfer;
	uint8_t buf[2] = { reg, val };

	msg.addr = g_addr;
	msg.flags = 0;
	msg.len = 2;
	msg.buf = buf;

	xfer.msgs = &msg;
	xfer.nmsgs = 1;

	if (ioctl(fd, I2C_RDWR, &xfer) < 0)
		return -1;
	return 0;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(int argc, char **argv)
{
	const char *dev = DEFAULT_DEV;
	int once = 0, verbose = 0, i;
	int fd, fixes = 0, errors = 0;
	long last_fix = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--once"))
			once = 1;
		else if (!strcmp(argv[i], "--verbose"))
			verbose = 1;
		else if (!strcmp(argv[i], "--addr") && i + 1 < argc)
			g_addr = (int)strtol(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--dev") && i + 1 < argc)
			dev = argv[++i];
		else {
			fprintf(stderr, "usage: %s [--once] [--verbose] [--dev PATH]\n",
				argv[0]);
			return 2;
		}
	}

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	while (!stop) {
		uint8_t lanes = 0, pll = 0;

		fd = open(dev, O_RDWR);
		if (fd < 0) {
			if (verbose || once)
				fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
			if (once)
				return 1;
			usleep(POLL_MS * 1000);
			continue;
		}

		if (reg_read(fd, REG_LANES, &lanes) == 0) {
			if (lanes != VAL_WANTED && now_ms() - last_fix > SETTLE_MS) {
				if (reg_write(fd, REG_LANES, VAL_WANTED) == 0) {
					fixes++;
					last_fix = now_ms();
					/* fixes are rare (only after a modeset), always log */
					printf("0x10: 0x%02x -> 0x%02x (fix #%d)\n",
					       lanes, VAL_WANTED, fixes);
					fflush(stdout);
				} else if (verbose) {
					fprintf(stderr, "write 0x10 failed: %s\n",
						strerror(errno));
				}
			} else if (verbose && lanes != VAL_WANTED) {
				printf("0x10 = 0x%02x, settling\n", lanes);
			} else if (verbose) {
				reg_read(fd, REG_DPPLL, &pll);
				printf("0x10 = 0x%02x ok, 0x0a = 0x%02x\n", lanes, pll);
			}
		} else if (verbose) {
			errors++;
			fprintf(stderr, "read 0x10 failed (%d)\n", errors);
		}

		close(fd);

		if (once)
			return lanes == VAL_WANTED ? 0 : 4;
		usleep(POLL_MS * 1000);
	}

	return 0;
}


