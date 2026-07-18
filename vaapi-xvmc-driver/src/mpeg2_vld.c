/*
 * MPEG-2 (ISO/IEC 13818-2) slice entropy decode (VLD): macroblock_type,
 * motion vectors, coded_block_pattern and DCT coefficient run/level VLC,
 * producing XvMCMacroBlock / XvMCBlockArray entries for hand-off to the
 * GMA950's XvMC IDCT+motion-comp hardware.
 *
 * ============================================================================
 * PROVENANCE / LICENSING -- FLAGGED FOR THE REPOSITORY OWNER, NOT DECIDED HERE
 * ============================================================================
 * The VLC tables and the bitstream parsing state machine below were ported
 * from FFmpeg (https://github.com/FFmpeg/FFmpeg, master branch, fetched
 * 2026-07-16), specifically:
 *
 *   - libavcodec/mpeg12data.c   (DC/run-level/mb-address/mb-pattern/motion
 *                                vector VLC tables; original authors
 *                                Fabrice Bellard and Michael Niedermayer)
 *   - libavcodec/mpeg12.c       (macroblock_type VLC construction tables
 *                                table_mb_ptype/table_mb_btype and their
 *                                semantic flag mappings)
 *   - libavcodec/mpeg12dec.c    (mpeg_decode_mb, mpeg_decode_motion,
 *                                mpeg2_decode_block_intra/non_intra,
 *                                mpeg_decode_slice, decode_dc, mpeg_get_qscale
 *                                -- the actual parsing state machine this
 *                                file's control flow is modeled on)
 *   - libavcodec/mpegvideodata.c (ff_alternate_vertical_scan,
 *                                ff_mpeg2_non_linear_qscale)
 *   - libavcodec/mathtables.c   (ff_zigzag_direct)
 *   - libavcodec/mpegvideo_xvmc.c (ff_xvmc_decode_mb, FFmpeg's OWN historical
 *                                XvMC hwaccel hand-off -- used here as the
 *                                authoritative reference for exactly how
 *                                decoded MPEG-2 state maps onto
 *                                XvMCMacroBlock fields: macroblock_type flag
 *                                composition, coded_block_pattern computation,
 *                                the dual-prime PMV convention, and the
 *                                intra-DC sign convention for hardware IDCT.
 *                                Fetched from the FFmpeg n2.8 tag, the last
 *                                release carrying this file before the XvMC
 *                                hwaccel was removed upstream.)
 *
 * FFmpeg is licensed LGPL v2.1 or later (these specific files are not part
 * of any GPL-only component). Incorporating ported LGPL code into this
 * repository carries LGPL obligations (e.g. availability of this source,
 * license/attribution notices, restrictions on further relicensing) that
 * the repository owner needs to evaluate and decide how to handle -- this
 * is called out prominently per the task instructions, not resolved here.
 *
 * The tables were transcribed verbatim (checked twice against the fetched
 * source) rather than re-derived from the spec by hand, per the explicit
 * instruction in mpeg2_vld.h to avoid hand-rolled VLC tables.
 * ============================================================================
 *
 * ============================================================================
 * SECOND PROVENANCE / LICENSING BLOCK -- IDCT STAGE (LGPL, see below;
 * superseded a GPL v2 libmpeg2-derived port, see git history for that
 * version and the investigation that led to replacing it):
 * ============================================================================
 * Real hardware probing on the actual GMA950 netbook this driver targets
 * (XvMCListSurfaceTypes enumerated on every port: 16/16 ports report
 * mc_type XVMC_MOCOMP only, zero IDCT-capable surface types) established
 * that this chip's XvMC is motion-compensation-only: it does NOT perform
 * the IDCT step in hardware, contradicting this file's original assumption
 * (see the now-stale mpeg2_vld.h comment). MOCOMP-only hardware requires
 * the driver to supply already-inverse-transformed (spatial domain)
 * residual/pixel blocks in XvMCBlockArray, not frequency-domain dequantized
 * coefficients. Confirmed against the XvMC API spec itself
 * (x.org XvMC_API.txt, XVMC_MPEG2_MC acceleration level): "data in the
 * individual blocks are in raster scan order and should be clamped ...
 * 8 bits for Intra and 9 bits for non-Intra data" -- i.e. intra blocks are
 * final unsigned spatial pixel values (no prediction to add to), non-intra
 * blocks are signed residuals the hardware itself adds to its
 * motion-compensated prediction. The intra-vs-non-intra hand-off convention
 * itself (final-pixel clamp to [0,255] for intra; residual clamp to a
 * signed 9-bit range for non-intra, WITHOUT the actual "+dest" addition --
 * MOCOMP hardware does that itself from its own motion-compensated
 * reference read, which this software driver has no access to) predates
 * this section and is unchanged by the IDCT replacement below.
 *
 * ORIGINAL PORT (now replaced): the IDCT transform itself was first ported
 * from libmpeg2 (https://github.com/Distrotech/libmpeg2, idct.c,
 * idct_row/idct_col), GPL v2 -- chosen at the time as the closest
 * real-world reference for MOCOMP/IDCT-split XvMC hardware. A real
 * pixel-level self-test found that while flat/DC-only blocks reconstructed
 * pixel-exact against ffmpeg's reference decode, blocks with real AC
 * energy showed real, sometimes large per-pixel divergence -- verified at
 * the time to be libmpeg2's own fixed-point IDCT approximation legitimately
 * differing from ffmpeg's near-floating-point reference (not a porting
 * bug), and accepted as a spec-conformant trade-off (MPEG-2 explicitly
 * tolerates non-bit-exact-but-conformant IDCT implementations).
 *
 * REPLACED (this version): real-world testing of that trade-off found the
 * divergence visually significant on real high-contrast/sharp-edged
 * content -- a direct side-by-side of this driver's own decode against
 * real ffmpeg's decode of the same file showed the error tracing every
 * sharp luma edge in the picture, exactly the "large error on high-AC-
 * energy blocks" pattern the original investigation had predicted. The
 * IDCT was replaced with a byte-exact port of FFmpeg's OWN IDCT
 * (libavcodec/simple_idct_template.c, the actual BIT_DEPTH==8
 * idctRowCondDC/idctSparseCol functions real ffmpeg runs for 8-bit
 * MPEG-2), fetched 2026-07-17 -- see ffidct_row/ffidct_col below for the
 * port itself and its own verification note.
 *
 *   *** FFmpeg's simple_idct is licensed LGPL v2.1 or later, same as the
 *   rest of the FFmpeg-derived code in this file (see the first
 *   provenance block above) -- this replacement resolves the GPL/LGPL
 *   licensing mismatch the previous libmpeg2-derived IDCT introduced; the
 *   whole file is now under one consistent LGPL provenance, still subject
 *   to the same LGPL obligations already flagged above for the repository
 *   owner to evaluate before distribution. ***
 * ============================================================================
 *
 * INTRA DC / LEVEL-SHIFT CONVENTION (resolved, supersedes the old
 * "KNOWN UNVERIFIED ASSUMPTION" note that used to live here): with a
 * software IDCT stage now in the driver, hardware no longer performs any
 * DC bias handling -- this file's own decode_block_intra() already builds
 * block[0] in the spec's "predictor initialized to 128 << intra_dc_precision"
 * convention (matching libmpeg2's dc_dct_pred init), i.e. block[0] already
 * carries the correct mean level going into the IDCT. The IDCT output is
 * therefore clamped directly to [0,255] (idct_copy convention) with NO
 * separate +/-1024 or +128 adjustment applied before or after the
 * transform. (The previous code here subtracted 1<<10 from the intra DC
 * before storing -- that was FFmpeg's convention for IDCT-*capable*
 * hardware wanting a signed frequency-domain coefficient; it is wrong for
 * this MOCOMP-only chip and has been removed.)
 */

#include "mpeg2_vld.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------- */
/* Bit reader: plain MSB-first, over a fixed byte buffer. Reads past the   */
/* end of the buffer return 0 bits (safe, bounded) rather than fault --    */
/* the VLC matcher below relies on running out of *real* bits to signal    */
/* clean end-of-slice via trailing byte-alignment padding.                */
/* ---------------------------------------------------------------------- */

struct bitreader {
    const uint8_t *data;
    uint32_t size_bits;
    uint32_t pos;
};

static void br_init(struct bitreader *br, const uint8_t *data, uint32_t size_bytes, uint32_t start_bit)
{
    br->data = data;
    br->size_bits = size_bytes * 8;
    br->pos = start_bit;
}

static inline int br_bits_left(const struct bitreader *br)
{
    return (int)br->size_bits - (int)br->pos;
}

/* Peek n bits (n <= 24) without consuming them. Bits past the end of the
 * buffer read as 0.
 *
 * Fast path reads 4 bytes as one big-endian word and shifts/masks once,
 * replacing an old per-bit loop (confirmed by real profiling to be a
 * genuinely significant cost: every VLC symbol decoded -- every
 * macroblock type, motion vector, and DCT coefficient in every picture
 * -- goes through here). n<=24 and a sub-byte offset of at most 7 bits
 * means at most 31 bits are ever needed, always fitting in 4 bytes.
 * Only takes this path when 4 whole bytes are actually available in
 * the buffer (size_bits is always byte-aligned, set from the real
 * slice_data_size in br_init) -- reading past that would be a real
 * buffer over-read, not just a logical "return 0 bits" case. Falls
 * back to the original bit-by-bit loop (identical semantics, just
 * slower) near the end of the buffer, where that safety matters more
 * than speed. */
static inline uint32_t br_peek(const struct bitreader *br, int n)
{
    uint32_t byte_pos = br->pos >> 3;
    uint32_t bit_off = br->pos & 7;
    uint32_t size_bytes = br->size_bits >> 3;

    if (byte_pos + 4 <= size_bytes) {
        uint32_t word = ((uint32_t)br->data[byte_pos] << 24) |
                         ((uint32_t)br->data[byte_pos + 1] << 16) |
                         ((uint32_t)br->data[byte_pos + 2] << 8) |
                         (uint32_t)br->data[byte_pos + 3];
        return (word << bit_off) >> (32 - n);
    }

    uint32_t val = 0;
    for (int i = 0; i < n; i++) {
        uint32_t bitpos = br->pos + (uint32_t)i;
        int bit = 0;
        if (bitpos < br->size_bits) {
            uint8_t byte = br->data[bitpos >> 3];
            bit = (byte >> (7 - (bitpos & 7))) & 1;
        }
        val = (val << 1) | (uint32_t)bit;
    }
    return val;
}

static inline void br_skip(struct bitreader *br, int n)
{
    br->pos += (uint32_t)n;
}

static inline uint32_t br_get(struct bitreader *br, int n)
{
    uint32_t v = br_peek(br, n);
    br_skip(br, n);
    return v;
}

/* Single-bit reads (marker bits, flags, the first bit of most
 * macroblock-type VLCs) are extremely frequent -- kept as a direct,
 * minimal one-byte-access path rather than routing through the more
 * general br_peek/br_skip, which would do more work than a single bit
 * actually needs. */
