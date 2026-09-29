/*
 * receiver.c  –  Flow-Control Receiver  (Assignment 2)
 *
 * Implements:
 *   Recv()   – receives incoming Data frames
 *   Check()  – validates CRC-32 (reused from Assignment 1 crc.c)
 *   Send()   – sends ACK or NAK back to the sender
 *
 * The receiver operates in the mode that matches the sender:
 *   1 = Stop-and-Wait    (receiver window = 1)
 *   2 = Go-Back-N        (receiver window = 1, discards out-of-order)
 *   3 = Selective-Repeat (receiver window = N, buffers out-of-order)
 *
 * Usage:
 *   ./receiver <mode> <window_size> [output_file]
 *
 *   output_file : optional; reconstructed data is written here.
 *                 Default: received_output.bin
 *
 * Listens on 0.0.0.0:9000 (TCP).  Start receiver BEFORE sender.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "frame.h"
#include "crc.h"

/* ================================================================== */
/*  Constants                                                          */
/* ================================================================== */

#define LISTEN_PORT   9000
#define MAX_WINDOW    64
#define SEQ_SPACE     256

/* ================================================================== */
/*  Statistics                                                         */
/* ================================================================== */

static long g_frames_received  = 0;
static long g_frames_accepted  = 0;
static long g_frames_corrupted = 0;
static long g_frames_ooo       = 0;

/* ================================================================== */
/*  Check() – verify CRC-32 integrity (Assignment 1 crc.c reused)    */
/*  Returns 1 if frame is clean, 0 if corrupted.                      */
/* ================================================================== */

static int Check(const Frame *f)
{
    if (f->header.length == 0) return 1;  /* EOF/control frame       */
    uint32_t computed = calculateCRC32((uint8_t *)f->payload, f->header.length);
    return (computed == f->trailer.crc);
}

/* ================================================================== */
/*  Send() – transmit an ACK or NAK                                   */
/* ================================================================== */

static void Send(int connfd, uint8_t type, uint8_t seq_no)
{
    AckFrame ack;
    ack.frame_type = type;
    ack.seq_no     = seq_no;
    send(connfd, &ack, sizeof(AckFrame), 0);

    if (type == FRAME_ACK)
        printf("[RX] Sent ACK  seq=%d\n", seq_no);
    else
        printf("[RX] Sent NAK  seq=%d\n", seq_no);
}

/* ================================================================== */
/*  Stop-and-Wait receiver                                             */
/* ================================================================== */

static void recv_stop_and_wait(int connfd, FILE *out)
{
    printf("\n=== Receiver: Stop-and-Wait ===\n");
    uint8_t expected_seq = 0;

    while (1) {
        Frame f;
        ssize_t r = recv(connfd, &f, sizeof(Frame), MSG_WAITALL);
        if (r <= 0) { perror("recv"); break; }

        g_frames_received++;

        if (f.header.frame_type == FRAME_EOF) {
            printf("[RX] EOF received\n");
            break;
        }

        if (!Check(&f)) {
            g_frames_corrupted++;
            printf("[RX] Frame seq=%d CORRUPTED – sending NAK\n",
                   f.header.seq_no);
            Send(connfd, FRAME_NAK, f.header.seq_no);
            continue;
        }

        if (f.header.seq_no != expected_seq) {
            printf("[RX] Out-of-order seq=%d (expected %d) – re-ACK\n",
                   f.header.seq_no, expected_seq);
            /* Re-send ACK for the last correctly received frame */
            uint8_t last = (expected_seq == 0) ? 1 : 0;
            Send(connfd, FRAME_ACK, last);
            continue;
        }

        /* Accept frame */
        g_frames_accepted++;
        printf("[RX] Accepted frame seq=%d | %d bytes\n",
               f.header.seq_no, f.header.length);
        if (out && f.header.length > 0)
            fwrite(f.payload, 1, f.header.length, out);

        Send(connfd, FRAME_ACK, f.header.seq_no);
        expected_seq = 1 - expected_seq;  /* toggle 0/1 */
    }
}

/* ================================================================== */
/*  Go-Back-N receiver (window = 1, discards OOO)                     */
/* ================================================================== */

static void recv_go_back_n(int connfd, FILE *out)
{
    printf("\n=== Receiver: Go-Back-N ===\n");
    uint8_t expected_seq = 0;

    while (1) {
        Frame f;
        ssize_t r = recv(connfd, &f, sizeof(Frame), MSG_WAITALL);
        if (r <= 0) { perror("recv"); break; }

        g_frames_received++;

        if (f.header.frame_type == FRAME_EOF) {
            printf("[RX] EOF received\n");
            break;
        }

        if (!Check(&f)) {
            g_frames_corrupted++;
            printf("[RX] Frame seq=%d CORRUPTED – sending NAK\n",
                   f.header.seq_no);
            Send(connfd, FRAME_NAK, f.header.seq_no);
            continue;
        }

        if (f.header.seq_no != expected_seq) {
            g_frames_ooo++;
            printf("[RX] Discarding OOO frame seq=%d (expected %d)\n",
                   f.header.seq_no, expected_seq);
            /* Send cumulative ACK for last correctly received */
            uint8_t last_good = (uint8_t)((expected_seq - 1 + SEQ_SPACE) % SEQ_SPACE);
            Send(connfd, FRAME_ACK, last_good);
            continue;
        }

        g_frames_accepted++;
        printf("[RX] Accepted frame seq=%d | %d bytes\n",
               f.header.seq_no, f.header.length);
        if (out && f.header.length > 0)
            fwrite(f.payload, 1, f.header.length, out);

        Send(connfd, FRAME_ACK, f.header.seq_no);
        expected_seq = (expected_seq + 1) % SEQ_SPACE;
    }
}

