/*
 * ps3recomp - cellVpost HLE implementation
 *
 * Rewritten 2026-08-05 against the SDK contract (see cellVpost.h header
 * comment). Implements the library's actual job: YUV420-planar input,
 * window cut, nearest-neighbor scale (documented first pass; the SDK's
 * scalerType selects fancier filters we approximate), window paste, and
 * RGBA-interleaved or YUV420-planar output, with BT.601/BT.709 matrices
 * and full/broadcast quantization ranges. PictureInfo is an OUTPUT the
 * library fills (the old stub read it).
 *
 * All struct fields are guest big-endian; this file receives host
 * pointers from the bridge and swaps explicitly.
 */

#include "cellVpost.h"
#include "ps3emu/endian.h"
#include <stdio.h>
#include <string.h>

#define VPOST_BE32(x) ps3_bswap32(x)
#define VPOST_BE64(x) ps3_bswap64(x)

/* Handle cookies: high bits pattern + slot, so a stale/garbage token never
 * validates by accident and 0 is never a valid handle. */
#define VPOST_MAX_HANDLES 16
#define VPOST_COOKIE_BASE 0x56503000u   /* 'VP0'<<8 | slot */

typedef struct {
    int in_use;
    CellVpostCfgParam cfg;              /* host-endian copy */
} VpostContext;

static VpostContext s_ctx[VPOST_MAX_HANDLES];

static void vpost_cfg_load(const CellVpostCfgParam* g, CellVpostCfgParam* h)
{
    const u32* src = (const u32*)g;
    u32* dst = (u32*)h;
    for (int i = 0; i < 10; i++) dst[i] = VPOST_BE32(src[i]);
}

static int vpost_cfg_valid(const CellVpostCfgParam* c)
{
    if (c->inMaxWidth == 0 || c->inMaxHeight == 0 ||
        c->outMaxWidth == 0 || c->outMaxHeight == 0)
        return 0;
    if (c->inDepth != CELL_VPOST_PIC_DEPTH_8 ||
        c->outDepth != CELL_VPOST_PIC_DEPTH_8)
        return 0;
    if (c->inPicFmt != CELL_VPOST_PIC_FMT_IN_YUV420_PLANAR)
        return 0;
    if (c->outPicFmt != CELL_VPOST_PIC_FMT_OUT_RGBA_ILV &&
        c->outPicFmt != CELL_VPOST_PIC_FMT_OUT_YUV420_PLANAR)
        return 0;
    return 1;
}

static VpostContext* vpost_from_handle(u32 token)
{
    u32 slot = token - VPOST_COOKIE_BASE;
    if (slot >= VPOST_MAX_HANDLES || !s_ctx[slot].in_use)
        return NULL;
    return &s_ctx[slot];
}

s32 cellVpostQueryAttr(const CellVpostCfgParam* cfgParam, CellVpostAttr* attr)
{
    if (!cfgParam) return CELL_VPOST_ERROR_Q_ARG_CFG_NULL;
    if (!attr)     return CELL_VPOST_ERROR_Q_ARG_ATTR_NULL;
    CellVpostCfgParam cfg;
    vpost_cfg_load(cfgParam, &cfg);
    if (!vpost_cfg_valid(&cfg)) return CELL_VPOST_ERROR_Q_ARG_CFG_INVALID;

    /* The real library reports its SPU work-area requirement; games hand us
     * back a buffer at least this large in Open's resource. We do the work
     * host-side, so any honest nonzero size works -- scale with the config
     * so a later real consumer sees plausible numbers. */
    u32 mem = cfg.inMaxWidth * cfg.inMaxHeight * 2u + 0x40000u;
    attr->memSize = VPOST_BE32(mem);
    attr->delay = 0;
    /* Library version: unverifiable from the header; report zeros. */
    attr->vpostVerUpper = 0;
    attr->vpostVerLower = 0;
    return CELL_OK;
}

s32 cellVpostOpen(const CellVpostCfgParam* cfgParam,
                  const CellVpostResource* resource, CellVpostHandle* handle)
{
    if (!cfgParam)  return CELL_VPOST_ERROR_O_ARG_CFG_NULL;
    if (!resource)  return CELL_VPOST_ERROR_O_ARG_RSRC_NULL;
    if (!handle)    return CELL_VPOST_ERROR_O_ARG_HDL_NULL;
    CellVpostCfgParam cfg;
    vpost_cfg_load(cfgParam, &cfg);
    if (!vpost_cfg_valid(&cfg)) return CELL_VPOST_ERROR_O_ARG_CFG_INVALID;
    if (VPOST_BE32(resource->memAddr) == 0 || VPOST_BE32(resource->memSize) == 0)
        return CELL_VPOST_ERROR_O_ARG_RSRC_INVALID;

    for (u32 i = 0; i < VPOST_MAX_HANDLES; i++) {
        if (!s_ctx[i].in_use) {
            s_ctx[i].in_use = 1;
            s_ctx[i].cfg = cfg;
            *handle = VPOST_BE32(VPOST_COOKIE_BASE + i);
            printf("[cellVpost] Open %ux%u -> %ux%u fmt %u -> handle 0x%08X\n",
                   cfg.inMaxWidth, cfg.inMaxHeight,
                   cfg.outMaxWidth, cfg.outMaxHeight, cfg.outPicFmt,
                   VPOST_COOKIE_BASE + i);
            return CELL_OK;
        }
    }
    /* Table full: the library's own failure namespace has no NOMEM for
     * Open; QUERY_FAIL is its internal-failure code. */
    return CELL_VPOST_ERROR_O_FATAL_QUERY_FAIL;
}