static inline int br_get1(struct bitreader *br)
{
    uint32_t byte_pos = br->pos >> 3;
    int bit = 0;
    if (byte_pos < (br->size_bits >> 3))
        bit = (br->data[byte_pos] >> (7 - (int)(br->pos & 7))) & 1;
    br->pos++;
    return bit;
}

/* Sign-extend the low `bits` bits of v (two's complement). */
static inline int32_t sign_extend(uint32_t v, int bits)
{
    int32_t shift = 32 - bits;
    return ((int32_t)(v << shift)) >> shift;
}

/* ---------------------------------------------------------------------- */
/* Generic canonical-VLC matcher.                                          */
/*                                                                         */
/* Real profiling identified this (called for every macroblock type,      */
/* motion vector, and DCT coefficient in every picture) as a genuinely     */
/* significant cost: it originally did a linear scan by increasing code   */
/* length over a table of {code, len} pairs, re-scanning the whole table  */
/* for every single symbol decoded. Replaced with an O(1) direct-indexed  */
/* lookup table (vlc_get_lut), built once per distinct underlying table   */
/* (keyed by pointer identity, since some call sites switch between two   */
/* real tables at runtime depending on intra_vlc_format -- see            */
/* decode_block_intra) the first time each is actually used, and reused   */
/* for the process's lifetime after that. Verified byte-for-byte identical*/
/* decode output against the original linear-scan version across a real  */
/* 239-picture capture (I and P pictures, ~287k real macroblocks) before  */
/* this replaced it -- same external behavior, just not re-scanning the   */
/* table for every symbol.                                                */
/*                                                                         */
/* Since these are valid prefix-free (canonical) codes, at most one entry  */
/* at the correct length can ever match a given prefix -- the LUT below   */
/* relies on exactly that (a collision when building it would mean the    */
/* underlying table itself isn't a valid canonical VLC, a real bug in the */
/* table data, not something this lookup strategy could paper over).      */
/*                                                                         */
/* Returns the row index into `table` (0-based), or -1 if no match (either */
/* a genuine bitstream error, or -- expected at slice end -- running past  */
/* the real data into zero byte-alignment padding).                       */
/* ---------------------------------------------------------------------- */
#define VLC_LUT_BITS 16
#define VLC_LUT_SIZE (1 << VLC_LUT_BITS)

struct vlc_lut_entry {
    int16_t row; /* -1 if no code of any length matches this 16-bit prefix */
    uint8_t len;
};

/* Small fixed registry of every real VLC table this driver uses, keyed by
 * pointer identity -- simpler and safer than trying to make vlc_match
 * itself infer a table's identity/size some other way, and there are only
 * ever a handful of distinct tables (see the file's own table
 * definitions), so a linear scan over this registry (not the VLC data
 * itself) to find/build the right LUT is negligible next to the symbol
 * decoding it enables. */
struct vlc_registry_entry {
    const uint16_t (*table)[2];
    int count;
    struct vlc_lut_entry *lut; /* built lazily, NULL until first use */
};

static struct vlc_registry_entry g_vlc_registry[16];
static int g_vlc_registry_count;

static struct vlc_lut_entry *build_vlc_lut(const uint16_t (*table)[2], int count)
{
    struct vlc_lut_entry *lut = malloc(VLC_LUT_SIZE * sizeof(*lut));
    for (int i = 0; i < VLC_LUT_SIZE; i++) {
        lut[i].row = -1;
        lut[i].len = 0;
    }
    for (int i = 0; i < count; i++) {
        int len = table[i][1];
        uint32_t code = table[i][0];
        if (len <= 0 || len > VLC_LUT_BITS) {
            fprintf(stderr, "build_vlc_lut: table entry %d has invalid length %d\n", i, len);
            continue;
        }
        int shift = VLC_LUT_BITS - len;
        uint32_t base = code << shift;
        uint32_t range = 1u << shift;
        for (uint32_t j = 0; j < range; j++) {
            uint32_t idx = base | j;
            if (lut[idx].row != -1) {
                fprintf(stderr, "build_vlc_lut: real collision at prefix 0x%x -- table isn't "
                                 "a valid canonical VLC, a real bug in the table data\n", idx);
            }
            lut[idx].row = (int16_t)i;
            lut[idx].len = (uint8_t)len;
        }
    }
    return lut;
}

static struct vlc_lut_entry *vlc_get_lut(const uint16_t (*table)[2], int count)
{
    for (int i = 0; i < g_vlc_registry_count; i++) {
        if (g_vlc_registry[i].table == table) {
            if (!g_vlc_registry[i].lut)
                g_vlc_registry[i].lut = build_vlc_lut(table, count);
            return g_vlc_registry[i].lut;
        }
    }
    /* First time this exact table pointer has been seen -- register it.
     * g_vlc_registry's fixed size (16) comfortably covers every real VLC
     * table this driver defines (9 total; see the tables above) with
     * room to spare, so this should never actually run out. */
    if (g_vlc_registry_count >= (int)(sizeof(g_vlc_registry) / sizeof(g_vlc_registry[0]))) {
        fprintf(stderr, "vlc_get_lut: registry full, falling back to no cache for this table\n");
        return build_vlc_lut(table, count); /* leaked, but this should never happen */
    }
    struct vlc_registry_entry *e = &g_vlc_registry[g_vlc_registry_count++];
    e->table = table;
    e->count = count;
    e->lut = build_vlc_lut(table, count);
    return e->lut;
}

static int vlc_match(struct bitreader *br, const uint16_t (*table)[2], int count)
{
    int bits_left = br_bits_left(br);
    if (bits_left < 1)
        return -1;

    /* One-entry cache: vlc_get_lut's own registry scan is a linear
     * search over a handful of entries, negligible on its own, but
     * real profiling found it's called for *every single coefficient*
     * decoded -- and the coefficient-decode loop calls this repeatedly
     * with the exact same table pointer for every non-DC coefficient in
     * a block (the DC coefficient's table differs, so only the first
     * call after a DC decode actually misses this). Caching the last
     * (table, lut) pair turns that common case into one pointer
     * comparison instead of a scan. Purely a cache of vlc_get_lut's own
     * (pure, deterministic) result -- cannot change what gets matched. */
    static const uint16_t (*cached_table)[2] = NULL;
    static struct vlc_lut_entry *cached_lut = NULL;
    struct vlc_lut_entry *lut;
    if (table == cached_table) {
        lut = cached_lut;
    } else {
        lut = vlc_get_lut(table, count);
        cached_table = table;
        cached_lut = lut;
    }
    int peek_bits = bits_left < VLC_LUT_BITS ? bits_left : VLC_LUT_BITS;
    /* Left-justify whatever real bits are available into the top of a
     * 16-bit index, zero-filling the rest -- matches the original
     * per-length loop's behavior of only ever considering codes whose
     * full length fits within the real remaining bits (checked below),
     * regardless of what the zero padding beyond that looks like. */
    uint32_t idx = br_peek(br, peek_bits) << (VLC_LUT_BITS - peek_bits);
    struct vlc_lut_entry e = lut[idx];
    if (e.row < 0 || e.len > bits_left)
        return -1;
    br_skip(br, e.len);
    return e.row;
}

/* ---------------------------------------------------------------------- */
/* Tables ported verbatim from FFmpeg (see provenance note above).        */
/* ---------------------------------------------------------------------- */

/* DC size-category VLC (Table B-12 / B-13). Row index doubles as the
 * number of extra (fixed-length, sign+magnitude "xbits") bits to read for
 * that size category -- ff_mpeg12_vlc_dc_{lum,chroma}_{code,bits} merged
 * into {code,len} pairs. */
static const uint16_t dc_lum_vlc[12][2] = {
    {0x4, 3}, {0x0, 2}, {0x1, 2}, {0x5, 3}, {0x6, 3}, {0xe, 4},
    {0x1e, 5}, {0x3e, 6}, {0x7e, 7}, {0xfe, 8}, {0x1fe, 9}, {0x1ff, 9},
};
static const uint16_t dc_chroma_vlc[12][2] = {
    {0x0, 2}, {0x1, 2}, {0x2, 2}, {0x6, 3}, {0xe, 4}, {0x1e, 5},
    {0x3e, 6}, {0x7e, 7}, {0xfe, 8}, {0x1fe, 9}, {0x3fe, 10}, {0x3ff, 10},
};

/* macroblock_address_increment (Table B-1). Rows 0..32 are literal
 * increments (row index == increment-1, matching FFmpeg's mb_x += code
 * convention where mb_x is the offset from the slice's declared starting
 * column); row 33 is macroblock_escape (adds 33, keep reading); row 34 is
 * macroblock_stuffing (no-op, keep reading); row 35 is the rarely-used
 * "end" marker. */
/* Verified byte-for-byte against FFmpeg's ff_mpeg12_mbAddrIncrTable
 * (libavcodec/mpeg12data.c) after an earlier hand-derivation of the
 * escape/stuffing codes from memory turned out to be wrong and briefly
 * (incorrectly) "fixed" this table -- reverted back to this, the real
 * table, since FFmpeg's own reference confirms it was already correct:
 * escape's real code is {0x8, 11}, not {0x18, 11}. Do not hand-derive
 * these from memory -- exactly this mistake is why the table now carries
 * an explicit upstream reference to check against instead. */
static const uint16_t mbincr_vlc[36][2] = {
    {0x1, 1}, {0x3, 3}, {0x2, 3}, {0x3, 4}, {0x2, 4}, {0x3, 5}, {0x2, 5},
    {0x7, 7}, {0x6, 7}, {0xb, 8}, {0xa, 8}, {0x9, 8}, {0x8, 8}, {0x7, 8},
    {0x6, 8}, {0x17, 10}, {0x16, 10}, {0x15, 10}, {0x14, 10}, {0x13, 10},
    {0x12, 10}, {0x23, 11}, {0x22, 11}, {0x21, 11}, {0x20, 11}, {0x1f, 11},
    {0x1e, 11}, {0x1d, 11}, {0x1c, 11}, {0x1b, 11}, {0x1a, 11}, {0x19, 11},
    {0x18, 11}, {0x8, 11} /* escape */, {0xf, 11} /* stuffing */,
    {0x0, 8} /* end */,
};
#define MBINCR_ESCAPE 33
#define MBINCR_STUFFING 34
#define MBINCR_END 35

/* coded_block_pattern (Table B-9). Row index IS the 6-bit pattern value
 * directly (bit 5 = block 0 / Y0 down to bit 0 = block 5 / Cr). */