/* ================================================================== */
/*  Selective-Repeat receiver (window = N, buffers OOO)               */
/* ================================================================== */

static void recv_selective_repeat(int connfd, FILE *out, int N)
{
    printf("\n=== Receiver: Selective Repeat (N=%d) ===\n", N);

    /*
     * We use a large buffer to hold frames that arrived out of order.
     * In practice we only need SEQ_SPACE slots but 4096 is safe for
     * the number of frames a small file will produce.
     */
    #define SR_BUF 4096
    Frame   *buf    = calloc(SR_BUF, sizeof(Frame));
    int     *filled = calloc(SR_BUF, sizeof(int));
    int      base   = 0;   /* next expected frame index (NOT seq no!) */

    if (!buf || !filled) { perror("calloc"); return; }

    while (1) {
        Frame f;
        ssize_t r = recv(connfd, &f, sizeof(Frame), MSG_WAITALL);
        if (r <= 0) { perror("recv"); break; }

        g_frames_received++;

        if (f.header.frame_type == FRAME_EOF) {
            printf("[RX] EOF received\n");
            break;
        }

        if (!Check(&f)) {
            g_frames_corrupted++;
            printf("[RX] Frame seq=%d CORRUPTED – sending NAK\n",
                   f.header.seq_no);
            Send(connfd, FRAME_NAK, f.header.seq_no);
            continue;
        }

        /*
         * Find the absolute frame index from the sequence number.
         * We search within the window [base, base+N-1].
         */
        int frame_idx = -1;
        for (int i = base; i < base + N && i < SR_BUF; i++) {
            if ((uint8_t)(i % SEQ_SPACE) == f.header.seq_no) {
                frame_idx = i;
                break;
            }
        }

        if (frame_idx < 0) {
            printf("[RX] seq=%d outside window – ignoring\n", f.header.seq_no);
            continue;
        }

        if (!filled[frame_idx]) {
            buf[frame_idx]    = f;
            filled[frame_idx] = 1;
            g_frames_accepted++;
            printf("[RX] Buffered frame idx=%d seq=%d | %d bytes\n",
                   frame_idx, f.header.seq_no, f.header.length);
        }

        /* Independent ACK */
        Send(connfd, FRAME_ACK, f.header.seq_no);

        /* Deliver consecutive in-order frames from base */
        while (base < SR_BUF && filled[base]) {
            printf("[RX] Delivering frame idx=%d to output\n", base);
            if (out && buf[base].header.length > 0)
                fwrite(buf[base].payload, 1, buf[base].header.length, out);
            base++;
        }
    }

    /* Flush remaining buffered frames in order */
    for (int i = base; i < SR_BUF; i++) {
        if (filled[i] && out && buf[i].header.length > 0)
            fwrite(buf[i].payload, 1, buf[i].header.length, out);
    }

    free(buf);
    free(filled);
}

/* ================================================================== */
/*  main                                                               */
/* ================================================================== */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <mode> <window_size> [output_file]\n"
            "  mode : 1=StopAndWait  2=GoBackN  3=SelectiveRepeat\n",
            argv[0]);
        return 1;
    }

    int mode = atoi(argv[1]);
    int win  = atoi(argv[2]);
    const char *outname = (argc >= 4) ? argv[3] : "received_output.bin";

    if (mode < 1 || mode > 3) {
        fprintf(stderr, "mode must be 1, 2, or 3.\n");
        return 1;
    }

    /* ---- Open output file ---- */
    FILE *out = fopen(outname, "wb");
    if (!out) { perror("fopen output"); return 1; }

    /* ---- Create TCP listening socket ---- */
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(LISTEN_PORT);

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listenfd);
        return 1;
    }
    if (listen(listenfd, 1) < 0) {
        perror("listen");
        close(listenfd);
        return 1;
    }

    printf("Receiver listening on port %d ...\n", LISTEN_PORT);

    struct sockaddr_in caddr;
    socklen_t caddrlen = sizeof(caddr);
    int connfd = accept(listenfd, (struct sockaddr *)&caddr, &caddrlen);
    if (connfd < 0) { perror("accept"); close(listenfd); return 1; }

    printf("Sender connected from %s:%d\n",
           inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));

    /* ---- Dispatch to protocol handler ---- */
    double t_start = (double)clock() / CLOCKS_PER_SEC;

    if      (mode == 1) recv_stop_and_wait(connfd, out);
    else if (mode == 2) recv_go_back_n    (connfd, out);
    else                recv_selective_repeat(connfd, out, win);

    double elapsed = (double)clock() / CLOCKS_PER_SEC - t_start;

    /* ---- Stats ---- */
    printf("\n========================================\n");
    printf(" Receiver Summary\n");
    printf("========================================\n");
    printf(" Protocol    : %s\n",
           mode == 1 ? "Stop-and-Wait" :
           mode == 2 ? "Go-Back-N" : "Selective-Repeat");
    printf(" Window size : %d\n", win);
    printf(" Frames rcvd : %ld\n", g_frames_received);
    printf(" Accepted    : %ld\n", g_frames_accepted);
    printf(" Corrupted   : %ld\n", g_frames_corrupted);
    printf(" Out-of-order: %ld\n", g_frames_ooo);
    printf(" Time        : %.4f s\n", elapsed);
    printf(" Output file : %s\n", outname);
    printf("========================================\n");

    fclose(out);
    close(connfd);
    close(listenfd);
    return 0;
}
