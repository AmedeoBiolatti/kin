#include <kin/audio/audio_decoder.hpp>

#include <kin/audio/audio_clip.hpp>
#include <kin/audio/backend.hpp>
#include <kin/platform/log.hpp>

#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace kin {
namespace {

enum class Container {
    Unknown,
    Wav, // and AIFF
    Flac, // native or in Ogg
    Vorbis,
    Opus,
    Mp3,
};

bool starts_with(std::span<const u8> bytes, std::size_t offset, std::string_view magic) {
    return bytes.size() >= offset + magic.size() &&
           std::memcmp(bytes.data() + offset, magic.data(), magic.size()) == 0;
}

Container sniff(std::span<const u8> bytes) {
    if ((starts_with(bytes, 0, "RIFF") || starts_with(bytes, 0, "RIFX") || starts_with(bytes, 0, "RF64") ||
         starts_with(bytes, 0, "BW64")) && starts_with(bytes, 8, "WAVE")) {
        return Container::Wav;
    }
    if (starts_with(bytes, 0, "FORM") && (starts_with(bytes, 8, "AIFF") || starts_with(bytes, 8, "AIFC"))) {
        return Container::Wav;
    }
    if (starts_with(bytes, 0, "fLaC")) {
        return Container::Flac;
    }
    if (starts_with(bytes, 0, "OggS")) {
        // The first page holds the codec's identification header at byte 28.
        if (starts_with(bytes, 28, "\x01vorbis")) {
            return Container::Vorbis;
        }
        if (starts_with(bytes, 28, "\x7f" "FLAC")) {
            return Container::Flac;
        }
        if (starts_with(bytes, 28, "OpusHead")) {
            return Container::Opus;
        }
        return Container::Unknown;
    }
    if (starts_with(bytes, 0, "ID3") || (bytes.size() >= 2 && bytes[0] == 0xff && (bytes[1] & 0xe0) == 0xe0)) {
        return Container::Mp3;
    }
    return Container::Unknown;
}

// Decoders whose library returns every channel: the first two pass through.
class NativeDecoder : public AudioDecoder {
public:
    i32 channels() const override { return std::min(_native_channels, 2); }
    i32 sample_rate() const override { return _sample_rate; }
    i64 frame_count() const override { return _frame_count; }

    i64 read(std::span<f32> out) override {
        const i64 frames = static_cast<i64>(out.size()) / channels();
        if (_native_channels <= 2) {
            return read_native(out.data(), frames);
        }
        constexpr i64 chunk = 256;
        _scratch.resize(static_cast<std::size_t>(chunk * _native_channels));
        i64 done = 0;
        while (done < frames) {
            const i64 got = read_native(_scratch.data(), std::min(chunk, frames - done));
            for (i64 frame = 0; frame < got; ++frame) {
                out[static_cast<std::size_t>((done + frame) * 2)] = _scratch[static_cast<std::size_t>(frame * _native_channels)];
                out[static_cast<std::size_t>((done + frame) * 2 + 1)] =
                    _scratch[static_cast<std::size_t>(frame * _native_channels + 1)];
            }
            done += got;
            if (got == 0) {
                break;
            }
        }
        return done;
    }

protected:
    explicit NativeDecoder(AudioFileBytes bytes)
        : _bytes(std::move(bytes)) {
    }

    void set_format(i64 channels, i64 sample_rate, i64 frame_count) {
        if (channels <= 0 || sample_rate <= 0) {
            throw std::runtime_error("audio file has no channels or no sample rate");
        }
        _native_channels = static_cast<i32>(channels);
        _sample_rate = static_cast<i32>(sample_rate);
        _frame_count = frame_count;
    }

    virtual i64 read_native(f32* out, i64 frames) = 0;

