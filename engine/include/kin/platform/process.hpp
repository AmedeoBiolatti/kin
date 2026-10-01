#pragma once

#include <kin/core/types.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kin {

struct ProcessOptions {
    // The program, then its arguments. A program without a path is looked up on PATH.
    std::vector<std::string> args;
    // Where it runs. Empty: the app's current directory.
    std::filesystem::path working_directory;
    // Write to its standard input (write(), close_input()). Off: it reads nothing.
    bool pipe_input = true;
    // Read its standard output (read(), read_line()). Off: it shares the app's.
    bool capture_output = true;
    // Its standard error joins the captured output. Off: it shares the app's.
    bool errors_to_output = false;
};

// A child process the app talks to through its standard input and output,
// e.g. a tool or a helper written in another language. Nothing here blocks the
// frame: writes are queued and sent as the pipe takes them, and reads return
// what has arrived so far. Call pump() once a frame (or let read_line() do it).
//
//   kin::Process helper;
//   helper.start({.args = {"python3", "-m", "helper"}, .working_directory = tools});
//   helper.write("{\"ask\": 1}\n");
//   while (auto line = helper.read_line()) handle(*line);   // each frame
//
// A Process that is destroyed while its child still runs ends the child.
class Process {
public:
    Process();
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;

    // Starts the child; false if it could not be started (error() says why).
    // Ends any child this Process was running.
    bool start(const ProcessOptions& options);
    bool started() const { return _process != nullptr; }

    // Whether the child is still running. Checks without waiting; once it has
    // exited, its exit code is kept and its last output read.
    bool running();
    std::optional<i64> exit_code() const { return _exit_code; }

    // Queues `data` for the child's input and sends what the pipe takes now.
    void write(std::string_view data);
    // Ends the child's input once the queued writes are sent: it reads end of file.
    void close_input();

    // Sends queued input and reads the output that has arrived.
    void pump();
    // All output read so far (pumping first), taken.
    std::string read();
    // The next complete line of output, without its line ending, if one has
    // arrived (pumping when none is waiting). After the child exits, a last
    // unfinished line comes out too.
    std::optional<std::string> read_line();

    // Pumps until the child exits or `timeout` passes; true if it exited.
    bool wait(std::chrono::milliseconds timeout);
    // Asks the child to end (force: ends it at once).
    void kill(bool force = false);

    const std::string& error() const { return _error; }

private:
    void flush_input();
    void read_output();
    void finish();
    void release();

    void* _process = nullptr; // SDL_Process
    std::string _to_write;
    bool _close_input = false;
    std::string _output;
    std::size_t _output_read = 0; // what read_line() has taken from _output
    std::optional<i64> _exit_code;
    std::string _error;
};

} // namespace kin
