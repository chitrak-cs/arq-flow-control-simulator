/*
 * sender.c  –  Flow-Control Sender  (Assignment 2)
 *
 * Implements:
 *   Framing()   – builds a Frame from raw file bytes + CRC-32 trailer
 *   Channel()   – sends frame bytes through an unreliable socket channel
 *   Send()      – transmits one frame (Stop-and-Wait) or a window (GBN / SR)
 *   Timer()     – per-frame retransmission timer using POSIX clock
 *   Timeout()   – computes adaptive timeout using RTT estimation
 *   Recv()      – receives and processes an ACK/NAK from the receiver
 *
 * Flow-control modes (selected by command-line argument):
 *   1 = Stop-and-Wait
 *   2 = Go-Back-N       (window size N, receiver window = 1)
 *   3 = Selective-Repeat (window size N, receiver buffers OOO)
 *
 * Usage:
 *   ./sender <mode> <window_size> <error_prob> <loss_prob> <input_file>
 *
 *   mode        : 1, 2, or 3
 *   window_size : N (ignored for mode 1, used for 2 and 3)
 *   error_prob  : probability [0.0–0.5] of a bit flip in a frame
 *   loss_prob   : probability [0.0–0.5] of a frame being lost
 *   input_file  : file to transmit
 *
 * The sender connects to 127.0.0.1:9000 (TCP).
 * Run receiver first, then sender.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/select.h>

#include "frame.h"
#include "crc.h"
#include "channel.h"

/* ================================================================== */
/*  Constants & tunables                                               */
/* ================================================================== */

#define SERVER_IP        "127.0.0.1"
#define SERVER_PORT      9000
#define MAX_WINDOW       64          /* maximum allowed window size     */
#define SEQ_SPACE        256         /* sequence number space  (0-255)  */
#define INITIAL_TIMEOUT  2.0         /* initial RTO in seconds          */
#define ALPHA            0.125       /* RTT EWMA smoothing factor       */
#define BETA             0.25        /* RTTVAR EWMA factor              */

/* ================================================================== */
/*  Timer state  (per frame slot)                                      */
/* ================================================================== */

typedef struct {
    struct timespec sent_at;
    int             active;
} Timer;

/* ================================================================== */
/*  Global sender state                                                */
/* ================================================================== */

static int     g_sockfd      = -1;
static double  g_rtt_est     = 0.0;   /* smoothed RTT                 */
static double  g_rtt_dev     = 0.0;   /* RTT deviation                */
static double  g_timeout     = INITIAL_TIMEOUT;

