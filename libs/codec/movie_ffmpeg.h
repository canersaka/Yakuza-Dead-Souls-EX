/*
 * ps3recomp - host movie decoder (FFmpeg) for CRI Sofdec .sfd playback.
 *
 * The game's intro/cutscene movies are CRI Sofdec: MPEG-1 video (a CRI
 * non-standard variant) + CRI ADX audio in an MPEG program stream. Our LLE
 * path (cri_mpvps3spurs SPU decoder) stalls, so we HLE the decode host-side
 * (doctrine-permitted: a codec is a pure data transform).
 *
 * FFmpeg's mpeg2video decoder decodes the Sofdec MPEG-1 correctly (proven
 * against hd_sega_logo_us1012.sfd / advertise.sfd). Built against the FFmpeg
 * dev libs when present (YZ_FFMPEG_DIR / rpcs3/3rdparty/ffmpeg); a no-op stub
 * otherwise so the runtime always links.
 */
#ifndef PS3RECOMP_MOVIE_FFMPEG_H
#define PS3RECOMP_MOVIE_FFMPEG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MoviePlayer MoviePlayer;

/* 1 if the runtime was built with FFmpeg movie support, 0 if stubbed. */
int movie_ffmpeg_available(void);

/* Open a Sofdec .sfd for video decode. Returns NULL on failure / no FFmpeg. */
MoviePlayer* movie_open(const char* path);

int    movie_width(MoviePlayer* m);
int    movie_height(MoviePlayer* m);
double movie_framerate(MoviePlayer* m);

/* Host-decoded movie audio. FFmpeg converts the selected ADX stream to
 * interleaved signed 16-bit stereo at 48 kHz during movie_open(). The buffer
 * belongs to MoviePlayer and remains valid until movie_close(). */
int            movie_has_audio(MoviePlayer* m);
const int16_t* movie_audio_s16(MoviePlayer* m);
uint64_t       movie_audio_frames(MoviePlayer* m);
int            movie_audio_sample_rate(MoviePlayer* m);
int            movie_audio_channels(MoviePlayer* m);

/* Decode the next video frame into the player's internal RGBA8 buffer and
 * return it (tightly packed, row pitch = width*4), or NULL at end of stream.
 * The buffer is valid until the next call. *pts_out (seconds) is optional. */
const uint8_t* movie_next_rgba(MoviePlayer* m, double* pts_out);

/* Decode the next video frame and expose its YUV planes directly (no swscale;
 * MPEG decode output is YUV 4:2:0 planar). Plane pointers are valid until the
 * next movie_next_* call. Returns 1 on frame, 0 at end of stream. */
typedef struct MovieYuvFrame {
    const uint8_t* y;   /* w   x h   luma,  stride ystride  */
    const uint8_t* u;   /* w/2 x h/2 chroma, stride cstride */
    const uint8_t* v;
    int ystride, cstride;
    int w, h;
    double pts;         /* seconds; 0 when unknown */
} MovieYuvFrame;
int movie_next_yuv(MoviePlayer* m, MovieYuvFrame* out);

/* Decode the next frame as big-endian A8R8G8B8 bytes (GCM linear ARGB).
 * Returns the internal buffer (pitch = width*4) or NULL at end of stream. */
const uint8_t* movie_next_argb(MoviePlayer* m, double* pts_out);

void movie_close(MoviePlayer* m);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_MOVIE_FFMPEG_H */
