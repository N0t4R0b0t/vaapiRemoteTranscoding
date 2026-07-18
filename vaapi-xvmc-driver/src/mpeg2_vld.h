#ifndef MPEG2_VLD_H
#define MPEG2_VLD_H

#include <stdint.h>

#include <X11/extensions/XvMC.h>
#include <va/va.h>

/*
 * This is the hard part: turning one MPEG-2 slice's entropy-coded bits
 * (macroblock headers + DCT coefficients) into the already-decoded
 * XvMCMacroBlock/XvMCBlockArray form XvMCRenderSurface expects. Entropy
 * decode has to happen in software, in the driver, because no mainstream
 * app (ffmpeg/mpv) drives VA-API's legacy MoComp/IDCT entrypoints anymore;
 * they only speak VAEntrypointVLD, i.e. hand the driver raw compressed
 * bits and expect it to fully decode them.
 *
 * CORRECTED (was wrong when this file was first written): the real XvMC
 * hardware on this GMA950 netbook was probed directly via
 * XvMCListSurfaceTypes across all 16 ports -- every port reports MOCOMP-
 * only surface types (mc_type XVMC_MOCOMP), zero IDCT-capable types
 * anywhere. This matches Intel's own xf86-video-intel documentation:
 * 915/945-generation XvMC is MC-only; IDCT-capable XvMC only exists on
 * later G33-series chips. So this driver's entropy decoder ALSO performs
 * the IDCT step in software (see mpeg2_vld.c's second provenance block,
 * ported from libmpeg2 under GPL v2) and hands XvMCRenderSurface already-
 * spatial-domain blocks: full pixel data for intra macroblocks (no
 * prediction to add to), and residuals for non-intra macroblocks (which
 * the MOCOMP hardware itself adds to its motion-compensated prediction).
 *
 * This means real MPEG-2 Annex B VLC tables (macroblock_type, motion
 * vectors, coded_block_pattern, DCT coefficient run/level codes) and the
 * slice/macroblock header state machine. Do not hand-roll these tables
 * from memory -- a transcription error produces silently-wrong pixels,
 * not a compile error. Port this out of an existing bit-exact
 * implementation (ffmpeg's libavcodec mpeg12dec.c, or the historical
 * libmpeg2, which was written for exactly this XvMC hand-off) instead of
 * re-deriving it.
 *
 * Decodes the macroblocks covered by one slice into `mb_array` starting
 * at index `mb_array_offset`, allocating their DCT coefficient blocks out
 * of `blocks` starting at *block_index_cursor (which this function should
 * advance by 6 per macroblock decoded, 4:2:0). Returns the number of
 * macroblocks decoded (0 currently, always), or -1 on a bitstream error.
 */
/*
 * intra_unsigned: whether the real XvMC surface type this driver is
 * rendering to advertises XVMC_INTRA_UNSIGNED (see XvMCSurfaceInfo.flags
 * via XvMCListSurfaceTypes). When set, intra blocks are handed off as
 * absolute unsigned pixel data (0-255), matching libmpeg2's idct_copy
 * convention. When NOT set -- confirmed true for the real i915/945
 * MOCOMP surface type on this exact GMA950 hardware, by reading the
 * real xf86-video-intel DDX source (src/uxa/intel_hwmc.c's
 * i915_YV12_mpg2_surface has flags=0) -- the hardware instead expects
 * intra blocks in the SAME signed, level-shifted-by-128 residual
 * convention as non-intra blocks (it adds its own flat 128 prediction
 * internally, the same fixed-function correction-data path used for
 * inter blocks' motion-compensated prediction). Handing it unsigned
 * data in that case double-applies the +128 shift and saturates,
 * which is a confirmed, mechanistically exact match for the real
 * magenta/pink tint this driver was seeing (Cb/Cr both pushed to
 * their extreme positive end after the hardware's own +128 add).
 */
int mpeg2_vld_decode_slice(
    const uint8_t *slice_data, uint32_t slice_data_size, uint32_t macroblock_bit_offset,
    uint32_t slice_horizontal_position, uint32_t slice_vertical_position,
    int32_t quantiser_scale_code, int32_t intra_slice_flag,
    const VAPictureParameterBufferMPEG2 *pic_params,
    const VAIQMatrixBufferMPEG2 *iq_matrix, /* NULL if none sent yet this picture */
    XvMCMacroBlockArray *mb_array, unsigned int mb_array_offset,
    XvMCBlockArray *blocks, unsigned int *block_index_cursor,
    int intra_unsigned);

#endif
