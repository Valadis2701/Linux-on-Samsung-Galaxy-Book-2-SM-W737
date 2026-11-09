// SPDX-License-Identifier: GPL-2.0
/*
 * w737-brightnessd — keeps the AMOLED panel calibration in memory so changing
 * the brightness costs one payload instead of a full re-initialisation.
 *
 * initBrightness() has to do the PARAAUX handshake and read 33 calibration
 * registers over DP AUX, which takes ~1.3 s. Windows reads them once at panel
 * init and reuses the result, so do the same here: the daemon initialises once
 * and then only runs setBrightness() (~50 AUX writes) per request.
 *
 * Requests arrive as the contents of /run/w737-brightness/request; writing that
 * file is all /usr/local/sbin/w737-brightness-set has to do.
 */
#include <errno.h>
#include <fcntl.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <grp.h>
#include <time.h>
#include <unistd.h>

#include "brightness.h"
#include "dpAux.h"

#define AUX_PATH	"/dev/drm_dp_aux0"
#define RUN_DIR		"/run/w737-brightness"
#define REQ_FILE	RUN_DIR "/request"
#define APPLIED_FILE	RUN_DIR "/applied"
#define HIGH_BRIGHTNESS	1
#define IDLE_REINIT_MS	5000	/* panel may have been power cycled */
#define POLL_US		10000	/* request file poll interval */
#define INIT_RETRIES	5

static aux_t aux = -1;
static displayBrightness_t *dp;
static long last_apply_ms;

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void drop_display(void)
{
	free(dp);
	dp = NULL;
	if (aux >= 0) {
		dpAuxClose(aux);
		aux = -1;
	}
}

static int ensure_ready(int force)
{
	int attempt;

	if (dp && !force)
		return 0;

	drop_display();

	for (attempt = 1; attempt <= INIT_RETRIES; attempt++) {
		aux = dpAuxOpen(AUX_PATH);
		if (aux < 0)
			return -1;

		dp = initBrightness(aux, HIGH_BRIGHTNESS);
		/*
		 * reg2_26 == 0 means the PARAAUX gateway answered with zeros: the
		 * calibration is garbage and every payload built from it would be
		 * wrong, so treat it as a failed init rather than as data.
		 */
		if (dp && dp->reg2_26 != 0)
			return 0;

		drop_display();		/* initBrightness failed: aux is gone anyway */
		usleep(200 * 1000);
	}
	return dp ? 0 : -1;
}

static void write_applied(int level)
{
	FILE *fh = fopen(APPLIED_FILE, "w");

	if (!fh)
		return;
	fprintf(fh, "%d\n", level);
	fclose(fh);
}

static int read_request(void)
{
	char buf[16];
	FILE *fh = fopen(REQ_FILE, "r");
	int level;

	if (!fh)
		return -1;
	if (!fgets(buf, sizeof(buf), fh)) {
		fclose(fh);
		return -1;
	}
	fclose(fh);

	level = atoi(buf);
	if (level < 0)
		level = 0;
	if (level > 100)
		level = 100;
	return level;
}

static int apply(int level)
{
	long idle = now_ms() - last_apply_ms;

	if (ensure_ready(idle > IDLE_REINIT_MS) < 0) {
		fprintf(stderr, "init failed, level %d dropped\n", level);
		return -1;
	}

	setBrightness(aux, dp, level);
	last_apply_ms = now_ms();
	write_applied(level);
	return 0;
}

int main(int argc, char **argv)
{
	int verbose = 0, i;
	struct timespec last_mtime = { 0, 0 };

	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--verbose"))
			verbose = 1;

	mkdir(RUN_DIR, 0775);
	{
		struct group *gr = getgrnam("video");
		gid_t gid = gr ? gr->gr_gid : (gid_t)-1;

		chown(RUN_DIR, -1, gid);
		chmod(RUN_DIR, 0775);

		/* Pre-create the request file group-writable so the slider can use it. */
		int rfd = open(REQ_FILE, O_CREAT | O_WRONLY, 0664);

		if (rfd >= 0) {
			if (gr)
				fchown(rfd, -1, gid);
			fchmod(rfd, 0664);
			close(rfd);
		}
	}
	setenv("LC_ALL", "C", 1);

	if (ensure_ready(1) < 0) {
		fprintf(stderr, "cannot initialise the panel\n");
		return 1;
	}
	printf("panel initialised (reg2_26=0x%02x)\n",
	       (unsigned char)dp->reg2_26);
	fflush(stdout);

	{
		FILE *rf = fopen(RUN_DIR "/ready", "w");

		if (rf)
			fclose(rf);
	}

	/* Apply whatever is already queued, so a restart re-syncs the panel. */
	if (read_request() >= 0)
		apply(read_request());

	last_apply_ms = now_ms();

	for (;;) {
		struct stat st;
		int level;

		if (stat(REQ_FILE, &st) == 0 &&
		    (st.st_mtim.tv_sec != last_mtime.tv_sec ||
		     st.st_mtim.tv_nsec != last_mtime.tv_nsec)) {
			last_mtime = st.st_mtim;
			level = read_request();
			if (level >= 0) {
				if (verbose)
					printf("request %d\n", level);
				apply(level);
				fflush(stdout);
			}
		}
		usleep(POLL_US);
	}

	drop_display();
	return 0;
}