    AudioFileBytes _bytes;
    i32 _native_channels = 0;
    i32 _sample_rate = 0;
    i64 _frame_count = 0;
    std::vector<f32> _scratch;
};

class WavDecoder final : public NativeDecoder {
public:
    explicit WavDecoder(AudioFileBytes bytes)
        : NativeDecoder(std::move(bytes)) {
        if (!drwav_init_memory(&_wav, _bytes->data(), _bytes->size(), nullptr)) {
            throw std::runtime_error("not a WAV or AIFF file dr_wav can read");
        }
        _open = true;
        set_format(_wav.channels, _wav.sampleRate, static_cast<i64>(_wav.totalPCMFrameCount));
    }
    ~WavDecoder() override {
        if (_open) {
            drwav_uninit(&_wav);
        }
    }
    bool seek(i64 frame) override { return drwav_seek_to_pcm_frame(&_wav, static_cast<drwav_uint64>(frame)); }

private:
    i64 read_native(f32* out, i64 frames) override {
        return static_cast<i64>(drwav_read_pcm_frames_f32(&_wav, static_cast<drwav_uint64>(frames), out));
    }

    drwav _wav{};
    bool _open = false;
};

class FlacDecoder final : public NativeDecoder {
public:
    explicit FlacDecoder(AudioFileBytes bytes)
        : NativeDecoder(std::move(bytes)) {
        _flac = drflac_open_memory(_bytes->data(), _bytes->size(), nullptr);
        if (!_flac) {
            throw std::runtime_error("not a FLAC file dr_flac can read");
        }
        set_format(_flac->channels, _flac->sampleRate, static_cast<i64>(_flac->totalPCMFrameCount));
    }
    ~FlacDecoder() override { drflac_close(_flac); }
    bool seek(i64 frame) override { return drflac_seek_to_pcm_frame(_flac, static_cast<drflac_uint64>(frame)); }

private:
    i64 read_native(f32* out, i64 frames) override {
        return static_cast<i64>(drflac_read_pcm_frames_f32(_flac, static_cast<drflac_uint64>(frames), out));
    }

    drflac* _flac = nullptr;
};

using Mp3SeekTable = std::vector<drmp3_seek_point>;

class Mp3Decoder final : public NativeDecoder {
public:
    // With `seek_table`, seeking jumps close to the frame instead of decoding
    // from the start; it is computed once per file and shared.
    Mp3Decoder(AudioFileBytes bytes, std::shared_ptr<const Mp3SeekTable> seek_table, i64 frame_count)
        : NativeDecoder(std::move(bytes)),
          _seek_table(std::move(seek_table)) {
        if (!drmp3_init_memory(&_mp3, _bytes->data(), _bytes->size(), nullptr)) {
            throw std::runtime_error("not an MP3 file dr_mp3 can read");
        }
        _open = true;
        if (frame_count < 0) {
            frame_count = static_cast<i64>(drmp3_get_pcm_frame_count(&_mp3)); // fast with a Xing/Info header
            drmp3_seek_to_pcm_frame(&_mp3, 0);
        }
        set_format(_mp3.channels, _mp3.sampleRate, frame_count);
        if (_seek_table && !_seek_table->empty()) {
            // dr_mp3 only reads the table; it is not const in its signature.
            drmp3_bind_seek_table(&_mp3, static_cast<drmp3_uint32>(_seek_table->size()),
                                  const_cast<drmp3_seek_point*>(_seek_table->data()));
        }
    }
    ~Mp3Decoder() override {
        if (_open) {
            drmp3_uninit(&_mp3);
        }
    }
    bool seek(i64 frame) override { return drmp3_seek_to_pcm_frame(&_mp3, static_cast<drmp3_uint64>(frame)); }

    std::shared_ptr<const Mp3SeekTable> make_seek_table() {
        drmp3_uint32 count = 64;
        auto table = std::make_shared<Mp3SeekTable>(count);
        if (!drmp3_calculate_seek_points(&_mp3, &count, table->data())) {
            return nullptr;
        }
        table->resize(count);
        drmp3_seek_to_pcm_frame(&_mp3, 0);
        return table;
    }

private:
    i64 read_native(f32* out, i64 frames) override {
        return static_cast<i64>(drmp3_read_pcm_frames_f32(&_mp3, static_cast<drmp3_uint64>(frames), out));
    }

