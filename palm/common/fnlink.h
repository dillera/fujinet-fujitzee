/* FujiNet over the HotSync cradle's serial port, speaking FujiBus.
 *
 * FujiNet shares that port with its HotSync listener, which takes it for
 * 1.5 s at a time, so every call resends its request until a reply arrives.
 */
#ifndef FNLINK_H
#define FNLINK_H

#include "fujibus.h"

#define FN_MAX_HOSTS 8
#define FN_HOSTNAME_LEN 32
#define FN_DIR_ENTRY_LEN 64

/* Long enough to span a 1.5 s HotSync window more than twice. */
#define FN_ATTEMPTS 8
#define FN_REPLY_WAIT_TICKS (SysTicksPerSecond() * 7 / 10)

/* Errors, in the application error range. */
#define fnErrNoReply  (appErrorClass | 1)
#define fnErrRefused  (appErrorClass | 2) /* FujiNet answered NAK */
#define fnErrTooBig   (appErrorClass | 3)

/* Mirrors AdapterConfig in fujinet-firmware lib/device/fujiDevice/fujiDevice.h. */
typedef struct {
    char ssid[33];
    char hostname[64];
    UInt8 localIP[4];
    UInt8 gateway[4];
    UInt8 netmask[4];
    UInt8 dnsIP[4];
    UInt8 macAddress[6];
    UInt8 bssid[6];
    char fn_version[15];
} FnAdapterConfig;

Err FnOpen(void);
void FnClose(void);

/* One request/reply exchange with any FujiBus device, sent up to attempts
 * times until it answers, waiting waitTicks for each reply. */
Err FnCallDevice(UInt8 device, UInt8 command, const FbParam *params, UInt16 nparams,
                 const void *payload, UInt16 payloadLen,
                 void *reply, UInt16 replyMax, UInt16 *replyLen,
                 UInt16 attempts, UInt32 waitTicks);

Err FnGetAdapterConfig(FnAdapterConfig *config);
Err FnReadHostSlots(char hosts[FN_MAX_HOSTS][FN_HOSTNAME_LEN]);
/* Replaces all host slots at once; FujiNet saves them to its config. */
Err FnWriteHostSlots(char hosts[FN_MAX_HOSTS][FN_HOSTNAME_LEN]);
Err FnMountHost(UInt8 slot);
Err FnOpenDirectory(UInt8 slot, const char *path);
/* Reads the entry at index into name; *more is false after the last one. */
Err FnReadDirEntry(UInt16 index, char name[FN_DIR_ENTRY_LEN], Boolean *more);
Err FnCloseDirectory(void);
/* Copies path on one host slot into folder (ending in '/') on another. */
Err FnCopyFile(UInt8 fromSlot, const char *path, UInt8 toSlot, const char *folder);

#endif /* FNLINK_H */
