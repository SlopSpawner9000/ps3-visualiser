#include "audio.h"
#include "firmware.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace {

constexpr size_t kRingFrames = AudioEngine::kRate * 2; // also the visualizer's look-ahead limit

std::string metaValue(AVFormatContext *fmt, int stream, const char *key)
{
    if (AVDictionaryEntry *e = av_dict_get(fmt->metadata, key, nullptr, AV_DICT_IGNORE_SUFFIX))
        return e->value;
    if (stream >= 0)
        if (AVDictionaryEntry *e = av_dict_get(fmt->streams[stream]->metadata, key, nullptr, AV_DICT_IGNORE_SUFFIX))
            return e->value;
    return {};
}

// Owns the FFmpeg objects needed to turn a file into 48 kHz stereo floats.
struct Decoder {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *ctx = nullptr;
    SwrContext *swr = nullptr;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;
    int stream = -1;
    std::vector<float> out;

    ~Decoder()
    {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        swr_free(&swr);
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
    }

    bool open(const std::string &path, TrackInfo *info)
    {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0)
            return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0)
            return false;
        const AVCodec *codec = nullptr;
        stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
        if (stream < 0 || !codec)
            return false;
        ctx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar);
        if (avcodec_open2(ctx, codec, nullptr) < 0)
            return false;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&ctx->ch_layout, ctx->ch_layout.nb_channels);
        if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, AudioEngine::kRate, &ctx->ch_layout,
                                ctx->sample_fmt, ctx->sample_rate, 0, nullptr) < 0 ||
            swr_init(swr) < 0)
            return false;
        pkt = av_packet_alloc();
        frame = av_frame_alloc();
        for (unsigned i = 0; i < fmt->nb_streams; i++) // skip cover art etc.
            if (int(i) != stream)
                fmt->streams[i]->discard = AVDISCARD_ALL;
        if (info) {
            info->sampleRate = ctx->sample_rate;
            info->title = metaValue(fmt, stream, "title");
            info->artist = metaValue(fmt, stream, "artist");
            if (info->artist.empty())
                info->artist = metaValue(fmt, stream, "album_artist");
            info->album = metaValue(fmt, stream, "album");
            info->duration = fmt->duration > 0 ? double(fmt->duration) / AV_TIME_BASE : 0.0;
            if (info->duration <= 0 && fmt->streams[stream]->duration > 0)
                info->duration = fmt->streams[stream]->duration * av_q2d(fmt->streams[stream]->time_base);
        }
        return true;
    }

    // Decodes the next chunk into `out` (interleaved stereo). Returns false at EOF.
    // `startTime` receives the presentation time of the first sample, or -1.
    bool next(double *startTime)
    {
        out.clear();
        *startTime = -1;
        for (;;) {
            int r = avcodec_receive_frame(ctx, frame);
            if (r == 0) {
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
                    *startTime = frame->best_effort_timestamp * av_q2d(fmt->streams[stream]->time_base);
                convert(frame);
                av_frame_unref(frame);
                return true;
            }
            if (r == AVERROR_EOF) {
                convert(nullptr);
                return !out.empty();
            }
            r = av_read_frame(fmt, pkt);
            if (r < 0) {
                avcodec_send_packet(ctx, nullptr);
                continue;
            }
            if (pkt->stream_index == stream)
                avcodec_send_packet(ctx, pkt);
            av_packet_unref(pkt);
        }
    }

    void convert(AVFrame *f)
    {
        const int inSamples = f ? f->nb_samples : 0;
        const int maxOut = swr_get_out_samples(swr, inSamples) + 32;
        if (maxOut <= 0)
            return;
        const size_t base = out.size();
        out.resize(base + size_t(maxOut) * 2);
        uint8_t *dst = reinterpret_cast<uint8_t *>(out.data() + base);
        const int got = swr_convert(swr, &dst, maxOut, f ? const_cast<const uint8_t **>(f->extended_data) : nullptr,
                                    inSamples);
        out.resize(base + size_t(std::max(got, 0)) * 2);
    }

    void seek(double t)
    {
        const int64_t ts = int64_t(t * AV_TIME_BASE);
        av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(ctx);
        swr_close(swr);
        swr_init(swr);
    }
};

} // namespace

