#include "h264_reconstitute.h"

#include <string.h>

/* MSB-first bit writer into a plain (non-escaped) byte buffer -- emulation
 * prevention is applied afterward, in a separate pass, exactly like a real
 * encoder: it's a property of the escaped bitstream, not of RBSP field
 * encoding, so keeping them separate avoids miscounting bits while also
 * tracking inserted 0x03 bytes. */
struct bs_writer {
    uint8_t *buf;
    int cap;
    int pos;
    int bit; /* 0-7: bits already used in buf[pos] */
};

static void bw_init(struct bs_writer *bw, uint8_t *buf, int cap)
{
    bw->buf = buf;
    bw->cap = cap;
    bw->pos = 0;
    bw->bit = 0;
    if (cap > 0)
        buf[0] = 0;
}

static int bw_put_bit(struct bs_writer *bw, int bit)
{
    if (bw->pos >= bw->cap)
        return -1;
    if (bw->bit == 0)
        bw->buf[bw->pos] = 0;
    bw->buf[bw->pos] |= (uint8_t)((bit & 1) << (7 - bw->bit));
    bw->bit++;
    if (bw->bit == 8) {
        bw->bit = 0;
        bw->pos++;
        if (bw->pos < bw->cap)
            bw->buf[bw->pos] = 0;
    }
    return 0;
}

static int bw_put_bits(struct bs_writer *bw, uint32_t val, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        if (bw_put_bit(bw, (int)((val >> i) & 1)) < 0)
            return -1;
    }
    return 0;
}

/* ue(v): Exp-Golomb unsigned, per spec 9.1 -- codeNum = val, encoded as
 * leadingZeroBits zeros, a 1, then leadingZeroBits bits of (codeNum+1). */
static int bw_put_ue(struct bs_writer *bw, uint32_t val)
{
    uint32_t code_plus_one = val + 1;
    int leading = 0;
    uint32_t tmp = code_plus_one;
    while (tmp > 1) {
        tmp >>= 1;
        leading++;
    }
    for (int i = 0; i < leading; i++) {
        if (bw_put_bit(bw, 0) < 0)
            return -1;
    }
    return bw_put_bits(bw, code_plus_one, leading + 1);
}

/* se(v): signed Exp-Golomb, per spec 9.1.1: codeNum = 2*|v| - (v>0), then
 * ue(codeNum) -- matches FFmpeg cbs_h264_syntax_template.c's se() usage. */
static int bw_put_se(struct bs_writer *bw, int32_t val)
{
    uint32_t code_num = (val <= 0) ? (uint32_t)(-2 * (int64_t)val)
                                    : (uint32_t)(2 * (int64_t)val - 1);
    return bw_put_ue(bw, code_num);
}

static int bw_rbsp_trailing_bits(struct bs_writer *bw)
{
    if (bw_put_bit(bw, 1) < 0)
        return -1;
    while (bw->bit != 0) {
        if (bw_put_bit(bw, 0) < 0)
            return -1;
    }
    return 0;
}

static int bw_byte_length(const struct bs_writer *bw)
{
    return bw->pos + (bw->bit > 0 ? 1 : 0);
}

/* Applies Annex-B emulation prevention (spec 7.4.1.1) to `in` (NAL header
 * byte followed by raw RBSP bytes) and writes the result to `out`. Returns
 * bytes written, or -1 if `cap` is too small. */
static int write_escaped(const uint8_t *in, int len, uint8_t *out, int cap)
{
    int zero_run = 0;
    int o = 0;
    for (int i = 0; i < len; i++) {
        uint8_t b = in[i];
        if (zero_run >= 2 && b <= 3) {
            if (o >= cap)
                return -1;
            out[o++] = 0x03;
            zero_run = 0;
        }
        if (o >= cap)
            return -1;
        out[o++] = b;
        zero_run = (b == 0) ? zero_run + 1 : 0;
    }
    return o;
}

int h264_write_start_code(uint8_t *out)
{
    out[0] = 0;
    out[1] = 0;
    out[2] = 1;
    return 3;
}

int h264_write_access_unit_delimiter(uint8_t *out, int cap)
{
    if (cap < 5)
        return -1;
    h264_write_start_code(out);
    out[3] = 0x09; /* nal_ref_idc=0, nal_unit_type=9 (AUD) */
    /* RBSP: primary_pic_type=7 (3 bits) + rbsp_trailing_bits
     * (stop_one_bit + zero padding to byte boundary) = 0b111 1 0000. */
    out[4] = 0xF0;
    return 5;
}

