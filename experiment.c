/*
 * experiment.c  –  Automated comparison of ARQ protocols  (Assignment 2)
 *
 * Runs all three flow-control schemes (Stop-and-Wait, Go-Back-N,
 * Selective-Repeat) with different error/loss probabilities and
 * measures:
 *   - Total time
 *   - Total frames sent (including retransmissions)
 *   - Efficiency  = (original frames / total frames sent) × 100%
 *
 * Results are printed to stdout and saved to experiment_results.csv.
 *
 * This program does NOT use sockets.  It simulates the sender and
 * receiver in the same process using the channel module so that we
 * can run hundreds of trials quickly.
 *
 * Build:  see Makefile  (target: experiment)
 * Run:    ./experiment
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "frame.h"
#include "crc.h"
#include "channel.h"

/* ================================================================== */
/*  Configuration                                                      */
/* ================================================================== */

#define NUM_FRAMES   50          /* frames to simulate per trial       */
#define TRIALS       200         /* trials per (protocol, prob) combo  */
#define WINDOW_SIZE  4           /* N for GBN and SR                   */

static const double PROB_LIST[] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5};
#define NUM_PROBS (int)(sizeof(PROB_LIST)/sizeof(PROB_LIST[0]))

/* ================================================================== */
/*  Simulation helpers                                                 */
/* ================================================================== */

/* Build a dummy frame */
static void make_frame(Frame *f, int idx)
{
    uint8_t dummy[PAYLOAD_SIZE];
    for (int i = 0; i < PAYLOAD_SIZE; i++) dummy[i] = (uint8_t)(idx * 7 + i);
    memset(f, 0, sizeof(Frame));
    memcpy(f->payload, dummy, PAYLOAD_SIZE);
    f->header.length     = PAYLOAD_SIZE;
    f->header.seq_no     = (uint8_t)(idx % 256);
    f->header.frame_type = FRAME_DATA;
    f->trailer.crc       = calculateCRC32(f->payload, PAYLOAD_SIZE);
}

/* Check CRC */
static int check_ok(const Frame *f)
{
    return (calculateCRC32((uint8_t *)f->payload, f->header.length) == f->trailer.crc);
}

/* ================================================================== */
/*  Protocol simulations (single-trial, returns #frames transmitted)  */
/* ================================================================== */

/* Stop-and-Wait */
static long sim_saw(Frame *originals, int n)
{
    long sent = 0;
    for (int i = 0; i < n; i++) {
        int ok = 0;
        while (!ok) {
            Frame copy = originals[i];
            sent++;
            int survived = channel_process(&copy);
            if (survived && check_ok(&copy))
                ok = 1;
            /* else: timeout / NAK → retransmit */
        }
    }
    return sent;
}

/* Go-Back-N */
static long sim_gbn(Frame *originals, int n, int N)
{
    long sent = 0;
    int base = 0;
    int next = 0;
    int acked[4096];
    memset(acked, 0, sizeof(acked));

    while (base < n) {
        /* Fill window */
        int window_full = (next - base >= N) || (next >= n);
        if (!window_full) {
            Frame copy = originals[next];
            sent++;
            int survived = channel_process(&copy);
            if (survived && check_ok(&copy)) {
                acked[next] = 1;
            }
            next++;
            continue;
        }

        /* Check oldest in window */
        if (acked[base]) {
            base++;
        } else {
            /* Go back: retransmit entire window */
            for (int i = base; i < next; i++) acked[i] = 0;
            next = base;
        }
    }
    return sent;
}

