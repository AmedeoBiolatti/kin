#pragma once

#include <kin/core/types.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace kin {

class App;
class Window;

// What a dialog may show: e.g. {"Images", "png;jpg;jpeg"} (extensions without
// dots, separated by semicolons; "*" for any).
struct FileFilter {
    std::string name;
    std::string patterns;
};

struct FileDialogOptions {
    std::vector<FileFilter> filters;
    // Where it opens: a folder, or (saving) a file name to suggest. Empty: the system's choice.
    std::filesystem::path location;
    // Open dialogs: let the user choose several.
    bool many = false;
};

struct FileDialogResult {
    u32 request = 0;                          // what open_file() and the others returned
    std::vector<std::filesystem::path> paths; // what was chosen; empty if cancelled
    std::string error;                        // the dialog could not be shown
    bool cancelled() const { return paths.empty() && error.empty(); }
};

// The system's file dialogs. The dialog runs on its own and answers later, from
// whatever thread the system uses; the answers wait here until the game takes
// them, once a frame:
//
//   kin::FileDialogs dialogs{ctx.app};
//   images = dialogs.open_file(&ctx.window, {.filters = {{"Images", "png;jpg"}}, .many = true});
//   ...
//   for (const kin::FileDialogResult& result : dialogs.take_results()) {
//       if (result.request == images) add_images(result.paths);
//   }
//
// Headless runs never show a dialog: one answers at once with an error, unless
// an answer was queued with answer_next(), which tests and agents use in any
// run. Destroying this while a dialog is open is safe; its answer is dropped.
class FileDialogs {
public:
    explicit FileDialogs(const App& app);
    ~FileDialogs();
    FileDialogs(const FileDialogs&) = delete;
    FileDialogs& operator=(const FileDialogs&) = delete;

    // Each opens a dialog (over `parent`, which may be null) and returns the
    // request number its result will carry. Call from the main thread.
    u32 open_file(const Window* parent, FileDialogOptions options = {});
    u32 save_file(const Window* parent, FileDialogOptions options = {});
    u32 open_folder(const Window* parent, FileDialogOptions options = {});

    // The answers that arrived since the last call, in the order they came.
    std::vector<FileDialogResult> take_results();

    // The next dialog answers with `paths` (empty: cancelled) without showing.
    void answer_next(std::vector<std::filesystem::path> paths);

private:
    enum class Kind { OpenFile, SaveFile, OpenFolder };
    u32 show(Kind kind, const Window* parent, FileDialogOptions options);

    struct Shared;
    std::shared_ptr<Shared> _shared;
    bool _headless = false;
    u32 _next_request = 1;
};

} // namespace kin
