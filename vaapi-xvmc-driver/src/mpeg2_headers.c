/*
 * MPEG-2 (ISO/IEC 13818-2) sequence/picture/slice HEADER parsing -- not to
 * be confused with mpeg2_vld.c's entropy (macroblock/DCT) decode, which
 * this file feeds. Ported from FFmpeg's libavcodec (mpeg12dec.c:
 * mpeg1_decode_sequence, mpeg_decode_sequence_extension,
 * mpeg_decode_quant_matrix_extension, mpeg1_decode_picture,
 * mpeg_decode_picture_coding_extension, and the vaapi_mpeg2.c hwaccel
 * glue's own slice-header field extraction, which is the authoritative
 * reference for exactly how macroblock_offset/slice_horizontal_position/
 * intra_slice_flag are meant to be derived for VA-API specifically) --
 * LGPL v2.1+, same as mpeg2_vld.c's own entropy-decode provenance. Ported
 * rather than hand-derived from the spec, per this project's standing
 * rule: a transcription error here produces a wrong VAPictureParameterBufferMPEG2/
 * VAIQMatrixBufferMPEG2/slice field silently, not a compile error.
 *
 * This module intentionally does NOT track or apply IDCT permutation
 * (ff_idsp.idct_permutation in the FFmpeg reference) -- that's an
 * FFmpeg-internal memory-layout optimization for its own software IDCT,
 * irrelevant to VAIQMatrixBufferMPEG2's public contract, which stores
 * matrices in plain zig-zag scan order (the order the bitstream already
 * transmits them in). Matrix "load" flags are left 0 (meaning "not
 * loaded") whenever the bitstream doesn't explicitly signal a custom
 * matrix -- mpeg2_vld.c's own setup_matrices() already falls back
 * correctly to its own (separately verified) default matrices in that
 * case, so this file doesn't need to know default matrix values at all.
 */

#include "mpeg2_headers.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* Bit reader: plain MSB-first, matching mpeg2_vld.c's own (that one is    */
/* static to that file, so this is a small independent copy).             */
/* ---------------------------------------------------------------------- */

struct bitreader {
    const uint8_t *data;
    uint32_t size_bits;
    uint32_t pos;
};

static void br_init(struct bitreader *br, const uint8_t *data, uint32_t size_bytes)
{
    br->data = data;
    br->size_bits = size_bytes * 8;
    br->pos = 0;
}

static inline int br_bits_left(const struct bitreader *br)
{
    return (int)br->size_bits - (int)br->pos;
}

static inline uint32_t br_get(struct bitreader *br, int n)
{
    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
        uint32_t bit = 0;
        if (br->pos < br->size_bits) {
            uint8_t byte = br->data[br->pos >> 3];
            bit = (byte >> (7 - (br->pos & 7))) & 1;
        }
        val = (val << 1) | bit;
        br->pos++;
    }
    return val;
}

static inline int br_get1(struct bitreader *br)
{
    return (int)br_get(br, 1);
}

static inline void br_skip(struct bitreader *br, int n)
{
    br->pos += (uint32_t)n;
}

/* ---------------------------------------------------------------------- */
/* Start-code scanning.                                                    */
/* ---------------------------------------------------------------------- */

#define SEQ_START_CODE      0xB3
#define GOP_START_CODE      0xB8
#define PICTURE_START_CODE  0x00
#define EXT_START_CODE      0xB5
#define SLICE_MIN_START     0x01
#define SLICE_MAX_START     0xAF

/* Finds the next 00 00 01 xx start code at or after `from`, returns its
 * byte offset (pointing at the first 0x00), or -1 if none found before
 * `size`. `*code_byte` receives the byte right after the 00 00 01 prefix
 * (the actual start code / extension identifier). */
