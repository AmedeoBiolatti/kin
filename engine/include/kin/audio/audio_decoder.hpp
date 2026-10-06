#pragma once

#include <kin/core/types.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace kin {

// Decodes one audio file held in memory to interleaved 32-bit float frames.
// WAV (and AIFF), FLAC, Ogg Vorbis and MP3 are recognised by their contents.
// Files with more than two channels decode their first two.
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    virtual i32 channels() const = 0; // 1 or 2
    virtual i32 sample_rate() const = 0;
    // Total frames in the file.
    virtual i64 frame_count() const = 0;
    // Decodes up to `out.size() / channels()` frames from the current position
    // and returns how many it wrote; fewer than asked only at the end.
    virtual i64 read(std::span<f32> out) = 0;
    // Moves to `frame`; false if the decoder could not.
    virtual bool seek(i64 frame) = 0;
};

using AudioFileBytes = std::shared_ptr<const std::vector<u8>>;

// Opens a decoder over `bytes`, which it keeps alive. Throws std::runtime_error
// when the format is not recognised or the file is damaged.
std::unique_ptr<AudioDecoder> open_audio_decoder(AudioFileBytes bytes);

AudioFileBytes read_audio_file(const std::filesystem::path& path);

} // namespace kin
