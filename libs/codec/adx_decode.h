/*
 * ps3recomp - clean-room CRI ADX decoder (host-side HLE).
 *
 * ADX is CRIWARE's own fixed-coefficient ADPCM format, documented publicly
 * (format reverse-engineered and described in numerous public write-ups,
 * e.g. the "ADX File Format" community notes and hcs64's public documentation
 * of the codec's header + block layout). This implementation is written from
 * that PUBLIC SPEC only -- no code was read from or copied out of vgmstream,
 * FFmpeg, or any GPL/proprietary source (codec data transforms may be
 * HLE'd; clean-room only, RPCS3/vgmstream/FFmpeg are read-only oracles for
 * semantics, never a copy source).
 *
 * Scope: standard (type 2/3) ADX, 4-bit ADPCM, mono or multi-channel,
 * fixed prediction coefficients derived from the header's highpass cutoff
 * frequency (the "type 2" scheme used by nearly all CRI titles including
 * this one's streamed voice/movie audio). AXD (type 4, keycode-encrypted)
 * is NOT implemented -- Yakuza's voice/movie ADX is unencrypted type-2/3.
 */
#ifndef PS3RECOMP_ADX_DECODE_H
#define PS3RECOMP_ADX_DECODE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AdxDecoder AdxDecoder;

/* Parse an ADX header from a raw byte buffer (as read from the .cvm stream /
 * ring buffer, big-endian on disk -- the parser byteswaps internally).
 * Returns a new decoder on success, NULL if `data` doesn't start with the
 * ADX 0x8000 magic or uses an unsupported encoding. `len` must cover at
 * least the header (copyright_offset + 4 bytes); it does not need to cover
 * the whole stream -- adx_decode_push() is fed incrementally.
 */
AdxDecoder* adx_open(const uint8_t* data, size_t len);

/* Decoded stream properties (valid once adx_open succeeds). */
int      adx_channels(const AdxDecoder* d);
int      adx_sample_rate(const AdxDecoder* d);
uint32_t adx_total_samples(const AdxDecoder* d);   /* 0 if unknown */
uint32_t adx_block_size(const AdxDecoder* d);      /* bytes/channel/block, incl. 2-byte block header */
uint32_t adx_data_offset(const AdxDecoder* d);     /* byte offset of first ADPCM block (== copyright_offset + 4) */
uint32_t adx_loop_start_sample(const AdxDecoder* d); /* 0 if no loop */
uint32_t adx_loop_end_sample(const AdxDecoder* d);   /* 0 if no loop */

/* Decode exactly one frame (one block per channel) starting at byte offset
 * `stream_off` into the ORIGINAL stream (i.e. relative to the same base
 * `data` passed to adx_open -- typically the game's ADX ring buffer/.cvm
 * byte offset). `stream_off` must be block-aligned (a multiple of
 * adx_block_size()) and >= adx_data_offset(). Writes up to
 * `out_capacity_frames` interleaved PCM16 samples (host-native
 * endianness; caller re-byteswaps for guest BE writes) starting at
 * `pcm_out[0]`, one sample per channel per frame (interleaved: LRLRLR...).
 * Returns the number of PCM sample-frames decoded (== samples-per-block,
 * usually 32), or -1 on error (bad offset / truncated block / EOF marker).
 * `block` must point at a whole-block's worth of bytes
 * (adx_block_size() * adx_channels()) available at data+stream_off.
 */
int adx_decode_block(AdxDecoder* d, const uint8_t* stream_base, size_t stream_len,
                      uint32_t stream_off, int16_t* pcm_out, int out_capacity_frames);

/* Reset per-channel predictor history (e.g. on loop-back or seek). */
void adx_reset_history(AdxDecoder* d);

void adx_close(AdxDecoder* d);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_ADX_DECODE_H */
