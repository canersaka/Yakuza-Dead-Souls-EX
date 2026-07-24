/*
 * ps3recomp - host movie decoder (FFmpeg). See movie_ffmpeg.h.
 *
 * When built without HAVE_FFMPEG, every entry point is an inert stub so the
 * runtime still links (movie_ffmpeg_available() returns 0).
 */
#include "movie_ffmpeg.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HAVE_FFMPEG

int movie_ffmpeg_available(void) { return 0; }
MoviePlayer*   movie_open(const char* p) { (void)p; return 0; }
int            movie_width(MoviePlayer* m) { (void)m; return 0; }
int            movie_height(MoviePlayer* m) { (void)m; return 0; }
double         movie_framerate(MoviePlayer* m) { (void)m; return 0.0; }
int            movie_has_audio(MoviePlayer* m) { (void)m; return 0; }
const int16_t* movie_audio_s16(MoviePlayer* m) { (void)m; return 0; }
uint64_t       movie_audio_frames(MoviePlayer* m) { (void)m; return 0; }
int            movie_audio_sample_rate(MoviePlayer* m) { (void)m; return 0; }
int            movie_audio_channels(MoviePlayer* m) { (void)m; return 0; }
const uint8_t* movie_next_rgba(MoviePlayer* m, double* p) { (void)m; (void)p; return 0; }
const uint8_t* movie_next_argb(MoviePlayer* m, double* p) { (void)m; (void)p; return 0; }
int            movie_next_yuv(MoviePlayer* m, MovieYuvFrame* o) { (void)m; (void)o; return 0; }
void           movie_close(MoviePlayer* m) { (void)m; }

#else /* HAVE_FFMPEG */

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define MOVIE_AUDIO_RATE     48000
#define MOVIE_AUDIO_CHANNELS 2

struct MoviePlayer {
    AVFormatContext*   fmt;
    AVCodecContext*    dec;
    struct SwsContext* sws;
    struct SwsContext* sws_argb;
    AVFrame*           frame;
    AVPacket*          pkt;
    int                vstream;
    int                w, h;
    double             fps;
    uint8_t*           rgba;      /* w*h*4 output buffer (RGBA or ARGB) */
    int16_t*           audio_pcm; /* interleaved stereo S16 @ 48 kHz */
    uint64_t           audio_frames;
    uint64_t           audio_capacity;
};

int movie_ffmpeg_available(void) { return 1; }

static int audio_reserve(MoviePlayer* m, uint64_t add_frames)
{
    const uint64_t needed = m->audio_frames + add_frames;
    if (needed <= m->audio_capacity) return 0;
    uint64_t capacity = m->audio_capacity ? m->audio_capacity : MOVIE_AUDIO_RATE;
    while (capacity < needed) {
        if (capacity > UINT64_MAX / 2) return -1;
        capacity *= 2;
    }
    if (capacity > (uint64_t)SIZE_MAX /
                   (MOVIE_AUDIO_CHANNELS * sizeof(*m->audio_pcm)))
        return -1;
    int16_t* pcm = (int16_t*)realloc(
        m->audio_pcm,
        (size_t)capacity * MOVIE_AUDIO_CHANNELS * sizeof(*m->audio_pcm));
    if (!pcm) return -1;
    m->audio_pcm = pcm;
    m->audio_capacity = capacity;
    return 0;
}

static int audio_convert_frame(MoviePlayer* m, struct SwrContext** swr_ptr,
                               AVCodecContext* dec, const AVFrame* frame)
{
    struct SwrContext* swr = *swr_ptr;
    const int input_rate = frame->sample_rate > 0
        ? frame->sample_rate : dec->sample_rate;
    const AVChannelLayout* input_layout =
        frame->ch_layout.nb_channels > 0 ? &frame->ch_layout : &dec->ch_layout;
    if (input_rate <= 0 || input_layout->nb_channels <= 0)
        return -1;
    if (!swr) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16,
                                MOVIE_AUDIO_RATE, input_layout,
                                (enum AVSampleFormat)frame->format,
                                input_rate, 0, NULL) < 0) {
            av_channel_layout_uninit(&stereo);
            return -1;
        }
        av_channel_layout_uninit(&stereo);
        if (swr_init(swr) < 0) {
            swr_free(&swr);
            return -1;
        }
        *swr_ptr = swr;
    }
    int out_count = (int)av_rescale_rnd(
        swr_get_delay(swr, input_rate) + frame->nb_samples,
        MOVIE_AUDIO_RATE, input_rate, AV_ROUND_UP);
    if (out_count <= 0 || audio_reserve(m, (uint64_t)out_count) < 0)
        return -1;
    uint8_t* out[1] = {
        (uint8_t*)(m->audio_pcm +
                   m->audio_frames * MOVIE_AUDIO_CHANNELS)
    };
    int converted = swr_convert(swr, out, out_count,
                                (const uint8_t* const*)frame->extended_data,
                                frame->nb_samples);
    if (converted < 0) return -1;
    m->audio_frames += (uint64_t)converted;
    return 0;
}

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t pos;
} MovieMemoryInput;

