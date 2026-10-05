#include <PalmOS.h>

#include "fnlink.h"

#define FN_BAUD 115200L
/* A copy runs before FujiNet replies; resending mid-copy would repeat it. */
#define FN_COPY_ATTEMPTS 3
#define FN_COPY_WAIT_TICKS (SysTicksPerSecond() * 60)
#define FN_RX_BUFFER 1024

#define CMD_READ_HOST_SLOTS        0xF4
#define CMD_WRITE_HOST_SLOTS       0xF3
#define CMD_MOUNT_HOST             0xF9
#define CMD_OPEN_DIRECTORY         0xF7
#define CMD_READ_DIR_ENTRY         0xF6
#define CMD_CLOSE_DIRECTORY        0xF5
#define CMD_SET_DIRECTORY_POSITION 0xE4
#define CMD_COPY_FILE              0xD8
#define CMD_GET_ADAPTERCONFIG      0xE8

/* The firmware marks the end of a directory with two 0x7F bytes. */
#define DIR_END_MARK 0x7F
#define OPEN_DIRECTORY_LEN 256

static UInt16 gPort;
static Boolean gOpen = false;
static UInt8 gRxBuffer[FN_RX_BUFFER];
static UInt8 gFrame[FB_MAX_FRAME];

Err FnOpen(void)
{
    Err err;

    if (gOpen)
        return errNone;
    err = SrmOpen(serPortCradlePort, FN_BAUD, &gPort);
    if (err != errNone)
        return err;
    SrmSetReceiveBuffer(gPort, gRxBuffer, sizeof(gRxBuffer));
    gOpen = true;
    return errNone;
}

void FnClose(void)
{
    if (!gOpen)
        return;
    SrmSetReceiveBuffer(gPort, NULL, 0);
    SrmClose(gPort);
    gOpen = false;
}

/* Collects one SLIP frame (END ... END) or gives up at the deadline. */
static UInt16 ReceiveFrame(UInt32 deadline)
{
    UInt16 len = 0;
    UInt8 byte;
    Err err;

    while (TimGetTicks() < deadline) {
        if (SrmReceive(gPort, &byte, 1, SysTicksPerSecond() / 20, &err) != 1) {
            if (err != errNone && err != serErrTimeOut)
                SrmClearErr(gPort);
            continue;
        }
        if (len == 0 && byte != 0xC0)
            continue;
        if (byte == 0xC0 && len > 1) {
            gFrame[len++] = byte;
            return len;
        }
        if (byte == 0xC0)
            len = 0; /* back-to-back ENDs: start over */
        if (len >= sizeof(gFrame))
            len = 0;
        gFrame[len++] = byte;
    }
    return 0;
}

Err FnCallDevice(UInt8 device, UInt8 command, const FbParam *params, UInt16 nparams,
                 const void *payload, UInt16 payloadLen,
                 void *reply, UInt16 replyMax, UInt16 *replyLen,
                 UInt16 attempts, UInt32 waitTicks)
{
    UInt8 request[FB_MAX_FRAME];
    UInt16 requestLen, frameLen, i;
    FbReply parsed;
    Err err;

    if (!gOpen && (err = FnOpen()) != errNone)
        return err;
    requestLen = fb_build_request(device, command, params, nparams,
                                  (const UInt8 *)payload, payloadLen, request, sizeof(request));
    if (requestLen == 0)
        return fnErrTooBig;

    for (i = 0; i < attempts; i++) {
        SrmReceiveFlush(gPort, 0);
        SrmSend(gPort, request, requestLen, &err);
        SrmSendWait(gPort);
        frameLen = ReceiveFrame(TimGetTicks() + waitTicks);
        if (frameLen == 0 || !fb_parse_reply(gFrame, frameLen, &parsed) ||
            parsed.device != device)
            continue;
        if (parsed.command != FB_CMD_ACK)
            return fnErrRefused;
        if (replyLen)
            *replyLen = parsed.data_len < replyMax ? parsed.data_len : replyMax;
        if (reply && replyLen)
            MemMove(reply, parsed.data, *replyLen);
        return errNone;
    }
    return fnErrNoReply;
}

static Err Call(UInt8 command, const FbParam *params, UInt16 nparams,
                const void *payload, UInt16 payloadLen,
                void *reply, UInt16 replyMax, UInt16 *replyLen, UInt16 attempts)
{
    return FnCallDevice(FB_DEVICE_FUJINET, command, params, nparams, payload, payloadLen,
                        reply, replyMax, replyLen, attempts, FN_REPLY_WAIT_TICKS);
}

Err FnGetAdapterConfig(FnAdapterConfig *config)
{
    UInt16 got;

    MemSet(config, sizeof(*config), 0);
    return Call(CMD_GET_ADAPTERCONFIG, NULL, 0, NULL, 0, config, sizeof(*config), &got,
                FN_ATTEMPTS);
}

