/*
 * ps3recomp - clean-room CRI ADX decoder implementation.
 * See adx_decode.h for provenance/scope notes.
 *
 * ADX STREAM LAYOUT (public format, "type 2" fixed-coefficient ADPCM):
 *
 *   offset 0x00  u16 magic              0x8000
 *   offset 0x02  u16 copyright_offset   byte offset of the "(c)CRI" footer;
 *                                       data starts at copyright_offset + 4
 *   offset 0x04  u8  encoding_type      2 = fixed coefficients (this game),
 *                                       3 = variable coefficients/per-block
 *   offset 0x05  u8  block_size         bytes per channel per block, header
 *                                       (2 bytes) + ADPCM nibbles included
 *   offset 0x06  u8  sample_bitdepth    nibble width, always 4 for CRI ADX
 *   offset 0x07  u8  channel_count
 *   offset 0x08  u32 sample_rate
 *   offset 0x0C  u32 total_samples
 *   offset 0x10  u16 highpass_hz        cutoff freq used to derive the two
 *                                       fixed prediction coefficients
 *   offset 0x12  u8  version            3 = has an optional loop-info block
 *   offset 0x13  u8  flags
 *   [optional loop-info block, version 3/4, 24 bytes -- enable flag +
 *    loop start/end sample + loop start/end byte offset, all big-endian]
 *   ...
 *   copyright_offset+0   "(c)CRI"-class footer (ignored)
 *   copyright_offset+4   first ADPCM block (data start)
 *
 * BLOCK LAYOUT (per channel, block_size bytes):
 *   u16 be scale                (the block's quantization scale)
 *   (block_size-2) bytes of packed 4-bit signed nibbles, MSB nibble first,
 *   (block_size-2)*2 samples per block per channel (typically 32 for the
 *   standard block_size=18).
 *
 * SAMPLE RECONSTRUCTION (fixed-coefficient / type 2, the standard CRI
 * decode recurrence widely documented for this format):
 *   nibble in [-8,7] (4-bit signed) -> raw = nibble * scale
 *   sample = raw + (coeff1 * hist1 + coeff2 * hist2) / 4096          (Q12)
 *   sample clamped to int16 range; hist2 <- hist1; hist1 <- sample
 *
 * The two Q12 fixed coefficients are derived once from the header's
 * highpass cutoff frequency and the stream sample rate via the standard
 * two-pole highpass-compensation design used by the format:
 *   a = sqrt(2) - cos(2*pi*cutoff/sample_rate)
 *   b = sqrt(2) - 1
 *   c = (a - sqrt((a+b)*(a-b))) / b
 *   coeff1 = c * 2 * 4096      (Q12, rounded)
 *   coeff2 = -c * c * 4096     (Q12, rounded)
 * (End-of-stream is caller's responsibility -- this module only decodes
 * whichever block range the caller has bytes for; total_samples/loop info
 * are exposed so the caller can stop at the right sample.)
 */
#include "adx_decode.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ADX_MAGIC       0x8000u
#define ADX_MAX_CHANNELS 8

struct AdxDecoder {
    int      channels;
    int      sample_rate;
    uint32_t total_samples;
    uint32_t block_size;       /* bytes/block/channel, header included */
    uint32_t data_offset;      /* byte offset of first block */
    int      encoding_type;    /* 2 = fixed coeff (only type this decodes) */
    uint16_t highpass_hz;

    uint32_t loop_start_sample;
    uint32_t loop_end_sample;

    /* Fixed prediction coefficients, Q12, shared by all channels (type 2). */
    int32_t coeff1;
    int32_t coeff2;

    /* Per-channel predictor history (2 samples back). */
    int32_t hist1[ADX_MAX_CHANNELS];
    int32_t hist2[ADX_MAX_CHANNELS];
};

