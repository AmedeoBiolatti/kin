#include <kin/platform/file_dialogs.hpp>

#include <kin/platform/app.hpp>
#include <kin/platform/window.hpp>

#include <SDL3/SDL.h>

#include <deque>
#include <mutex>
#include <utility>

namespace kin {

// Shared with every open dialog, so an answer has somewhere to go even after
// the FileDialogs that asked is gone.
struct FileDialogs::Shared {
    std::mutex mutex;
    std::vector<FileDialogResult> results;
    std::deque<std::vector<std::filesystem::path>> answers;
};

namespace {

// One open dialog: what SDL needs until it answers (filters and location must
// stay valid until then), and where the answer goes.
struct Request {
    std::shared_ptr<void> shared_owner;
    std::mutex* mutex = nullptr;
    std::vector<FileDialogResult>* results = nullptr;
    u32 id = 0;
    std::vector<std::string> filter_names;
    std::vector<std::string> filter_patterns;
    std::vector<SDL_DialogFileFilter> filters;
    std::string location;
};

void SDLCALL answered(void* userdata, const char* const* files, int) {
    std::unique_ptr<Request> request{static_cast<Request*>(userdata)};
    FileDialogResult result{.request = request->id};
    if (!files) {
        result.error = SDL_GetError();
        if (result.error.empty()) {
            result.error = "the file dialog failed";
        }
    } else {
        for (const char* const* file = files; *file; ++file) {
            result.paths.emplace_back(std::u8string_view(reinterpret_cast<const char8_t*>(*file)));
        }
    }
    const std::lock_guard lock{*request->mutex};
    request->results->push_back(std::move(result));
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace

FileDialogs::FileDialogs(const App& app)
    : _shared(std::make_shared<Shared>()), _headless(app.headless()) {
}

FileDialogs::~FileDialogs() = default;

u32 FileDialogs::open_file(const Window* parent, FileDialogOptions options) {
    return show(Kind::OpenFile, parent, std::move(options));
}

u32 FileDialogs::save_file(const Window* parent, FileDialogOptions options) {
    return show(Kind::SaveFile, parent, std::move(options));
}

u32 FileDialogs::open_folder(const Window* parent, FileDialogOptions options) {
    return show(Kind::OpenFolder, parent, std::move(options));
}

u32 FileDialogs::show(Kind kind, const Window* parent, FileDialogOptions options) {
    const u32 id = _next_request++;
    {
        const std::lock_guard lock{_shared->mutex};
        if (!_shared->answers.empty()) {
            _shared->results.push_back({.request = id, .paths = std::move(_shared->answers.front())});
            _shared->answers.pop_front();
            return id;
        }
        if (_headless) {
            _shared->results.push_back({.request = id, .error = "file dialogs are not shown in headless runs"});
            return id;
        }
    }

    auto request = std::make_unique<Request>();
    request->shared_owner = _shared;
    request->mutex = &_shared->mutex;
    request->results = &_shared->results;
    request->id = id;
    for (FileFilter& filter : options.filters) {
        request->filter_names.push_back(std::move(filter.name));
        request->filter_patterns.push_back(std::move(filter.patterns));
    }
    for (std::size_t i = 0; i < request->filter_names.size(); ++i) {
        request->filters.push_back({request->filter_names[i].c_str(), request->filter_patterns[i].c_str()});
    }
    request->location = utf8(options.location);

    SDL_Window* window = parent ? static_cast<SDL_Window*>(parent->native_handle()) : nullptr;
    const SDL_DialogFileFilter* filters = request->filters.empty() ? nullptr : request->filters.data();
    const int filter_count = static_cast<int>(request->filters.size());
    const char* location = request->location.empty() ? nullptr : request->location.c_str();
    // SDL owns the request until it answers.
    Request* pending = request.release();
    switch (kind) {
    case Kind::OpenFile:
        SDL_ShowOpenFileDialog(answered, pending, window, filters, filter_count, location, options.many);
        break;
    case Kind::SaveFile:
        SDL_ShowSaveFileDialog(answered, pending, window, filters, filter_count, location);
        break;
    case Kind::OpenFolder:
        SDL_ShowOpenFolderDialog(answered, pending, window, location, options.many);
        break;
    }
    return id;
}

std::vector<FileDialogResult> FileDialogs::take_results() {
    const std::lock_guard lock{_shared->mutex};
    return std::exchange(_shared->results, {});
}

void FileDialogs::answer_next(std::vector<std::filesystem::path> paths) {
    const std::lock_guard lock{_shared->mutex};
    _shared->answers.push_back(std::move(paths));
}

} // namespace kin
