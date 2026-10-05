/* FujiBus framing for a FujiNet client: SLIP-framed request/reply packets.
 *
 * A C89 port of lib/bus/rs232/FujiBusPacket.cpp in fujinet-firmware, which
 * is the authoritative definition, modelled on the client in
 * pico/o2/firmware/src/fujibus.c there. It is byte-oriented, so it works on
 * the big-endian 68000 and builds on a desktop for its self-test.
 */
#ifndef FUJIBUS_H
#define FUJIBUS_H

#include "fn_types.h"

#define FB_DEVICE_FUJINET 0x70

#define FB_CMD_ACK 0x06
#define FB_CMD_NAK 0x15

/* Largest decoded packet this client builds or accepts. READ_HOST_SLOTS
 * replies with 8 x 32 bytes, the largest used here. */
#define FB_MAX_PACKET 320

/* A SLIP frame can double every byte, plus the two END bytes. */
#define FB_MAX_FRAME (2 * FB_MAX_PACKET + 2)

typedef struct {
    fn_u32 value;
    fn_u8 size; /* 1, 2 or 4 bytes on the wire */
} FbParam;

typedef struct {
    fn_u8 device;
    fn_u8 command;          /* FB_CMD_ACK or FB_CMD_NAK */
    const fn_u8 *data;      /* points into the frame buffer */
    fn_u16 data_len;
} FbReply;

/* Builds a request frame into out; returns its length, or 0 if it does not fit. */
fn_u16 fb_build_request(fn_u8 device, fn_u8 command,
                        const FbParam *params, fn_u16 nparams,
                        const fn_u8 *payload, fn_u16 payload_len,
                        fn_u8 *out, fn_u16 out_cap);

/* Parses one frame in place (it is SLIP-decoded over itself). Returns 1 on
 * success, 0 on a framing, length or checksum error. */
int fb_parse_reply(fn_u8 *frame, fn_u16 frame_len, FbReply *reply);

#endif /* FUJIBUS_H */