static int find_start_code(const uint8_t *buf, uint32_t size, uint32_t from, uint8_t *code_byte)
{
    if (size < 4)
        return -1;
    for (uint32_t i = from; i + 3 < size; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            *code_byte = buf[i + 3];
            return (int)i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------------- */
/* Header parsers. Each takes a bitreader already positioned right after   */
/* the 00 00 01 xx start code it belongs to.                              */
/* ---------------------------------------------------------------------- */

static void parse_sequence_header(struct mpeg2_header_state *st, struct bitreader *br)
{
    /* ISO/IEC 13818-2 6.2.2.1 / FFmpeg mpeg1_decode_sequence. */
    st->horizontal_size = (uint16_t)br_get(br, 12);
    st->vertical_size = (uint16_t)br_get(br, 12);
    br_skip(br, 4);  /* aspect_ratio_information */
    br_skip(br, 4);  /* frame_rate_code */
    br_skip(br, 18); /* bit_rate_value */
    br_skip(br, 1);  /* marker_bit */
    br_skip(br, 10); /* vbv_buffer_size_value */
    br_skip(br, 1);  /* constrained_parameters_flag */

    if (br_get1(br)) { /* load_intra_quantiser_matrix */
        st->has_iq_matrix = 1;
        st->iq_matrix.load_intra_quantiser_matrix = 1;
        for (int i = 0; i < 64; i++)
            st->iq_matrix.intra_quantiser_matrix[i] = (uint8_t)br_get(br, 8);
    }
    if (br_get1(br)) { /* load_non_intra_quantiser_matrix */
        st->has_iq_matrix = 1;
        st->iq_matrix.load_non_intra_quantiser_matrix = 1;
        for (int i = 0; i < 64; i++)
            st->iq_matrix.non_intra_quantiser_matrix[i] = (uint8_t)br_get(br, 8);
    }

    st->has_sequence = 1;
    st->pic_params.horizontal_size = st->horizontal_size;
    st->pic_params.vertical_size = st->vertical_size;
}

static void parse_sequence_extension(struct mpeg2_header_state *st, struct bitreader *br)
{
    /* FFmpeg mpeg_decode_sequence_extension. Only horizontal/vertical size
     * extension bits matter for us -- profile/level/bitrate/frame-rate
     * extension fields are parsed-and-discarded (still must consume the
     * right bit count to stay aligned for whatever follows). */
    br_skip(br, 1); /* profile/level escape bit */
    br_skip(br, 3); /* profile */
    br_skip(br, 4); /* level */
    br_skip(br, 1); /* progressive_sequence (not tracked; frame_pred_frame_dct
                      * in picture_coding_extension is what this driver uses) */
    br_skip(br, 2); /* chroma_format (4:2:0 assumed elsewhere in this driver) */
    int horiz_ext = (int)br_get(br, 2);
    int vert_ext = (int)br_get(br, 2);
    st->horizontal_size = (uint16_t)(st->horizontal_size | (horiz_ext << 12));
    st->vertical_size = (uint16_t)(st->vertical_size | (vert_ext << 12));
    st->pic_params.horizontal_size = st->horizontal_size;
    st->pic_params.vertical_size = st->vertical_size;
    br_skip(br, 12); /* bit_rate_extension */
    br_skip(br, 1);  /* marker_bit */
    br_skip(br, 8);  /* vbv_buffer_size_extension */
    br_skip(br, 1);  /* low_delay */
    br_skip(br, 2);  /* frame_rate_extension_n */
    br_skip(br, 5);  /* frame_rate_extension_d */
}

static void parse_quant_matrix_extension(struct mpeg2_header_state *st, struct bitreader *br)
{
    /* FFmpeg mpeg_decode_quant_matrix_extension / load_matrix. Order is
     * intra luma, non-intra luma, intra chroma, non-intra chroma -- the
     * VAIQMatrixBufferMPEG2 chroma fields are independently loadable only
     * here (sequence header only has luma). */
    st->has_iq_matrix = 1;
    if (br_get1(br)) {
        st->iq_matrix.load_intra_quantiser_matrix = 1;
        for (int i = 0; i < 64; i++)
            st->iq_matrix.intra_quantiser_matrix[i] = (uint8_t)br_get(br, 8);
    }
    if (br_get1(br)) {
        st->iq_matrix.load_non_intra_quantiser_matrix = 1;
        for (int i = 0; i < 64; i++)
            st->iq_matrix.non_intra_quantiser_matrix[i] = (uint8_t)br_get(br, 8);
    }
    if (br_get1(br)) {
        st->iq_matrix.load_chroma_intra_quantiser_matrix = 1;
        for (int i = 0; i < 64; i++)
            st->iq_matrix.chroma_intra_quantiser_matrix[i] = (uint8_t)br_get(br, 8);
    }
    if (br_get1(br)) {
        st->iq_matrix.load_chroma_non_intra_quantiser_matrix = 1;
        for (int i = 0; i < 64; i++)
            st->iq_matrix.chroma_non_intra_quantiser_matrix[i] = (uint8_t)br_get(br, 8);
    }
}

static void parse_picture_header(struct mpeg2_header_state *st, struct bitreader *br)
{
    /* FFmpeg mpeg1_decode_picture. picture_coding_type: 1=I, 2=P, 3=B --
     * matches VAPictureParameterBufferMPEG2.picture_coding_type directly. */
    br_skip(br, 10); /* temporal_reference */
    int32_t picture_coding_type = (int32_t)br_get(br, 3);
    st->pic_params.picture_coding_type = picture_coding_type;
    br_skip(br, 16); /* vbv_delay */

    /* Old-style (MPEG-1-shaped) forward/backward f_code fields. Real
     * f_code for MPEG-2 comes from picture_coding_extension, which always
     * immediately follows for a real MPEG-2 stream -- these bits still
     * have to be consumed to stay aligned, but their values are
     * discarded. */
    if (picture_coding_type == 2 /* P */ || picture_coding_type == 3 /* B */) {
        br_skip(br, 1); /* full_pel_forward_vector */
        br_skip(br, 3); /* forward_f_code */
    }
    if (picture_coding_type == 3 /* B */) {
        br_skip(br, 1); /* full_pel_backward_vector */
        br_skip(br, 3); /* backward_f_code */
    }

    /* New picture: any picture-scoped struct fields not (re-)supplied by
     * picture_coding_extension should not leak from a previous picture. */
    st->pic_params.forward_reference_picture = VA_INVALID_SURFACE;
    st->pic_params.backward_reference_picture = VA_INVALID_SURFACE;
}

static void parse_picture_coding_extension(struct mpeg2_header_state *st, struct bitreader *br)
{
    /* FFmpeg mpeg_decode_picture_coding_extension. f_code nibbles of 0 are
     * invalid per spec and mpeg2_vld.c already treats a 0 nibble as "use
     * 1" (see decode_motion's caller) -- ported as-is, no need to
     * pre-correct here. */
    int f00 = (int)br_get(br, 4);
    int f01 = (int)br_get(br, 4);
    int f10 = (int)br_get(br, 4);
    int f11 = (int)br_get(br, 4);
    st->pic_params.f_code = (int32_t)((f00 << 12) | (f01 << 8) | (f10 << 4) | f11);

    st->pic_params.picture_coding_extension.bits.intra_dc_precision = (uint32_t)br_get(br, 2);
    st->pic_params.picture_coding_extension.bits.picture_structure = (uint32_t)br_get(br, 2);
    st->pic_params.picture_coding_extension.bits.top_field_first = (uint32_t)br_get1(br);
    st->pic_params.picture_coding_extension.bits.frame_pred_frame_dct = (uint32_t)br_get1(br);
    st->pic_params.picture_coding_extension.bits.concealment_motion_vectors = (uint32_t)br_get1(br);
    st->pic_params.picture_coding_extension.bits.q_scale_type = (uint32_t)br_get1(br);
    st->pic_params.picture_coding_extension.bits.intra_vlc_format = (uint32_t)br_get1(br);
    st->pic_params.picture_coding_extension.bits.alternate_scan = (uint32_t)br_get1(br);
    st->pic_params.picture_coding_extension.bits.repeat_first_field = (uint32_t)br_get1(br);
    br_skip(br, 1); /* chroma_420_type -- not in VA-API's picture_coding_extension bits */
    st->pic_params.picture_coding_extension.bits.progressive_frame = (uint32_t)br_get1(br);
    /* composite_display_flag and beyond: not needed by mpeg2_vld_decode_slice,
     * not parsed. */

    st->has_pic_params = 1;
}

/* Parses one slice's header (quantiser_scale_code, intra_slice_flag, the
 * optional intra_slice/extra_information_slice bits, and the resulting
 * macroblock_bit_offset) -- ported from FFmpeg's vaapi_mpeg2.c
 * vaapi_mpeg2_decode_slice, the authoritative reference for exactly how
 * these fields are meant to be derived for VA-API's own
 * VASliceParameterBufferMPEG2 (its shape is VA-API-specific, not something
 * FFmpeg's own software decoder tracks the same way). `br` must already be
 * positioned right after the slice's 00 00 01 xx start code. */
static void parse_slice_header(struct bitreader *br, struct mpeg2_parsed_slice *slice)
{
    slice->quantiser_scale_code = (int32_t)br_get(br, 5);
    slice->intra_slice_flag = br_get1(br);
    if (slice->intra_slice_flag) {
        br_skip(br, 8); /* intra_slice (1 bit) + reserved_bits (7 bits) */
        /* extra_bit_slice / extra_information_slice: a run of (1-bit
         * flag=1, 8 data bits) pairs, terminated by a single flag=0 bit. */
        while (br_get1(br))
            br_skip(br, 8);
    } else {
        /* The single extra_bit_slice terminator bit is only present when
         * intra_slice_flag itself was 1 in the general syntax -- but
         * intra_slice_flag IS that same first bit read via nextbits()==1
         * in the spec's slice() syntax; ffmpeg's vaapi_mpeg2.c reads it
         * unconditionally as a plain bit (matching real encoder output,
         * where this bit is always present) and only conditionally skips
         * the extra_information_slice loop -- ported exactly as-is above,
         * this else-branch needs no additional bits.
         */
    }
    /* +32: br is positioned relative to right after the 4-byte start
     * code, but macroblock_bit_offset (like slice->data) is measured from
     * the start of the whole slice buffer, start code included -- matches
     * this project's existing convention (see mpeg2_vld_decode_slice's
     * callers) and the real captured ground truth this was verified
     * against (a known slice's offset of 38 = 32-bit start code + 5-bit
     * quantiser_scale_code + 1-bit intra_slice_flag=0). */
    slice->macroblock_bit_offset = 32 + br->pos;
    /* slice_horizontal_position is always 0 at this point in real streams:
     * it reflects mb_x's value *before* the initial macroblock_address_increment
     * is parsed (which mpeg2_vld_decode_slice already does itself, adding
     * it on top -- see that function's slice_horizontal_position doc
     * comment), and mb_x always starts a slice at 0 in this convention
     * (matches FFmpeg's own vaapi_mpeg2.c, which takes this field directly
     * from its decoder's mb_x state at the same point, always 0 there). */
    slice->slice_horizontal_position = 0;
}

/* ---------------------------------------------------------------------- */
/* Top-level entry point.                                                  */
/* ---------------------------------------------------------------------- */

int mpeg2_headers_parse_picture(
    struct mpeg2_header_state *state,
    const uint8_t *buf, uint32_t size,
    struct mpeg2_parsed_slice *slices, unsigned int max_slices,
    unsigned int *num_slices)
{
    *num_slices = 0;

    int have_picture_start = 0;
    uint32_t consumed = 0;
    uint32_t pos = 0;
    uint8_t code;

    for (;;) {
        int start = find_start_code(buf, size, pos, &code);
        if (start < 0) {
            /* Ran off the end without seeing the start of a following
             * picture -- if we've already seen this picture's own
             * PICTURE_START_CODE and at least one slice, we still can't
             * be sure it's complete (more slices for THIS picture could
             * be in the next chunk of relay bytes), so ask for more. */
            return 0;
        }

        if (have_picture_start && code == PICTURE_START_CODE) {
            /* Start of the NEXT picture: the one we were building is
             * complete. Don't consume this start code -- leave it for the
             * next call. */
            consumed = (uint32_t)start;
            break;
        }

        if (code == SEQ_START_CODE) {
            struct bitreader br;
            br_init(&br, buf + start + 4, size - (uint32_t)start - 4);
            parse_sequence_header(state, &br);
            pos = (uint32_t)start + 4;
            continue;
        }

        if (code == GOP_START_CODE) {
            /* Nothing in here that VAPictureParameterBufferMPEG2/
             * VAIQMatrixBufferMPEG2 needs -- skip past the start code and
             * let the next find_start_code call locate whatever follows. */
            pos = (uint32_t)start + 4;
            continue;
        }

        if (code == EXT_START_CODE) {
            struct bitreader br;
            br_init(&br, buf + start + 4, size - (uint32_t)start - 4);
            int ext_id = (int)br_get(&br, 4);
            switch (ext_id) {
            case 0x1: /* sequence_extension */
                parse_sequence_extension(state, &br);
                break;
            case 0x3: /* quant_matrix_extension */
                parse_quant_matrix_extension(state, &br);
                break;
            case 0x8: /* picture_coding_extension */
                parse_picture_coding_extension(state, &br);
                break;
            default:
                /* sequence_display_extension, picture_display_extension,
                 * etc. -- not needed. */
                break;
            }
            pos = (uint32_t)start + 4;
            continue;
        }

        if (code == PICTURE_START_CODE) {
            struct bitreader br;
            br_init(&br, buf + start + 4, size - (uint32_t)start - 4);
            parse_picture_header(state, &br);
            have_picture_start = 1;
            pos = (uint32_t)start + 4;
            continue;
        }

        if (code >= SLICE_MIN_START && code <= SLICE_MAX_START) {
            if (!have_picture_start || !state->has_sequence)
                return -1; /* slice before any picture/sequence header -- malformed */

            if (*num_slices >= max_slices)
                return -1; /* caller's array too small -- treat as an error, not silently drop */

            struct mpeg2_parsed_slice *slice = &slices[*num_slices];
            slice->slice_vertical_position = (uint32_t)(code - 1);

            struct bitreader br;
            br_init(&br, buf + start + 4, size - (uint32_t)start - 4);
            parse_slice_header(&br, slice);

            /* This slice's data runs from its own start code to wherever
             * the next start code is (found on the next loop iteration) --
             * fill in data/size once we know that boundary. Store the
             * start offset for now via macroblock_bit_offset's caller-side
             * bookkeeping: simplest correct approach is a second pass, but
             * since we scan strictly forward and never need to revisit an
             * earlier slice's end boundary except the immediately
             * preceding one, fix up the PREVIOUS slice's size here instead
             * (see below). */
            slice->data = buf + start;
            slice->size = 0; /* filled in once the next start code is found */
            if (*num_slices > 0) {
                struct mpeg2_parsed_slice *prev = &slices[*num_slices - 1];
                prev->size = (uint32_t)start - (uint32_t)(prev->data - buf);
            }
            (*num_slices)++;
            pos = (uint32_t)start + 4;
            continue;
        }

        /* Any other start code (user data, sequence-end, etc.): skip. */
        pos = (uint32_t)start + 4;
    }

    /* Fix up the last slice's size now that we know where the picture
     * (and therefore the last slice) ends. */
    if (*num_slices > 0) {
        struct mpeg2_parsed_slice *last = &slices[*num_slices - 1];
        last->size = consumed - (uint32_t)(last->data - buf);
    }

    return (int)consumed;
}
