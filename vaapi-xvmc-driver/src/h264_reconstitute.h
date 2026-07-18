#ifndef H264_RECONSTITUTE_H
#define H264_RECONSTITUTE_H

#include <stdint.h>

#include <va/va.h>

/*
 * Real client software (ffmpeg) hands this driver each H.264 slice's
 * VASliceDataBufferType bytes as the *original bitstream NAL unit*
 * (1-byte NAL header + slice data, still with emulation-prevention bytes
 * intact) minus only the leading Annex-B start code -- confirmed directly
 * against both FFmpeg's real vaapi_h264.c (nal->raw_data/raw_size, the
 * pre-de-escape form) and va.h's own VASliceParameterBufferH264.slice_data_bit_offset
 * doc comment ("the slice data buffer passed to the hardware is the
 * original bitstream, thus including any emulation prevention bytes").
 * So reconstituting a valid Annex-B stream for relay-server's ffmpeg to
 * decode from scratch needs only two things this driver doesn't do yet:
 *
 *   1. A 00 00 01 start code before each slice NAL (this driver currently
 *      just concatenates consecutive VASliceDataBufferType buffers with
 *      no framing between them at all).
 *   2. SPS and PPS NAL units, which real VA-API clients never hand the
 *      driver as raw bytes (only as parsed VAPictureParameterBufferH264
 *      fields, since real hardware decoders configure themselves from
 *      that directly) -- but a standalone Annex-B decoder like
 *      relay-server's ffmpeg needs a real SPS/PPS to know how to
 *      interpret the slice NALs at all. This file synthesizes minimal
 *      but valid ones from VAPictureParameterBufferH264.
 *
 * A third thing turned out to matter for the *synchronous* relay
 * protocol specifically, discovered only once real end-to-end testing
 * got past (1) and (2) above and still deadlocked: Annex-B has no
 * explicit NAL length field, so a demuxer only knows a NAL is complete
 * once it sees the start of the *next* NAL (or EOF) -- there is no
 * other way to know the current one has ended. Since this driver sends
 * exactly one picture's worth of bytes per network round trip and then
 * blocks waiting for the decoded result, relay-server's ffmpeg is stuck
 * forever unable to confirm the one slice NAL it received is finished
 * (confirmed by real testing: the response only ever arrived at the
 * exact moment the connection was torn down externally, which is what
 * finally delivered ffmpeg an EOF). The standard fix (used by real
 * pipe/RTP H.264 streamers for the same reason) is to follow each
 * picture with a lightweight Access Unit Delimiter NAL -- its own start
 * code is what tells the demuxer the previous NAL is done, without
 * waiting for real next-frame data or EOF.
 *
 * This is a bounded, well-defined bitstream *writing* task (exp-Golomb
 * encoding a small fixed set of fields), not a decoder -- much simpler in
 * kind than mpeg2_vld.c's entropy decode or mpeg2_headers.c's header
 * parsing, but still bit-exact-or-wrong the same way, so it's
 * implemented directly against the spec's exp-Golomb conventions rather
 * than guessed.
 */

/* Writes a 00 00 01 Annex-B start code into out (3 bytes). Returns 3. */
int h264_write_start_code(uint8_t *out);

/*
 * Synthesizes a minimal SPS NAL unit (start code + NAL header + RBSP,
 * with emulation-prevention bytes correctly inserted) from `pp` and the
 * profile this config/context was created for (VA-API doesn't carry
 * profile_idc directly in VAPictureParameterBufferH264 -- it's implied by
 * which VAProfile the config was created with). Writes into `out` (must
 * have at least `cap` bytes); returns the number of bytes written, or -1
 * if `cap` is too small.
 */
int h264_synthesize_sps(const VAPictureParameterBufferH264 *pp, VAProfile profile,
                         uint8_t *out, int cap);

/*
 * Synthesizes a minimal PPS NAL unit referencing seq_parameter_set_id=0
 * (matching h264_synthesize_sps's fixed SPS id) and pic_parameter_set_id=0.
 * num_ref_idx_l{0,1}_default_active_minus1 aren't carried by
 * VAPictureParameterBufferH264 at all -- pass the values captured from
 * the first VASliceParameterBufferH264 seen this session (va.h's own doc
 * comment on that struct says clients set it to the PPS default unless a
 * slice overrides it, so the first slice's value is the right one to
 * use). Getting this field wrong causes a real entropy-decode desync in
 * downstream decoders (confirmed against a real encode). Same buffer
 * contract as h264_synthesize_sps.
 */
int h264_synthesize_pps(const VAPictureParameterBufferH264 *pp,
                         uint8_t num_ref_idx_l0_active_minus1,
                         uint8_t num_ref_idx_l1_active_minus1,
                         uint8_t *out, int cap);

/*
 * Writes a minimal Access Unit Delimiter NAL (start code + NAL header +
 * one-byte RBSP with primary_pic_type=7, meaning "any slice type may be
 * present" -- always valid regardless of what the picture actually
 * contains, since this driver doesn't track per-slice types). Meant to
 * be sent immediately after each picture's slice NAL(s) so a
 * downstream Annex-B demuxer has an immediate, unambiguous boundary
 * marker instead of needing to wait for the next real frame or EOF.
 * Writes into `out` (must have at least 5 bytes); returns the number of
 * bytes written (always 5), or -1 if `cap` is too small.
 */
int h264_write_access_unit_delimiter(uint8_t *out, int cap);

#endif
