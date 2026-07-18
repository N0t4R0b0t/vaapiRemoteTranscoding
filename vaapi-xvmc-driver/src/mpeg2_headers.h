#ifndef MPEG2_HEADERS_H
#define MPEG2_HEADERS_H

#include <stdint.h>

#include <va/va.h>

/*
 * Incrementally parses raw MPEG-2 Annex B bytes (sequence header, sequence
 * extension, quantiser matrix extension, picture header, picture coding
 * extension, slice headers) out of a byte stream, producing the pre-parsed
 * VAPictureParameterBufferMPEG2 / VAIQMatrixBufferMPEG2 / per-slice fields
 * mpeg2_vld_decode_slice() already expects -- the local-playback path gets
 * these for free from whatever real client software (ffmpeg/mpv) parses
 * before calling vaRenderPicture; the relay path (H.264 in, MPEG-2 out over
 * relay_client.c) has no such client-side parser, so this module exists to
 * fill that gap for the bytes that come back over the relay connection.
 *
 * State that persists across pictures (sequence-level info, quantiser
 * matrices) lives in struct mpeg2_header_state, which the caller owns
 * across the lifetime of one relay connection/decode session and passes to
 * every parse call.
 */

struct mpeg2_header_state {
    VAPictureParameterBufferMPEG2 pic_params;
    int has_pic_params;
    VAIQMatrixBufferMPEG2 iq_matrix;
    int has_iq_matrix; /* set once any quant-matrix-loading header is seen */

    /* Sequence-level state, parsed once and reused across pictures until a
     * new sequence header appears. */
    int has_sequence;
    uint16_t horizontal_size;
    uint16_t vertical_size;
};

/* One extracted slice: `data`/`size` point directly into the caller's input
 * buffer (including the leading 00 00 01 xx slice start code, matching this
 * project's existing macroblock_bit_offset convention already used by the
 * local-playback path) -- valid only as long as that buffer isn't freed or
 * overwritten before the caller is done with it. */
struct mpeg2_parsed_slice {
    const uint8_t *data;
    uint32_t size;
    uint32_t macroblock_bit_offset;
    uint32_t slice_horizontal_position;
    uint32_t slice_vertical_position;
    int32_t quantiser_scale_code;
    int32_t intra_slice_flag;
};

/*
 * Attempts to parse ONE complete picture out of buf[0..size): any
 * sequence/GOP/picture-level headers present, updating state->pic_params /
 * state->iq_matrix as they're seen, plus every slice belonging to that
 * picture. A picture is only known "complete" once parsing sees the start
 * of the NEXT picture (or runs past a length the caller has confirmed is
 * final) -- streamed input naturally works this way, matching how the
 * relay connection actually delivers bytes (more keep arriving after the
 * current picture's last real slice).
 *
 * Returns:
 *   > 0  -- one full picture was parsed; return value is the number of
 *           bytes of `buf` it consumed (the caller should keep any
 *           trailing unconsumed bytes for the next call). `slices` (an
 *           array of `max_slices` capacity provided by the caller) is
 *           filled in with that picture's slices, `*num_slices` set to how
 *           many.
 *     0  -- `buf` does not yet contain a complete picture (the start of a
 *           following picture hasn't appeared yet) -- not an error; the
 *           caller should append more bytes (e.g. via another relay_recv)
 *           and retry.
 *    -1  -- a genuine parse error (malformed/unrecognized data).
 */
int mpeg2_headers_parse_picture(
    struct mpeg2_header_state *state,
    const uint8_t *buf, uint32_t size,
    struct mpeg2_parsed_slice *slices, unsigned int max_slices,
    unsigned int *num_slices);

#endif
