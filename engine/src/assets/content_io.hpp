#pragma once

#include <filesystem>

struct SDL_IOStream;

namespace kin {

// A content file (from a mounted pack or from disk) as an SDL stream, for the
// SDL loaders (images, fonts, sounds). Close it with SDL_CloseIO, or let the
// loader close it; null, with SDL_GetError() set, if there is no such file.
SDL_IOStream* open_content_stream(const std::filesystem::path& path);

} // namespace kin
