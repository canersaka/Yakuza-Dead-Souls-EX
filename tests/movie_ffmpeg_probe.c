#include "../libs/codec/movie_ffmpeg.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 4) {
        fprintf(stderr,
                "usage: movie_ffmpeg_probe <movie.sfd> "
                "[dump-frame dump.ppm]\n");
        return 2;
    }
    const int dump_frame = argc == 4 ? atoi(argv[2]) : -1;
    MoviePlayer* movie = movie_open(argv[1]);
    if (!movie) {
        fprintf(stderr, "movie_open failed: %s\n", argv[1]);
        return 1;
    }

    printf("video=%dx%d fps=%.6f\n",
           movie_width(movie), movie_height(movie), movie_framerate(movie));
    printf("audio=%d channels=%d rate=%d frames=%llu duration=%.6f\n",
           movie_has_audio(movie), movie_audio_channels(movie),
           movie_audio_sample_rate(movie),
           (unsigned long long)movie_audio_frames(movie),
           movie_audio_sample_rate(movie)
               ? (double)movie_audio_frames(movie) /
                     movie_audio_sample_rate(movie)
               : 0.0);

    int decoded = 0;
    double pts = 0.0;
    double max_pts = 0.0;
    const unsigned char* rgba;
    while ((rgba = movie_next_rgba(movie, &pts)) != NULL) {
        if (decoded == dump_frame) {
            FILE* out = fopen(argv[3], "wb");
            if (!out) {
                fprintf(stderr, "could not create %s\n", argv[3]);
                movie_close(movie);
                return 1;
            }
            const int width = movie_width(movie);
            const int height = movie_height(movie);
            fprintf(out, "P6\n%d %d\n255\n", width, height);
            for (int i = 0; i < width * height; ++i)
                fwrite(rgba + i * 4, 1, 3, out);
            fclose(out);
        }
        if (pts > max_pts) max_pts = pts;
        decoded++;
    }
    printf("decoded_video_frames=%d max_pts=%.6f nominal_duration=%.6f\n",
           decoded, max_pts,
           movie_framerate(movie) > 0.0
               ? decoded / movie_framerate(movie)
               : 0.0);

    const int ok = movie_has_audio(movie) &&
                   movie_audio_channels(movie) == 2 &&
                   movie_audio_sample_rate(movie) == 48000 &&
                   movie_audio_frames(movie) > 0 &&
                   decoded > 0;
    movie_close(movie);
    return ok ? 0 : 1;
}