    drmp3 _mp3{};
    bool _open = false;
    std::shared_ptr<const Mp3SeekTable> _seek_table;
};

class VorbisDecoder final : public AudioDecoder {
public:
    explicit VorbisDecoder(AudioFileBytes bytes)
        : _bytes(std::move(bytes)) {
        if (_bytes->size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("Ogg Vorbis file too large");
        }
        int error = 0;
        _vorbis = stb_vorbis_open_memory(_bytes->data(), static_cast<int>(_bytes->size()), &error, nullptr);
        if (!_vorbis) {
            throw std::runtime_error("not an Ogg Vorbis file stb_vorbis can read (error " + std::to_string(error) + ")");
        }
        const stb_vorbis_info info = stb_vorbis_get_info(_vorbis);
        if (info.channels <= 0 || info.sample_rate == 0) {
            stb_vorbis_close(_vorbis);
            throw std::runtime_error("Ogg Vorbis file has no channels or no sample rate");
        }
        _channels = std::min(info.channels, 2);
        _sample_rate = static_cast<i32>(info.sample_rate);
        _frame_count = static_cast<i64>(stb_vorbis_stream_length_in_samples(_vorbis));
    }
    ~VorbisDecoder() override { stb_vorbis_close(_vorbis); }

    i32 channels() const override { return _channels; }
    i32 sample_rate() const override { return _sample_rate; }
    i64 frame_count() const override { return _frame_count; }

    i64 read(std::span<f32> out) override {
        // stb_vorbis mixes files with more channels down to the two asked for.
        const std::size_t samples = std::min(out.size(), static_cast<std::size_t>(std::numeric_limits<int>::max()));
        return stb_vorbis_get_samples_float_interleaved(_vorbis, _channels, out.data(), static_cast<int>(samples));
    }

    bool seek(i64 frame) override {
        return frame >= 0 && frame <= std::numeric_limits<unsigned int>::max() &&
               stb_vorbis_seek(_vorbis, static_cast<unsigned int>(frame)) != 0;
    }

private:
    AudioFileBytes _bytes;
    stb_vorbis* _vorbis = nullptr;
    i32 _channels = 0;
    i32 _sample_rate = 0;
    i64 _frame_count = 0;
};

std::unique_ptr<AudioDecoder> open_decoder(AudioFileBytes bytes,
                                           std::shared_ptr<const Mp3SeekTable> mp3_seek_table = nullptr,
                                           i64 mp3_frame_count = -1) {
    if (!bytes) {
        throw std::runtime_error("no audio data");
    }
    switch (sniff(*bytes)) {
    case Container::Wav: return std::make_unique<WavDecoder>(std::move(bytes));
    case Container::Flac: return std::make_unique<FlacDecoder>(std::move(bytes));
    case Container::Vorbis: return std::make_unique<VorbisDecoder>(std::move(bytes));
    case Container::Mp3: return std::make_unique<Mp3Decoder>(std::move(bytes), std::move(mp3_seek_table), mp3_frame_count);
    case Container::Opus: throw std::runtime_error("Ogg Opus is not supported; use Ogg Vorbis");
    case Container::Unknown: break;
    }
    throw std::runtime_error("unrecognised audio format (kin reads WAV, AIFF, FLAC, Ogg Vorbis and MP3)");
}

// A clip that stays compressed in memory; every voice playing it decodes its
// own copy as it goes.
class StreamClipBackend final : public IAudioClipBackend {
public:
    StreamClipBackend(AudioFileBytes bytes, std::shared_ptr<const Mp3SeekTable> seek_table, i64 frame_count)
        : _bytes(std::move(bytes)),
          _seek_table(std::move(seek_table)),
          _frame_count(frame_count) {
    }