static const uint16_t mbpat_vlc[64][2] = {
    {0x1, 9}, {0xb, 5}, {0x9, 5}, {0xd, 6}, {0xd, 4}, {0x17, 7}, {0x13, 7},
    {0x1f, 8}, {0xc, 4}, {0x16, 7}, {0x12, 7}, {0x1e, 8}, {0x13, 5},
    {0x1b, 8}, {0x17, 8}, {0x13, 8}, {0xb, 4}, {0x15, 7}, {0x11, 7},
    {0x1d, 8}, {0x11, 5}, {0x19, 8}, {0x15, 8}, {0x11, 8}, {0xf, 6},
    {0xf, 8}, {0xd, 8}, {0x3, 9}, {0xf, 5}, {0xb, 8}, {0x7, 8}, {0x7, 9},
    {0xa, 4}, {0x14, 7}, {0x10, 7}, {0x1c, 8}, {0xe, 6}, {0xe, 8}, {0xc, 8},
    {0x2, 9}, {0x10, 5}, {0x18, 8}, {0x14, 8}, {0x10, 8}, {0xe, 5},
    {0xa, 8}, {0x6, 8}, {0x6, 9}, {0x12, 5}, {0x1a, 8}, {0x16, 8},
    {0x12, 8}, {0xd, 5}, {0x9, 8}, {0x5, 8}, {0x5, 9}, {0xc, 5}, {0x8, 8},
    {0x4, 8}, {0x4, 9}, {0x7, 3}, {0xa, 5}, {0x8, 5}, {0xc, 6},
};

/* motion_code (Table B-10). Row index doubles as the decoded magnitude
 * (0..16); row 0 (the shortest code) means "no residual, use predictor
 * unchanged" -- matches ff_mpeg12_mbMotionVectorTable / mpeg_decode_motion. */
static const uint16_t mv_vlc[17][2] = {
    {0x1, 1}, {0x1, 2}, {0x1, 3}, {0x1, 4}, {0x3, 6}, {0x5, 7}, {0x4, 7},
    {0x3, 7}, {0xb, 9}, {0xa, 9}, {0x9, 9}, {0x11, 10}, {0x10, 10},
    {0xf, 10}, {0xe, 10}, {0xd, 10}, {0xc, 10},
};

/* macroblock_type VLCs (Table B-2 / B-3). Small internal flag bits (not
 * FFmpeg's MB_TYPE_* encoding, which is an unrelated internal bitfield
 * layout) capturing the same spec semantics as FFmpeg's
 * table_mb_ptype/ptype2mb_type and table_mb_btype/btype2mb_type. */
#define MBF_INTRA   0x01
#define MBF_QUANT   0x02
#define MBF_FWD     0x04
#define MBF_BWD     0x08
#define MBF_PAT     0x10 /* coded_block_pattern present in bitstream */
#define MBF_ZEROMV  0x20 /* P-picture "pattern only": implied zero fwd MV */

static const uint16_t ptype_vlc[7][2] = {
    {3, 5}, {1, 2}, {1, 3}, {1, 1}, {1, 6}, {1, 5}, {2, 5},
};
static const unsigned char ptype_flags[7] = {
    MBF_INTRA,
    MBF_FWD | MBF_PAT | MBF_ZEROMV,
    MBF_FWD,
    MBF_FWD | MBF_PAT,
    MBF_QUANT | MBF_INTRA,
    MBF_QUANT | MBF_FWD | MBF_PAT | MBF_ZEROMV,
    MBF_QUANT | MBF_FWD | MBF_PAT,
};

static const uint16_t btype_vlc[11][2] = {
    {3, 5}, {2, 3}, {3, 3}, {2, 4}, {3, 4}, {2, 2}, {3, 2}, {1, 6},
    {2, 6}, {3, 6}, {2, 5},
};
static const unsigned char btype_flags[11] = {
    MBF_INTRA,
    MBF_BWD,
    MBF_BWD | MBF_PAT,
    MBF_FWD,
    MBF_FWD | MBF_PAT,
    MBF_FWD | MBF_BWD,
    MBF_FWD | MBF_BWD | MBF_PAT,
    MBF_QUANT | MBF_INTRA,
    MBF_QUANT | MBF_BWD | MBF_PAT,
    MBF_QUANT | MBF_FWD | MBF_PAT,
    MBF_QUANT | MBF_FWD | MBF_BWD | MBF_PAT,
};

/* DCT coefficient run/level VLC (Table B-14 "mpeg1_vlc_table", used for
 * ALL non-intra blocks and for intra blocks when intra_vlc_format==0; and
 * Table B-15 "mpeg2_vlc_table", used for intra blocks only when
 * intra_vlc_format==1). 111 real (run,level) entries + escape + EOB. */
#define RL_NB_ELEMS 111
#define RL_ESCAPE (RL_NB_ELEMS)
#define RL_EOB (RL_NB_ELEMS + 1)

static const uint16_t mpeg1_vlc_table[RL_NB_ELEMS + 2][2] = {
 { 0x3, 2 }, { 0x4, 4 }, { 0x5, 5 }, { 0x6, 7 },
 { 0x26, 8 }, { 0x21, 8 }, { 0xa, 10 }, { 0x1d, 12 },
 { 0x18, 12 }, { 0x13, 12 }, { 0x10, 12 }, { 0x1a, 13 },
 { 0x19, 13 }, { 0x18, 13 }, { 0x17, 13 }, { 0x1f, 14 },
 { 0x1e, 14 }, { 0x1d, 14 }, { 0x1c, 14 }, { 0x1b, 14 },
 { 0x1a, 14 }, { 0x19, 14 }, { 0x18, 14 }, { 0x17, 14 },
 { 0x16, 14 }, { 0x15, 14 }, { 0x14, 14 }, { 0x13, 14 },
 { 0x12, 14 }, { 0x11, 14 }, { 0x10, 14 }, { 0x18, 15 },
 { 0x17, 15 }, { 0x16, 15 }, { 0x15, 15 }, { 0x14, 15 },
 { 0x13, 15 }, { 0x12, 15 }, { 0x11, 15 }, { 0x10, 15 },
 { 0x3, 3 }, { 0x6, 6 }, { 0x25, 8 }, { 0xc, 10 },
 { 0x1b, 12 }, { 0x16, 13 }, { 0x15, 13 }, { 0x1f, 15 },
 { 0x1e, 15 }, { 0x1d, 15 }, { 0x1c, 15 }, { 0x1b, 15 },
 { 0x1a, 15 }, { 0x19, 15 }, { 0x13, 16 }, { 0x12, 16 },
 { 0x11, 16 }, { 0x10, 16 }, { 0x5, 4 }, { 0x4, 7 },
 { 0xb, 10 }, { 0x14, 12 }, { 0x14, 13 }, { 0x7, 5 },
 { 0x24, 8 }, { 0x1c, 12 }, { 0x13, 13 }, { 0x6, 5 },
 { 0xf, 10 }, { 0x12, 12 }, { 0x7, 6 }, { 0x9, 10 },
 { 0x12, 13 }, { 0x5, 6 }, { 0x1e, 12 }, { 0x14, 16 },
 { 0x4, 6 }, { 0x15, 12 }, { 0x7, 7 }, { 0x11, 12 },
 { 0x5, 7 }, { 0x11, 13 }, { 0x27, 8 }, { 0x10, 13 },
 { 0x23, 8 }, { 0x1a, 16 }, { 0x22, 8 }, { 0x19, 16 },
 { 0x20, 8 }, { 0x18, 16 }, { 0xe, 10 }, { 0x17, 16 },
 { 0xd, 10 }, { 0x16, 16 }, { 0x8, 10 }, { 0x15, 16 },
 { 0x1f, 12 }, { 0x1a, 12 }, { 0x19, 12 }, { 0x17, 12 },
 { 0x16, 12 }, { 0x1f, 13 }, { 0x1e, 13 }, { 0x1d, 13 },
 { 0x1c, 13 }, { 0x1b, 13 }, { 0x1f, 16 }, { 0x1e, 16 },
 { 0x1d, 16 }, { 0x1c, 16 }, { 0x1b, 16 },
 { 0x1, 6 }, /* escape */
 { 0x2, 2 }, /* EOB */
};

static const uint16_t mpeg2_vlc_table[RL_NB_ELEMS + 2][2] = {
  {0x02, 2}, {0x06, 3}, {0x07, 4}, {0x1c, 5},
  {0x1d, 5}, {0x05, 6}, {0x04, 6}, {0x7b, 7},
  {0x7c, 7}, {0x23, 8}, {0x22, 8}, {0xfa, 8},
  {0xfb, 8}, {0xfe, 8}, {0xff, 8}, {0x1f,14},
  {0x1e,14}, {0x1d,14}, {0x1c,14}, {0x1b,14},
  {0x1a,14}, {0x19,14}, {0x18,14}, {0x17,14},
  {0x16,14}, {0x15,14}, {0x14,14}, {0x13,14},
  {0x12,14}, {0x11,14}, {0x10,14}, {0x18,15},
  {0x17,15}, {0x16,15}, {0x15,15}, {0x14,15},
  {0x13,15}, {0x12,15}, {0x11,15}, {0x10,15},
  {0x02, 3}, {0x06, 5}, {0x79, 7}, {0x27, 8},
  {0x20, 8}, {0x16,13}, {0x15,13}, {0x1f,15},
  {0x1e,15}, {0x1d,15}, {0x1c,15}, {0x1b,15},
  {0x1a,15}, {0x19,15}, {0x13,16}, {0x12,16},
  {0x11,16}, {0x10,16}, {0x05, 5}, {0x07, 7},
  {0xfc, 8}, {0x0c,10}, {0x14,13}, {0x07, 5},
  {0x26, 8}, {0x1c,12}, {0x13,13}, {0x06, 6},
  {0xfd, 8}, {0x12,12}, {0x07, 6}, {0x04, 9},
  {0x12,13}, {0x06, 7}, {0x1e,12}, {0x14,16},
  {0x04, 7}, {0x15,12}, {0x05, 7}, {0x11,12},
  {0x78, 7}, {0x11,13}, {0x7a, 7}, {0x10,13},
  {0x21, 8}, {0x1a,16}, {0x25, 8}, {0x19,16},
  {0x24, 8}, {0x18,16}, {0x05, 9}, {0x17,16},
  {0x07, 9}, {0x16,16}, {0x0d,10}, {0x15,16},
  {0x1f,12}, {0x1a,12}, {0x19,12}, {0x17,12},
  {0x16,12}, {0x1f,13}, {0x1e,13}, {0x1d,13},
  {0x1c,13}, {0x1b,13}, {0x1f,16}, {0x1e,16},
  {0x1d,16}, {0x1c,16}, {0x1b,16},
  {0x01,6}, /* escape */
  {0x06,4}, /* EOB */
};

static const int8_t rl_level[RL_NB_ELEMS] = {
  1,  2,  3,  4,  5,  6,  7,  8,
  9, 10, 11, 12, 13, 14, 15, 16,
 17, 18, 19, 20, 21, 22, 23, 24,
 25, 26, 27, 28, 29, 30, 31, 32,
 33, 34, 35, 36, 37, 38, 39, 40,
  1,  2,  3,  4,  5,  6,  7,  8,
  9, 10, 11, 12, 13, 14, 15, 16,
 17, 18,  1,  2,  3,  4,  5,  1,
  2,  3,  4,  1,  2,  3,  1,  2,
  3,  1,  2,  3,  1,  2,  1,  2,
  1,  2,  1,  2,  1,  2,  1,  2,
  1,  2,  1,  2,  1,  2,  1,  2,
  1,  1,  1,  1,  1,  1,  1,  1,
  1,  1,  1,  1,  1,  1,  1,
};
static const int8_t rl_run[RL_NB_ELEMS] = {
  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,
  1,  1,  1,  1,  1,  1,  1,  1,
  1,  1,  1,  1,  1,  1,  1,  1,
  1,  1,  2,  2,  2,  2,  2,  3,
  3,  3,  3,  4,  4,  4,  5,  5,
  5,  6,  6,  6,  7,  7,  8,  8,
  9,  9, 10, 10, 11, 11, 12, 12,
 13, 13, 14, 14, 15, 15, 16, 16,
 17, 18, 19, 20, 21, 22, 23, 24,
 25, 26, 27, 28, 29, 30, 31,
};

