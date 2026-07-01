#!/usr/bin/env python3
"""NV40/RSX (NV4097-class) method-offset name table.

Facts (offsets/names) derived from the MIT-licensed envytools register database
(rnndb/graph/nv30-40_3d.xml) plus the FIFO-control / gcm driver-queue methods
this port already established empirically (SET_REFERENCE 0x50, NV406E semaphores,
the 0xE9xx driver-queue flip methods observed in the BLUS30826 stream).
Method numbers are interface constants, not creative expression.

resolve(method) -> (name, index) where index is the array element for banked
methods (texture units, vertex attributes, ...), or (None, 0) if unknown.
"""

# scalar methods: offset -> name
SCALAR = {
    0x0000: "NOP_PAD",               # zero word: padding/no-op in the stream
    0x0050: "NV406E_SET_REFERENCE",
    0x0060: "NV406E_SET_CONTEXT_DMA_SEMAPHORE",
    0x0064: "NV406E_SEMAPHORE_OFFSET",
    0x0068: "NV406E_SEMAPHORE_ACQUIRE",
    0x006C: "NV406E_SEMAPHORE_RELEASE",
    0x0100: "NV4097_NOP",
    0x0104: "NV4097_NOTIFY",
    0x0110: "NV4097_WAIT_FOR_IDLE",
    0x0120: "FLIP_SET_READ",
    0x0124: "FLIP_SET_WRITE",
    0x0128: "FLIP_MAX",
    0x012C: "FLIP_INCR_WRITE",
    0x0130: "FLIP_WAIT",
    0x0180: "DMA_NOTIFY",
    0x0184: "DMA_TEXTURE0",
    0x0188: "DMA_TEXTURE1",
    0x018C: "DMA_COLOR1",
    0x0194: "DMA_COLOR0",
    0x0198: "DMA_ZETA",
    0x019C: "DMA_VTXBUF0",
    0x01A0: "DMA_VTXBUF1",
    0x01A4: "DMA_FENCE",
    0x01A8: "DMA_QUERY",
    0x01B4: "DMA_COLOR2",
    0x01B8: "DMA_COLOR3",
    0x0200: "RT_HORIZ",
    0x0204: "RT_VERT",
    0x0208: "RT_FORMAT",
    0x020C: "COLOR0_PITCH",
    0x0210: "COLOR0_OFFSET",
    0x0214: "ZETA_OFFSET",
    0x0218: "COLOR1_OFFSET",
    0x021C: "COLOR1_PITCH",
    0x0220: "RT_ENABLE",
    0x022C: "ZETA_PITCH",
    0x0280: "COLOR2_PITCH",
    0x0284: "COLOR3_PITCH",
    0x0288: "COLOR2_OFFSET",
    0x028C: "COLOR3_OFFSET",
    0x02B8: "VIEWPORT_TX_ORIGIN",
    0x02BC: "VIEWPORT_CLIP_MODE",
    0x02C0: "VIEWPORT_CLIP_HORIZ",
    0x02C4: "VIEWPORT_CLIP_VERT",
    0x0300: "DITHER_ENABLE",
    0x0304: "ALPHA_FUNC_ENABLE",
    0x0308: "ALPHA_FUNC_FUNC",
    0x030C: "ALPHA_FUNC_REF",
    0x0310: "BLEND_FUNC_ENABLE",
    0x0314: "BLEND_FUNC_SRC",
    0x0318: "BLEND_FUNC_DST",
    0x031C: "BLEND_COLOR",
    0x0320: "BLEND_EQUATION",
    0x0324: "COLOR_MASK",
    0x0368: "SHADE_MODEL",
    0x0370: "MRT_COLOR_MASK",
    0x0374: "COLOR_LOGIC_OP_ENABLE",
    0x0378: "COLOR_LOGIC_OP_OP",
    0x0380: "DEPTH_BOUNDS_TEST_ENABLE",
    0x0384: "DEPTH_BOUNDS_TEST_ZMIN",
    0x0388: "DEPTH_BOUNDS_TEST_ZMAX",
    0x0394: "DEPTH_RANGE_NEAR",
    0x0398: "DEPTH_RANGE_FAR",
    0x03B0: "MIPMAP_ROUNDING",
    0x03B8: "LINE_WIDTH",
    0x03BC: "LINE_SMOOTH_ENABLE",
    0x08C0: "SCISSOR_HORIZ",
    0x08C4: "SCISSOR_VERT",
    0x08CC: "FOG_MODE",
    0x08D0: "FOG_EQUATION_CONSTANT",
    0x08D4: "FOG_EQUATION_LINEAR",
    0x08D8: "FOG_EQUATION_QUADRATIC",
    0x08E4: "FP_ACTIVE_PROGRAM",
    0x0A00: "VIEWPORT_HORIZ",
    0x0A04: "VIEWPORT_VERT",
    0x0A6C: "DEPTH_FUNC",
    0x0A70: "DEPTH_WRITE_ENABLE",
    0x0A74: "DEPTH_TEST_ENABLE",
    0x0A78: "POLYGON_OFFSET_FACTOR",
    0x0A7C: "POLYGON_OFFSET_UNITS",
    0x1450: "FP_REG_CONTROL",
    0x1454: "FLATSHADE_FIRST",
    0x145C: "EDGEFLAG",
    0x1478: "VP_CLIP_PLANES_ENABLE",
    0x147C: "POLYGON_STIPPLE_ENABLE",
    0x1714: "VTX_CACHE_INVALIDATE",
    0x173C: "VB_ELEMENT_BASE",
    0x17C8: "QUERY_RESET",
    0x17CC: "QUERY_ENABLE",
    0x1800: "QUERY_GET",
    0x1808: "VERTEX_BEGIN_END",
    0x180C: "VB_ELEMENT_U16",
    0x1810: "VB_ELEMENT_U32",
    0x1814: "VB_VERTEX_BATCH",
    0x1818: "VERTEX_DATA_INLINE",
    0x181C: "IDXBUF_OFFSET",
    0x1820: "IDXBUF_FORMAT",
    0x1824: "VB_INDEX_BATCH",
    0x1828: "POLYGON_MODE_FRONT",
    0x182C: "POLYGON_MODE_BACK",
    0x1830: "CULL_FACE",
    0x1834: "FRONT_FACE",
    0x1838: "POLYGON_SMOOTH_ENABLE",
    0x183C: "CULL_FACE_ENABLE",
    0x1D60: "FP_CONTROL",
    0x1D6C: "FENCE_OFFSET",
    0x1D70: "FENCE_VALUE",
    0x1D78: "DEPTH_CONTROL",
    0x1D7C: "MULTISAMPLE_CONTROL",
    0x1D88: "COORD_CONVENTIONS",
    0x1D8C: "CLEAR_DEPTH_VALUE",
    0x1D90: "CLEAR_COLOR_VALUE",
    0x1D94: "CLEAR_BUFFERS",
    0x1DAC: "PRIMITIVE_RESTART_ENABLE",
    0x1DB0: "PRIMITIVE_RESTART_INDEX",
    0x1DB4: "LINE_STIPPLE_ENABLE",
    0x1DB8: "LINE_STIPPLE_PATTERN",
    0x1E94: "ENGINE",
    0x1E9C: "VP_UPLOAD_FROM_ID",
    0x1EA0: "VP_START_FROM_ID",
    0x1EE0: "POINT_SIZE",
    0x1EE8: "POINT_SPRITE",
    0x1EFC: "VP_UPLOAD_CONST_ID",
    0x1FD8: "TEX_CACHE_CTL",
    0x1FF0: "VP_ATTRIB_EN",
    0x1FF4: "VP_RESULT_EN",
    # gcm driver-queue / flip methods (established empirically from the
    # BLUS30826 stream + our consumer; not in the NV40 3D class proper)
    0xE940: "GCM_DRIVER_QUEUE+0",
    0xE944: "GCM_DRIVER_FLIP",       # the 0x0004E944 flip command
    0xE948: "GCM_DRIVER_QUEUE+8",
    0xE94C: "GCM_DRIVER_QUEUE+C",
    0xE950: "GCM_DRIVER_QUEUE+10",
    0xE954: "GCM_DRIVER_QUEUE+14",
    0xE958: "GCM_DRIVER_QUEUE+18",
    0xE95C: "GCM_DRIVER_QUEUE+1C",
}

