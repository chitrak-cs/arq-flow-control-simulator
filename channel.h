/*
 * channel.h  –  Simulated unreliable channel  (Assignment 2)
 *
 * Provides:
 *   channel_init()   – seed RNG and configure probabilities
 *   channel_send()   – inject bit errors / simulate packet loss
 *                      returns 1 if frame should be "sent", 0 if "dropped"
 */

#ifndef CHANNEL_H
#define CHANNEL_H

#include "frame.h"

/* ------------------------------------------------------------------ */
/*  Channel configuration                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    double error_prob;   /* probability of a bit error in a frame      */
    double loss_prob;    /* probability of a frame being lost entirely  */
    int    seed;         /* RNG seed (0 = use time)                     */
} ChannelConfig;

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void channel_init(ChannelConfig cfg);

/*
 * channel_process() – mutate frame in-place (bit errors) and
 *                     return 1 if frame survives (should be sent),
 *                     0 if frame is dropped (lost).
 */
int  channel_process(Frame *frame);

#endif /* CHANNEL_H */