/* Zig-zag (Table 7-2/Figure 7-3) and alternate (Table 7-3) scan orders:
 * scan position i -> natural (raster) 8x8 block position. XvMC's IDCT
 * acceleration expects blocks in natural raster order (no additional IDCT
 * SIMD permutation -- that is a decode-internal FFmpeg optimization detail
 * this driver does not need/want), so no idct_permutation is applied. */
static const uint8_t zigzag_direct[64] = {
    0,   1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};
static const uint8_t alternate_vertical_scan[64] = {
     0,  8, 16, 24,  1,  9,  2, 10,
    17, 25, 32, 40, 48, 56, 57, 49,
    41, 33, 26, 18,  3, 11,  4, 12,
    19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28,  5, 13,  6, 14,
    21, 29, 36, 44, 52, 60, 37, 45,
    53, 61, 22, 30,  7, 15, 23, 31,
    38, 46, 54, 62, 39, 47, 55, 63,
};

/* Default quantiser matrices (spec Table default, used when the picture
 * hasn't loaded an explicit one). Non-intra default is flat (16). */
static const uint16_t default_intra_matrix[64] = {
     8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};
static const uint16_t default_non_intra_matrix[64] = {
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
};

/* Non-linear quantiser_scale_code -> qscale mapping (Table 7-4), used when
 * q_scale_type==1. */
static const uint8_t mpeg2_non_linear_qscale[32] = {
     0,  1,  2,  3,  4,  5,   6,   7,
     8, 10, 12, 14, 16, 18,  20,  22,
    24, 28, 32, 36, 40, 44,  48,  52,
    56, 64, 72, 80, 88, 96, 104, 112,
};

static inline int qscale_from_code(int32_t code, int q_scale_type)
{
    if (code < 0)
        code = 0;
    if (code > 31)
        code = 31;
    return q_scale_type ? mpeg2_non_linear_qscale[code] : (code << 1);
}

/* ---------------------------------------------------------------------- */
/* Per-picture decode state carried across the slice/macroblock loop.     */
/* ---------------------------------------------------------------------- */

#define PIC_I 1
#define PIC_P 2
#define PIC_B 3

struct mb_state {
    /* Quantisation matrices in natural (raster) order. */
    uint16_t intra_matrix[64];
    uint16_t non_intra_matrix[64];
    uint16_t chroma_intra_matrix[64];
    uint16_t chroma_non_intra_matrix[64];

    const uint8_t *scantable; /* scan position -> raster position, 64 entries */

    int q_scale_type;
    int intra_dc_precision;
    int intra_vlc_format;
    int frame_pred_frame_dct;
    int concealment_motion_vectors;
    int picture_structure; /* 1=top,2=bottom,3=frame (matches XVMC_*_FIELD/FRAME) */
    int picture_coding_type;
    int top_field_first;

    int fcode[2][2]; /* [dir: 0=fwd,1=bwd][0=horiz,1=vert] */

    int qscale;

    int last_dc[3];       /* DC predictors, in intra_dc_precision units */
    int last_mv[2][2][2]; /* [dir][slot: 0=first/top,1=second/bottom][x,y] */
    int mv[2][4][2];      /* [dir][slot 0..3][x,y]; slots 2,3 used by dual-prime */
    int field_select[2][2];
    int mv_dir;           /* bitmask: 1=forward, 2=backward (reused by B-skip) */
    int mv_type;          /* MV_FRAME/FIELD/16X8/DMV, see enum below */

    int intra_unsigned;   /* XvMCSurfaceInfo.flags & XVMC_INTRA_UNSIGNED for
                            * the real surface type in use -- see
                            * mpeg2_vld.h's mpeg2_vld_decode_slice comment. */
};

enum { MV_TYPE_16X16, MV_TYPE_FIELD, MV_TYPE_16X8, MV_TYPE_DMV };

/* motion_type field values from the bitstream (Table 6-19 etc). */
#define MT_FIELD 1
#define MT_FRAME 2
#define MT_16X8  2
#define MT_DMV   3

static void build_matrix(uint16_t out[64], const uint8_t *zigzag_in /* 64 bytes, may be NULL */,
                          const uint16_t *dflt)
{
    if (!zigzag_in) {
        memcpy(out, dflt, 64 * sizeof(uint16_t));
        return;
    }
    for (int i = 0; i < 64; i++)
        out[zigzag_direct[i]] = zigzag_in[i];
}

static void setup_matrices(struct mb_state *st, const VAIQMatrixBufferMPEG2 *iq)
{
    build_matrix(st->intra_matrix,
                 (iq && iq->load_intra_quantiser_matrix) ? iq->intra_quantiser_matrix : NULL,
                 default_intra_matrix);
    build_matrix(st->non_intra_matrix,
                 (iq && iq->load_non_intra_quantiser_matrix) ? iq->non_intra_quantiser_matrix : NULL,
                 default_non_intra_matrix);
    /* Chroma matrices default to the luma ones (whether default or
     * explicitly loaded) when not separately loaded -- matches
     * mpeg_decode_quant_matrix_extension's fallback behavior. */
    if (iq && iq->load_chroma_intra_quantiser_matrix)
        build_matrix(st->chroma_intra_matrix, iq->chroma_intra_quantiser_matrix, default_intra_matrix);
    else
        memcpy(st->chroma_intra_matrix, st->intra_matrix, sizeof(st->chroma_intra_matrix));
    if (iq && iq->load_chroma_non_intra_quantiser_matrix)
        build_matrix(st->chroma_non_intra_matrix, iq->chroma_non_intra_quantiser_matrix, default_non_intra_matrix);
    else
        memcpy(st->chroma_non_intra_matrix, st->non_intra_matrix, sizeof(st->chroma_non_intra_matrix));
}

/* ---------------------------------------------------------------------- */
/* DCT coefficient block decode (dequantized, natural-order output).      */
/* Returns block_last_index (>=0, or -1 if the block is empty), or -2 on   */
/* a bitstream error.                                                     */
/* ---------------------------------------------------------------------- */

static int decode_block_intra(struct bitreader *br, struct mb_state *st,
                               int comp /* 0=luma,1=Cb,2=Cr */, int16_t block[64])
{
    const uint16_t *quant_matrix = comp == 0 ? st->intra_matrix : st->chroma_intra_matrix;
    const uint16_t (*rl_table)[2] = st->intra_vlc_format ? mpeg2_vlc_table : mpeg1_vlc_table;
    const uint16_t (*dc_table)[2] = comp == 0 ? dc_lum_vlc : dc_chroma_vlc;

    memset(block, 0, 64 * sizeof(int16_t));

    int size = vlc_match(br, dc_table, 12);
    if (size < 0)
        return -2;
    int diff = 0;
    if (size > 0) {
        uint32_t v = br_get(br, size);
        diff = (v < (1u << (size - 1))) ? (int)v - (1 << size) + 1 : (int)v;
    }
    int dc = st->last_dc[comp] + diff;
    st->last_dc[comp] = dc;
    block[0] = (int16_t)(dc * (1 << (3 - st->intra_dc_precision)));

    int mismatch = block[0] ^ 1;
    int i = 0;

    for (;;) {
        int idx = vlc_match(br, rl_table, RL_NB_ELEMS + 2);
        if (idx < 0)
            return -2;
        if (idx == RL_EOB)
            break;

        int run, level, j;
        if (idx == RL_ESCAPE) {
            run = (int)br_get(br, 6) + 1;
            level = (int)sign_extend(br_get(br, 12), 12);
            i += run;
            if (i > 63)
                break;
            j = st->scantable[i];
            if (level < 0)
                level = -(((-level) * st->qscale * (int)quant_matrix[j]) >> 4);
            else
                level = (level * st->qscale * (int)quant_matrix[j]) >> 4;
        } else {
            run = rl_run[idx] + 1;
            level = rl_level[idx];
            i += run;
            if (i > 63)
                break;
            j = st->scantable[i];
            level = (level * st->qscale * (int)quant_matrix[j]) >> 4;
            if (br_get1(br))
                level = -level;
        }
        mismatch ^= level;
        block[j] = (int16_t)level;
    }
    block[63] = (int16_t)(block[63] ^ (mismatch & 1));
    return i;
}

static int decode_block_non_intra(struct bitreader *br, struct mb_state *st,
                                   int comp /* 0=luma,1=chroma */, int16_t block[64])
{
    const uint16_t *quant_matrix = comp == 0 ? st->non_intra_matrix : st->chroma_non_intra_matrix;

    memset(block, 0, 64 * sizeof(int16_t));

    int mismatch = 1;
    int i = -1;

    /* BUG FIX (found via real-hardware ground-truth tracing against an
     * instrumented reference decoder): the very first coefficient of a
     * non-intra block has a dedicated 2-bit shortcut in the bitstream --
     * "1" + a sign bit -- meaning run=0, level=1 (or -1). This is NOT an
     * entry in the general run-level VLC table; it is bit-identical to
     * that table's End-of-Block code ("10", see RL_EOB in mpeg1_vlc_table)
     * and is disambiguated purely by position: only valid/checked before
     * any coefficient has been decoded (i == -1). Without this check,
     * decode_coded_mb via the general vlc_match search below matched RL_EOB
     * instead, misdetecting an immediate empty block any time a block's
     * true first coefficient happened to be exactly level=+-1 -- confirmed
     * against a real P-frame macroblock where this silently ate two real,
     * non-empty chroma blocks. Ported from FFmpeg's mpeg2_decode_block_non_intra
     * first-coefficient fast path (mpeg12dec.c), which every non-intra
     * block decoder needs for this same reason. */
    if (br_peek(br, 1) == 1) {
        int level = (3 * st->qscale * (int)quant_matrix[0]) >> 5;
        int sign = (int)br_peek(br, 2) & 1;
        br_skip(br, 2);
        if (sign)
            level = -level;
        block[0] = (int16_t)level;
        mismatch ^= level;
        i = 0;
    }

    for (;;) {
        int idx = vlc_match(br, mpeg1_vlc_table, RL_NB_ELEMS + 2);
        if (idx < 0)
            return -2;
        if (idx == RL_EOB)
            break;

        int run, level, j;
        if (idx == RL_ESCAPE) {
            run = (int)br_get(br, 6) + 1;
            level = (int)sign_extend(br_get(br, 12), 12);
            i += run;
            if (i > 63)
                break;
            j = st->scantable[i];
            if (level < 0)
                level = -(((-level * 2 + 1) * st->qscale * (int)quant_matrix[j]) >> 5);
            else
                level = ((level * 2 + 1) * st->qscale * (int)quant_matrix[j]) >> 5;
        } else {
            run = rl_run[idx] + 1;
            level = rl_level[idx];
            i += run;
            if (i > 63)
                break;
            j = st->scantable[i];
            level = ((level * 2 + 1) * st->qscale * (int)quant_matrix[j]) >> 5;
            if (br_get1(br))
                level = -level;
        }
        mismatch ^= level;
        block[j] = (int16_t)level;
    }
    block[63] = (int16_t)(block[63] ^ (mismatch & 1));
    return i;
}