/* profile_idc values are fixed per spec Table A-1 / Annex A, keyed off
 * which VAProfile this config/context was created with -- VA-API never
 * hands the driver profile_idc directly. constraint_set flags follow what
 * real encoders (e.g. x264) emit for each profile, since decoders may use
 * them for compatibility checks. */
static int h264_profile_idc(VAProfile profile, uint8_t *constraint_set0_5)
{
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        *constraint_set0_5 = 0x30; /* constraint_set0_flag=1, constraint_set1_flag=1 */
        return 66;
    case VAProfileH264Main:
        *constraint_set0_5 = 0x00;
        return 77;
    case VAProfileH264High:
    default:
        *constraint_set0_5 = 0x00;
        return 100;
    }
}

int h264_synthesize_sps(const VAPictureParameterBufferH264 *pp, VAProfile profile,
                         uint8_t *out, int cap)
{
    uint8_t rbsp[256];
    struct bs_writer bw;
    bw_init(&bw, rbsp, sizeof(rbsp));

    uint8_t constraint_set0_5 = 0;
    int profile_idc = h264_profile_idc(profile, &constraint_set0_5);

    bw_put_bits(&bw, (uint32_t)profile_idc, 8);
    for (int i = 5; i >= 0; i--)
        bw_put_bit(&bw, (constraint_set0_5 >> i) & 1);
    bw_put_bits(&bw, 0, 2); /* reserved_zero_2bits */
    bw_put_bits(&bw, 41, 8); /* level_idc: fixed default (level 4.1) */
    bw_put_ue(&bw, 0); /* seq_parameter_set_id */

    /* High-profile-family SPS fields (spec 7.3.2.1.1) -- only these
     * profile_idc values carry chroma/bit-depth/scaling-matrix fields. */
    int high_profile_family =
        profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138;
    if (high_profile_family) {
        bw_put_ue(&bw, pp->seq_fields.bits.chroma_format_idc);
        if (pp->seq_fields.bits.chroma_format_idc == 3)
            bw_put_bit(&bw, pp->seq_fields.bits.residual_colour_transform_flag);
        bw_put_ue(&bw, pp->bit_depth_luma_minus8);
        bw_put_ue(&bw, pp->bit_depth_chroma_minus8);
        bw_put_bit(&bw, 0); /* qpprime_y_zero_transform_bypass_flag */
        bw_put_bit(&bw, 0); /* seq_scaling_matrix_present_flag: no custom scaling lists */
    }

    bw_put_ue(&bw, pp->seq_fields.bits.log2_max_frame_num_minus4);
    bw_put_ue(&bw, pp->seq_fields.bits.pic_order_cnt_type);
    if (pp->seq_fields.bits.pic_order_cnt_type == 0) {
        bw_put_ue(&bw, pp->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    } else if (pp->seq_fields.bits.pic_order_cnt_type == 1) {
        bw_put_bit(&bw, pp->seq_fields.bits.delta_pic_order_always_zero_flag);
        bw_put_se(&bw, 0); /* offset_for_non_ref_pic */
        bw_put_se(&bw, 0); /* offset_for_top_to_bottom_field */
        bw_put_ue(&bw, 0); /* num_ref_frames_in_pic_order_cnt_cycle */
    }

    bw_put_ue(&bw, pp->num_ref_frames);
    bw_put_bit(&bw, pp->seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
    bw_put_ue(&bw, pp->picture_width_in_mbs_minus1);
    bw_put_ue(&bw, pp->picture_height_in_mbs_minus1);
    bw_put_bit(&bw, pp->seq_fields.bits.frame_mbs_only_flag);
    if (!pp->seq_fields.bits.frame_mbs_only_flag)
        bw_put_bit(&bw, pp->seq_fields.bits.mb_adaptive_frame_field_flag);
    bw_put_bit(&bw, pp->seq_fields.bits.direct_8x8_inference_flag);
    bw_put_bit(&bw, 0); /* frame_cropping_flag: none, dimensions are whole MBs */
    bw_put_bit(&bw, 0); /* vui_parameters_present_flag */
    if (bw_rbsp_trailing_bits(&bw) < 0)
        return -1;

    int rbsp_len = bw_byte_length(&bw);
    uint8_t nal[300];
    nal[0] = (uint8_t)((3 << 5) | 7); /* nal_ref_idc=3, nal_unit_type=7 (SPS) */
    memcpy(nal + 1, rbsp, (size_t)rbsp_len);

    if (cap < 3)
        return -1;
    h264_write_start_code(out);
    int escaped = write_escaped(nal, rbsp_len + 1, out + 3, cap - 3);
    if (escaped < 0)
        return -1;
    return 3 + escaped;
}

int h264_synthesize_pps(const VAPictureParameterBufferH264 *pp,
                         uint8_t num_ref_idx_l0_active_minus1,
                         uint8_t num_ref_idx_l1_active_minus1,
                         uint8_t *out, int cap)
{
    uint8_t rbsp[256];
    struct bs_writer bw;
    bw_init(&bw, rbsp, sizeof(rbsp));

    bw_put_ue(&bw, 0); /* pic_parameter_set_id */
    bw_put_ue(&bw, 0); /* seq_parameter_set_id: matches h264_synthesize_sps's fixed id */
    bw_put_bit(&bw, pp->pic_fields.bits.entropy_coding_mode_flag);
    bw_put_bit(&bw, pp->pic_fields.bits.pic_order_present_flag);
    bw_put_ue(&bw, 0); /* num_slice_groups_minus1: single slice group */
    bw_put_ue(&bw, num_ref_idx_l0_active_minus1);
    bw_put_ue(&bw, num_ref_idx_l1_active_minus1);
    bw_put_bit(&bw, pp->pic_fields.bits.weighted_pred_flag);
    bw_put_bits(&bw, pp->pic_fields.bits.weighted_bipred_idc, 2);
    bw_put_se(&bw, pp->pic_init_qp_minus26);
    bw_put_se(&bw, pp->pic_init_qs_minus26);
    bw_put_se(&bw, pp->chroma_qp_index_offset);
    bw_put_bit(&bw, pp->pic_fields.bits.deblocking_filter_control_present_flag);
    bw_put_bit(&bw, pp->pic_fields.bits.constrained_intra_pred_flag);
    bw_put_bit(&bw, pp->pic_fields.bits.redundant_pic_cnt_present_flag);

    /* pps_extension (spec 7.3.2.2, gated by more_rbsp_data() -- a real
     * encoder omits this block entirely when it has nothing beyond the
     * spec's own inferred defaults to say, and that presence/absence is
     * apparently not just cosmetic: confirmed by real testing that
     * unconditionally emitting this block, even with values that match
     * what would have been inferred anyway (transform_8x8_mode_flag=0,
     * second_chroma_qp_index_offset==chroma_qp_index_offset), caused a
     * real entropy-decode desync a few frames into a real decode
     * (present in FFmpeg's own h264 decoder) that plain field-value
     * comparison against a reference PPS didn't reveal -- only omitting
     * the block when there's truly nothing non-default to signal, byte-
     * for-byte matching what a real encoder would have written, fixed
     * it. */
    int second_chroma_qp_index_offset = pp->second_chroma_qp_index_offset;
    if (pp->pic_fields.bits.transform_8x8_mode_flag ||
        second_chroma_qp_index_offset != pp->chroma_qp_index_offset) {
        bw_put_bit(&bw, pp->pic_fields.bits.transform_8x8_mode_flag);
        bw_put_bit(&bw, 0); /* pic_scaling_matrix_present_flag: no custom scaling lists */
        bw_put_se(&bw, second_chroma_qp_index_offset);
    }

    if (bw_rbsp_trailing_bits(&bw) < 0)
        return -1;

    int rbsp_len = bw_byte_length(&bw);
    uint8_t nal[300];
    nal[0] = (uint8_t)((3 << 5) | 8); /* nal_ref_idc=3, nal_unit_type=8 (PPS) */
    memcpy(nal + 1, rbsp, (size_t)rbsp_len);

    if (cap < 3)
        return -1;
    h264_write_start_code(out);
    int escaped = write_escaped(nal, rbsp_len + 1, out + 3, cap - 3);
    if (escaped < 0)
        return -1;
    return 3 + escaped;
}