static int movie_memory_read(void* opaque, uint8_t* dst, int dst_size)
{
    MovieMemoryInput* input = (MovieMemoryInput*)opaque;
    size_t remaining = input->size - input->pos;
    if (!remaining) return AVERROR_EOF;
    if ((size_t)dst_size > remaining) dst_size = (int)remaining;
    memcpy(dst, input->data + input->pos, (size_t)dst_size);
    input->pos += (size_t)dst_size;
    return dst_size;
}

static int64_t movie_memory_seek(void* opaque, int64_t offset, int whence)
{
    MovieMemoryInput* input = (MovieMemoryInput*)opaque;
    if (whence == AVSEEK_SIZE) return (int64_t)input->size;
    const int origin = whence & ~AVSEEK_FORCE;
    int64_t base;
    if (origin == SEEK_SET) base = 0;
    else if (origin == SEEK_CUR) base = (int64_t)input->pos;
    else if (origin == SEEK_END) base = (int64_t)input->size;
    else return AVERROR(EINVAL);
    if (offset < -base || offset > (int64_t)input->size - base)
        return AVERROR(EINVAL);
    input->pos = (size_t)(base + offset);
    return (int64_t)input->pos;
}

#define MOVIE_AIX_MAX_STREAMS 16

/* AIX is a layered container. Sofdec commonly stores music/ambience and
 * dialogue as separate synchronized ADX streams; selecting only stream zero
 * produces a perfectly timed movie with no voices. Decode every audio stream
 * into its own timeline, then combine them sample-for-sample. */
