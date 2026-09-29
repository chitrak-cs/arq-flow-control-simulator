/*
 * channel.c  –  Simulated unreliable channel  (Assignment 2)
 *
 * Reuses the error-injection concept from Assignment 1 error.c but
 * operates directly on Frame payloads and is driven by configurable
 * probabilities instead of hard-coded injection calls.
 */

#include <stdlib.h>
#include <time.h>
#include "channel.h"

/* ------------------------------------------------------------------ */
/*  Module-level state                                                 */
/* ------------------------------------------------------------------ */

static ChannelConfig g_cfg = {0.0, 0.0, 0};

/* ------------------------------------------------------------------ */
/*  channel_init                                                       */
/* ------------------------------------------------------------------ */

void channel_init(ChannelConfig cfg)
{
    g_cfg = cfg;
    if (cfg.seed == 0)
        srand((unsigned int)time(NULL));
    else
        srand((unsigned int)cfg.seed);
}

/* ------------------------------------------------------------------ */
/*  Helper: uniform random double in [0, 1)                           */
/* ------------------------------------------------------------------ */

static double rand_prob(void)
{
    return (double)rand() / ((double)RAND_MAX + 1.0);
}

/* ------------------------------------------------------------------ */
/*  channel_process                                                    */
/*                                                                     */
/*  Returns 0  → frame is lost (do NOT send on socket).               */
/*  Returns 1  → frame survives (send on socket, possibly corrupted). */
/* ------------------------------------------------------------------ */

int channel_process(Frame *frame)
{
    /* Step 1: frame loss (simulates timeout / delay beyond limit) */
    if (rand_prob() < g_cfg.loss_prob)
        return 0;   /* dropped */

    /* Step 2: single-bit error injection */
    if (rand_prob() < g_cfg.error_prob) {
        int payload_len = frame->header.length;
        if (payload_len > 0) {
            int byte_idx = rand() % payload_len;
            int bit_idx  = rand() % 8;
            frame->payload[byte_idx] ^= (1u << bit_idx);
        }
    }

    return 1;   /* survives */
}