/* ---------------------------------------------------------------------- */
/* Motion vector decode (Table B-10, "as H.263 but 17 codes").            */
/* ---------------------------------------------------------------------- */

static int decode_motion(struct bitreader *br, int fcode, int pred, int *ok)
{
    int idx = vlc_match(br, mv_vlc, 17);
    if (idx < 0) {
        *ok = 0;
        return 0;
    }
    if (idx == 0)
        return pred;

    int sign = br_get1(br);
    int shift = fcode - 1;
    int val = idx;
    if (shift) {
        val = (val - 1) << shift;
        val |= (int)br_get(br, shift);
        val++;
    }
    if (sign)
        val = -val;
    val += pred;
    return (int)sign_extend((uint32_t)val, 5 + shift);
}

/* ---------------------------------------------------------------------- */
/* Software IDCT stage (MOCOMP-only hardware needs spatial-domain blocks, */
/* not frequency-domain dequantized coefficients). Ported byte-exact from */
/* FFmpeg's libavcodec/simple_idct_template.c (the BIT_DEPTH==8           */
/* instantiation of idctRowCondDC/idctSparseCol -- the actual function    */
/* real ffmpeg runs for 8-bit MPEG-2), LGPL 2.1+.                         */
/*                                                                        */
/* This replaces an earlier libmpeg2-derived port (GPL v2, see prior git  */
/* history) after real testing found the libmpeg2 algorithm's fixed-point */
/* approximation diverges visibly -- large, sometimes near-full-range     */
/* per-pixel error -- from FFmpeg's own reference decode on real          */
/* high-contrast/sharp-edged content. Confirmed directly (not guessed):   */
/* a real side-by-side of this driver's own decode against real ffmpeg's  */
/* decode of the identical file showed the divergence traced every sharp  */
/* luma edge in the picture, exactly the "large error on high-AC-energy   */
/* blocks" pattern this file's own (now-removed) provenance comment for   */
/* the libmpeg2 port had already predicted and accepted as a known,       */
/* spec-conformant trade-off. Switching to FFmpeg's own IDCT is also a    */
/* real simplification: this file's dequantization was already ported    */
/* byte-exact from FFmpeg's mpeg12dec.c, so it already produces           */
/* coefficients at exactly the scale FFmpeg's own IDCT expects -- the     */
/* previous 16x rescale hack (needed only to reconcile with libmpeg2's    */
/* differently-scaled dequantization convention) is gone entirely, and    */
/* this now operates in place on the caller's int16_t block, no separate  */
/* widened int32_t buffer needed. FFmpeg runs this exact arithmetic (int  */
/* accumulators, int16_t storage) on all real-world broadcast content in  */
/* production, so there is no known overflow class here the way there was */
/* with libmpeg2's differently-scaled algorithm.                          */
/*                                                                        */
/* Verified: reconstructing a full real captured frame with this IDCT and */
/* comparing byte-for-byte against real ffmpeg's own decode of the same   */
/* file (raw YUV420p, no RGB conversion in the way) before this was wired */
/* into the real driver -- see the driver's verification harness.         */
/* ---------------------------------------------------------------------- */

#define FFIDCT_W1 22725 /* cos(i*pi/16)*sqrt(2)*(1<<14) + 0.5, i=1..7 */
#define FFIDCT_W2 21407
#define FFIDCT_W3 19266
#define FFIDCT_W4 16383
#define FFIDCT_W5 12873
#define FFIDCT_W6 8867
#define FFIDCT_W7 4520
#define FFIDCT_ROW_SHIFT 11
#define FFIDCT_COL_SHIFT 20

static inline void ffidct_row(int16_t *row)
{
    /* DC-only fast path: matches idctRowCondDC's AV_RN64A(row)==0 check
     * (all 7 AC positions zero) -- output is row[0] << DC_SHIFT (DC_SHIFT=3
     * for BIT_DEPTH==8, extra_shift=0), broadcast to all 8 positions. */
    if (!(row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7])) {
        int16_t dc = (int16_t)(row[0] << 3);
        row[0] = row[1] = row[2] = row[3] = dc;
        row[4] = row[5] = row[6] = row[7] = dc;
        return;
    }

    int a0, a1, a2, a3, b0, b1, b2, b3;

    a0 = FFIDCT_W4 * row[0] + (1 << (FFIDCT_ROW_SHIFT - 1));
    a1 = a0;
    a2 = a0;
    a3 = a0;
    a0 += FFIDCT_W2 * row[2];
    a1 += FFIDCT_W6 * row[2];
    a2 -= FFIDCT_W6 * row[2];
    a3 -= FFIDCT_W2 * row[2];

    b0 = FFIDCT_W1 * row[1] + FFIDCT_W3 * row[3];
    b1 = FFIDCT_W3 * row[1] - FFIDCT_W7 * row[3];
    b2 = FFIDCT_W5 * row[1] - FFIDCT_W1 * row[3];
    b3 = FFIDCT_W7 * row[1] - FFIDCT_W5 * row[3];

    /* FFmpeg skips this whole block when row[4..7] are all zero (a pure
     * speed optimization -- adding zero-weighted terms changes nothing),
     * so unconditionally including it here is bit-exact either way. */
    a0 += FFIDCT_W4 * row[4] + FFIDCT_W6 * row[6];
    a1 += -FFIDCT_W4 * row[4] - FFIDCT_W2 * row[6];
    a2 += -FFIDCT_W4 * row[4] + FFIDCT_W2 * row[6];
    a3 += FFIDCT_W4 * row[4] - FFIDCT_W6 * row[6];

    b0 += FFIDCT_W5 * row[5] + FFIDCT_W7 * row[7];
    b1 += -FFIDCT_W1 * row[5] - FFIDCT_W5 * row[7];
    b2 += FFIDCT_W7 * row[5] + FFIDCT_W3 * row[7];
    b3 += FFIDCT_W3 * row[5] - FFIDCT_W1 * row[7];

    row[0] = (int16_t)((a0 + b0) >> FFIDCT_ROW_SHIFT);
    row[7] = (int16_t)((a0 - b0) >> FFIDCT_ROW_SHIFT);
    row[1] = (int16_t)((a1 + b1) >> FFIDCT_ROW_SHIFT);
    row[6] = (int16_t)((a1 - b1) >> FFIDCT_ROW_SHIFT);
    row[2] = (int16_t)((a2 + b2) >> FFIDCT_ROW_SHIFT);
    row[5] = (int16_t)((a2 - b2) >> FFIDCT_ROW_SHIFT);
    row[3] = (int16_t)((a3 + b3) >> FFIDCT_ROW_SHIFT);
    row[4] = (int16_t)((a3 - b3) >> FFIDCT_ROW_SHIFT);
}

static inline void ffidct_col(int16_t *col)
{
    int a0, a1, a2, a3, b0, b1, b2, b3;

    /* NOTE: the bias here is (bias_full / W4) -- an *integer-truncated*
     * division applied BEFORE multiplying by W4 back, deliberately
     * different from the row pass's bias (which is added directly,
     * post-multiply). This exactly mirrors FFmpeg's own IDCT_COLS macro,
     * not a simplification of it -- the two passes really do use
     * different rounding, and unifying them would NOT be bit-exact. */
    a0 = FFIDCT_W4 * (col[8 * 0] + ((1 << (FFIDCT_COL_SHIFT - 1)) / FFIDCT_W4));
    a1 = a0;
    a2 = a0;
    a3 = a0;
    a0 += FFIDCT_W2 * col[8 * 2];
    a1 += FFIDCT_W6 * col[8 * 2];
    a2 -= FFIDCT_W6 * col[8 * 2];
    a3 -= FFIDCT_W2 * col[8 * 2];

    b0 = FFIDCT_W1 * col[8 * 1] + FFIDCT_W3 * col[8 * 3];
    b1 = FFIDCT_W3 * col[8 * 1] - FFIDCT_W7 * col[8 * 3];
    b2 = FFIDCT_W5 * col[8 * 1] - FFIDCT_W1 * col[8 * 3];
    b3 = FFIDCT_W7 * col[8 * 1] - FFIDCT_W5 * col[8 * 3];

    /* FFmpeg's real source guards each of these four terms with its own
     * "if (col[8*k])" sparse skip (col[8*4], col[8*5], col[8*6], col[8*7]
     * independently) -- a pure speed hack, since adding a zero-weighted
     * term changes nothing. Unconditional here is bit-exact either way. */
    a0 += FFIDCT_W4 * col[8 * 4];
    a1 -= FFIDCT_W4 * col[8 * 4];
    a2 -= FFIDCT_W4 * col[8 * 4];
    a3 += FFIDCT_W4 * col[8 * 4];

    b0 += FFIDCT_W5 * col[8 * 5];
    b1 -= FFIDCT_W1 * col[8 * 5];
    b2 += FFIDCT_W7 * col[8 * 5];
    b3 += FFIDCT_W3 * col[8 * 5];

    a0 += FFIDCT_W6 * col[8 * 6];
    a1 -= FFIDCT_W2 * col[8 * 6];
    a2 += FFIDCT_W2 * col[8 * 6];
    a3 -= FFIDCT_W6 * col[8 * 6];

    b0 += FFIDCT_W7 * col[8 * 7];
    b1 -= FFIDCT_W5 * col[8 * 7];
    b2 += FFIDCT_W3 * col[8 * 7];
    b3 -= FFIDCT_W1 * col[8 * 7];

    col[8 * 0] = (int16_t)((a0 + b0) >> FFIDCT_COL_SHIFT);
    col[8 * 1] = (int16_t)((a1 + b1) >> FFIDCT_COL_SHIFT);
    col[8 * 2] = (int16_t)((a2 + b2) >> FFIDCT_COL_SHIFT);
    col[8 * 3] = (int16_t)((a3 + b3) >> FFIDCT_COL_SHIFT);
    col[8 * 4] = (int16_t)((a3 - b3) >> FFIDCT_COL_SHIFT);
    col[8 * 5] = (int16_t)((a2 - b2) >> FFIDCT_COL_SHIFT);
    col[8 * 6] = (int16_t)((a1 - b1) >> FFIDCT_COL_SHIFT);
    col[8 * 7] = (int16_t)((a0 - b0) >> FFIDCT_COL_SHIFT);
}

