#include <kin/platform/process.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <thread>
#include <utility>

namespace kin {

namespace {

SDL_Process* sdl(void* process) {
    return static_cast<SDL_Process*>(process);
}

} // namespace

Process::Process() = default;

Process::~Process() {
    release();
}

Process::Process(Process&& other) noexcept
    : _process(std::exchange(other._process, nullptr)),
      _to_write(std::move(other._to_write)),
      _close_input(other._close_input),
      _output(std::move(other._output)),
      _output_read(other._output_read),
      _exit_code(other._exit_code),
      _error(std::move(other._error)) {
}

Process& Process::operator=(Process&& other) noexcept {
    if (this != &other) {
        release();
        _process = std::exchange(other._process, nullptr);
        _to_write = std::move(other._to_write);
        _close_input = other._close_input;
        _output = std::move(other._output);
        _output_read = other._output_read;
        _exit_code = other._exit_code;
        _error = std::move(other._error);
    }
    return *this;
}

void Process::release() {
    if (!_process) {
        return;
    }
    int code = 0;
    if (!SDL_WaitProcess(sdl(_process), false, &code)) {
        SDL_KillProcess(sdl(_process), true);
        SDL_WaitProcess(sdl(_process), true, &code);
    }
    SDL_DestroyProcess(sdl(_process));
    _process = nullptr;
}

bool Process::start(const ProcessOptions& options) {
    release();
    _to_write.clear();
    _close_input = false;
    _output.clear();
    _output_read = 0;
    _exit_code.reset();
    _error.clear();
    if (options.args.empty()) {
        _error = "no program to start";
        return false;
    }

    std::vector<const char*> args;
    for (const std::string& arg : options.args) {
        args.push_back(arg.c_str());
    }
    args.push_back(nullptr);
    const std::u8string directory = options.working_directory.u8string();
    const std::string working_directory{directory.begin(), directory.end()};

    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(args.data()));
    if (!working_directory.empty()) {
        SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, working_directory.c_str());
    }
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                          options.pipe_input ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                          options.capture_output ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_INHERITED);
    if (options.capture_output && options.errors_to_output) {
        SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
    } else {
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    }
    _process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!_process) {
        _error = std::string("could not start ") + options.args.front() + ": " + SDL_GetError();
        return false;
    }
    return true;
}

bool Process::running() {
    if (!_process) {
        return false;
    }
    if (_exit_code) {
        return false;
    }
    int code = 0;
    if (SDL_WaitProcess(sdl(_process), false, &code)) {
        _exit_code = code;
        finish();
        return false;
    }
    return true;
}

void Process::finish() {
    // It has exited: whatever it wrote last is still in the pipe.
    read_output();
    _to_write.clear();
}

void Process::write(std::string_view data) {
    if (!_process || _close_input) {
        return;
    }
    _to_write.append(data);
    flush_input();
}

void Process::close_input() {
    _close_input = true;
    flush_input();
}

void Process::flush_input() {
    if (!_process) {
        return;
    }
    SDL_IOStream* input = SDL_GetProcessInput(sdl(_process));
    if (!input) {
        _to_write.clear();
        return;
    }
    if (!_to_write.empty()) {
        const std::size_t sent = SDL_WriteIO(input, _to_write.data(), _to_write.size());
        _to_write.erase(0, sent);
        SDL_FlushIO(input);
    }
    if (_close_input && _to_write.empty()) {
        // Closing the stream ends the child's input; SDL forgets it.
        SDL_CloseIO(input);
    }
}

void Process::read_output() {
    if (!_process) {
        return;
    }
    SDL_IOStream* output = SDL_GetProcessOutput(sdl(_process));
    if (!output) {
        return;
    }
    std::array<char, 4096> buffer{};
    for (;;) {
        const std::size_t got = SDL_ReadIO(output, buffer.data(), buffer.size());
        if (got == 0) {
            break; // nothing more yet, or the child closed it
        }
        _output.append(buffer.data(), got);
    }
}

void Process::pump() {
    flush_input();
    read_output();
    running();
}

std::string Process::read() {
    pump();
    std::string rest = _output.substr(_output_read);
    _output.clear();
    _output_read = 0;
    return rest;
}

std::optional<std::string> Process::read_line() {
    std::size_t end = _output.find('\n', _output_read);
    if (end == std::string::npos) {
        pump(); // only when no whole line is waiting
        end = _output.find('\n', _output_read);
    }
    if (end == std::string::npos) {
        if (_exit_code && _output_read < _output.size()) {
            return read(); // its last words, unfinished
        }
        return std::nullopt;
    }
    std::string line = _output.substr(_output_read, end - _output_read);
    _output_read = end + 1;
    // Drop what was read now and then, not per line (that would be quadratic).
    if (_output_read == _output.size()) {
        _output.clear();
        _output_read = 0;
    } else if (_output_read > 65536) {
        _output.erase(0, _output_read);
        _output_read = 0;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

bool Process::wait(std::chrono::milliseconds timeout) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        pump();
        if (!running()) {
            return _process != nullptr;
        }
        if (std::chrono::steady_clock::now() >= until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Process::kill(bool force) {
    if (_process && !_exit_code) {
        SDL_KillProcess(sdl(_process), force);
    }
}

} // namespace kin