static int movie_decode_aix(MoviePlayer* m, const uint8_t* data, size_t size)
{
    MovieMemoryInput input = { data, size, 0 };
    AVIOContext* io = NULL;
    AVFormatContext* fmt = NULL;
    AVFrame* frame = NULL;
    AVPacket* pkt = NULL;
    uint8_t* io_buffer = NULL;
    int* stream_slot = NULL;
    AVCodecContext* dec[MOVIE_AIX_MAX_STREAMS] = {0};
    struct SwrContext* swr[MOVIE_AIX_MAX_STREAMS] = {0};
    MoviePlayer layer[MOVIE_AIX_MAX_STREAMS] = {0};
    int layer_count = 0;
    int ok = -1;

    const AVInputFormat* aix = av_find_input_format("aix");
    if (!aix) goto done;
    io_buffer = (uint8_t*)av_malloc(32768);
    if (!io_buffer) goto done;
    io = avio_alloc_context(io_buffer, 32768, 0, &input,
                            movie_memory_read, NULL, movie_memory_seek);
    if (!io) goto done;
    io_buffer = NULL; /* owned through io->buffer from here */
    fmt = avformat_alloc_context();
    if (!fmt) goto done;
    fmt->pb = io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&fmt, NULL, aix, NULL) < 0) goto done;
    if (avformat_find_stream_info(fmt, NULL) < 0) goto done;

    stream_slot = (int*)malloc(fmt->nb_streams * sizeof(*stream_slot));
    if (!stream_slot) goto done;
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        stream_slot[i] = -1;

    for (unsigned i = 0; i < fmt->nb_streams &&
                             layer_count < MOVIE_AIX_MAX_STREAMS; i++) {
        AVCodecParameters* par = fmt->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec)
            continue;
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx)
            goto done;
        if (avcodec_parameters_to_context(ctx, par) < 0 ||
            avcodec_open2(ctx, codec, NULL) < 0) {
            avcodec_free_context(&ctx);
            continue;
        }
        stream_slot[i] = layer_count;
        dec[layer_count++] = ctx;
    }
    if (!layer_count) goto done;

    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt) goto done;
    while (av_read_frame(fmt, pkt) >= 0) {
        const int slot =
            pkt->stream_index >= 0 &&
            (unsigned)pkt->stream_index < fmt->nb_streams
                ? stream_slot[pkt->stream_index] : -1;
        if (slot < 0 || avcodec_send_packet(dec[slot], pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);
        for (;;) {
            int rr = avcodec_receive_frame(dec[slot], frame);
            if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
            if (rr < 0 ||
                audio_convert_frame(&layer[slot], &swr[slot],
                                    dec[slot], frame) < 0)
                goto done;
            av_frame_unref(frame);
        }
    }

    for (int slot = 0; slot < layer_count; slot++) {
        if (avcodec_send_packet(dec[slot], NULL) < 0)
            continue;
        for (;;) {
            int rr = avcodec_receive_frame(dec[slot], frame);
            if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
            if (rr < 0 ||
                audio_convert_frame(&layer[slot], &swr[slot],
                                    dec[slot], frame) < 0)
                goto done;
            av_frame_unref(frame);
        }
    }

    {
        uint64_t mixed_frames = 0;
        for (int slot = 0; slot < layer_count; slot++)
            if (layer[slot].audio_frames > mixed_frames)
                mixed_frames = layer[slot].audio_frames;
        if (!mixed_frames || audio_reserve(m, mixed_frames) < 0)
            goto done;
        memset(m->audio_pcm + m->audio_frames * MOVIE_AUDIO_CHANNELS, 0,
               (size_t)mixed_frames * MOVIE_AUDIO_CHANNELS *
               sizeof(*m->audio_pcm));
        const uint64_t base = m->audio_frames;
        for (int slot = 0; slot < layer_count; slot++) {
            const uint64_t samples =
                layer[slot].audio_frames * MOVIE_AUDIO_CHANNELS;
            for (uint64_t i = 0; i < samples; i++) {
                int mixed = m->audio_pcm[base * MOVIE_AUDIO_CHANNELS + i] +
                            layer[slot].audio_pcm[i];
                if (mixed > 32767) mixed = 32767;
                if (mixed < -32768) mixed = -32768;
                m->audio_pcm[base * MOVIE_AUDIO_CHANNELS + i] =
                    (int16_t)mixed;
            }
        }
        m->audio_frames += mixed_frames;
        fprintf(stderr,
                "[movie-ffmpeg] AIX mixed %d synchronized audio layer(s), "
                "%llu frames\n",
                layer_count, (unsigned long long)mixed_frames);
        ok = 0;
    }

done:
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    for (int i = 0; i < MOVIE_AIX_MAX_STREAMS; i++) {
        if (swr[i]) swr_free(&swr[i]);
        if (dec[i]) avcodec_free_context(&dec[i]);
        free(layer[i].audio_pcm);
    }
    free(stream_slot);
    if (fmt) avformat_close_input(&fmt);
    if (io) {
        av_freep(&io->buffer);
        avio_context_free(&io);
    } else if (io_buffer) {
        av_free(io_buffer);
    }
    return ok;
}

static int byte_append(uint8_t** dst, size_t* size, size_t* capacity,
                       const uint8_t* src, size_t count)
{
    if (count > SIZE_MAX - *size) return -1;
    const size_t needed = *size + count;
    if (needed > *capacity) {
        size_t next = *capacity ? *capacity : 65536;
        while (next < needed) {
            if (next > SIZE_MAX / 2) return -1;
            next *= 2;
        }
        uint8_t* grown = (uint8_t*)realloc(*dst, next);
        if (!grown) return -1;
        *dst = grown;
        *capacity = next;
    }
    memcpy(*dst + *size, src, count);
    *size = needed;
    return 0;
}

/* Decode the selected ADX stream using a second demux context. Keeping video
 * packet delivery independent preserves the existing pull-based video API,
 * while both tracks still use the same FFmpeg demux/codec implementation.
 * The decoded S16 buffer is consumed by cellAudio's fixed 48 kHz heartbeat. */