Err FnReadHostSlots(char hosts[FN_MAX_HOSTS][FN_HOSTNAME_LEN])
{
    UInt16 got, i;
    Err err;

    MemSet(hosts, FN_MAX_HOSTS * FN_HOSTNAME_LEN, 0);
    err = Call(CMD_READ_HOST_SLOTS, NULL, 0, NULL, 0, hosts,
               FN_MAX_HOSTS * FN_HOSTNAME_LEN, &got, FN_ATTEMPTS);
    for (i = 0; i < FN_MAX_HOSTS; i++)
        hosts[i][FN_HOSTNAME_LEN - 1] = '\0';
    return err;
}

Err FnWriteHostSlots(char hosts[FN_MAX_HOSTS][FN_HOSTNAME_LEN])
{
    return Call(CMD_WRITE_HOST_SLOTS, NULL, 0, hosts, FN_MAX_HOSTS * FN_HOSTNAME_LEN, NULL, 0,
                NULL, FN_ATTEMPTS);
}

Err FnMountHost(UInt8 slot)
{
    FbParam p;

    p.value = slot;
    p.size = 1;
    return Call(CMD_MOUNT_HOST, &p, 1, NULL, 0, NULL, 0, NULL, FN_ATTEMPTS);
}

/* The payload is "path\0filter\0"; an empty filter lists everything. */
Err FnOpenDirectory(UInt8 slot, const char *path)
{
    char payload[OPEN_DIRECTORY_LEN];
    UInt16 len = StrLen(path);
    FbParam p;

    if (len + 2 > sizeof(payload))
        return fnErrTooBig;
    MemSet(payload, sizeof(payload), 0);
    StrCopy(payload, path);
    p.value = slot;
    p.size = 1;
    return Call(CMD_OPEN_DIRECTORY, &p, 1, payload, len + 2, NULL, 0, NULL, FN_ATTEMPTS);
}

static Err SetDirectoryPosition(UInt16 index)
{
    FbParam p;

    p.value = index;
    p.size = 2;
    return Call(CMD_SET_DIRECTORY_POSITION, &p, 1, NULL, 0, NULL, 0, NULL, FN_ATTEMPTS);
}

/* Reading an entry advances the directory, so a retry first seeks back in
 * case FujiNet read the entry but the reply was lost. */
Err FnReadDirEntry(UInt16 index, char name[FN_DIR_ENTRY_LEN], Boolean *more)
{
    FbParam p[2];
    UInt16 got, i;
    Err err = fnErrNoReply;

    p[0].value = FN_DIR_ENTRY_LEN;
    p[0].size = 1;
    p[1].value = 0; /* no extra details */
    p[1].size = 1;
    for (i = 0; i < FN_ATTEMPTS; i++) {
        if (i > 0 && (err = SetDirectoryPosition(index)) != errNone)
            return err;
        err = Call(CMD_READ_DIR_ENTRY, p, 2, NULL, 0, name, FN_DIR_ENTRY_LEN, &got, 1);
        if (err != fnErrNoReply)
            break;
    }
    if (err != errNone)
        return err;
    name[FN_DIR_ENTRY_LEN - 1] = '\0';
    *more = !((UInt8)name[0] == DIR_END_MARK && (UInt8)name[1] == DIR_END_MARK);
    return errNone;
}

Err FnCloseDirectory(void)
{
    return Call(CMD_CLOSE_DIRECTORY, NULL, 0, NULL, 0, NULL, 0, NULL, FN_ATTEMPTS);
}

/* COPY_FILE takes 1-based slots and "source|destination"; a destination
 * ending in '/' keeps the source file name. */
Err FnCopyFile(UInt8 fromSlot, const char *path, UInt8 toSlot, const char *folder)
{
    char spec[OPEN_DIRECTORY_LEN];
    FbParam p[2];

    if (StrLen(path) + StrLen(folder) + 2 > sizeof(spec))
        return fnErrTooBig;
    StrCopy(spec, path);
    StrCat(spec, "|");
    StrCat(spec, folder);
    p[0].value = fromSlot + 1;
    p[0].size = 1;
    p[1].value = toSlot + 1;
    p[1].size = 1;
    /* A quick call that retries fast lands the copy in the bus's window, so
     * the long wait below is for the copy and not for a lost request. */
    {
        FnAdapterConfig config;
        Err err = FnGetAdapterConfig(&config);
        if (err != errNone)
            return err;
    }
    return FnCallDevice(FB_DEVICE_FUJINET, CMD_COPY_FILE, p, 2, spec, StrLen(spec), NULL, 0, NULL,
                       FN_COPY_ATTEMPTS, FN_COPY_WAIT_TICKS);
}