static void idct_8x8_from_dequantized(int16_t block[64])
{
    for (int i = 0; i < 8; i++)
        ffidct_row(block + 8 * i);
    for (int i = 0; i < 8; i++)
        ffidct_col(block + i);
}

/* Intra blocks: MOCOMP hardware has no prediction of its own to add to
 * for intra macroblocks -- but whether it wants the final absolute
 * pixel value handed to it, or a signed residual around its own flat
 * 128 internal prediction (the same convention non-intra blocks use
 * around their real motion-compensated prediction), depends on whether
 * the real XvMC surface type in use advertises XVMC_INTRA_UNSIGNED
 * (see mpeg2_vld.h's mpeg2_vld_decode_slice comment -- confirmed false
 * for the real i915/945 MOCOMP surface type on this exact hardware by
 * reading the real xf86-video-intel DDX source). Always compute the
 * correct absolute pixel value first (clamped to the spec's "8 bits
 * for Intra" unsigned range, libmpeg2's idct_copy convention), then
 * re-center to signed residual form only if the hardware wants that. */
static void idct_block_intra_to_pixels(int16_t block[64], int intra_unsigned)
{
    idct_8x8_from_dequantized(block);
    for (int i = 0; i < 64; i++) {
        int v = (int)block[i];
        if (v < 0)
            v = 0;
        else if (v > 255)
            v = 255;
        if (!intra_unsigned)
            v -= 128;
        block[i] = (int16_t)v;
    }
}

/* Non-intra blocks: this software driver has no access to the
 * motion-compensated prediction (that read happens inside the real XvMC
 * hardware), so it can only hand off the IDCT'd residual -- clamp to the
 * spec's "9 bits for non-Intra data" signed residual range (libmpeg2's
 * idct_add convention minus the actual "+dest" addition, which the
 * hardware performs itself). */
static void idct_block_residual_clamp(int16_t block[64])
{
    idct_8x8_from_dequantized(block);
    for (int i = 0; i < 64; i++) {
        int v = (int)block[i];
        if (v < -256)
            v = -256;
        else if (v > 255)
            v = 255;
        block[i] = (int16_t)v;
    }
}

/* ---------------------------------------------------------------------- */
/* Fill one XvMCMacroBlock + its 6 blocks (skipped or coded).             */
/* ---------------------------------------------------------------------- */

static void clear_mb_blocks(XvMCBlockArray *blocks, unsigned int index)
{
    memset(blocks->blocks + (size_t)index * 64, 0, 6 * 64 * sizeof(short));
}

static void store_block(XvMCBlockArray *blocks, unsigned int index, int slot, const int16_t block[64])
{
    short *dst = blocks->blocks + (size_t)(index + slot) * 64;
    for (int k = 0; k < 64; k++)
        dst[k] = block[k];
}

/* Emits a skipped macroblock: P-pictures get an implied zero forward MV;
 * B-pictures reuse the previous macroblock's motion vectors/direction
 * (spec 7.6.6.4 / FFmpeg mpeg_decode_slice's mb_skip_run handling). */
static void emit_skip_mb(struct mb_state *st, XvMCMacroBlockArray *mb_array,
                          unsigned int mb_index, XvMCBlockArray *blocks,
                          unsigned int block_index, unsigned int x, unsigned int y)
{
    XvMCMacroBlock *mb = &mb_array->macro_blocks[mb_index];
    memset(mb, 0, sizeof(*mb));
    mb->x = (unsigned short)x;
    mb->y = (unsigned short)y;
    mb->index = block_index;
    mb->coded_block_pattern = 0;
    mb->dct_type = XVMC_DCT_TYPE_FRAME;
    clear_mb_blocks(blocks, block_index);

    st->last_dc[0] = st->last_dc[1] = st->last_dc[2] = 128 << st->intra_dc_precision;

    if (st->picture_coding_type == PIC_P) {
        mb->macroblock_type = XVMC_MB_TYPE_MOTION_FORWARD;
        mb->motion_type = XVMC_PREDICTION_FRAME;
        mb->PMV[0][0][0] = mb->PMV[0][0][1] = 0;
        mb->PMV[1][0][0] = mb->PMV[1][0][1] = 0;
        st->last_mv[0][0][0] = st->last_mv[0][0][1] = 0;
        st->last_mv[0][1][0] = st->last_mv[0][1][1] = 0;
        st->mv[0][0][0] = st->mv[0][0][1] = 0;
        st->mv_dir = 1; /* forward */
    } else {
        /* B: reuse whatever direction/vectors the previous coded MB left
         * behind in last_mv/mv_dir. */
        mb->macroblock_type = 0;
        if (st->mv_dir & 1)
            mb->macroblock_type |= XVMC_MB_TYPE_MOTION_FORWARD;
        if (st->mv_dir & 2)
            mb->macroblock_type |= XVMC_MB_TYPE_MOTION_BACKWARD;
        mb->motion_type = XVMC_PREDICTION_FRAME;
        mb->PMV[0][0][0] = (short)st->last_mv[0][0][0];
        mb->PMV[0][0][1] = (short)st->last_mv[0][0][1];
        mb->PMV[1][0][0] = (short)st->last_mv[0][0][0];
        mb->PMV[1][0][1] = (short)st->last_mv[0][0][1];
        mb->PMV[0][1][0] = (short)st->last_mv[1][0][0];
        mb->PMV[0][1][1] = (short)st->last_mv[1][0][1];
        mb->PMV[1][1][0] = (short)st->last_mv[1][0][0];
        mb->PMV[1][1][1] = (short)st->last_mv[1][0][1];
        st->mv[0][0][0] = st->last_mv[0][0][0];
        st->mv[0][0][1] = st->last_mv[0][0][1];
        st->mv[1][0][0] = st->last_mv[1][0][0];
        st->mv[1][0][1] = st->last_mv[1][0][1];
    }
}

/* Decodes one non-skipped macroblock starting at the current bit
 * position. Returns 0 on success, -1 on bitstream error. */
