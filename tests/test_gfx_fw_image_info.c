/* SPDX-License-Identifier: MIT */
/* Pure synthetic parser checks: no firmware or physical memory access. */
#include <assert.h>
#include <string.h>
#include "../src/gfx_fw_image_info.h"

int main(void)
{
    u32 hdr[GFX_FW_HDR_SIZE / 4] = {
        [0] = GFX_FW_HDR_BRANCH,
        [1] = GFX_FW_HDR_MAGIC,
        [2] = GFX_FW_HDR_VER,
        [11] = 0x1000,
        [12] = 0x200,
        [13] = 0x64000,
    };
    for (int i = 0; i < 4; i++)
        hdr[5 + i] = 0x03020100u + 0x04040404u * i;
    struct gfx_fw_image_info out = {0};
    assert(gfx_fw_parse_image_info(hdr, 0x64000, &out));
    for (int i = 0; i < 16; i++)
        assert(out.uuid[i] == i);
    assert(out.patchbay_offset == 0x1000 && out.patchbay_size == 0x200 &&
           out.text_size == 0x64000);

    struct gfx_fw_image_info saved = out;
    assert(!gfx_fw_parse_image_info(NULL, 0x64000, &out));
    assert(!gfx_fw_parse_image_info(hdr, 0x64000, NULL));
    assert(!gfx_fw_parse_image_info(hdr, 0x63000, &out));
    assert(!memcmp(&saved, &out, sizeof(out)));

    hdr[0] = 0;
    assert(!gfx_fw_parse_image_info(hdr, 0x64000, &out));
    hdr[0] = GFX_FW_HDR_BRANCH;
    hdr[1] = 0;
    assert(!gfx_fw_parse_image_info(hdr, 0x64000, &out));
    hdr[1] = GFX_FW_HDR_MAGIC;
    hdr[2] = 0;
    assert(!gfx_fw_parse_image_info(hdr, 0x64000, &out));
    hdr[2] = GFX_FW_HDR_VER;
    hdr[11] = 0x64001;
    assert(!gfx_fw_parse_image_info(hdr, 0x64000, &out));
    hdr[11] = 0x1000;
    hdr[12] = 0x63001;
    assert(!gfx_fw_parse_image_info(hdr, 0x64000, &out));
    hdr[12] = 0x200;
    hdr[13] = 0;
    assert(!gfx_fw_parse_image_info(hdr, 0x64000, &out));
    assert(!memcmp(&saved, &out, sizeof(out)));
    return 0;
}
