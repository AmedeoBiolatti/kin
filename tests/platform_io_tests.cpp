#include <kin/platform/app.hpp>
#include <kin/platform/file_dialogs.hpp>
#include <kin/platform/input.hpp>
#include <kin/platform/process.hpp>

#include <SDL3/SDL.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace kin {
// Feeds native SDL events to an Input, as App does.
struct InputFrameTestHook {
    static void event(Input& input, const SDL_Event& event) { input.process_native_event(&event); }
};
} // namespace kin

namespace {

using namespace std::chrono_literals;

// This program is also the child the process tests start: `--echo` answers
// each line with "echo:<line>" and exits with N on "exit N" (0 at end of
// input); `--burst N` writes N numbered lines; `--complain` writes to stderr.
int child_main(int argc, char** argv) {
    const std::string mode = argv[1];
    if (mode == "--echo") {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.rfind("exit ", 0) == 0) {
                return std::stoi(line.substr(5));
            }
            std::cout << "echo:" << line << std::endl;
        }
        return 0;
    }
    if (mode == "--burst" && argc > 2) {
        const int lines = std::stoi(argv[2]);
        for (int i = 0; i < lines; ++i) {
            std::cout << "line " << i << '\n';
        }
        std::cout << "unfinished" << std::flush;
        return 0;
    }
    if (mode == "--complain") {
        std::cerr << "oops" << std::endl;
        return 2;
    }
    return 99;
}

std::string self;

// Reads lines until `want` of them arrived or a few seconds pass.
std::vector<std::string> lines_from(kin::Process& process, std::size_t want) {
    std::vector<std::string> lines;
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (lines.size() < want && std::chrono::steady_clock::now() < until) {
        if (auto line = process.read_line()) {
            lines.push_back(std::move(*line));
        } else {
            SDL_Delay(1);
        }
    }
    return lines;
}

void test_process_round_trip() {
    kin::Process child;
    assert(child.start({.args = {self, "--echo"}}));
    assert(child.started() && child.running());
    child.write("hello\nsecond line\n");
    const std::vector<std::string> lines = lines_from(child, 2);
    assert(lines.size() == 2 && lines[0] == "echo:hello" && lines[1] == "echo:second line");
    child.write("exit 7\n");
    assert(child.wait(10s));
    assert(!child.running() && child.exit_code() == 7);
}

void test_process_end_of_input() {
    kin::Process child;
    assert(child.start({.args = {self, "--echo"}}));
    child.write("last\n");
    child.close_input(); // the child reads end of file after "last"
    assert(child.wait(10s) && child.exit_code() == 0);
    assert(child.read_line() == "echo:last");
    child.write("ignored\n"); // after the end: nothing to send it to
}

void test_process_burst_and_last_words() {
    kin::Process child;
    assert(child.start({.args = {self, "--burst", "20000"}, .pipe_input = false}));
    const std::vector<std::string> lines = lines_from(child, 20001);
    assert(lines.size() == 20001);
    assert(lines[0] == "line 0" && lines[19999] == "line 19999");
    assert(lines[20000] == "unfinished"); // a last line without an ending still comes out
    assert(child.exit_code() == 0);
}

void test_process_kill_and_errors() {
    kin::Process stuck;
    assert(stuck.start({.args = {self, "--echo"}}));
    stuck.kill(true);
    assert(stuck.wait(10s) && !stuck.running());

    kin::Process complaining;
    assert(complaining.start({.args = {self, "--complain"}, .pipe_input = false, .errors_to_output = true}));
    assert(complaining.wait(10s) && complaining.exit_code() == 2);
    assert(complaining.read_line() == "oops");

    kin::Process missing;
    assert(!missing.start({.args = {"kin-no-such-program-anywhere"}}) || !missing.wait(10s) ||
           missing.exit_code() != 0); // some systems report a failed exec as an exit code
    kin::Process nothing;
    assert(!nothing.start({}) && !nothing.error().empty());

    // A Process that goes away while its child runs ends the child.
    {
        kin::Process left;
        assert(left.start({.args = {self, "--echo"}}));
    }
    kin::Process moved;
    {
        kin::Process first;
        assert(first.start({.args = {self, "--echo"}}));
        moved = std::move(first);
    }
    moved.write("still here\n");
    assert((lines_from(moved, 1) == std::vector<std::string>{"echo:still here"}));
}

void test_dropped_files() {
    kin::Input input;
    assert(!input.has_dropped_files() && !input.drop_position());
    SDL_Event begin{};
    begin.type = SDL_EVENT_DROP_BEGIN;
    begin.drop.x = 10.0f;
    begin.drop.y = 20.0f;
    kin::InputFrameTestHook::event(input, begin);
    SDL_Event moving = begin;
    moving.type = SDL_EVENT_DROP_POSITION;
    moving.drop.x = 30.0f;
    kin::InputFrameTestHook::event(input, moving);
    assert((input.drop_position() == kin::Vec2f{30.0f, 20.0f})); // a drag is over the window

    char name[] = "/tmp/caf\xc3\xa9.png"; // UTF-8
    SDL_Event drop{};
    drop.type = SDL_EVENT_DROP_FILE;
    drop.drop.windowID = 4;
    drop.drop.x = 31.0f;
    drop.drop.y = 21.0f;
    drop.drop.data = name;
    kin::InputFrameTestHook::event(input, drop);
    SDL_Event done{};
    done.type = SDL_EVENT_DROP_COMPLETE;
    kin::InputFrameTestHook::event(input, done);
    assert(!input.drop_position());

    input.add_dropped_file({.path = "notes.txt", .pos = {1.0f, 2.0f}});
    // Drops wait however many frames pass until they are taken.
    input.begin_frame();
    input.consume_frame_edges();
    assert(input.has_dropped_files());
    const std::vector<kin::DroppedFile> dropped = input.take_dropped_files();
    assert(dropped.size() == 2 && !input.has_dropped_files());
    assert(dropped[0].path == std::filesystem::path(u8"/tmp/café.png"));
    assert((dropped[0].window == 4 && dropped[0].pos == kin::Vec2f{31.0f, 21.0f}));
    assert(dropped[1].path == "notes.txt");
}

void test_file_dialogs() {
    kin::App app{{.mode = kin::AppMode::Headless}};
    kin::FileDialogs dialogs{app};
    // Headless runs never show a dialog...
    const kin::u32 refused = dialogs.open_file(nullptr, {.filters = {{"Images", "png;jpg"}}});
    // ...but answer what tests and agents queued, in any run.
    dialogs.answer_next({"a.png", "b.png"});
    dialogs.answer_next({});
    const kin::u32 chosen = dialogs.open_file(nullptr, {.many = true});
    const kin::u32 cancelled = dialogs.save_file(nullptr);
    std::vector<kin::FileDialogResult> results = dialogs.take_results();
    assert(results.size() == 3);
    assert(results[0].request == refused && !results[0].error.empty() && !results[0].cancelled());
    assert(results[1].request == chosen && results[1].paths.size() == 2 && results[1].paths[1] == "b.png");
    assert(results[2].request == cancelled && results[2].cancelled());
    assert(dialogs.take_results().empty());
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        return child_main(argc, argv);
    }
    self = std::filesystem::absolute(argv[0]).string();
    test_process_round_trip();
    test_process_end_of_input();
    test_process_burst_and_last_words();
    test_process_kill_and_errors();
    test_dropped_files();
    test_file_dialogs();
    return 0;
}
