/* FujiNet's N: network devices over the cradle, on top of fnlink.
 *
 * Unit 1 is N1: (FujiBus device 0x71). OPEN, STATUS and CLOSE can be
 * repeated safely and are resent like any fnlink call. READ and WRITE are
 * not: a resent READ would drop the bytes the lost reply carried, and a
 * resent WRITE could send the data twice. So each is sent once, and a
 * caller that sees fnErrNoReply must treat the stream as out of step (drain
 * it, or reconnect). FnNetRead should be sized by FnNetGetStatus's avail,
 * so FujiNet never waits on the network while the link waits on FujiNet.
 */
#ifndef FNNET_H
#define FNNET_H

#include "fnlink.h"

#define FN_NET_DEVICE(unit) (FB_DEVICE_FUJINET + (unit))

/* Largest single READ or WRITE; it keeps each packet under FB_MAX_PACKET. */
#define FN_NET_CHUNK 256

/* OPEN modes and translations, as for N: on the 8-bit machines. */
#define FN_NET_READ_WRITE 12
#define FN_NET_TRANS_NONE 0
/* For an HTTP URL, 12 is a plain GET. */
#define FN_NET_HTTP_GET FN_NET_READ_WRITE

/* NDEV status codes in FnNetStatus.error (status_error_codes.h). */
#define FN_NET_OK          1
#define FN_NET_EOF         136
#define FN_NET_NOT_FOUND   170

typedef struct {
    UInt16 avail;       /* bytes waiting to be read */
    UInt8 connected;
    UInt8 error;        /* the firmware's NDEV status code */
} FnNetStatus;

Err FnNetOpen(UInt8 unit, const char *url, UInt8 mode, UInt8 trans);
Err FnNetClose(UInt8 unit);
Err FnNetGetStatus(UInt8 unit, FnNetStatus *status);
/* As FnNetGetStatus, sent at most twice with a long wait for each reply. The
 * first STATUS after opening an HTTP URL is when FujiNet makes the request,
 * which can take longer than an ordinary reply; resending it meanwhile would
 * leave late STATUS replies queued in front of the next READ. */
Err FnNetGetStatusWait(UInt8 unit, FnNetStatus *status, UInt32 waitTicks);
/* Reads up to len (at most FN_NET_CHUNK) bytes; *got says how many came. */
Err FnNetRead(UInt8 unit, void *buf, UInt16 len, UInt16 *got);
Err FnNetWrite(UInt8 unit, const void *buf, UInt16 len);

#endif /* FNNET_H */
