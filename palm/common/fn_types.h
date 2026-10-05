/* Fixed-width types for code shared by the Palm apps and desktop tests. */
#ifndef FN_TYPES_H
#define FN_TYPES_H

#ifdef __palmos__
#include <PalmOS.h>
typedef UInt8 fn_u8;
typedef UInt16 fn_u16;
typedef UInt32 fn_u32;
#define fn_memcpy(dst, src, n) MemMove((dst), (src), (n))
/* The app's creator ID, from CREATOR in its Makefile (see apps/app.mk). */
#ifdef APP_CREATOR_STR
#define APP_CREATOR (((UInt32)(UInt8)APP_CREATOR_STR[0] << 24) | ((UInt32)(UInt8)APP_CREATOR_STR[1] << 16) | \
                     ((UInt32)(UInt8)APP_CREATOR_STR[2] << 8) | (UInt32)(UInt8)APP_CREATOR_STR[3])
#endif
#else
#include <stdint.h>
#include <string.h>
typedef uint8_t fn_u8;
typedef uint16_t fn_u16;
typedef uint32_t fn_u32;
#define fn_memcpy(dst, src, n) memmove((dst), (src), (n))
#endif

#endif /* FN_TYPES_H */
