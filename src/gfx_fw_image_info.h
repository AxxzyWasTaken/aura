/* SPDX-License-Identifier: MIT */
#ifndef __GFX_FW_IMAGE_INFO_H__
#define __GFX_FW_IMAGE_INFO_H__

#include "firmware.h"
#include "string.h"

#define GFX_FW_HDR_OFFSET 0x4200
#define GFX_FW_HDR_BRANCH 0x14000011
#define GFX_FW_HDR_MAGIC  0x64697575
#define GFX_FW_HDR_VER    5
#define GFX_FW_HDR_SIZE   0x38

/* Parse an already-read image-info header; it is an identity, not ABI approval. */
static inline bool gfx_fw_parse_image_info(const u32 hdr[GFX_FW_HDR_SIZE / 4], u32 segment_size,
                                           struct gfx_fw_image_info *out)
{
    if (!hdr || !out || hdr[0] != GFX_FW_HDR_BRANCH || hdr[1] != GFX_FW_HDR_MAGIC ||
        hdr[2] != GFX_FW_HDR_VER || !hdr[13] || hdr[13] > segment_size ||
        hdr[11] > hdr[13] || hdr[12] > hdr[13] - hdr[11])
        return false;

    struct gfx_fw_image_info found = {
        .patchbay_offset = hdr[11],
        .patchbay_size = hdr[12],
        .text_size = hdr[13],
    };
    memcpy(found.uuid, &hdr[5], sizeof(found.uuid));
    *out = found;
    return true;
}

#endif
