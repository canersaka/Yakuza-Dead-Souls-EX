/*
 * ps3recomp - host movie decoder (FFmpeg). See movie_ffmpeg.h.
 *
 * When built without HAVE_FFMPEG, every entry point is an inert stub so the
 * runtime still links (movie_ffmpeg_available() returns 0).
 */
#include "movie_ffmpeg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HAVE_FFMPEG

int movie_ffmpeg_available(void) { return 0; }
MoviePlayer*   movie_open(const char* p) { (void)p; return 0; }
int            movie_width(MoviePlayer* m) { (void)m; return 0; }
int            movie_height(MoviePlayer* m) { (void)m; return 0; }
double         movie_framerate(MoviePlayer* m) { (void)m; return 0.0; }
const uint8_t* movie_next_rgba(MoviePlayer* m, double* p) { (void)m; (void)p; return 0; }
void           movie_close(MoviePlayer* m) { (void)m; }

#else /* HAVE_FFMPEG */

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>

struct MoviePlayer {
    AVFormatContext*   fmt;
    AVCodecContext*    dec;
    struct SwsContext* sws;
    AVFrame*           frame;
    AVPacket*          pkt;
    int                vstream;
    int                w, h;
    double             fps;
    uint8_t*           rgba;      /* w*h*4 output buffer */
};

int movie_ffmpeg_available(void) { return 1; }

MoviePlayer* movie_open(const char* path)
{
    MoviePlayer* m = (MoviePlayer*)calloc(1, sizeof(*m));
    if (!m) return 0;
    if (avformat_open_input(&m->fmt, path, NULL, NULL) < 0) { free(m); return 0; }
    /* Sofdec pads the head heavily; probe deep enough to find the video codec. */
    m->fmt->probesize = 50LL * 1024 * 1024;
    m->fmt->max_analyze_duration = 20LL * 1000000;
    avformat_find_stream_info(m->fmt, NULL);

    m->vstream = -1;
    for (unsigned i = 0; i < m->fmt->nb_streams; i++)
        if (m->fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { m->vstream = (int)i; break; }
    if (m->vstream < 0) { movie_close(m); return 0; }

    AVCodecParameters* par = m->fmt->streams[m->vstream]->codecpar;
    /* Sofdec video is MPEG-1; RPCS3's minimal FFmpeg build ships mpeg2video
     * (which decodes MPEG-1) but not mpeg1video -- fall back to it. */
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO);
    if (!codec) codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
    if (!codec) { movie_close(m); return 0; }
    m->dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m->dec, par);
    m->dec->codec_id = codec->id;                       /* mpeg2video on the mpeg1 stream */
    if (avcodec_open2(m->dec, codec, NULL) < 0) { movie_close(m); return 0; }

    m->w = par->width; m->h = par->height;
    AVRational r = av_guess_frame_rate(m->fmt, m->fmt->streams[m->vstream], NULL);
    m->fps = r.den ? (double)r.num / (double)r.den : 30.0;
    m->frame = av_frame_alloc();
    m->pkt   = av_packet_alloc();
    m->rgba  = (uint8_t*)malloc((size_t)m->w * m->h * 4);
    if (!m->frame || !m->pkt || !m->rgba) { movie_close(m); return 0; }
    return m;
}

int    movie_width(MoviePlayer* m)     { return m ? m->w : 0; }
int    movie_height(MoviePlayer* m)    { return m ? m->h : 0; }
double movie_framerate(MoviePlayer* m) { return m ? m->fps : 0.0; }

const uint8_t* movie_next_rgba(MoviePlayer* m, double* pts_out)
{
    if (!m) return 0;
    for (;;) {
        /* drain any already-decoded frames first */
        int rr = avcodec_receive_frame(m->dec, m->frame);
        if (rr == 0) {
            if (!m->sws)
                m->sws = sws_getContext(m->w, m->h, m->dec->pix_fmt, m->w, m->h,
                                        AV_PIX_FMT_RGBA, SWS_BILINEAR, 0, 0, 0);
            uint8_t* dst[1] = { m->rgba };
            int stride[1] = { m->w * 4 };
            sws_scale(m->sws, (const uint8_t* const*)m->frame->data, m->frame->linesize,
                      0, m->h, dst, stride);
            if (pts_out) {
                AVRational tb = m->fmt->streams[m->vstream]->time_base;
                int64_t pts = m->frame->best_effort_timestamp;
                *pts_out = (pts == AV_NOPTS_VALUE) ? 0.0 : pts * av_q2d(tb);
            }
            return m->rgba;
        }
        /* need more input: read packets until one feeds the video decoder */
        int got = 0;
        while (av_read_frame(m->fmt, m->pkt) >= 0) {
            if (m->pkt->stream_index == m->vstream) {
                avcodec_send_packet(m->dec, m->pkt);
                av_packet_unref(m->pkt);
                got = 1;
                break;
            }
            av_packet_unref(m->pkt);
        }
        if (!got) {
            avcodec_send_packet(m->dec, NULL);           /* flush */
            if (avcodec_receive_frame(m->dec, m->frame) != 0) return 0;   /* EOF */
            /* fallthrough handled next loop iteration would re-receive; convert here */
            if (!m->sws)
                m->sws = sws_getContext(m->w, m->h, m->dec->pix_fmt, m->w, m->h,
                                        AV_PIX_FMT_RGBA, SWS_BILINEAR, 0, 0, 0);
            uint8_t* dst[1] = { m->rgba };
            int stride[1] = { m->w * 4 };
            sws_scale(m->sws, (const uint8_t* const*)m->frame->data, m->frame->linesize,
                      0, m->h, dst, stride);
            if (pts_out) *pts_out = 0.0;
            return m->rgba;
        }
    }
}

void movie_close(MoviePlayer* m)
{
    if (!m) return;
    if (m->sws)   sws_freeContext(m->sws);
    if (m->frame) av_frame_free(&m->frame);
    if (m->pkt)   av_packet_free(&m->pkt);
    if (m->dec)   avcodec_free_context(&m->dec);
    if (m->fmt)   avformat_close_input(&m->fmt);
    if (m->rgba)  free(m->rgba);
    free(m);
}

#endif /* HAVE_FFMPEG */
