/*
 * ps3recomp - cellVpost HLE
 *
 * Video post-processing: YUV420-planar -> RGBA/YUV conversion with window
 * cut/paste scaling. Rewritten 2026-08-05 against the SDK contract
 * (ORACLE cell/vpost.h): the previous stub exported invented names
 * (Init/End/Query -- 3 of the game's 4 imported NIDs fell to ENOSYS),
 * transposed Exec's ctrlParam/picInfo, never wrote outputs, and carried
 * fabricated struct layouts and error codes.
 *
 * ABI note: the generic bridge passes pointer params as HOST pointers into
 * guest memory; every multi-byte struct field is guest big-endian and is
 * accessed through explicit byte swaps in the implementation. The struct
 * definitions below use plain u32/u64 fields whose natural MSVC layout is
 * byte-identical to the guest layout (all fields naturally aligned;
 * userData sits at an 8-aligned offset in both structs).
 */

#ifndef PS3RECOMP_CELL_VPOST_H
#define PS3RECOMP_CELL_VPOST_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes -- real SDK values (cell/vpost.h, PPM module). */
#define CELL_VPOST_ERROR_Q_ARG_CFG_NULL          (s32)0x80610410
#define CELL_VPOST_ERROR_Q_ARG_CFG_INVALID       (s32)0x80610411
#define CELL_VPOST_ERROR_Q_ARG_ATTR_NULL         (s32)0x80610412

#define CELL_VPOST_ERROR_O_ARG_CFG_NULL          (s32)0x80610440
#define CELL_VPOST_ERROR_O_ARG_CFG_INVALID       (s32)0x80610441
#define CELL_VPOST_ERROR_O_ARG_RSRC_NULL         (s32)0x80610442
#define CELL_VPOST_ERROR_O_ARG_RSRC_INVALID      (s32)0x80610443
#define CELL_VPOST_ERROR_O_ARG_HDL_NULL          (s32)0x80610444
#define CELL_VPOST_ERROR_O_FATAL_QUERY_FAIL      (s32)0x80610460

#define CELL_VPOST_ERROR_C_ARG_HDL_NULL          (s32)0x80610470
#define CELL_VPOST_ERROR_C_ARG_HDL_INVALID       (s32)0x80610471

#define CELL_VPOST_ERROR_E_ARG_HDL_NULL          (s32)0x806104a0
#define CELL_VPOST_ERROR_E_ARG_HDL_INVALID       (s32)0x806104a1
#define CELL_VPOST_ERROR_E_ARG_INPICBUF_NULL     (s32)0x806104a2
#define CELL_VPOST_ERROR_E_ARG_INPICBUF_INVALID  (s32)0x806104a3
#define CELL_VPOST_ERROR_E_ARG_CTRL_NULL         (s32)0x806104a4
#define CELL_VPOST_ERROR_E_ARG_CTRL_INVALID      (s32)0x806104a5
#define CELL_VPOST_ERROR_E_ARG_OUTPICBUF_NULL    (s32)0x806104a6
#define CELL_VPOST_ERROR_E_ARG_OUTPICBUF_INVALID (s32)0x806104a7
#define CELL_VPOST_ERROR_E_ARG_PICINFO_NULL      (s32)0x806104a8

/* Enum values (cell/vpost.h; each enum is a 4-byte guest word) */
#define CELL_VPOST_PIC_DEPTH_8               0

#define CELL_VPOST_PIC_FMT_IN_YUV420_PLANAR  0
#define CELL_VPOST_PIC_FMT_OUT_RGBA_ILV      0
#define CELL_VPOST_PIC_FMT_OUT_YUV420_PLANAR 1

#define CELL_VPOST_SCAN_TYPE_P               0
#define CELL_VPOST_SCAN_TYPE_I               1

#define CELL_VPOST_QUANT_RANGE_FULL          0
#define CELL_VPOST_QUANT_RANGE_BROADCAST     1

#define CELL_VPOST_COLOR_MATRIX_BT601        0
#define CELL_VPOST_COLOR_MATRIX_BT709        1

#define CELL_VPOST_PIC_STRUCT_PFRM           0

#define CELL_VPOST_EXEC_TYPE_PFRM_PFRM       0

/* Handle: opaque token (SDK: typedef void*). Our tokens are cookies. */
typedef u32 CellVpostHandle;

/* Guest-layout structs (fields big-endian in guest memory). */
typedef struct CellVpostCfgParam {      /* 40 bytes */
    u32 inMaxWidth;
    u32 inMaxHeight;
    u32 inDepth;        /* CellVpostPictureDepth */
    u32 inPicFmt;       /* CellVpostPictureFormatIn */
    u32 outMaxWidth;
    u32 outMaxHeight;
    u32 outDepth;
    u32 outPicFmt;      /* CellVpostPictureFormatOut */
    u32 reserved1;
    u32 reserved2;
} CellVpostCfgParam;

typedef struct CellVpostAttr {          /* 16 bytes */
    u32 memSize;
    u8  delay;
    u8  pad[3];
    u32 vpostVerUpper;
    u32 vpostVerLower;
} CellVpostAttr;

typedef struct CellVpostResource {      /* 24 bytes */
    u32 memAddr;        /* guest EA */
    u32 memSize;
    s32 ppuThreadPriority;
    u32 ppuThreadStackSize;
    s32 spuThreadPriority;
    u32 numOfSpus;
} CellVpostResource;

typedef struct CellVpostWindow {        /* 16 bytes */
    u32 x;
    u32 y;
    u32 width;
    u32 height;
} CellVpostWindow;

typedef struct CellVpostCtrlParam {     /* 96 bytes */
    u32 execType;
    u32 scalerType;
    u32 ipcType;
    u32 inWidth;
    u32 inHeight;
    u32 inChromaPosType;
    u32 inQuantRange;
    u32 inColorMatrix;
    CellVpostWindow inWindow;           /* cut */
    u32 outWidth;
    u32 outHeight;
    CellVpostWindow outWindow;          /* paste */
    u8  outAlpha;
    u8  pad[7];
    u64 userData;
    u32 reserved1;
    u32 reserved2;
} CellVpostCtrlParam;

typedef struct CellVpostPictureInfo {   /* 88 bytes */
    u32 inWidth;
    u32 inHeight;
    u32 inDepth;
    u32 inScanType;
    u32 inPicFmt;
    u32 inChromaPosType;
    u32 inPicStruct;
    u32 inQuantRange;
    u32 inColorMatrix;
    u32 outWidth;
    u32 outHeight;
    u32 outDepth;
    u32 outScanType;
    u32 outPicFmt;
    u32 outChromaPosType;
    u32 outPicStruct;
    u32 outQuantRange;
    u32 outColorMatrix;
    u64 userData;
    u32 reserved1;
    u32 reserved2;
} CellVpostPictureInfo;

/* Functions -- real export names (the game's 4 imported NIDs hash exactly
 * these; gen_imports.py resolves them by name). */
s32 cellVpostQueryAttr(const CellVpostCfgParam* cfgParam, CellVpostAttr* attr);
s32 cellVpostOpen(const CellVpostCfgParam* cfgParam,
                  const CellVpostResource* resource, CellVpostHandle* handle);
s32 cellVpostClose(CellVpostHandle handle);
s32 cellVpostExec(CellVpostHandle handle, const void* inPicBuff,
                  const CellVpostCtrlParam* ctrlParam, void* outPicBuff,
                  CellVpostPictureInfo* picInfo);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VPOST_H */