/* Selective Repeat */
static long sim_sr(Frame *originals, int n, int N)
{
    long sent = 0;
    int base = 0;
    int next = 0;
    int acked[4096];
    memset(acked, 0, sizeof(acked));

    while (base < n) {
        /* Fill window */
        if (next - base < N && next < n) {
            Frame copy = originals[next];
            sent++;
            int survived = channel_process(&copy);
            if (survived && check_ok(&copy)) {
                acked[next] = 1;
            }
            next++;
            continue;
        }

        /* Slide window or retransmit individual failed frames */
        if (acked[base]) {
            while (base < n && acked[base]) base++;
        } else {
            /* Retransmit only base */
            Frame copy = originals[base];
            sent++;
            int survived = channel_process(&copy);
            if (survived && check_ok(&copy))
                acked[base] = 1;
        }
    }
    return sent;
}

/* ================================================================== */
/*  Run one experiment row                                             */
/* ================================================================== */

typedef struct {
    double prob;
    long   saw_avg;
    long   gbn_avg;
    long   sr_avg;
    double saw_eff;
    double gbn_eff;
    double sr_eff;
} ExpRow;

static ExpRow run_experiment(double prob)
{
    Frame originals[NUM_FRAMES];
    for (int i = 0; i < NUM_FRAMES; i++) make_frame(&originals[i], i);

    ChannelConfig cfg = { prob, prob / 2.0, 0 };  /* error + loss prob */
    channel_init(cfg);

    long saw_total = 0, gbn_total = 0, sr_total = 0;

    for (int t = 0; t < TRIALS; t++) {
        saw_total += sim_saw(originals, NUM_FRAMES);
        gbn_total += sim_gbn(originals, NUM_FRAMES, WINDOW_SIZE);
        sr_total  += sim_sr (originals, NUM_FRAMES, WINDOW_SIZE);
    }

    ExpRow row;
    row.prob    = prob;
    row.saw_avg = saw_total / TRIALS;
    row.gbn_avg = gbn_total / TRIALS;
    row.sr_avg  = sr_total  / TRIALS;
    row.saw_eff = 100.0 * NUM_FRAMES / (double)(row.saw_avg > 0 ? row.saw_avg : 1);
    row.gbn_eff = 100.0 * NUM_FRAMES / (double)(row.gbn_avg > 0 ? row.gbn_avg : 1);
    row.sr_eff  = 100.0 * NUM_FRAMES / (double)(row.sr_avg  > 0 ? row.sr_avg  : 1);
    return row;
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */

int main(void)
{
    srand((unsigned)time(NULL));

    printf("\n");
    printf("================================================================\n");
    printf(" Flow Control Experiment  (frames=%d, trials=%d, window=%d)\n",
           NUM_FRAMES, TRIALS, WINDOW_SIZE);
    printf("================================================================\n");
    printf(" %-8s | %-10s | %-10s | %-10s | %-8s | %-8s | %-8s\n",
           "Prob", "SAW sent", "GBN sent", "SR sent",
           "SAW eff%", "GBN eff%", "SR eff%");
    printf("---------|-----------|-----------|-----------|---------|---------|--------\n");

    FILE *csv = fopen("experiment_results.csv", "w");
    if (!csv) { perror("fopen csv"); return 1; }
    fprintf(csv,
            "prob,saw_avg_sent,gbn_avg_sent,sr_avg_sent,"
            "saw_efficiency,gbn_efficiency,sr_efficiency\n");

    for (int p = 0; p < NUM_PROBS; p++) {
        ExpRow row = run_experiment(PROB_LIST[p]);

        printf(" %-8.2f | %-10ld | %-10ld | %-10ld | %7.2f%% | %7.2f%% | %7.2f%%\n",
               row.prob,
               row.saw_avg, row.gbn_avg, row.sr_avg,
               row.saw_eff, row.gbn_eff, row.sr_eff);

        fprintf(csv, "%.2f,%ld,%ld,%ld,%.2f,%.2f,%.2f\n",
                row.prob,
                row.saw_avg, row.gbn_avg, row.sr_avg,
                row.saw_eff, row.gbn_eff, row.sr_eff);
    }

    printf("================================================================\n");
    printf("Results saved to experiment_results.csv\n");
    fclose(csv);
    return 0;
}