/* Statistics */
static long    g_total_sent         = 0;
static long    g_total_retransmit   = 0;
static long    g_total_acked        = 0;

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static struct timespec now_ts(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

static double elapsed_sec(struct timespec start)
{
    return now_sec() - ((double)start.tv_sec + (double)start.tv_nsec * 1e-9);
}

/* ------------------------------------------------------------------ */
/*  Timeout() – update RTO using RFC 6298 style estimator             */
/* ------------------------------------------------------------------ */

static void update_timeout(double rtt_sample)
{
    if (g_rtt_est == 0.0) {
        /* first measurement */
        g_rtt_est = rtt_sample;
        g_rtt_dev = rtt_sample / 2.0;
    } else {
        double diff = rtt_sample - g_rtt_est;
        g_rtt_est += ALPHA * diff;
        g_rtt_dev += BETA * (diff < 0 ? -diff : diff);
    }
    g_timeout = g_rtt_est + 4.0 * g_rtt_dev;
    if (g_timeout < 0.1) g_timeout = 0.1;   /* floor */
    if (g_timeout > 10.0) g_timeout = 10.0; /* cap   */
}

/* ================================================================== */
/*  Framing()                                                          */
/*  Fills a Frame struct from raw bytes; computes CRC-32 trailer.     */
/* ================================================================== */

static void Framing(Frame *f,
                    const uint8_t *payload, uint16_t len,
                    uint8_t seq_no, uint8_t frame_type)
{
    static const uint8_t SRC_MAC[6] = {0x00,0x11,0x22,0x33,0x44,0x55};
    static const uint8_t DST_MAC[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};

    memset(f, 0, sizeof(Frame));

    memcpy(f->header.src_mac, SRC_MAC, 6);
    memcpy(f->header.dst_mac, DST_MAC, 6);
    f->header.length     = len;
    f->header.seq_no     = seq_no;
    f->header.frame_type = frame_type;

    if (payload && len > 0)
        memcpy(f->payload, payload, len);

    /* CRC-32 over payload (from Assignment 1 crc.c) */
    f->trailer.crc = (len > 0)
        ? calculateCRC32(f->payload, len)
        : 0;
}

/* ================================================================== */
/*  Channel() / send one frame over the socket                        */
/*  Applies simulated channel errors then transmits.                  */
/*  Returns 0 if the channel dropped the frame, 1 otherwise.         */
/* ================================================================== */

static int Channel(Frame *f)
{
    Frame copy = *f;   /* mutate a copy so we can retransmit original */
    int survived = channel_process(&copy);

    if (!survived) {
        printf("    [CHANNEL] Frame seq=%d LOST (simulated drop)\n",
               f->header.seq_no);
        return 0;
    }

    ssize_t sent = send(g_sockfd, &copy, sizeof(Frame), 0);
    if (sent < 0) {
        perror("send()");
        return -1;
    }
    return 1;
}

/* ================================================================== */
/*  Recv() – receive one ACK/NAK from the receiver                    */
/*  Blocks up to timeout_sec seconds; returns:                        */
/*    -1  = socket error                                               */
/*     0  = timeout                                                    */
/*     1  = ACK received  (*ack filled)                               */
/*     2  = NAK received  (*ack filled)                               */
/* ================================================================== */

static int Recv(AckFrame *ack, double timeout_sec)
{
    fd_set rfds;
    struct timeval tv;

    FD_ZERO(&rfds);
    FD_SET(g_sockfd, &rfds);

    tv.tv_sec  = (long)timeout_sec;
    tv.tv_usec = (long)((timeout_sec - (long)timeout_sec) * 1e6);

    int ret = select(g_sockfd + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0)  { perror("select()"); return -1; }
    if (ret == 0) { return 0; }   /* timeout */

    ssize_t r = recv(g_sockfd, ack, sizeof(AckFrame), 0);
    if (r <= 0)   { perror("recv()"); return -1; }

    return (ack->frame_type == FRAME_ACK) ? 1 : 2;
}

/* ================================================================== */
/*  ----------------------------------------------------------------   */
/*                 STOP-AND-WAIT                                        */
/*  ----------------------------------------------------------------   */
/* ================================================================== */

static void run_stop_and_wait(uint8_t **frames, uint16_t *lens,
                               int total_frames)
{
    printf("\n=== Stop-and-Wait ===\n");

    int next = 0;
    while (next < total_frames) {
        Frame f;
        Framing(&f, frames[next], lens[next], next % 2, FRAME_DATA);

        int acked = 0;
        int retries = 0;

        while (!acked) {
            double t_send = now_sec();
            int ch = Channel(&f);
            g_total_sent++;

            if (ch > 0) {
                printf("[SAW] Sent frame %d (seq=%d), timeout=%.2fs\n",
                       next, f.header.seq_no, g_timeout);
            } else {
                printf("[SAW] Frame %d dropped by channel\n", next);
            }

            AckFrame ack;
            int r = Recv(&ack, g_timeout);

            if (r == 1 && ack.seq_no == f.header.seq_no) {
                double rtt = now_sec() - t_send;
                update_timeout(rtt);
                printf("[SAW] ACK %d received | RTT=%.4fs | RTO=%.4fs\n",
                       ack.seq_no, rtt, g_timeout);
                acked = 1;
                g_total_acked++;
            } else if (r == 2) {
                printf("[SAW] NAK received – retransmitting frame %d\n", next);
                g_total_retransmit++;
                retries++;
            } else {
                printf("[SAW] Timeout / bad ACK – retransmitting frame %d\n", next);
                g_total_retransmit++;
                retries++;
            }

            if (retries > 20) {
                fprintf(stderr, "[SAW] Too many retries – aborting.\n");
                return;
            }
        }
        next++;
    }

    /* EOF frame */
    Frame eof;
    Framing(&eof, NULL, 0, 0, FRAME_EOF);
    send(g_sockfd, &eof, sizeof(Frame), 0);
    printf("[SAW] EOF sent. Total sent=%ld, retransmits=%ld\n",
           g_total_sent, g_total_retransmit);
}

/* ================================================================== */
/*  ----------------------------------------------------------------   */
/*                 GO-BACK-N                                            */
/*  ----------------------------------------------------------------   */
/* ================================================================== */

static void run_go_back_n(uint8_t **frames, uint16_t *lens,
                           int total_frames, int N)
{
    printf("\n=== Go-Back-N  (N=%d) ===\n", N);

    int base   = 0;   /* oldest unacknowledged frame index */
    int next_seq = 0; /* next frame to send                */
    Timer timers[MAX_WINDOW];
    memset(timers, 0, sizeof(timers));

    /*
     * Build all Frame objects upfront.
     * Sequence numbers wrap mod SEQ_SPACE.
     */
    Frame *fbufs = calloc(total_frames, sizeof(Frame));
    if (!fbufs) { perror("calloc"); return; }
    for (int i = 0; i < total_frames; i++)
        Framing(&fbufs[i], frames[i], lens[i], (uint8_t)(i % SEQ_SPACE), FRAME_DATA);

    while (base < total_frames) {
        /* Send as many frames as the window allows */
        while (next_seq < total_frames && next_seq - base < N) {
            int ch = Channel(&fbufs[next_seq]);
            g_total_sent++;
            timers[next_seq % MAX_WINDOW].sent_at = now_ts();
            timers[next_seq % MAX_WINDOW].active   = 1;
            if (ch > 0)
                printf("[GBN] Sent frame %d (seq=%d)\n",
                       next_seq, fbufs[next_seq].header.seq_no);
            else
                printf("[GBN] Frame %d dropped by channel\n", next_seq);
            next_seq++;
        }

        /* Wait for ACK with timeout on oldest unacked frame */
        double wait = g_timeout - elapsed_sec(timers[base % MAX_WINDOW].sent_at);
        if (wait < 0.01) wait = 0.01;

        AckFrame ack;
        int r = Recv(&ack, wait);

        if (r == 1) {
            /* Cumulative ACK: ack.seq_no acknowledges all frames up to that seq */
            int ack_idx = -1;
            /* find which frame index matches this seq */
            for (int i = base; i < next_seq; i++) {
                if (fbufs[i].header.seq_no == ack.seq_no) {
                    ack_idx = i;
                    break;
                }
            }
            if (ack_idx >= base) {
                double rtt = elapsed_sec(timers[base % MAX_WINDOW].sent_at);
                update_timeout(rtt);
                printf("[GBN] ACK %d  → sliding base %d→%d | RTO=%.4fs\n",
                       ack.seq_no, base, ack_idx + 1, g_timeout);
                /* slide window */
                for (int i = base; i <= ack_idx; i++)
                    timers[i % MAX_WINDOW].active = 0;
                base     = ack_idx + 1;
                g_total_acked += (ack_idx - base + 2);
            }
        } else if (r == 2) {
            /* NAK: retransmit from base */
            printf("[GBN] NAK received – retransmitting from frame %d\n", base);
            next_seq = base;   /* go back */
            g_total_retransmit += (next_seq - base);
        } else {
            /* Timeout on base frame: go back to base */
            if (elapsed_sec(timers[base % MAX_WINDOW].sent_at) >= g_timeout) {
                printf("[GBN] Timeout – retransmitting window from frame %d\n", base);
                g_total_retransmit += (next_seq - base);
                next_seq = base;
            }
        }
    }

    /* EOF */
    Frame eof;
    Framing(&eof, NULL, 0, 0, FRAME_EOF);
    send(g_sockfd, &eof, sizeof(Frame), 0);
    printf("[GBN] EOF sent. Total sent=%ld, retransmits=%ld\n",
           g_total_sent, g_total_retransmit);

    free(fbufs);
}

/* ================================================================== */
/*  ----------------------------------------------------------------   */
/*                 SELECTIVE REPEAT                                     */
/*  ----------------------------------------------------------------   */
/* ================================================================== */

static void run_selective_repeat(uint8_t **frames, uint16_t *lens,
                                  int total_frames, int N)
{
    printf("\n=== Selective Repeat  (N=%d) ===\n", N);

    int base     = 0;
    int next_seq = 0;
    int acked[4096];   /* bool: has frame i been ACKed? */
    memset(acked, 0, sizeof(acked));
    Timer timers[MAX_WINDOW];
    memset(timers, 0, sizeof(timers));

    Frame *fbufs = calloc(total_frames, sizeof(Frame));
    if (!fbufs) { perror("calloc"); return; }
    for (int i = 0; i < total_frames; i++)
        Framing(&fbufs[i], frames[i], lens[i], (uint8_t)(i % SEQ_SPACE), FRAME_DATA);

    while (base < total_frames) {

        /* Fill window */
        while (next_seq < total_frames && next_seq - base < N) {
            if (!acked[next_seq]) {
                int ch = Channel(&fbufs[next_seq]);
                g_total_sent++;
                timers[next_seq % MAX_WINDOW].sent_at = now_ts();
                timers[next_seq % MAX_WINDOW].active   = 1;
                if (ch > 0)
                    printf("[SR] Sent frame %d (seq=%d)\n",
                           next_seq, fbufs[next_seq].header.seq_no);
                else
                    printf("[SR] Frame %d dropped by channel\n", next_seq);
            }
            next_seq++;
        }

        /* Wait for any ACK */
        double min_wait = g_timeout;
        for (int i = base; i < next_seq; i++) {
            if (timers[i % MAX_WINDOW].active && !acked[i]) {
                double rem = g_timeout - elapsed_sec(timers[i % MAX_WINDOW].sent_at);
                if (rem < min_wait) min_wait = rem;
            }
        }
        if (min_wait < 0.01) min_wait = 0.01;

        AckFrame ack;
        int r = Recv(&ack, min_wait);

        if (r == 1) {
            /* Independent ACK: acknowledge exactly this frame */
            int ack_idx = -1;
            for (int i = base; i < next_seq; i++) {
                if (fbufs[i].header.seq_no == ack.seq_no) {
                    ack_idx = i;
                    break;
                }
            }
            if (ack_idx >= 0 && !acked[ack_idx]) {
                double rtt = elapsed_sec(timers[ack_idx % MAX_WINDOW].sent_at);
                update_timeout(rtt);
                acked[ack_idx] = 1;
                timers[ack_idx % MAX_WINDOW].active = 0;
                g_total_acked++;
                printf("[SR] ACK %d (frame %d) | RTO=%.4fs\n",
                       ack.seq_no, ack_idx, g_timeout);
            }
            /* Slide base over consecutive ACKed frames */
            while (base < total_frames && acked[base])
                base++;
        } else if (r == 2) {
            /* NAK: retransmit only that specific frame */
            int nak_idx = -1;
            for (int i = base; i < next_seq; i++) {
                if (fbufs[i].header.seq_no == ack.seq_no) {
                    nak_idx = i;
                    break;
                }
            }
            if (nak_idx >= 0) {
                printf("[SR] NAK %d – retransmitting frame %d\n",
                       ack.seq_no, nak_idx);
                int ch = Channel(&fbufs[nak_idx]);
                g_total_sent++;
                g_total_retransmit++;
                timers[nak_idx % MAX_WINDOW].sent_at = now_ts();
                if (ch <= 0)
                    printf("[SR] Retransmit of frame %d also dropped\n", nak_idx);
            }
        } else {
            /* Timeout: retransmit all unACKed frames whose timer expired */
            for (int i = base; i < next_seq; i++) {
                if (!acked[i] && timers[i % MAX_WINDOW].active &&
                    elapsed_sec(timers[i % MAX_WINDOW].sent_at) >= g_timeout) {
                    printf("[SR] Timeout – retransmitting frame %d\n", i);
                    int ch = Channel(&fbufs[i]);
                    g_total_sent++;
                    g_total_retransmit++;
                    timers[i % MAX_WINDOW].sent_at = now_ts();
                    if (ch <= 0)
                        printf("[SR] Retransmit of frame %d dropped\n", i);
                }
            }
        }
    }

    /* EOF */
    Frame eof;
    Framing(&eof, NULL, 0, 0, FRAME_EOF);
    send(g_sockfd, &eof, sizeof(Frame), 0);
    printf("[SR] EOF sent. Total sent=%ld, retransmits=%ld\n",
           g_total_sent, g_total_retransmit);

    free(fbufs);
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */

int main(int argc, char *argv[])
{
    if (argc != 6) {
        fprintf(stderr,
            "Usage: %s <mode> <window> <error_prob> <loss_prob> <file>\n"
            "  mode : 1=StopAndWait  2=GoBackN  3=SelectiveRepeat\n"
            "  window : window size N (mode 2 & 3)\n"
            "  error_prob : [0.0-0.5] bit-error probability\n"
            "  loss_prob  : [0.0-0.5] frame-loss probability\n",
            argv[0]);
        return 1;
    }

    int    mode       = atoi(argv[1]);
    int    win        = atoi(argv[2]);
    double err_prob   = atof(argv[3]);
    double loss_prob  = atof(argv[4]);
    const char *fname = argv[5];

    if (mode < 1 || mode > 3) {
        fprintf(stderr, "mode must be 1, 2, or 3.\n");
        return 1;
    }
    if (win < 1 || win > MAX_WINDOW) {
        fprintf(stderr, "window must be 1-%d.\n", MAX_WINDOW);
        return 1;
    }

    /* ---- Initialise channel ---- */
    ChannelConfig ccfg = { err_prob, loss_prob, 0 };
    channel_init(ccfg);

    /* ---- Read input file into frame payloads ---- */
    FILE *fp = fopen(fname, "rb");
    if (!fp) { perror("fopen"); return 1; }

    /* Count frames first */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    rewind(fp);

    int total_frames = (int)((file_size + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE);
    if (total_frames == 0) { fprintf(stderr, "Empty file.\n"); fclose(fp); return 1; }

    uint8_t  **payloads = calloc(total_frames, sizeof(uint8_t *));
    uint16_t  *lens     = calloc(total_frames, sizeof(uint16_t));

    for (int i = 0; i < total_frames; i++) {
        payloads[i] = calloc(1, PAYLOAD_SIZE);
        size_t n = fread(payloads[i], 1, PAYLOAD_SIZE, fp);
        lens[i] = (uint16_t)n;
    }
    fclose(fp);

    printf("File: %s  (%ld bytes, %d frames)\n", fname, file_size, total_frames);

    /* ---- Connect to receiver ---- */
    g_sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sockfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family      = AF_INET;
    srv.sin_port        = htons(SERVER_PORT);
    srv.sin_addr.s_addr = inet_addr(SERVER_IP);

    if (connect(g_sockfd, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("connect");
        close(g_sockfd);
        return 1;
    }
    printf("Connected to receiver at %s:%d\n", SERVER_IP, SERVER_PORT);

    /* ---- Run selected protocol ---- */
    double t_start = now_sec();

    if      (mode == 1) run_stop_and_wait(payloads, lens, total_frames);
    else if (mode == 2) run_go_back_n    (payloads, lens, total_frames, win);
    else                run_selective_repeat(payloads, lens, total_frames, win);

    double elapsed = now_sec() - t_start;

    /* ---- Stats ---- */
    printf("\n========================================\n");
    printf(" Transmission Summary\n");
    printf("========================================\n");
    printf(" Protocol    : %s\n",
           mode == 1 ? "Stop-and-Wait" :
           mode == 2 ? "Go-Back-N" : "Selective-Repeat");
    printf(" Window size : %d\n", win);
    printf(" Error prob  : %.2f\n", err_prob);
    printf(" Loss prob   : %.2f\n", loss_prob);
    printf(" Frames sent : %ld  (retransmits: %ld)\n",
           g_total_sent, g_total_retransmit);
    printf(" Total time  : %.4f s\n", elapsed);
    double efficiency = (total_frames > 0)
        ? (100.0 * total_frames / g_total_sent) : 0.0;
    printf(" Efficiency  : %.2f%%\n", efficiency);
    printf("========================================\n");

    /* ---- Cleanup ---- */
    close(g_sockfd);
    for (int i = 0; i < total_frames; i++) free(payloads[i]);
    free(payloads);
    free(lens);
    return 0;
}
