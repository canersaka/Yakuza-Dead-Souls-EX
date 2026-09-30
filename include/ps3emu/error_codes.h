/*
 * ps3recomp - PS3 / CellOS error codes
 *
 * Values match the codes the firmware returns.  All codes are negative 32-bit
 * integers except CELL_OK (0).  Module-specific error bases follow the
 * generic codes.
 */

#ifndef PS3EMU_ERROR_CODES_H
#define PS3EMU_ERROR_CODES_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Generic system error codes  (0x8001_xxxx)
 * -----------------------------------------------------------------------*/
#define CELL_OK                         0x00000000

#define CELL_EAGAIN                     0x80010001  /* Resource temporarily unavailable */
#define CELL_EINVAL                     0x80010002  /* Invalid argument */
#define CELL_ENOSYS                     0x80010003  /* Function not implemented */
#define CELL_ENOMEM                     0x80010004  /* Not enough memory */
#define CELL_ESRCH                      0x80010005  /* No such entity */
#define CELL_ENOENT                     0x80010006  /* No such file or directory */
#define CELL_ENOEXEC                    0x80010007  /* Exec format error */
#define CELL_EDEADLK                    0x80010008  /* Resource deadlock avoided */
#define CELL_EPERM                      0x80010009  /* Not permitted */
#define CELL_EBUSY                      0x8001000A  /* Device or resource busy */
#define CELL_ETIMEDOUT                  0x8001000B  /* Connection timed out */
#define CELL_EABORT                     0x8001000C  /* Operation aborted */
#define CELL_EFAULT                     0x8001000D  /* Bad address */
#define CELL_ESTAT                      0x8001000F  /* State error */
#define CELL_EALIGN                     0x80010010  /* Alignment error */
#define CELL_EKRESOURCE                 0x80010011  /* Kernel resource shortage */
#define CELL_EISDIR                     0x80010012  /* Is a directory */
#define CELL_ECANCELED                  0x80010013  /* Operation canceled */
#define CELL_EEXIST                     0x80010014  /* Already exists */
#define CELL_EISCONN                    0x80010015  /* Already connected */
#define CELL_ENOTCONN                   0x80010016  /* Not connected */
#define CELL_EAUTHFAIL                  0x80010017  /* Authentication failed */
#define CELL_ENOTMSELF                  0x80010018  /* Not a valid MSELF */
#define CELL_ESYSVER                    0x80010019  /* System version error */
#define CELL_EBADF                      0x8001002A  /* Bad file descriptor */

/* ---------------------------------------------------------------------------
 * Module-specific error bases
 *
 * Each module error is (base | local_code).
 * -----------------------------------------------------------------------*/
#define CELL_ERROR_BASE_SYSMODULE       0x80012000
#define CELL_ERROR_BASE_FS              0x80010700
#define CELL_ERROR_BASE_AUDIO           0x80310700
#define CELL_ERROR_BASE_VIDEO           0x8002B100
/* Real libgcm base is 0x80210000 (facility GFX 0x021, range
 * 0x8021_0001 - 0x8021_00ff). */
#define CELL_ERROR_BASE_GCM             0x80210000
#define CELL_ERROR_BASE_NET             0x80130100
#define CELL_ERROR_BASE_PAD             0x80121100
#define CELL_ERROR_BASE_KB              0x80121200
#define CELL_ERROR_BASE_MOUSE           0x80121300
#define CELL_ERROR_BASE_SYSUTIL         0x8002B000
#define CELL_ERROR_BASE_SYSUTIL_SAVE    0x8002B400
/* Real cellGame base is 0x8002cb00. Offsets are sparse (04-09, 20-28, 50),
 * so cellGame.h defines each CELL_GAME_ERROR_* value verbatim instead of
 * OR-ing this base. Kept only as documentation of the range. */
#define CELL_ERROR_BASE_SYSUTIL_GAME    0x8002CB00
#define CELL_ERROR_BASE_PNG             0x80611200
#define CELL_ERROR_BASE_JPG             0x80611300
#define CELL_ERROR_BASE_FONT            0x80540000
#define CELL_ERROR_BASE_SPURS           0x80410700
#define CELL_ERROR_BASE_FIBER           0x80410800
#define CELL_ERROR_BASE_SYNC            0x80410100
#define CELL_ERROR_BASE_USBD            0x80110100
#define CELL_ERROR_BASE_HTTP            0x80710100
#define CELL_ERROR_BASE_SSL             0x80720100
#define CELL_ERROR_BASE_VDEC            0x80610100
#define CELL_ERROR_BASE_ADEC            0x80610200

/* sysmodule convenience errors */
#define CELL_SYSMODULE_ERROR_DUPLICATED     (CELL_ERROR_BASE_SYSMODULE | 0x01)
#define CELL_SYSMODULE_ERROR_UNKNOWN        (CELL_ERROR_BASE_SYSMODULE | 0x02)
#define CELL_SYSMODULE_ERROR_UNLOADED       (CELL_ERROR_BASE_SYSMODULE | 0x03)
#define CELL_SYSMODULE_ERROR_FATAL          (CELL_ERROR_BASE_SYSMODULE | 0xFF)