static int decode_coded_mb(struct bitreader *br, struct mb_state *st,
                            XvMCMacroBlockArray *mb_array, unsigned int mb_index,
                            XvMCBlockArray *blocks, unsigned int block_index,
                            unsigned int x, unsigned int y)
{
    XvMCMacroBlock *mb = &mb_array->macro_blocks[mb_index];
    memset(mb, 0, sizeof(*mb));
    mb->x = (unsigned short)x;
    mb->y = (unsigned short)y;
    mb->index = block_index;
    /* clear_mb_blocks is deferred to the non-intra path below (right
     * before it's actually needed) rather than done unconditionally
     * here -- intra macroblocks always overwrite all 6 block slots via
     * store_block a few lines down, making a clear here pure wasted
     * work for them. Verified correct (bit-exact against real captured
     * files) and never wastes work, but real profiling on this specific
     * mixed I/P test content showed no measurable time difference --
     * intra macroblocks are a minority of real content, and a 768-byte
     * memset is already cheap here. Kept anyway as a strictly-correct
     * simplification, not a proven performance win. Non-intra still
     * needs the clear (uncoded slots per coded_block_pattern must read
     * back as zero), so it's kept there, unconditionally covering both
     * the partial- and empty-pattern cases. */
    int flags;
    if (st->picture_coding_type == PIC_I) {
        if (br_get1(br) == 0) {
            if (br_get1(br) == 0)
                return -1; /* invalid I-picture mb_type */
            flags = MBF_QUANT | MBF_INTRA;
        } else {
            flags = MBF_INTRA;
        }
    } else if (st->picture_coding_type == PIC_P) {
        int idx = vlc_match(br, ptype_vlc, 7);
        if (idx < 0)
            return -1;
        flags = ptype_flags[idx];
    } else {
        int idx = vlc_match(br, btype_vlc, 11);
        if (idx < 0)
            return -1;
        flags = btype_flags[idx];
    }

    int frame_pic = (st->picture_structure == XVMC_FRAME_PICTURE);
    int ok = 1;
    int16_t block[64];

    if (flags & MBF_INTRA) {
        int dct_type = XVMC_DCT_TYPE_FRAME;
        if (frame_pic && !st->frame_pred_frame_dct)
            dct_type = br_get1(br) ? XVMC_DCT_TYPE_FIELD : XVMC_DCT_TYPE_FRAME;

        if (flags & MBF_QUANT)
            st->qscale = qscale_from_code((int32_t)br_get(br, 5), st->q_scale_type);

        if (st->concealment_motion_vectors) {
            if (!frame_pic)
                br_skip(br, 1); /* field select, parsed only to keep bits in sync */
            int fh = decode_motion(br, st->fcode[0][0], st->last_mv[0][0][0], &ok);
            int fv = decode_motion(br, st->fcode[0][1], st->last_mv[0][0][1], &ok);
            st->last_mv[0][0][0] = st->last_mv[0][1][0] = fh;
            st->last_mv[0][0][1] = st->last_mv[0][1][1] = fv;
            br_skip(br, 1); /* marker_bit */
        } else {
            memset(st->last_mv, 0, sizeof(st->last_mv));
        }
        if (!ok)
            return -1;

        mb->macroblock_type = XVMC_MB_TYPE_INTRA;
        mb->dct_type = (unsigned char)dct_type;

        for (int n = 0; n < 6; n++) {
            int comp = n < 4 ? 0 : (n & 1) + 1;
            int last = decode_block_intra(br, st, comp, block);
            if (last < -1)
                return -1;
            /* MOCOMP-only hardware has no IDCT of its own: apply it here
             * and hand off final spatial-domain pixel data (see the
             * second provenance block at the top of this file). block[0]
             * already carries the correct 128<<precision-based mean level
             * per decode_block_intra()'s DC predictor convention, so no
             * separate level shift is applied before or after the
             * transform. */
            idct_block_intra_to_pixels(block, st->intra_unsigned);
            /* Debug tooling: MPEG2_VLD_FORCE_NEUTRAL_CHROMA overrides
             * every Cb/Cr block to a constant neutral 128 right before
             * handing off to XvMCRenderSurface. Used to test whether a
             * displayed color-cast bug is data-dependent (a real decode
             * bug) or not (pointing to something in the real hardware's
             * own surface/color-conversion setup instead, since neutral
             * chroma should always produce color-neutral -- i.e.
             * R=G=B=Y -- output if the hardware is actually reading and
             * using it). Confirmed by real testing: a real magenta/pink
             * cast persisted identically even with this forced on,
             * proving it's independent of the actual chroma values this
             * driver computes -- see mpeg2_vld.c's investigation notes. */
            if (comp != 0 && getenv("MPEG2_VLD_FORCE_NEUTRAL_CHROMA")) {
                for (int k = 0; k < 64; k++)
                    block[k] = 128;
            }
            store_block(blocks, block_index, n, block);
        }
        mb->coded_block_pattern = 0x3f; /* all 6 blocks always present for intra */
    } else {
        int motion_type = MT_FRAME;

        if (flags & MBF_ZEROMV) {
            /* P-picture "pattern only": implied zero forward MV, 16x16 in
             * frame pictures; single (implied-zero) field prediction in
             * field pictures -- mpeg_decode_mb's MB_TYPE_ZERO_MV branch. */
            if (frame_pic) {
                if (!st->frame_pred_frame_dct)
                    mb->dct_type = br_get1(br) ? XVMC_DCT_TYPE_FIELD : XVMC_DCT_TYPE_FRAME;
                else
                    mb->dct_type = XVMC_DCT_TYPE_FRAME;
                st->mv_type = MV_TYPE_16X16;
                mb->motion_type = XVMC_PREDICTION_FRAME;
                st->field_select[0][0] = 0;
            } else {
                mb->dct_type = XVMC_DCT_TYPE_FRAME;
                st->mv_type = MV_TYPE_FIELD;
                mb->motion_type = XVMC_PREDICTION_FIELD;
                st->field_select[0][0] = st->picture_structure - 1;
            }

            if (flags & MBF_QUANT)
                st->qscale = qscale_from_code((int32_t)br_get(br, 5), st->q_scale_type);

            st->last_mv[0][0][0] = st->last_mv[0][0][1] = 0;
            st->last_mv[0][1][0] = st->last_mv[0][1][1] = 0;
            st->mv[0][0][0] = st->mv[0][0][1] = 0;
            st->mv_dir = 1;

            mb->macroblock_type = XVMC_MB_TYPE_MOTION_FORWARD;
            mb->PMV[0][0][0] = mb->PMV[0][0][1] = 0;
            mb->PMV[1][0][0] = mb->PMV[1][0][1] = 0;
            mb->motion_vertical_field_select = 0;
            if (!frame_pic)
                mb->motion_vertical_field_select |= (unsigned char)st->field_select[0][0];
        } else {
            st->mv_dir = ((flags & MBF_FWD) ? 1 : 0) | ((flags & MBF_BWD) ? 2 : 0);

            if (frame_pic && st->frame_pred_frame_dct) {
                motion_type = MT_FRAME;
            } else {
                motion_type = (int)br_get(br, 2);
                if (frame_pic && (flags & MBF_PAT))
                    mb->dct_type = br_get1(br) ? XVMC_DCT_TYPE_FIELD : XVMC_DCT_TYPE_FRAME;
            }

            if (flags & MBF_QUANT)
                st->qscale = qscale_from_code((int32_t)br_get(br, 5), st->q_scale_type);

            mb->macroblock_type = 0;
            if (st->mv_dir & 1)
                mb->macroblock_type |= XVMC_MB_TYPE_MOTION_FORWARD;
            if (st->mv_dir & 2)
                mb->macroblock_type |= XVMC_MB_TYPE_MOTION_BACKWARD;

            switch (motion_type) {
            case MT_FRAME: /* == MT_16X8 numerically; frame_pic decides which */
                if (frame_pic) {
                    st->mv_type = MV_TYPE_16X16;
                    mb->motion_type = XVMC_PREDICTION_FRAME;
                    for (int d = 0; d < 2; d++) {
                        if (!((d == 0 && (st->mv_dir & 1)) || (d == 1 && (st->mv_dir & 2))))
                            continue;
                        int h = decode_motion(br, st->fcode[d][0], st->last_mv[d][0][0], &ok);
                        int v = decode_motion(br, st->fcode[d][1], st->last_mv[d][0][1], &ok);
                        st->last_mv[d][0][0] = st->last_mv[d][1][0] = h;
                        st->last_mv[d][0][1] = st->last_mv[d][1][1] = v;
                        st->mv[d][0][0] = h;
                        st->mv[d][0][1] = v;
                    }
                } else {
                    st->mv_type = MV_TYPE_16X8;
                    mb->motion_type = XVMC_PREDICTION_16x8;
                    for (int d = 0; d < 2; d++) {
                        if (!((d == 0 && (st->mv_dir & 1)) || (d == 1 && (st->mv_dir & 2))))
                            continue;
                        for (int f = 0; f < 2; f++) {
                            st->field_select[d][f] = br_get1(br);
                            int h = decode_motion(br, st->fcode[d][0], st->last_mv[d][f][0], &ok);
                            int v = decode_motion(br, st->fcode[d][1], st->last_mv[d][f][1], &ok);
                            st->last_mv[d][f][0] = h;
                            st->last_mv[d][f][1] = v;
                            st->mv[d][f][0] = h;
                            st->mv[d][f][1] = v;
                        }
                    }
                }
                break;
            case MT_FIELD:
                st->mv_type = MV_TYPE_FIELD;
                mb->motion_type = XVMC_PREDICTION_FIELD;
                if (frame_pic) {
                    for (int d = 0; d < 2; d++) {
                        if (!((d == 0 && (st->mv_dir & 1)) || (d == 1 && (st->mv_dir & 2))))
                            continue;
                        for (int f = 0; f < 2; f++) {
                            st->field_select[d][f] = br_get1(br);
                            int h = decode_motion(br, st->fcode[d][0], st->last_mv[d][f][0], &ok);
                            st->last_mv[d][f][0] = h;
                            st->mv[d][f][0] = h;
                            int v = decode_motion(br, st->fcode[d][1], st->last_mv[d][f][1] >> 1, &ok);
                            st->last_mv[d][f][1] = 2 * v;
                            st->mv[d][f][1] = v;
                        }
                    }
                } else {
                    for (int d = 0; d < 2; d++) {
                        if (!((d == 0 && (st->mv_dir & 1)) || (d == 1 && (st->mv_dir & 2))))
                            continue;
                        st->field_select[d][0] = br_get1(br);
                        int h = decode_motion(br, st->fcode[d][0], st->last_mv[d][0][0], &ok);
                        int v = decode_motion(br, st->fcode[d][1], st->last_mv[d][0][1], &ok);
                        st->last_mv[d][0][0] = st->last_mv[d][1][0] = h;
                        st->last_mv[d][0][1] = st->last_mv[d][1][1] = v;
                        st->mv[d][0][0] = h;
                        st->mv[d][0][1] = v;
                    }
                }
                break;
            case MT_DMV:
                st->mv_type = MV_TYPE_DMV;
                mb->motion_type = XVMC_PREDICTION_DUAL_PRIME;
                for (int d = 0; d < 2; d++) {
                    if (!((d == 0 && (st->mv_dir & 1)) || (d == 1 && (st->mv_dir & 2))))
                        continue;
                    int my_shift = frame_pic ? 1 : 0;
                    int mx = decode_motion(br, st->fcode[d][0], st->last_mv[d][0][0], &ok);
                    st->last_mv[d][0][0] = st->last_mv[d][1][0] = mx;
                    int dmx = br_get1(br) ? (1 - ((int)br_get1(br) << 1)) : 0;
                    int my = decode_motion(br, st->fcode[d][1], st->last_mv[d][0][1] >> my_shift, &ok);
                    int dmy = br_get1(br) ? (1 - ((int)br_get1(br) << 1)) : 0;
                    st->last_mv[d][0][1] = my * (1 << my_shift);
                    st->last_mv[d][1][1] = my * (1 << my_shift);
                    st->mv[d][0][0] = mx;
                    st->mv[d][0][1] = my;
                    if (frame_pic) {
                        int m = st->top_field_first ? 1 : 3;
                        st->mv[d][2][0] = ((mx * m + (mx > 0)) >> 1) + dmx;
                        st->mv[d][2][1] = ((my * m + (my > 0)) >> 1) + dmy - 1;
                        m = 4 - m;
                        st->mv[d][3][0] = ((mx * m + (mx > 0)) >> 1) + dmx;
                        st->mv[d][3][1] = ((my * m + (my > 0)) >> 1) + dmy + 1;
                    } else {
                        st->mv[d][2][0] = ((mx + (mx > 0)) >> 1) + dmx;
                        st->mv[d][2][1] = ((my + (my > 0)) >> 1) + dmy;
                        if (st->picture_structure == XVMC_TOP_FIELD)
                            st->mv[d][2][1]--;
                        else
                            st->mv[d][2][1]++;
                    }
                }
                break;
            default:
                return -1;
            }

            /* PMV hand-off per FFmpeg's ff_xvmc_decode_mb (mpegvideo_xvmc.c). */
            if (st->mv_dir & 1) {
                mb->PMV[0][0][0] = (short)st->mv[0][0][0];
                mb->PMV[0][0][1] = (short)st->mv[0][0][1];
                mb->PMV[1][0][0] = (short)st->mv[0][1][0];
                mb->PMV[1][0][1] = (short)st->mv[0][1][1];
            }
            if (st->mv_dir & 2) {
                mb->PMV[0][1][0] = (short)st->mv[1][0][0];
                mb->PMV[0][1][1] = (short)st->mv[1][0][1];
                mb->PMV[1][1][0] = (short)st->mv[1][1][0];
                mb->PMV[1][1][1] = (short)st->mv[1][1][1];
            }
            if (st->mv_type == MV_TYPE_FIELD && frame_pic) {
                mb->PMV[0][0][1] = (short)(mb->PMV[0][0][1] << 1);
                mb->PMV[1][0][1] = (short)(mb->PMV[1][0][1] << 1);
                mb->PMV[0][1][1] = (short)(mb->PMV[0][1][1] << 1);
                mb->PMV[1][1][1] = (short)(mb->PMV[1][1][1] << 1);
            }
            if (st->mv_type == MV_TYPE_DMV) {
                if (frame_pic) {
                    mb->PMV[0][0][0] = (short)st->mv[0][0][0];
                    mb->PMV[0][0][1] = (short)(st->mv[0][0][1] << 1);
                    mb->PMV[0][1][0] = (short)st->mv[0][0][0];
                    mb->PMV[0][1][1] = (short)(st->mv[0][0][1] << 1);
                    mb->PMV[1][0][0] = (short)st->mv[0][2][0];
                    mb->PMV[1][0][1] = (short)(st->mv[0][2][1] << 1);
                    mb->PMV[1][1][0] = (short)st->mv[0][3][0];
                    mb->PMV[1][1][1] = (short)(st->mv[0][3][1] << 1);
                } else {
                    mb->PMV[0][1][0] = (short)st->mv[0][2][0];
                    mb->PMV[0][1][1] = (short)st->mv[0][2][1];
                }
            }
            mb->motion_vertical_field_select = 0;
            if (st->mv_type == MV_TYPE_FIELD || st->mv_type == MV_TYPE_16X8) {
                mb->motion_vertical_field_select |= (unsigned char)st->field_select[0][0];
                mb->motion_vertical_field_select |= (unsigned char)(st->field_select[1][0] << 1);
                mb->motion_vertical_field_select |= (unsigned char)(st->field_select[0][1] << 2);
                mb->motion_vertical_field_select |= (unsigned char)(st->field_select[1][1] << 3);
            }
        }

        if (!ok)
            return -1;

        st->last_dc[0] = st->last_dc[1] = st->last_dc[2] = 128 << st->intra_dc_precision;
        clear_mb_blocks(blocks, block_index);

        int block_last_index[6] = { -1, -1, -1, -1, -1, -1 };
        if (flags & MBF_PAT) {
            int cbp_idx = vlc_match(br, mbpat_vlc, 64);
            if (cbp_idx < 0)
                return -1;
            /* The real i915 MOCOMP hardware packs coded blocks
             * contiguously starting at this macroblock's base offset --
             * confirmed against the real xf86-video-intel source
             * (i915_xvmc.c: bspm = mb_bytes_420[cbp]; memcpy(corrdata,
             * block_ptr, bspm)), where mb_bytes_420[cbp] is simply
             * popcount(cbp)*128 bytes, i.e. exactly the coded blocks'
             * worth of data with NO gaps for skipped ones. Storing each
             * block at its fixed spec-order slot n (as intra macroblocks
             * correctly do, since intra always codes all 6) silently
             * shifts every block after the first skip into the wrong
             * hardware slot whenever coded_block_pattern is partial --
             * the overwhelmingly common case for real P-frame content,
             * unlike this driver's one long-standing local-path ground
             * truth test slice, which happens to always code all 6
             * blocks and so never exposed this. */
            int packed_slot = 0;
            for (int n = 0; n < 6; n++) {
                if ((cbp_idx >> (5 - n)) & 1) {
                    int comp = n < 4 ? 0 : 1;
                    int last = decode_block_non_intra(br, st, comp, block);
                    if (last < -1)
                        return -1;
                    block_last_index[n] = last;
                    /* MOCOMP hardware adds this residual to its own
                     * motion-compensated prediction -- hand off the IDCT'd
                     * residual (spatial domain), not the addition result
                     * (this driver has no access to the prediction). */
                    idct_block_residual_clamp(block);
                    store_block(blocks, block_index, packed_slot, block);
                    packed_slot++;
                }
            }
        }

        unsigned cbp = 0;
        for (int n = 0; n < 6; n++) {
            cbp <<= 1;
            if (block_last_index[n] >= 0)
                cbp |= 1;
        }
        mb->coded_block_pattern = (unsigned short)cbp;
        if (cbp == 0)
            mb->macroblock_type &= (unsigned char)~XVMC_MB_TYPE_PATTERN;
        else
            mb->macroblock_type |= XVMC_MB_TYPE_PATTERN;
    }

    return 0;
}