static int movie_decode_audio(MoviePlayer* m, const char* path)
{
    AVFormatContext* fmt = NULL;
    AVCodecContext* dec = NULL;
    struct SwrContext* swr = NULL;
    AVFrame* frame = NULL;
    AVPacket* pkt = NULL;
    const AVCodec* codec = NULL;
    int stream = -1;
    int ok = -1;
    int step = 0;
    int fferr = 0;
    unsigned rejected_packets = 0;
    uint8_t* aix_data = NULL;
    size_t aix_size = 0, aix_capacity = 0;
    int is_aix = -1;

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) { step = 1; goto done; }
    fmt->probesize = 50LL * 1024 * 1024;
    fmt->max_analyze_duration = 20LL * 1000000;
    if (avformat_find_stream_info(fmt, NULL) < 0) { step = 2; goto done; }

    /* av_find_best_stream rejects some Sofdec streams because MPEG-PS cannot
     * know ADX channels/rate until the in-band ADX header is decoded. Select
     * the first decodable audio stream without requiring those parameters. */
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters* par = fmt->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        const AVCodec* candidate = avcodec_find_decoder(par->codec_id);
        if (candidate) {
            stream = (int)i;
            codec = candidate;
            break;
        }
    }
    if (stream < 0 || !codec) { step = 3; goto done; }
    dec = avcodec_alloc_context3(codec);
    if (!dec) { step = 4; goto done; }
    if (avcodec_parameters_to_context(dec, fmt->streams[stream]->codecpar) < 0)
        { step = 5; goto done; }
    if (avcodec_open2(dec, codec, NULL) < 0) { step = 6; goto done; }
    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt) { step = 7; goto done; }

    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == stream) {
            if (is_aix < 0)
                is_aix = pkt->size >= 4 &&
                         !memcmp(pkt->data, "AIXF", 4);
            if (is_aix) {
                if (byte_append(&aix_data, &aix_size, &aix_capacity,
                                pkt->data, (size_t)pkt->size) < 0) {
                    av_packet_unref(pkt);
                    step = 16;
                    goto done;
                }
                av_packet_unref(pkt);
                continue;
            }
            int sr = avcodec_send_packet(dec, pkt);
            if (sr < 0) {
                rejected_packets++;
                if (rejected_packets <= 4) {
                    const int dump = pkt->size < 24 ? pkt->size : 24;
                    int magic_at = -1;
                    for (int i = 0; i + 1 < pkt->size; i++) {
                        if (pkt->data[i] == 0x80 && pkt->data[i + 1] == 0x00) {
                            magic_at = i;
                            break;
                        }
                    }
                    fprintf(stderr,
                            "[movie-ffmpeg] rejected leading ADX packet #%u: "
                            "size=%d magic_at=%d data=",
                            rejected_packets, pkt->size, magic_at);
                    for (int i = 0; i < dump; i++)
                        fprintf(stderr, "%02X", pkt->data[i]);
                    fprintf(stderr, "\n");
                }
            }
            av_packet_unref(pkt);
            /* Sofdec intersperses padding PES packets in the ADX stream. The
             * decoder correctly rejects them as invalid ADX, so skip them. */
            if (sr < 0) {
                fferr = sr;
                continue;
            }
            for (;;) {
                int rr = avcodec_receive_frame(dec, frame);
                if (rr == AVERROR(EAGAIN) || rr == AVERROR_EOF) break;
                if (rr < 0) { step = 9; goto done; }
                if (audio_convert_frame(m, &swr, dec, frame) < 0)
                    { step = 10; goto done; }
                av_frame_unref(frame);
            }
        } else {
            av_packet_unref(pkt);
        }
    }

    if (is_aix) {
        ok = movie_decode_aix(m, aix_data, aix_size);
        if (ok < 0) step = 17;
        goto done;
    }

    if (avcodec_send_packet(dec, NULL) < 0) { step = 11; goto done; }
    for (;;) {
        int rr = avcodec_receive_frame(dec, frame);
        if (rr == AVERROR_EOF || rr == AVERROR(EAGAIN)) break;
        if (rr < 0) { step = 12; goto done; }
        if (audio_convert_frame(m, &swr, dec, frame) < 0)
            { step = 13; goto done; }
        av_frame_unref(frame);
    }
    for (;;) {
        int out_count = (int)av_rescale_rnd(
            swr ? swr_get_delay(swr, MOVIE_AUDIO_RATE) : 0,
            MOVIE_AUDIO_RATE, MOVIE_AUDIO_RATE, AV_ROUND_UP);
        if (out_count <= 0) break;
        if (audio_reserve(m, (uint64_t)out_count) < 0)
            { step = 14; goto done; }
        uint8_t* out[1] = {
            (uint8_t*)(m->audio_pcm +
                       m->audio_frames * MOVIE_AUDIO_CHANNELS)
        };
        int converted = swr_convert(swr, out, out_count, NULL, 0);
        if (converted < 0) { step = 15; goto done; }
        if (converted == 0) break;
        m->audio_frames += (uint64_t)converted;
    }

    ok = m->audio_frames ? 0 : -1;

done:
    if (pkt) av_packet_free(&pkt);
    if (frame) av_frame_free(&frame);
    if (swr) swr_free(&swr);
    if (dec) avcodec_free_context(&dec);
    if (fmt) avformat_close_input(&fmt);
    free(aix_data);
    if (ok < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(fferr, errbuf, sizeof(errbuf));
        fprintf(stderr,
                "[movie-ffmpeg] ADX decode failed for '%s': step=%d "
                "stream=%d decoder=%s error=%s (%d)\n",
                path, step, stream, codec ? codec->name : "none",
                fferr ? errbuf : "none", fferr);
        free(m->audio_pcm);
        m->audio_pcm = NULL;
        m->audio_frames = 0;
        m->audio_capacity = 0;
    }
    return ok;
}

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
    movie_decode_audio(m, path);
    return m;
}

