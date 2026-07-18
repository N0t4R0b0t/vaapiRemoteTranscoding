/*
 * Standalone sanity check: decode the first (intra) picture of a real
 * MPEG-2 elementary stream using this driver's OWN mpeg2_headers.c/
 * mpeg2_vld.c code -- the same software entropy-decode+IDCT path the
 * real driver uses before handing blocks to XvMCRenderSurface -- but
 * skip XvMC/hardware entirely and instead assemble the decoded blocks
 * into a full picture buffer here, in software, and write it as a PPM.
 *
 * This isolates whether the magenta/pink tint seen on real hardware is
 * already present in this driver's own decode output (a bug in
 * mpeg2_vld.c/mpeg2_headers.c) or is introduced later, specifically by
 * the real XvMCRenderSurface/XvMCPutSurface hardware call (which this
 * tool never touches).
 *
 * Only the first picture is reconstructed: it's confirmed intra-only
 * (I-frame), so mpeg2_vld.c's intra blocks already contain final
 * absolute spatial-domain pixel values (idct_block_intra_to_pixels),
 * not residuals -- no motion compensation needed to assemble a full
 * image, unlike P-frames (whose non-intra blocks are residuals the
 * real hardware adds to its own motion-compensated prediction).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mpeg2_headers.h"
#include "mpeg2_vld.h"

static inline uint8_t clamp_u8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "xvmc_recv_pure.m2v";
    const char *out_path = argc > 2 ? argv[2] : "reconstructed_frame0.ppm";

    FILE *fp = fopen(path, "rb");
    if (!fp) { perror("fopen"); return 1; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *filebuf = malloc((size_t)fsize);
    fread(filebuf, 1, (size_t)fsize, fp);
    fclose(fp);

    XvMCMacroBlockArray mb_array;
    memset(&mb_array, 0, sizeof(mb_array));
    mb_array.num_blocks = 4096;
    mb_array.macro_blocks = calloc(4096, sizeof(XvMCMacroBlock));

    XvMCBlockArray blocks;
    memset(&blocks, 0, sizeof(blocks));
    blocks.num_blocks = 4096 * 6;
    blocks.blocks = calloc((size_t)4096 * 6 * 64, sizeof(short));

    struct mpeg2_header_state state;
    memset(&state, 0, sizeof(state));

    uint32_t consumed_total = 0;
    unsigned int mb_count = 0;
    unsigned int block_cursor = 0;
    int got_picture = 0;

    while (consumed_total < (uint32_t)fsize && !got_picture) {
        uint32_t remaining = (uint32_t)fsize - consumed_total;
        struct mpeg2_parsed_slice slices[256];
        unsigned int num_slices = 0;
        int consumed = mpeg2_headers_parse_picture(
            &state, filebuf + consumed_total, remaining, slices, 256, &num_slices);
        if (consumed <= 0) break;

        for (unsigned int s = 0; s < num_slices; s++) {
            struct mpeg2_parsed_slice *sl = &slices[s];
            int decoded = mpeg2_vld_decode_slice(
                sl->data, sl->size, sl->macroblock_bit_offset,
                sl->slice_horizontal_position, sl->slice_vertical_position,
                sl->quantiser_scale_code, sl->intra_slice_flag,
                &state.pic_params, state.has_iq_matrix ? &state.iq_matrix : NULL,
                &mb_array, mb_count, &blocks, &block_cursor,
                /* intra_unsigned=1: this tool wants absolute pixel values
                 * to assemble a viewable image directly, regardless of
                 * what the real hardware's surface type actually expects
                 * (see xvmc_drv_video.c for that real, hardware-specific
                 * convention lookup). */
                1);
            if (decoded < 0) {
                fprintf(stderr, "slice #%u: decode failed\n", s);
                continue;
            }
            mb_count += (unsigned int)decoded;
        }
        printf("picture: type=%d mb_count=%u\n", state.pic_params.picture_coding_type, mb_count);
        got_picture = 1; /* only reconstruct the first picture (confirmed I-frame) */
        consumed_total += (uint32_t)consumed;
    }

    if (state.pic_params.picture_coding_type != 1) {
        fprintf(stderr, "WARNING: first picture is not intra (type=%d) -- "
                "reconstruction will be wrong (residuals only, no MC applied)\n",
                state.pic_params.picture_coding_type);
    }

    /* Real dimensions from the parsed sequence header -- NOT hardcoded,
     * since this tool is fed arbitrary real .m2v files of whatever
     * resolution they actually are (confirmed necessary by real
     * testing: an earlier hardcoded 352x288 caused a real heap
     * overflow/corruption when fed a 640x480 file, writing macroblock
     * data for mb_x/mb_y coordinates the undersized Y/Cb/Cr buffers
     * below never accounted for). Rounded up to a whole number of
     * macroblocks, matching how the real decode path sizes things. */
    int width = (((int)state.pic_params.horizontal_size + 15) / 16) * 16;
    int height = (((int)state.pic_params.vertical_size + 15) / 16) * 16;

    /* Assemble Y/Cb/Cr planes from the decoded macroblocks. */
    uint8_t *Y = calloc((size_t)width * height, 1);
    uint8_t *Cb = calloc((size_t)(width / 2) * (height / 2), 1);
    uint8_t *Cr = calloc((size_t)(width / 2) * (height / 2), 1);

    for (unsigned int m = 0; m < mb_count; m++) {
        XvMCMacroBlock *mb = &mb_array.macro_blocks[m];
        unsigned int mb_x = mb->x, mb_y = mb->y;
        for (int n = 0; n < 4; n++) {
            short *b = blocks.blocks + (size_t)(mb->index + n) * 64;
            int ox = (int)mb_x * 16 + (n % 2) * 8;
            int oy = (int)mb_y * 16 + (n / 2) * 8;
            for (int yy = 0; yy < 8; yy++)
                for (int xx = 0; xx < 8; xx++)
                    Y[(oy + yy) * width + (ox + xx)] = clamp_u8(b[yy * 8 + xx]);
        }
        {
            short *b = blocks.blocks + (size_t)(mb->index + 4) * 64;
            int ox = (int)mb_x * 8, oy = (int)mb_y * 8;
            for (int yy = 0; yy < 8; yy++)
                for (int xx = 0; xx < 8; xx++)
                    Cb[(oy + yy) * (width / 2) + (ox + xx)] = clamp_u8(b[yy * 8 + xx]);
        }
        {
            short *b = blocks.blocks + (size_t)(mb->index + 5) * 64;
            int ox = (int)mb_x * 8, oy = (int)mb_y * 8;
            for (int yy = 0; yy < 8; yy++)
                for (int xx = 0; xx < 8; xx++)
                    Cr[(oy + yy) * (width / 2) + (ox + xx)] = clamp_u8(b[yy * 8 + xx]);
        }
    }

    if (getenv("RECONSTRUCT_DUMP_YUV420P")) {
        FILE *yf = fopen(getenv("RECONSTRUCT_DUMP_YUV420P"), "wb");
        fwrite(Y, 1, (size_t)width * height, yf);
        fwrite(Cb, 1, (size_t)(width / 2) * (height / 2), yf);
        fwrite(Cr, 1, (size_t)(width / 2) * (height / 2), yf);
        fclose(yf);
    }

    FILE *out = fopen(out_path, "wb");
    fprintf(out, "P6\n%d %d\n255\n", width, height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int cy = Y[y * width + x];
            int cb = Cb[(y / 2) * (width / 2) + (x / 2)] - 128;
            int cr = Cr[(y / 2) * (width / 2) + (x / 2)] - 128;
            int r = cy + (91881 * cr >> 16);
            int g = cy - ((22554 * cb + 46802 * cr) >> 16);
            int b = cy + (116130 * cb >> 16);
            unsigned char rgb[3] = { clamp_u8(r), clamp_u8(g), clamp_u8(b) };
            fwrite(rgb, 1, 3, out);
        }
    }
    fclose(out);
    printf("wrote %s (%dx%d), mb_count=%u\n", out_path, width, height, mb_count);
    return 0;
}