/* ---------------------------------------------------------------------- */
/* Top-level entry point (signature fixed by mpeg2_vld.h).                */
/* ---------------------------------------------------------------------- */

int mpeg2_vld_decode_slice(
    const uint8_t *slice_data, uint32_t slice_data_size, uint32_t macroblock_bit_offset,
    uint32_t slice_horizontal_position, uint32_t slice_vertical_position,
    int32_t quantiser_scale_code, int32_t intra_slice_flag,
    const VAPictureParameterBufferMPEG2 *pic_params,
    const VAIQMatrixBufferMPEG2 *iq_matrix,
    XvMCMacroBlockArray *mb_array, unsigned int mb_array_offset,
    XvMCBlockArray *blocks, unsigned int *block_index_cursor,
    int intra_unsigned)
{
    /* intra_slice_flag reflects a bit already consumed by whatever parsed
     * the slice header and computed macroblock_bit_offset for us -- there
     * is nothing left for us to do with it here. */
    (void)intra_slice_flag;

    if (!slice_data || !pic_params || !mb_array || !blocks || !block_index_cursor)
        return -1;

    struct mb_state st;
    memset(&st, 0, sizeof(st));
    st.intra_unsigned = intra_unsigned;

    setup_matrices(&st, iq_matrix);
    st.scantable = pic_params->picture_coding_extension.bits.alternate_scan
                       ? alternate_vertical_scan : zigzag_direct;
    st.q_scale_type = pic_params->picture_coding_extension.bits.q_scale_type;
    st.intra_dc_precision = pic_params->picture_coding_extension.bits.intra_dc_precision;
    st.intra_vlc_format = pic_params->picture_coding_extension.bits.intra_vlc_format;
    st.frame_pred_frame_dct = pic_params->picture_coding_extension.bits.frame_pred_frame_dct;
    st.concealment_motion_vectors = pic_params->picture_coding_extension.bits.concealment_motion_vectors;
    st.picture_structure = (int)pic_params->picture_coding_extension.bits.picture_structure;
    st.picture_coding_type = pic_params->picture_coding_type;
    st.top_field_first = pic_params->picture_coding_extension.bits.top_field_first;
    if (getenv("MPEG2_VLD_DEBUG_PICFLAGS")) {
        static int n = 0;
        if (st.picture_coding_type != PIC_I && n++ < 3)
            fprintf(stderr, "DBG picflags: type=%d concealment=%d intra_vlc_format=%d q_scale_type=%d "
                            "alternate_scan=%d frame_pred_frame_dct=%d picture_structure=%d intra_dc_precision=%d\n",
                    st.picture_coding_type, st.concealment_motion_vectors, st.intra_vlc_format,
                    st.q_scale_type, pic_params->picture_coding_extension.bits.alternate_scan,
                    st.frame_pred_frame_dct, st.picture_structure, st.intra_dc_precision);
    }

    int32_t fc = pic_params->f_code;
    st.fcode[0][0] = (fc >> 12) & 0xf; if (!st.fcode[0][0]) st.fcode[0][0] = 1;
    st.fcode[0][1] = (fc >> 8) & 0xf;  if (!st.fcode[0][1]) st.fcode[0][1] = 1;
    st.fcode[1][0] = (fc >> 4) & 0xf;  if (!st.fcode[1][0]) st.fcode[1][0] = 1;
    st.fcode[1][1] = fc & 0xf;         if (!st.fcode[1][1]) st.fcode[1][1] = 1;

    st.qscale = qscale_from_code(quantiser_scale_code, st.q_scale_type);
    if (st.qscale == 0)
        return -1;

    st.last_dc[0] = st.last_dc[1] = st.last_dc[2] = 128 << st.intra_dc_precision;

    unsigned int mb_width = ((unsigned int)pic_params->horizontal_size + 15) / 16;
    unsigned int mb_height = ((unsigned int)pic_params->vertical_size + 15) / 16;
    if (mb_width == 0)
        return -1;

    struct bitreader br;
    br_init(&br, slice_data, slice_data_size, macroblock_bit_offset);

    /* Initial macroblock_address_increment: sets this slice's first
     * macroblock's column as an offset from slice_horizontal_position
     * (see the mbincr_vlc comment above). */
    unsigned int mb_x = slice_horizontal_position;
    unsigned int mb_y = slice_vertical_position;
    {
        int got_final = 0;
        for (;;) {
            int idx = vlc_match(&br, mbincr_vlc, 36);
            if (idx < 0)
                return -1; /* no macroblocks at all -- real error */
            if (idx == MBINCR_ESCAPE) {
                mb_x += 33;
                continue;
            }
            if (idx == MBINCR_STUFFING)
                continue;
            if (idx == MBINCR_END)
                return 0; /* empty slice */
            mb_x += (unsigned int)idx;
            got_final = 1;
            break;
        }
        if (!got_final)
            return -1;
    }
    if (mb_x >= mb_width)
        return -1;

    int dbg = 0;
    if (getenv("MPEG2_VLD_DEBUG_ROW22")) {
        static int row22_calls = 0;
        if (slice_vertical_position == 22) {
            row22_calls++;
            if (row22_calls == 2)
                dbg = 1;
        }
    }

    unsigned int decoded = 0;
    unsigned int block_index = *block_index_cursor;
    unsigned int mb_index = mb_array_offset;
    int first = 1;

    for (;;) {
        if (!first) {
            /* Address-increment / skip-run before the next coded MB. */
            unsigned int skip = 0;
            int stop = 0;
            for (;;) {
                if (br_bits_left(&br) <= 0) {
                    stop = 1;
                    break;
                }
                int idx = vlc_match(&br, mbincr_vlc, 36);
                if (idx < 0) {
                    /* Likely trailing byte-alignment padding at slice end. */
                    stop = 1;
                    break;
                }
                if (idx == MBINCR_ESCAPE) {
                    skip += 33;
                    continue;
                }
                if (idx == MBINCR_STUFFING)
                    continue;
                if (idx == MBINCR_END) {
                    stop = 1;
                    break;
                }
                skip += (unsigned int)idx;
                break;
            }
            if (stop) {
                if (dbg) fprintf(stderr, "DBG row22: stop at mb_x=%u mb_y=%u bits_left=%d\n", mb_x, mb_y, br_bits_left(&br));
                break;
            }
            if (dbg && skip != 0)
                fprintf(stderr, "DBG row22: skip=%u before mb_x=%u mb_y=%u\n", skip, mb_x, mb_y);

            if (st.picture_coding_type == PIC_I && skip > 0) {
                /* Skipped MBs are illegal in I-pictures. */
                break;
            }

            for (unsigned int s = 0; s < skip; s++) {
                if (mb_x >= mb_width) {
                    mb_x = 0;
                    mb_y++;
                }
                if (mb_y >= mb_height ||
                    mb_index >= mb_array->num_blocks ||
                    (size_t)(block_index + 6) * 64 > (size_t)blocks->num_blocks * 64)
                    goto done;
                emit_skip_mb(&st, mb_array, mb_index, blocks, block_index, mb_x, mb_y);
                mb_index++;
                block_index += 6;
                decoded++;
                mb_x++;
            }
        }
        first = 0;

        if (br_bits_left(&br) <= 0)
            break;
        if (mb_y >= mb_height)
            break;
        if (mb_index >= mb_array->num_blocks ||
            (size_t)(block_index + 6) * 64 > (size_t)blocks->num_blocks * 64)
            break;

        int bits_before_mb = br_bits_left(&br);
        int rc = decode_coded_mb(&br, &st, mb_array, mb_index, blocks, block_index, mb_x, mb_y);
        if (dbg) {
            XvMCMacroBlock *dbgmb = &mb_array->macro_blocks[mb_index];
            fprintf(stderr, "DBG row22: coded mb_x=%u mb_y=%u rc=%d bits_before=%d bits_after=%d consumed=%d "
                            "mbtype=0x%02x motion_type=%u cbp=0x%02x PMV=[%d,%d]\n",
                    mb_x, mb_y, rc, bits_before_mb, br_bits_left(&br), bits_before_mb - br_bits_left(&br),
                    dbgmb->macroblock_type, dbgmb->motion_type, dbgmb->coded_block_pattern,
                    dbgmb->PMV[0][0][0], dbgmb->PMV[0][0][1]);
        }
        if (rc < 0)
            break;

        mb_index++;
        block_index += 6;
        decoded++;
        mb_x++;
        if (mb_x >= mb_width) {
            mb_x = 0;
            mb_y++;
        }

        /* Clean end-of-slice: only trailing zero stuffing remains. */
        if (br_bits_left(&br) < 8) {
            int left = br_bits_left(&br);
            if (left <= 0 || br_peek(&br, left) == 0)
                break;
        }
    }
done:

    *block_index_cursor = block_index;

    if (decoded == 0) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "mpeg2_vld_decode_slice: decoded 0 macroblocks this slice "
                            "(bitstream error on the very first macroblock)\n");
            warned = 1;
        }
        return -1;
    }
    return (int)decoded;
}