s32 cellVpostClose(CellVpostHandle handle)
{
    if (handle == 0) return CELL_VPOST_ERROR_C_ARG_HDL_NULL;
    VpostContext* c = vpost_from_handle(handle);
    if (!c) return CELL_VPOST_ERROR_C_ARG_HDL_INVALID;
    c->in_use = 0;
    return CELL_OK;
}

/* BT.601/BT.709 integer coefficients (x1024), broadcast-range biases. */
static void yuv_to_rgb(int y, int u, int v, int matrix, int quant_range,
                       u8* r, u8* g, u8* b)
{
    int c = y, d = u - 128, e = v - 128;
    if (quant_range == CELL_VPOST_QUANT_RANGE_BROADCAST)
        c = ((y - 16) * 1192) >> 10;    /* 255/219 */
    int rr, gg, bb;
    if (matrix == CELL_VPOST_COLOR_MATRIX_BT709) {
        rr = c + ((1613 * e) >> 10);
        gg = c - ((192 * d + 479 * e) >> 10);
        bb = c + ((1900 * d) >> 10);
    } else {                             /* BT.601 */
        rr = c + ((1436 * e) >> 10);
        gg = c - ((352 * d + 731 * e) >> 10);
        bb = c + ((1815 * d) >> 10);
    }
    *r = (u8)(rr < 0 ? 0 : rr > 255 ? 255 : rr);
    *g = (u8)(gg < 0 ? 0 : gg > 255 ? 255 : gg);
    *b = (u8)(bb < 0 ? 0 : bb > 255 ? 255 : bb);
}