static uint16_t rd_be16(const uint8_t* p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* Derive the fixed Q12 prediction coefficients from the highpass cutoff,
 * per the header (public "type 2" ADX coefficient formula). */
static void adx_compute_coeffs(int sample_rate, uint16_t highpass_hz,
                                int32_t* coeff1, int32_t* coeff2)
{
    if (sample_rate <= 0 || highpass_hz == 0) {
        /* No filtering info: degenerate to a plain differential predictor
         * (coeff1=2,coeff2=-1 in Q12*would* be pass-through-ish; safer to
         * fall back to "no prediction" so a malformed header doesn't diverge). */
        *coeff1 = 0;
        *coeff2 = 0;
        return;
    }

    double a = sqrt(2.0) - cos(2.0 * M_PI * (double)highpass_hz / (double)sample_rate);
    double b = sqrt(2.0) - 1.0;
    double c = (a - sqrt((a + b) * (a - b))) / b;

    double c1 = c * 2.0 * 4096.0;
    double c2 = -(c * c) * 4096.0;

    *coeff1 = (int32_t)lround(c1);
    *coeff2 = (int32_t)lround(c2);
}

AdxDecoder* adx_open(const uint8_t* data, size_t len)
{
    if (!data || len < 20) return NULL;
    if (rd_be16(data + 0x00) != ADX_MAGIC) return NULL;

    uint16_t copyright_offset = rd_be16(data + 0x02);
    uint8_t  encoding_type    = data[0x04];
    uint8_t  block_size       = data[0x05];
    uint8_t  bitdepth         = data[0x06];
    uint8_t  channels         = data[0x07];

    if (channels == 0 || channels > ADX_MAX_CHANNELS) return NULL;
    if (block_size < 3) return NULL;          /* need >=1 byte of nibbles */
    if (bitdepth != 4) return NULL;           /* only 4-bit ADX supported */
    if (encoding_type != 2) return NULL;      /* only fixed-coeff (type 2) */

    uint32_t data_offset = (uint32_t)copyright_offset + 4u;
    if (len < 0x14 || data_offset < 0x14) return NULL;

    AdxDecoder* d = (AdxDecoder*)calloc(1, sizeof(AdxDecoder));
    if (!d) return NULL;

    d->channels      = channels;
    d->sample_rate   = (int)rd_be32(data + 0x08);
    d->total_samples = rd_be32(data + 0x0C);
    d->highpass_hz   = rd_be16(data + 0x10);
    d->encoding_type = encoding_type;
    d->block_size    = block_size;
    d->data_offset   = data_offset;

    uint8_t version = (len > 0x12) ? data[0x12] : 0;

    /* Optional loop-info block: version 3/4 streams carry a 24-byte loop
     * descriptor right after the fixed 0x14-byte header (enable flag +
     * loop-start sample + loop-start byte + loop-end sample + loop-end
     * byte, all big-endian u32, per the public documented layout). Only
     * consulted if present and in-range; absence just means no loop. */
    if (version >= 3 && len >= 0x14 + 24) {
        uint32_t enabled    = rd_be32(data + 0x14 + 0);
        uint32_t loop_start = rd_be32(data + 0x14 + 4);
        uint32_t loop_end   = rd_be32(data + 0x14 + 16);
        if (enabled) {
            d->loop_start_sample = loop_start;
            d->loop_end_sample   = loop_end;
        }
    }

    adx_compute_coeffs(d->sample_rate, d->highpass_hz, &d->coeff1, &d->coeff2);

    return d;
}

int      adx_channels(const AdxDecoder* d)          { return d ? d->channels : 0; }
int      adx_sample_rate(const AdxDecoder* d)       { return d ? d->sample_rate : 0; }
uint32_t adx_total_samples(const AdxDecoder* d)     { return d ? d->total_samples : 0; }
uint32_t adx_block_size(const AdxDecoder* d)        { return d ? d->block_size : 0; }
uint32_t adx_data_offset(const AdxDecoder* d)       { return d ? d->data_offset : 0; }
uint32_t adx_loop_start_sample(const AdxDecoder* d) { return d ? d->loop_start_sample : 0; }
uint32_t adx_loop_end_sample(const AdxDecoder* d)   { return d ? d->loop_end_sample : 0; }

void adx_reset_history(AdxDecoder* d)
{
    if (!d) return;
    memset(d->hist1, 0, sizeof(d->hist1));
    memset(d->hist2, 0, sizeof(d->hist2));
}

static int16_t adx_clamp16(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Sign-extend a 4-bit nibble to a signed int. */
static int32_t adx_sext4(uint8_t nib)
{
    return (nib & 0x8) ? (int32_t)nib - 16 : (int32_t)nib;
}

int adx_decode_block(AdxDecoder* d, const uint8_t* stream_base, size_t stream_len,
                      uint32_t stream_off, int16_t* pcm_out, int out_capacity_frames)
{
    if (!d || !stream_base || !pcm_out) return -1;
    if (stream_off < d->data_offset) return -1;
    if (((stream_off - d->data_offset) % (d->block_size * (uint32_t)d->channels)) != 0)
        return -1; /* not block-aligned */

    uint32_t block_bytes_total = d->block_size * (uint32_t)d->channels;
    if ((uint64_t)stream_off + block_bytes_total > (uint64_t)stream_len)
        return -1; /* truncated -- not enough bytes buffered yet */

    int samples_per_block = (int)(d->block_size - 2) * 2; /* 2 nibbles/byte */
    if (samples_per_block <= 0) return -1;
    if (samples_per_block > out_capacity_frames) samples_per_block = out_capacity_frames;

    for (int ch = 0; ch < d->channels; ch++) {
        const uint8_t* blk = stream_base + stream_off + (uint32_t)ch * d->block_size;
        int16_t scale = (int16_t)rd_be16(blk);
        const uint8_t* nibbles = blk + 2;

        int32_t h1 = d->hist1[ch];
        int32_t h2 = d->hist2[ch];

        for (int s = 0; s < samples_per_block; s++) {
            uint8_t byte = nibbles[s / 2];
            uint8_t nib = (s & 1) ? (byte & 0x0F) : (byte >> 4);
            int32_t raw = adx_sext4(nib) * (int32_t)scale;

            int32_t predicted = (d->coeff1 * h1 + d->coeff2 * h2) >> 12;
            int32_t sample = raw + predicted;
            int16_t out = adx_clamp16(sample);

            pcm_out[(size_t)s * d->channels + ch] = out;

            h2 = h1;
            h1 = (int32_t)out;
        }

        d->hist1[ch] = h1;
        d->hist2[ch] = h2;
    }

    return samples_per_block;
}

void adx_close(AdxDecoder* d)
{
    free(d);
}
