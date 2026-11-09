/*
 * setbrit <level 0..101> [hbm 0|1] [colorProfile 0..6] [send-profile 1]
 *
 * Bends the Galaxy Book 12 brightness engine onto this panel's AUX channel.
 * initBrightness() only reads (handshake + 33 calibration registers +
 * 0x26..0x29); setBrightness() then sends the computed payload.
 * The colour profile is only sent when the 4th argument is 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include "brightness.h"
#include "tables.h"

#define AUX_PATH "/dev/drm_dp_aux0"

int main(int argc, char **argv)
{
    int level, hbm = 0, profile = 3, sendProfile = 0;
    aux_t aux;
    displayBrightness_t *dp;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <level 0..101> [hbm 0|1] [profile 0..6] [send-profile 1]\n",
                argv[0]);
        return 2;
    }
    level = atoi(argv[1]);
    if (argc > 2) hbm = atoi(argv[2]);
    if (argc > 3) profile = atoi(argv[3]);
    if (argc > 4) sendProfile = atoi(argv[4]);
    if (level < 0 || level > 101) {
        fprintf(stderr, "level out of range\n");
        return 2;
    }

    aux = dpAuxOpen(AUX_PATH);
    if (aux < 0) { perror("open " AUX_PATH); return 1; }

    dp = initBrightness(aux, hbm);
    if (!dp) { fprintf(stderr, "initBrightness() failed\n"); return 3; }

    printf("hbm=%d profile=%d reg2_26=0x%02x level=%d ptr=%d\n",
           dp->highBrightnessMode, profile, (unsigned char)dp->reg2_26,
           level, ptrIntoChannelAdjustmentTable[level]);

    if (sendProfile)
        setColorProfile(aux, dp, profile);
    setBrightness(aux, dp, level);

    free(dp);
    dpAuxClose(aux);
    printf("sent\n");
    return 0;
}
