/*
 * ps3recomp - host movie decoder (FFmpeg) for CRI Sofdec .sfd playback.
 *
 * The game's intro/cutscene movies are CRI Sofdec: MPEG-1 video (a CRI
 * non-standard variant) + CRI ADX audio in an MPEG program stream. Our LLE
 * path (cri_mpvps3spurs SPU decoder) stalls, so we HLE the decode host-side
 * (doctrine-permitted: a codec is a pure data transform -- docs/LESSONS.md #14).
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

/* Decode the next video frame into the player's internal RGBA8 buffer and
 * return it (tightly packed, row pitch = width*4), or NULL at end of stream.
 * The buffer is valid until the next call. *pts_out (seconds) is optional. */
const uint8_t* movie_next_rgba(MoviePlayer* m, double* pts_out);

void movie_close(MoviePlayer* m);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_MOVIE_FFMPEG_H */
