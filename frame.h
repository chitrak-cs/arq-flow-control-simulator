/*
 * frame.h  –  Data Link Layer Frame Structure  (Assignment 2)
 *
 * Frame layout (matches the assignment spec):
 *   Header  : 12 bytes
 *     src_mac   [6]  – source MAC address
 *     dst_mac   [6]  – destination MAC address  (unused in loopback but kept for spec compliance)
 *     length    [2]  – payload length in bytes (0 = EOF / control)
 *     seq_no    [1]  – frame sequence number
 *     frame_type[1]  – DATA / ACK / NAK
 *   Data    : PAYLOAD_SIZE bytes  (configurable, default 46 to match spec minimum)
 *   Trailer : 4 bytes  – CRC-32 over payload
 *
 * Total frame size = HEADER_SIZE + PAYLOAD_SIZE + TRAILER_SIZE
 */

#ifndef FRAME_H
#define FRAME_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Size constants                                                      */
/* ------------------------------------------------------------------ */

#define PAYLOAD_SIZE    46      /* bytes – spec says 46-1500, we use 46 */
#define HEADER_SIZE     12      /* src(6)+dst(6)+len(2)+seq(1)+type(1)  */
#define TRAILER_SIZE    4       /* CRC-32                               */
#define FRAME_SIZE      (HEADER_SIZE + PAYLOAD_SIZE + TRAILER_SIZE)

/* ------------------------------------------------------------------ */
/*  Frame type codes                                                    */
/* ------------------------------------------------------------------ */

#define FRAME_DATA  0x01
#define FRAME_ACK   0x02
#define FRAME_NAK   0x03
#define FRAME_EOF   0x04   /* signals end of transmission              */

/* ------------------------------------------------------------------ */
/*  In-memory frame structure                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  src_mac[6];
    uint8_t  dst_mac[6];
    uint16_t length;        /* payload bytes actually used             */
    uint8_t  seq_no;        /* sequence number                         */
    uint8_t  frame_type;    /* FRAME_DATA / FRAME_ACK / FRAME_NAK ...  */
} FrameHeader;

typedef struct {
    uint32_t crc;           /* CRC-32 over payload                     */
} FrameTrailer;

typedef struct {
    FrameHeader  header;
    uint8_t      payload[PAYLOAD_SIZE];
    FrameTrailer trailer;
} Frame;

/* ------------------------------------------------------------------ */
/*  ACK frame  (only header + trailer, no payload)                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  frame_type;    /* FRAME_ACK or FRAME_NAK                  */
    uint8_t  seq_no;        /* which frame is being acknowledged       */
} AckFrame;

#endif /* FRAME_H */