# banked/array methods: (base, count, stride, name)
ARRAYS = [
    (0x0900, 8, 0x10, "RC_STAGE"),          # register combiner stages
    (0x0A20, 4, 0x04, "VIEWPORT_TRANSLATE"),  # x,y,z,w
    (0x0A30, 4, 0x04, "VIEWPORT_SCALE"),      # x,y,z,w
    (0x0B00, 4, 0x04, "TEX_FILTER_OPT"),
    (0x0B80, 4, 0x04, "VP_UPLOAD_INST"),
    (0x1480, 32, 0x04, "POLYGON_STIPPLE_PATTERN"),
    (0x1500, 16, 0x10, "VTX_ATTR_3F"),
    (0x1680, 16, 0x04, "VTXBUF_OFFSET"),
    (0x1740, 16, 0x04, "VTXFMT"),
    (0x1840, 8, 0x04, "TEX_SIZE1"),
    (0x1880, 16, 0x08, "VTX_ATTR_2F"),
    (0x1900, 16, 0x04, "VTX_ATTR_2I"),
    (0x1940, 16, 0x04, "VTX_ATTR_4UB"),
    (0x1980, 16, 0x08, "VTX_ATTR_4I"),
    (0x1A00, 8, 0x20, "TEX_UNIT"),          # OFFSET/FORMAT/WRAP/ENABLE/SWIZZLE/FILTER/NPOT/BORDER
    (0x1C00, 16, 0x10, "VTX_ATTR_4F"),
    (0x1E40, 16, 0x04, "VTX_ATTR_1F"),
    (0x1F00, 4, 0x04, "VP_UPLOAD_CONST"),
]

TEX_UNIT_FIELDS = ["TEX_OFFSET", "TEX_FORMAT", "TEX_WRAP", "TEX_ENABLE",
                   "TEX_SWIZZLE", "TEX_FILTER", "TEX_NPOT_SIZE", "TEX_BORDER_COLOR"]


def resolve(method):
    """method (cmd & 0x3FFFC) -> (name, bank_index) or (None, 0)."""
    if method in SCALAR:
        return SCALAR[method], 0
    for base, count, stride, name in ARRAYS:
        if base <= method < base + count * stride:
            off = method - base
            idx, rem = divmod(off, stride)
            if name == "TEX_UNIT":
                return "%s[%d]" % (TEX_UNIT_FIELDS[rem // 4], idx), idx
            if rem == 0 or stride == 4:
                return "%s[%d]" % (name, idx), idx
            return "%s[%d]+0x%X" % (name, idx, rem), idx
    return None, 0