bool AudioEngine::init()
{
    histL_.assign(kHistory, 0.0f);
    histR_.assign(kHistory, 0.0f);
    for (Voice &v : voices_)
        v.ring.assign(kRingFrames * 2, 0.0f);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, kRate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &AudioEngine::callback, this);
    if (!stream_) {
        SDL_LogError(SDL_LOG_CATEGORY_AUDIO, "cannot open audio device: %s", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    return true;
}

void AudioEngine::shutdown()
{
    stop();
    if (stream_)
        SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
}

bool AudioEngine::startVoice(Voice &v, const std::filesystem::path &path)
{
    // Probe on this thread so failures and metadata are known immediately.
    Decoder probe;
    TrackInfo info;
    if (!probe.open(utf8(path), &info))
        return false;
    if (info.title.empty())
        info.title = utf8(path.stem());
    v.info = info;
    {
        std::lock_guard lock(v.mtx);
        v.ringRead = v.ringCount = 0;
    }
    v.played = 0;
    v.sinceOpen = 0;
    v.base = 0.0;
    v.eof = false;
    v.quit = false;
    v.seekTo = -1.0;
    v.open = true;
    v.thread = std::thread(&AudioEngine::decodeLoop, this, &v, utf8(path));
    return true;
}

void AudioEngine::stopVoice(Voice &v)
{
    if (v.thread.joinable()) {
        v.quit = true;
        v.cv.notify_all();
        v.thread.join();
    }
    v.open = false;
    std::lock_guard lock(v.mtx);
    v.ringRead = v.ringCount = 0;
}

bool AudioEngine::open(const std::filesystem::path &path, bool fadeIn)
{
    stop();
    Voice &v = voices_[mix_ & 1];
    if (!startVoice(v, path))
        return false;
    if (!fadeIn)
        v.sinceOpen = int64_t(1) << 40; // counts as long past the fade-in
    return true;
}

bool AudioEngine::crossfadeTo(const std::filesystem::path &path, float seconds)
{
    update();
    const int cur = mix_ & 1;
    if (!voices_[cur].open || crossfading())
        return open(path);
    Voice &next = voices_[1 - cur];
    stopVoice(next); // (left over from an earlier crossfade)
    if (!startVoice(next, path))
        return false;
    xfadeLen_ = std::max<int64_t>(1, int64_t(double(seconds) * kRate));
    xfadePos_ = 0;
    xfadeDone_ = false;
    mix_ = (1 - cur) | 2; // the new track is current; the old one fades out
    return true;
}

void AudioEngine::update()
{
    if (xfadeDone_.exchange(false)) {
        const int cur = mix_ & 1;
        mix_ = cur; // stop mixing the old track before stopping its decoder
        stopVoice(voices_[1 - cur]);
    }
}

void AudioEngine::stop()
{
    for (Voice &v : voices_)
        stopVoice(v);
    mix_ = mix_ & 1;
    xfadeDone_ = false;
}

void AudioEngine::setPaused(bool p) { paused_ = p; }

void AudioEngine::seek(double seconds)
{
    Voice &v = voices_[mix_ & 1];
    if (!v.open)
        return;
    seconds = std::max(0.0, seconds);
    if (v.info.duration > 0)
        seconds = std::min(seconds, std::max(0.0, v.info.duration - 0.25));
    v.seekTo = seconds;
    v.cv.notify_all();
}

double AudioEngine::position() const
{
    const Voice &v = voices_[mix_ & 1];
    if (!v.open)
        return 0.0;
    return v.base + double(v.played) / kRate;
}

double AudioEngine::remaining() const
{
    const Voice &v = voices_[mix_ & 1];
    if (!v.open || v.seekTo >= 0)
        return 1e9;
    if (v.eof) { // all decoded: what's left is exactly what's queued
        std::lock_guard lock(v.mtx);
        return double(v.ringCount) / kRate;
    }
    return v.info.duration > 0 ? std::max(0.0, v.info.duration - position()) : 1e9;
}

bool AudioEngine::finished() const
{
    const Voice &v = voices_[mix_ & 1];
    if (!v.open || !v.eof || v.seekTo >= 0)
        return false;
    std::lock_guard lock(v.mtx);
    return v.ringCount == 0;
}

void AudioEngine::recentSamples(float *left, float *right, int n) const
{
    n = std::min(n, kHistory);
    const uint64_t w = histWrite_.load();
    for (int i = 0; i < n; i++) {
        const size_t idx = size_t(w + kHistory - n + i) % kHistory;
        left[i] = histL_[idx];
        right[i] = histR_[idx];
    }
}

void AudioEngine::setEq(const EqSettings &eq)
{
    std::lock_guard lock(eqMtx_);
    if (eq == eqPending_)
        return;
    eqPending_ = eq;
    eqVersion_++;
}

void AudioEngine::upcomingSamples(float *left, float *right, int n, int ahead) const
{
    ahead = std::max(ahead, 0);
    if (ahead == 0 || paused_) {
        recentSamples(left, right, n);
        return;
    }
    const Voice &v = voices_[mix_ & 1];
    n = std::min(n, kHistory);
    std::lock_guard lock(v.mtx);
    const uint64_t w = histWrite_.load();
    for (int i = 0; i < n; i++) {
        const long rel = long(i) - n + ahead; // < 0: already played, >= 0: queued
        if (rel < 0) {
            if (-rel > kHistory) {
                left[i] = right[i] = 0.0f;
                continue;
            }
            const size_t idx = size_t(int64_t(w) + rel + kHistory) % kHistory;
            left[i] = histL_[idx];
            right[i] = histR_[idx];
        } else if (size_t(rel) < v.ringCount) {
            const size_t idx = (v.ringRead + size_t(rel)) % kRingFrames;
            left[i] = v.ring[idx * 2];
            right[i] = v.ring[idx * 2 + 1];
        } else {
            left[i] = right[i] = 0.0f;
        }
    }
}

void AudioEngine::pushFrames(Voice &v, const float *data, int frames)
{
    while (frames > 0 && !v.quit && v.seekTo < 0) {
        std::unique_lock lock(v.mtx);
        v.cv.wait(lock, [&] { return v.quit || v.seekTo >= 0 || v.ringCount < kRingFrames; });
        if (v.quit || v.seekTo >= 0)
            return;
        const size_t space = kRingFrames - v.ringCount;
        const size_t n = std::min<size_t>(space, size_t(frames));
        for (size_t i = 0; i < n; i++) {
            const size_t idx = (v.ringRead + v.ringCount + i) % kRingFrames;
            v.ring[idx * 2] = data[i * 2];
            v.ring[idx * 2 + 1] = data[i * 2 + 1];
        }
        v.ringCount += n;
        data += n * 2;
        frames -= int(n);
    }
}

void AudioEngine::decodeLoop(Voice *vp, std::string path)
{
    Voice &v = *vp;
    Decoder dec;
    if (!dec.open(path, nullptr)) {
        v.eof = true;
        return;
    }
    double skipUntil = 0;
    while (!v.quit) {
        const double target = v.seekTo.exchange(-1.0);
        if (target >= 0) {
            dec.seek(target);
            {
                std::lock_guard lock(v.mtx);
                v.ringRead = v.ringCount = 0;
                v.played = 0;
                v.base = target;
            }
            skipUntil = target;
            v.eof = false;
        }
        if (v.eof) {
            std::unique_lock lock(v.mtx);
            v.cv.wait(lock, [&] { return v.quit || v.seekTo >= 0; });
            continue;
        }
        double t0 = -1;
        if (!dec.next(&t0)) {
            v.eof = true;
            continue;
        }
        const float *data = dec.out.data();
        int frames = int(dec.out.size() / 2);
        if (skipUntil > 0 && t0 >= 0) {
            // Drop audio before the seek target so playback starts exactly there.
            const int skip = int((skipUntil - t0) * kRate);
            if (skip >= frames)
                continue;
            if (skip > 0) {
                data += skip * 2;
                frames -= skip;
            }
            skipUntil = 0;
        }
        pushFrames(v, data, frames);
    }
}

int AudioEngine::pull(Voice &v, float *dst, int frames)
{
    int got = 0;
    {
        std::lock_guard lock(v.mtx);
        got = int(std::min<size_t>(v.ringCount, size_t(frames)));
        for (int i = 0; i < got; i++) {
            const size_t idx = (v.ringRead + i) % kRingFrames;
            dst[i * 2] = v.ring[idx * 2];
            dst[i * 2 + 1] = v.ring[idx * 2 + 1];
        }
        v.ringRead = (v.ringRead + got) % kRingFrames;
        v.ringCount -= got;
        v.played += got;
        v.sinceOpen += got;
    }
    v.cv.notify_all();
    return got;
}

void AudioEngine::callback(void *user, SDL_AudioStream *stream, int additional, int)
{
    auto *self = static_cast<AudioEngine *>(user);
    const int frames = additional / int(sizeof(float) * 2);
    if (frames <= 0)
        return;
    static thread_local std::vector<float> buf, out;
    buf.assign(size_t(frames) * 2, 0.0f);
    const int mix = self->mix_;
    Voice &cur = self->voices_[mix & 1];
    const int64_t openedBefore = cur.sinceOpen; // for the fade-in below
    if (!self->paused_) {
        pull(cur, buf.data(), frames);
        if (mix & 2) { // crossfade: equal-power curves, the old track out, the new one in
            out.assign(size_t(frames) * 2, 0.0f);
            pull(self->voices_[1 - (mix & 1)], out.data(), frames);
            const double len = double(self->xfadeLen_);
            const int64_t pos = self->xfadePos_;
            for (int i = 0; i < frames; i++) {
                const double t = std::clamp(double(pos + i) / len, 0.0, 1.0);
                const float gin = float(std::sin(t * 1.5707963)), gout = float(std::cos(t * 1.5707963));
                buf[i * 2] = buf[i * 2] * gin + out[i * 2] * gout;
                buf[i * 2 + 1] = buf[i * 2 + 1] * gin + out[i * 2 + 1] * gout;
            }
            self->xfadePos_ = pos + frames;
            if (pos + frames >= int64_t(len))
                self->xfadeDone_ = true;
        } else if (const float fade = self->fade_; fade > 0.0f && openedBefore < int64_t(double(fade) * kRate)) {
            // A track started by hand (or the first one) fades in.
            const double fadeFrames = double(fade) * kRate;
            for (int i = 0; i < frames; i++) {
                const double g = std::clamp(double(openedBefore + i) / fadeFrames, 0.0, 1.0);
                const float k = float(g * g * (3.0 - 2.0 * g));
                buf[i * 2] *= k;
                buf[i * 2 + 1] *= k;
            }
        }
    }

    // History for the analyser (pre-volume, silence while paused).
    uint64_t w = self->histWrite_.load();
    for (int i = 0; i < frames; i++) {
        self->histL_[w % kHistory] = buf[i * 2];
        self->histR_[w % kHistory] = buf[i * 2 + 1];
        w++;
    }
    self->histWrite_.store(w);

    // Equalizer (after the history, so the visualizer can choose to apply it itself).
    if (const int v = self->eqVersion_.load(); v != self->eqApplied_) {
        std::lock_guard lock(self->eqMtx_);
        self->eqActive_ = self->eqPending_;
        self->eq_.configure(self->eqActive_, float(kRate));
        self->eqApplied_ = v;
    }
    if (self->eqActive_.affectsAudio()) {
        static thread_local std::vector<float> l, r;
        l.resize(size_t(frames));
        r.resize(size_t(frames));
        for (int i = 0; i < frames; i++) {
            l[size_t(i)] = buf[i * 2];
            r[size_t(i)] = buf[i * 2 + 1];
        }
        self->eq_.process(l.data(), r.data(), frames);
        for (int i = 0; i < frames; i++) {
            buf[i * 2] = std::clamp(l[size_t(i)], -1.0f, 1.0f);
            buf[i * 2 + 1] = std::clamp(r[size_t(i)], -1.0f, 1.0f);
        }
    }

    const float vol = self->volume_;
    for (auto &s : buf)
        s *= vol;
    SDL_PutAudioStreamData(stream, buf.data(), frames * int(sizeof(float) * 2));
}

bool AudioEngine::decodeFile(const std::filesystem::path &path, std::vector<float> &interleaved, TrackInfo *info)
{
    Decoder dec;
    if (!dec.open(utf8(path), info))
        return false;
    double t0;
    while (dec.next(&t0))
        interleaved.insert(interleaved.end(), dec.out.begin(), dec.out.end());
    return true;
}

bool AudioEngine::coverArt(const std::filesystem::path &path, std::vector<uint8_t> &encoded)
{
    encoded.clear();
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, utf8(path).c_str(), nullptr, nullptr) < 0)
        return false;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        const AVStream *st = fmt->streams[i];
        if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) && st->attached_pic.size > 0) {
            encoded.assign(st->attached_pic.data, st->attached_pic.data + st->attached_pic.size);
            break;
        }
    }
    avformat_close_input(&fmt);
    return !encoded.empty();
}