/* sysutil errors: the common block is at 0x8002b1xx, not the 0x8002b0xx
 * internal block. */
#define CELL_SYSUTIL_ERROR_TYPE             0x8002b101
#define CELL_SYSUTIL_ERROR_VALUE            0x8002b102
#define CELL_SYSUTIL_ERROR_SIZE             0x8002b103
#define CELL_SYSUTIL_ERROR_NUM              0x8002b104
#define CELL_SYSUTIL_ERROR_BUSY             0x8002b105
#define CELL_SYSUTIL_ERROR_STATUS           0x8002b106
#define CELL_SYSUTIL_ERROR_MEMORY           0x8002b107

/* pad errors */
#define CELL_PAD_ERROR_FATAL                (CELL_ERROR_BASE_PAD | 0x01)
#define CELL_PAD_ERROR_INVALID_PARAMETER    (CELL_ERROR_BASE_PAD | 0x02)
#define CELL_PAD_ERROR_ALREADY_OPENED       (CELL_ERROR_BASE_PAD | 0x03)
#define CELL_PAD_ERROR_NOT_OPENED           (CELL_ERROR_BASE_PAD | 0x04)
/* NO_DEVICE is 0x80121107; |0x05 is RESOURCE_ALLOCATION_FAILED's slot. */
#define CELL_PAD_ERROR_NO_DEVICE            (CELL_ERROR_BASE_PAD | 0x07)

/* audio errors */
#define CELL_AUDIO_ERROR_ALREADY_INIT       (CELL_ERROR_BASE_AUDIO | 0x01)
#define CELL_AUDIO_ERROR_AUDIOSYSTEM        (CELL_ERROR_BASE_AUDIO | 0x02)
#define CELL_AUDIO_ERROR_NOT_INIT           (CELL_ERROR_BASE_AUDIO | 0x03)
#define CELL_AUDIO_ERROR_PARAM              (CELL_ERROR_BASE_AUDIO | 0x04)
#define CELL_AUDIO_ERROR_PORT_FULL          (CELL_ERROR_BASE_AUDIO | 0x05)
#define CELL_AUDIO_ERROR_PORT_ALREADY_RUN   (CELL_ERROR_BASE_AUDIO | 0x06)
#define CELL_AUDIO_ERROR_PORT_NOT_OPEN      (CELL_ERROR_BASE_AUDIO | 0x07)
#define CELL_AUDIO_ERROR_PORT_NOT_RUN       (CELL_ERROR_BASE_AUDIO | 0x08)
/* 0x80310709 - 0x8031070F, sequential */
#define CELL_AUDIO_ERROR_TRANS_EVENT             (CELL_ERROR_BASE_AUDIO | 0x09)
#define CELL_AUDIO_ERROR_PORT_OPEN               (CELL_ERROR_BASE_AUDIO | 0x0A)
#define CELL_AUDIO_ERROR_SHAREDMEMORY            (CELL_ERROR_BASE_AUDIO | 0x0B)
#define CELL_AUDIO_ERROR_MUTEX                   (CELL_ERROR_BASE_AUDIO | 0x0C)
#define CELL_AUDIO_ERROR_EVENT_QUEUE             (CELL_ERROR_BASE_AUDIO | 0x0D)
#define CELL_AUDIO_ERROR_AUDIOSYSTEM_NOT_FOUND   (CELL_ERROR_BASE_AUDIO | 0x0E)
#define CELL_AUDIO_ERROR_TAG_NOT_FOUND           (CELL_ERROR_BASE_AUDIO | 0x0F)

/* GCM errors:
 *   FAILURE          0x802100ff
 *   NO_IO_PAGE_TABLE 0x80210001
 *   INVALID_ENUM     0x80210002
 *   INVALID_VALUE    0x80210003
 *   INVALID_ALIGNMENT 0x80210004
 *   ADDRESS_OVERWRAP 0x80210005 */
#define CELL_GCM_ERROR_FAILURE              (CELL_ERROR_BASE_GCM | 0xFF)
#define CELL_GCM_ERROR_NO_IO_PAGE_TABLE     (CELL_ERROR_BASE_GCM | 0x01)
#define CELL_GCM_ERROR_INVALID_ENUM         (CELL_ERROR_BASE_GCM | 0x02)
#define CELL_GCM_ERROR_INVALID_VALUE        (CELL_ERROR_BASE_GCM | 0x03)
#define CELL_GCM_ERROR_INVALID_ALIGNMENT    (CELL_ERROR_BASE_GCM | 0x04)
#define CELL_GCM_ERROR_ADDRESS_OVERWRAP     (CELL_ERROR_BASE_GCM | 0x05)

#endif /* PS3EMU_ERROR_CODES_H */
