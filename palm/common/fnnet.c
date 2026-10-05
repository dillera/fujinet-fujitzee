#include <PalmOS.h>

#include "fnnet.h"

#define NETCMD_OPEN   0x4F
#define NETCMD_CLOSE  0x43
#define NETCMD_READ   0x52
#define NETCMD_WRITE  0x57
#define NETCMD_STATUS 0x53

#define URL_MAX 256
/* FujiNet's HotSync listener may take the port after 10 s without FujiBus
 * traffic; a link idle this long is reclaimed with a STATUS first. */
#define IDLE_TICKS (SysTicksPerSecond() * 5)
/* Opening a TCP connection takes FujiNet a while; resending meanwhile
 * would only restart it. */
#define OPEN_ATTEMPTS 2
#define OPEN_WAIT_TICKS (SysTicksPerSecond() * 10)

static UInt32 gLastTicks;

static Err NetCall(UInt8 unit, UInt8 command, const FbParam *params, UInt16 nparams,
                   const void *payload, UInt16 payloadLen,
                   void *reply, UInt16 replyMax, UInt16 *replyLen, UInt16 attempts)
{
    Err err = FnCallDevice(FN_NET_DEVICE(unit), command, params, nparams, payload,
                           payloadLen, reply, replyMax, replyLen, attempts,
                           FN_REPLY_WAIT_TICKS);
    if (err == errNone)
        gLastTicks = TimGetTicks();
    return err;
}

/* Makes sure FujiNet is listening before a request that is sent only once. */
static Err Reclaim(UInt8 unit)
{
    FnNetStatus status;

    if (gLastTicks != 0 && TimGetTicks() - gLastTicks < IDLE_TICKS)
        return errNone;
    return FnNetGetStatus(unit, &status);
}

Err FnNetOpen(UInt8 unit, const char *url, UInt8 mode, UInt8 trans)
{
    FbParam p[2];
    UInt16 len = StrLen(url) + 1;
    Err err;

    if (len > URL_MAX)
        return fnErrTooBig;
    if ((err = Reclaim(unit)) != errNone)
        return err;
    p[0].value = mode;
    p[0].size = 1;
    p[1].value = trans;
    p[1].size = 1;
    err = FnCallDevice(FN_NET_DEVICE(unit), NETCMD_OPEN, p, 2, url, len, NULL, 0, NULL,
                       OPEN_ATTEMPTS, OPEN_WAIT_TICKS);
    if (err == errNone)
        gLastTicks = TimGetTicks();
    return err;
}

Err FnNetClose(UInt8 unit)
{
    return NetCall(unit, NETCMD_CLOSE, NULL, 0, NULL, 0, NULL, 0, NULL, FN_ATTEMPTS);
}

/* The reply is the firmware's NDeviceStatus: avail (little-endian), conn, err. */
static Err GetStatus(UInt8 unit, FnNetStatus *status, UInt16 attempts, UInt32 waitTicks)
{
    UInt8 raw[4];
    UInt16 got = 0;
    Err err;

    MemSet(status, sizeof(*status), 0);
    err = FnCallDevice(FN_NET_DEVICE(unit), NETCMD_STATUS, NULL, 0, NULL, 0, raw,
                       sizeof(raw), &got, attempts, waitTicks);
    if (err != errNone)
        return err;
    gLastTicks = TimGetTicks();
    if (got < sizeof(raw))
        return fnErrRefused;
    status->avail = raw[0] | ((UInt16)raw[1] << 8);
    status->connected = raw[2];
    status->error = raw[3];
    return errNone;
}

Err FnNetGetStatus(UInt8 unit, FnNetStatus *status)
{
    return GetStatus(unit, status, FN_ATTEMPTS, FN_REPLY_WAIT_TICKS);
}

Err FnNetGetStatusWait(UInt8 unit, FnNetStatus *status, UInt32 waitTicks)
{
    return GetStatus(unit, status, 2, waitTicks);
}

Err FnNetRead(UInt8 unit, void *buf, UInt16 len, UInt16 *got)
{
    FbParam p;

    *got = 0;
    if (len == 0)
        return errNone;
    if (len > FN_NET_CHUNK)
        len = FN_NET_CHUNK;
    p.value = len;
    p.size = 2;
    return NetCall(unit, NETCMD_READ, &p, 1, NULL, 0, buf, len, got, 1);
}

Err FnNetWrite(UInt8 unit, const void *buf, UInt16 len)
{
    FbParam p;
    Err err;

    if (len > FN_NET_CHUNK)
        return fnErrTooBig;
    if ((err = Reclaim(unit)) != errNone)
        return err;
    p.value = len;
    p.size = 2;
    return NetCall(unit, NETCMD_WRITE, &p, 1, buf, len, NULL, 0, NULL, 1);
}