int    movie_width(MoviePlayer* m)     { return m ? m->w : 0; }
int    movie_height(MoviePlayer* m)    { return m ? m->h : 0; }
double movie_framerate(MoviePlayer* m) { return m ? m->fps : 0.0; }
int movie_has_audio(MoviePlayer* m)
{
    return m && m->audio_pcm && m->audio_frames;
}
const int16_t* movie_audio_s16(MoviePlayer* m)
{
    return m ? m->audio_pcm : NULL;
}
uint64_t movie_audio_frames(MoviePlayer* m)
{
    return m ? m->audio_frames : 0;
}
int movie_audio_sample_rate(MoviePlayer* m)
{
    return movie_has_audio(m) ? MOVIE_AUDIO_RATE : 0;
}
int movie_audio_channels(MoviePlayer* m)
{
    return movie_has_audio(m) ? MOVIE_AUDIO_CHANNELS : 0;
}

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

/* Decode the next frame and convert to big-endian A8R8G8B8 byte order
 * (A,R,G,B per pixel) — the GCM linear ARGB layout the game's movie plane
 * samples (fmt 0xA5, measured from the RSX oracle). Same buffer/lifetime
 * rules as movie_next_rgba. */
const uint8_t* movie_next_argb(MoviePlayer* m, double* pts_out)
{
    if (!m) return 0;
    for (;;) {
        int rr = avcodec_receive_frame(m->dec, m->frame);
        if (rr != 0) {
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
                avcodec_send_packet(m->dec, NULL);
                if (avcodec_receive_frame(m->dec, m->frame) != 0) return 0;
            } else {
                continue;
            }
        }
        if (!m->sws_argb)
            m->sws_argb = sws_getContext(m->w, m->h, m->dec->pix_fmt, m->w, m->h,
                                         AV_PIX_FMT_ARGB, SWS_BILINEAR, 0, 0, 0);
        uint8_t* dst[1] = { m->rgba };
        int stride[1] = { m->w * 4 };
        sws_scale(m->sws_argb, (const uint8_t* const*)m->frame->data,
                  m->frame->linesize, 0, m->h, dst, stride);
        if (pts_out) {
            AVRational tb = m->fmt->streams[m->vstream]->time_base;
            int64_t pts = m->frame->best_effort_timestamp;
            *pts_out = (pts == AV_NOPTS_VALUE) ? 0.0 : pts * av_q2d(tb);
        }
        return m->rgba;
    }
}

int movie_next_yuv(MoviePlayer* m, MovieYuvFrame* out)
{
    if (!m || !out) return 0;
    for (;;) {
        int rr = avcodec_receive_frame(m->dec, m->frame);
        if (rr == 0) {
            out->y = m->frame->data[0];
            out->u = m->frame->data[1];
            out->v = m->frame->data[2];
            out->ystride = m->frame->linesize[0];
            out->cstride = m->frame->linesize[1];
            out->w = m->w;
            out->h = m->h;
            AVRational tb = m->fmt->streams[m->vstream]->time_base;
            int64_t pts = m->frame->best_effort_timestamp;
            out->pts = (pts == AV_NOPTS_VALUE) ? 0.0 : pts * av_q2d(tb);
            return 1;
        }
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
            avcodec_send_packet(m->dec, NULL);
            if (avcodec_receive_frame(m->dec, m->frame) != 0) return 0;
            out->y = m->frame->data[0];
            out->u = m->frame->data[1];
            out->v = m->frame->data[2];
            out->ystride = m->frame->linesize[0];
            out->cstride = m->frame->linesize[1];
            out->w = m->w;
            out->h = m->h;
            out->pts = 0.0;
            return 1;
        }
    }
}

void movie_close(MoviePlayer* m)
{
    if (!m) return;
    if (m->sws)      sws_freeContext(m->sws);
    if (m->sws_argb) sws_freeContext(m->sws_argb);
    if (m->frame) av_frame_free(&m->frame);
    if (m->pkt)   av_packet_free(&m->pkt);
    if (m->dec)   avcodec_free_context(&m->dec);
    if (m->fmt)   avformat_close_input(&m->fmt);
    if (m->rgba)  free(m->rgba);
    if (m->audio_pcm) free(m->audio_pcm);
    free(m);
}

#endif /* HAVE_FFMPEG */