s32 cellVpostExec(CellVpostHandle handle, const void* inPicBuff,
                  const CellVpostCtrlParam* ctrlParam, void* outPicBuff,
                  CellVpostPictureInfo* picInfo)
{
    if (handle == 0)  return CELL_VPOST_ERROR_E_ARG_HDL_NULL;
    VpostContext* c = vpost_from_handle(handle);
    if (!c)           return CELL_VPOST_ERROR_E_ARG_HDL_INVALID;
    if (!inPicBuff)   return CELL_VPOST_ERROR_E_ARG_INPICBUF_NULL;
    if (!ctrlParam)   return CELL_VPOST_ERROR_E_ARG_CTRL_NULL;
    if (!outPicBuff)  return CELL_VPOST_ERROR_E_ARG_OUTPICBUF_NULL;
    if (!picInfo)     return CELL_VPOST_ERROR_E_ARG_PICINFO_NULL;

    const u32* ctl = (const u32*)ctrlParam;
    const u32 in_w  = VPOST_BE32(ctl[3]);
    const u32 in_h  = VPOST_BE32(ctl[4]);
    const u32 chroma = VPOST_BE32(ctl[5]);
    const u32 quant  = VPOST_BE32(ctl[6]);
    const u32 matrix = VPOST_BE32(ctl[7]);
    const u32 cut_x = VPOST_BE32(ctl[8]),  cut_y = VPOST_BE32(ctl[9]);
    const u32 cut_w = VPOST_BE32(ctl[10]), cut_h = VPOST_BE32(ctl[11]);
    const u32 out_w = VPOST_BE32(ctl[12]);
    const u32 out_h = VPOST_BE32(ctl[13]);
    const u32 pst_x = VPOST_BE32(ctl[14]), pst_y = VPOST_BE32(ctl[15]);
    const u32 pst_w = VPOST_BE32(ctl[16]), pst_h = VPOST_BE32(ctl[17]);
    const u8  out_alpha = ((const u8*)ctrlParam)[72];

    if (in_w == 0 || in_h == 0 || in_w > c->cfg.inMaxWidth ||
        in_h > c->cfg.inMaxHeight)
        return CELL_VPOST_ERROR_E_ARG_CTRL_INVALID;
    if (out_w == 0 || out_h == 0 || out_w > c->cfg.outMaxWidth ||
        out_h > c->cfg.outMaxHeight)
        return CELL_VPOST_ERROR_E_ARG_CTRL_INVALID;
    if (cut_x + cut_w > in_w || cut_y + cut_h > in_h ||
        cut_w == 0 || cut_h == 0)
        return CELL_VPOST_ERROR_E_ARG_CTRL_INVALID;
    if (pst_x + pst_w > out_w || pst_y + pst_h > out_h ||
        pst_w == 0 || pst_h == 0)
        return CELL_VPOST_ERROR_E_ARG_CTRL_INVALID;

    const u8* ysrc = (const u8*)inPicBuff;
    const u8* usrc = ysrc + (size_t)in_w * in_h;
    const u8* vsrc = usrc + (size_t)(in_w / 2) * (in_h / 2);

    if (c->cfg.outPicFmt == CELL_VPOST_PIC_FMT_OUT_RGBA_ILV) {
        u8* out = (u8*)outPicBuff;
        /* Outside the paste window the real library leaves black; write
         * the full surface so a partial paste is deterministic. */
        for (u32 py = 0; py < out_h; py++) {
            u8* row = out + (size_t)py * out_w * 4;
            if (py < pst_y || py >= pst_y + pst_h) {
                for (u32 px = 0; px < out_w; px++) {
                    row[px * 4 + 0] = 0; row[px * 4 + 1] = 0;
                    row[px * 4 + 2] = 0; row[px * 4 + 3] = out_alpha;
                }
                continue;
            }
            const u32 sy = cut_y + (u32)(((u64)(py - pst_y) * cut_h) / pst_h);
            for (u32 px = 0; px < out_w; px++) {
                u8* p = row + px * 4;
                if (px < pst_x || px >= pst_x + pst_w) {
                    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = out_alpha;
                    continue;
                }
                const u32 sx = cut_x +
                    (u32)(((u64)(px - pst_x) * cut_w) / pst_w);
                const int y = ysrc[(size_t)sy * in_w + sx];
                const int u = usrc[(size_t)(sy / 2) * (in_w / 2) + sx / 2];
                const int v = vsrc[(size_t)(sy / 2) * (in_w / 2) + sx / 2];
                yuv_to_rgb(y, u, v, (int)matrix, (int)quant,
                           &p[0], &p[1], &p[2]);
                p[3] = out_alpha;
            }
        }
    } else { /* CELL_VPOST_PIC_FMT_OUT_YUV420_PLANAR */
        u8* yd = (u8*)outPicBuff;
        u8* ud = yd + (size_t)out_w * out_h;
        u8* vd = ud + (size_t)(out_w / 2) * (out_h / 2);
        memset(yd, 16, (size_t)out_w * out_h);
        memset(ud, 128, (size_t)(out_w / 2) * (out_h / 2) * 2);
        for (u32 py = pst_y; py < pst_y + pst_h; py++) {
            const u32 sy = cut_y + (u32)(((u64)(py - pst_y) * cut_h) / pst_h);
            for (u32 px = pst_x; px < pst_x + pst_w; px++) {
                const u32 sx = cut_x +
                    (u32)(((u64)(px - pst_x) * cut_w) / pst_w);
                yd[(size_t)py * out_w + px] = ysrc[(size_t)sy * in_w + sx];
                if (!(py & 1) && !(px & 1)) {
                    ud[(size_t)(py / 2) * (out_w / 2) + px / 2] =
                        usrc[(size_t)(sy / 2) * (in_w / 2) + sx / 2];
                    vd[(size_t)(py / 2) * (out_w / 2) + px / 2] =
                        vsrc[(size_t)(sy / 2) * (in_w / 2) + sx / 2];
                }
            }
        }
    }

    /* PictureInfo is an OUTPUT describing what was performed. */
    u32* pi = (u32*)picInfo;
    pi[0]  = VPOST_BE32(in_w);
    pi[1]  = VPOST_BE32(in_h);
    pi[2]  = VPOST_BE32(CELL_VPOST_PIC_DEPTH_8);
    pi[3]  = VPOST_BE32(CELL_VPOST_SCAN_TYPE_P);
    pi[4]  = VPOST_BE32(CELL_VPOST_PIC_FMT_IN_YUV420_PLANAR);
    pi[5]  = VPOST_BE32(chroma);
    pi[6]  = VPOST_BE32(CELL_VPOST_PIC_STRUCT_PFRM);
    pi[7]  = VPOST_BE32(quant);
    pi[8]  = VPOST_BE32(matrix);
    pi[9]  = VPOST_BE32(out_w);
    pi[10] = VPOST_BE32(out_h);
    pi[11] = VPOST_BE32(CELL_VPOST_PIC_DEPTH_8);
    pi[12] = VPOST_BE32(CELL_VPOST_SCAN_TYPE_P);
    pi[13] = VPOST_BE32(c->cfg.outPicFmt);
    pi[14] = VPOST_BE32(chroma);
    pi[15] = VPOST_BE32(CELL_VPOST_PIC_STRUCT_PFRM);
    pi[16] = VPOST_BE32(quant);
    pi[17] = VPOST_BE32(matrix);
    /* userData: passed through from ctrl (u64 at byte 80, 8-aligned) */
    memcpy((u8*)picInfo + 72, (const u8*)ctrlParam + 80, 8);
    pi[20] = 0;
    pi[21] = 0;
    return CELL_OK;
}