    std::span<const f32> samples() const override { return {}; }
    bool streamed() const override { return true; }
    std::unique_ptr<AudioDecoder> open_stream() const override {
        return open_decoder(_bytes, _seek_table, _frame_count);
    }

private:
    AudioFileBytes _bytes;
    std::shared_ptr<const Mp3SeekTable> _seek_table;
    i64 _frame_count = 0;
};

[[noreturn]] void fail_load(const std::filesystem::path& path, std::string_view message) {
    const std::string error = "audio clip load failed for " + path.string() + ": " + std::string{message};
    KIN_LOG_ERROR_F("audio",
                    "audio clip load failed",
                    (LogFields{
                        {.name = "path", .value = path.string()},
                        {.name = "error", .value = std::string{message}},
                    }));
    throw std::runtime_error(error);
}

i32 checked_frame_count(const std::filesystem::path& path, i64 frames) {
    if (frames <= 0 || frames > std::numeric_limits<i32>::max()) {
        fail_load(path, frames <= 0 ? "no audio frames" : "too long");
    }
    return static_cast<i32>(frames);
}

void log_loaded(const std::filesystem::path& path, const AudioClip& clip, bool streamed) {
    KIN_LOG_INFO_F("audio",
                   "audio clip loaded",
                   (LogFields{
                       {.name = "path", .value = path.string()},
                       {.name = "frames", .value = std::to_string(clip.frame_count())},
                       {.name = "channels", .value = std::to_string(clip.channels())},
                       {.name = "sample_rate", .value = std::to_string(clip.sample_rate())},
                       {.name = "streamed", .value = streamed ? "true" : "false"},
                   }));
}

} // namespace

std::unique_ptr<AudioDecoder> open_audio_decoder(AudioFileBytes bytes) {
    return open_decoder(std::move(bytes));
}

AudioFileBytes read_audio_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("cannot open " + path.string());
    }
    const std::streamsize size = file.tellg();
    auto bytes = std::make_shared<std::vector<u8>>(static_cast<std::size_t>(std::max<std::streamsize>(0, size)));
    file.seekg(0);
    if (size > 0 && !file.read(reinterpret_cast<char*>(bytes->data()), size)) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return bytes;
}

// Needs no audio device, so it is safe on worker threads.
AudioClip load_audio_clip(const std::filesystem::path& path) {
    std::unique_ptr<AudioDecoder> decoder;
    try {
        decoder = open_decoder(read_audio_file(path));
    } catch (const std::exception& error) {
        fail_load(path, error.what());
    }
    const i32 channels = decoder->channels();
    std::vector<f32> samples;
    if (decoder->frame_count() > 0) {
        samples.reserve(static_cast<std::size_t>(decoder->frame_count() * channels));
    }
    constexpr std::size_t chunk_frames = 4096;
    for (;;) {
        const std::size_t at = samples.size();
        samples.resize(at + chunk_frames * static_cast<std::size_t>(channels));
        const i64 got = decoder->read(std::span<f32>{samples.data() + at, chunk_frames * static_cast<std::size_t>(channels)});
        samples.resize(at + static_cast<std::size_t>(got * channels));
        if (got < static_cast<i64>(chunk_frames)) {
            break;
        }
    }
    checked_frame_count(path, static_cast<i64>(samples.size()) / channels);
    AudioClip clip = make_memory_audio_clip(path.filename().string(), std::move(samples), channels, decoder->sample_rate());
    log_loaded(path, clip, false);
    return clip;
}

AudioClip load_audio_stream(const std::filesystem::path& path) {
    AudioFileBytes bytes;
    std::unique_ptr<AudioDecoder> decoder;
    std::shared_ptr<const Mp3SeekTable> seek_table;
    try {
        bytes = read_audio_file(path);
        decoder = open_decoder(bytes);
        if (auto* mp3 = dynamic_cast<Mp3Decoder*>(decoder.get())) {
            seek_table = mp3->make_seek_table();
        }
    } catch (const std::exception& error) {
        fail_load(path, error.what());
    }
    const i32 frames = checked_frame_count(path, decoder->frame_count());
    AudioClip clip{
        path.filename().string(),
        AudioFormat::F32,
        decoder->channels(),
        decoder->sample_rate(),
        frames,
        std::make_shared<StreamClipBackend>(std::move(bytes), std::move(seek_table), decoder->frame_count()),
    };
    log_loaded(path, clip, true);
    return clip;
}

} // namespace kin
