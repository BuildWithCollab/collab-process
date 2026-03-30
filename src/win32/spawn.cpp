#include "../platform.hpp"
#include "../running_process_impl.hpp"

#include <windows.h>

#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace collab::process::detail {

void reset_console_for_interactive();  // console.cpp

// Windows implementation of RunningProcess::Impl
struct Win32ProcessImpl : RunningProcess::Impl {
    HANDLE process_handle = nullptr;
    HANDLE job_handle = nullptr;
    int process_id = 0;

    // Pipes for captured output
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;

    // Callbacks
    collab::process::move_only_function<void(std::string_view)> on_stdout;
    collab::process::move_only_function<void(std::string_view)> on_stderr;

    // Captured output (populated by wait)
    std::string stdout_content;
    std::string stderr_content;
    bool waited = false;

    ~Win32ProcessImpl() override {
        if (stdout_read) CloseHandle(stdout_read);
        if (stderr_read) CloseHandle(stderr_read);
        if (process_handle) CloseHandle(process_handle);
        if (job_handle) CloseHandle(job_handle);
    }

    auto pid() const -> int override { return process_id; }

    auto is_alive() const -> bool override {
        if (!process_handle) return false;
        DWORD exit_code;
        GetExitCodeProcess(process_handle, &exit_code);
        return exit_code == STILL_ACTIVE;
    }

    void read_pipes() {
        // Read stdout
        auto read_handle = [](HANDLE h, std::string& out,
                              collab::process::move_only_function<void(std::string_view)>& cb) {
            if (!h) return;
            char buf[4096];
            DWORD bytes_read;
            while (ReadFile(h, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
                std::string_view chunk(buf, bytes_read);
                if (cb) cb(chunk);
                out.append(chunk);
            }
        };

        // If we have both stdout and stderr pipes, read them concurrently
        // to avoid deadlock
        if (stdout_read && stderr_read) {
            std::thread stderr_thread([&] {
                read_handle(stderr_read, stderr_content, on_stderr);
            });
            read_handle(stdout_read, stdout_content, on_stdout);
            stderr_thread.join();
        } else {
            read_handle(stdout_read, stdout_content, on_stdout);
            read_handle(stderr_read, stderr_content, on_stderr);
        }
    }

    auto wait() -> std::expected<Result, SpawnError> override {
        if (!process_handle)
            return std::unexpected(SpawnError{SpawnError::platform_error, 0});

        if (!waited) {
            read_pipes();
            WaitForSingleObject(process_handle, INFINITE);
            waited = true;
        }

        DWORD exit_code = 0;
        GetExitCodeProcess(process_handle, &exit_code);

        // Trim trailing whitespace
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
        };
        trim(stdout_content);
        trim(stderr_content);

        return Result{
            .stdout_content = std::move(stdout_content),
            .stderr_content = std::move(stderr_content),
            .exit_code = static_cast<int>(exit_code),
            .timed_out = false,
        };
    }

    auto wait_for(std::chrono::milliseconds timeout) -> std::expected<Result, SpawnError> override {
        if (!process_handle)
            return std::unexpected(SpawnError{SpawnError::platform_error, 0});

        if (!waited) {
            read_pipes();
            waited = true;
        }

        DWORD wait_ms = static_cast<DWORD>(timeout.count());
        DWORD wait_result = WaitForSingleObject(process_handle, wait_ms);

        if (wait_result == WAIT_TIMEOUT) {
            TerminateProcess(process_handle, 1);
            WaitForSingleObject(process_handle, 5000);

            return Result{
                .stdout_content = std::move(stdout_content),
                .stderr_content = std::move(stderr_content),
                .exit_code = -1,
                .timed_out = true,
            };
        }

        DWORD exit_code = 0;
        GetExitCodeProcess(process_handle, &exit_code);

        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
        };
        trim(stdout_content);
        trim(stderr_content);

        return Result{
            .stdout_content = std::move(stdout_content),
            .stderr_content = std::move(stderr_content),
            .exit_code = static_cast<int>(exit_code),
            .timed_out = false,
        };
    }

    auto stop(std::chrono::milliseconds grace) -> StopResult override {
        if (!is_alive()) return StopResult::not_running;

        // Send CTRL_BREAK to the process group
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, process_id);

        // Wait for grace period
        DWORD wait_result = WaitForSingleObject(
            process_handle, static_cast<DWORD>(grace.count()));

        if (wait_result == WAIT_OBJECT_0)
            return StopResult::stopped_gracefully;

        // Escalate — terminate the job (tree kill)
        if (job_handle)
            TerminateJobObject(job_handle, 1);
        else
            TerminateProcess(process_handle, 1);

        WaitForSingleObject(process_handle, 5000);
        return StopResult::killed;
    }

    auto kill() -> bool override {
        if (!is_alive()) return false;

        if (job_handle)
            return TerminateJobObject(job_handle, 1) != 0;
        return TerminateProcess(process_handle, 1) != 0;
    }
};

// Build a command line string with proper quoting
static auto build_command_line(bool needs_cmd_wrapper,
                               const std::filesystem::path& program,
                               const std::vector<std::string>& args) -> std::wstring {
    std::wostringstream cmd;

    if (needs_cmd_wrapper) {
        cmd << L"cmd /c ";
    }

    // Quote the program path
    auto prog_str = program.wstring();
    cmd << L"\"" << prog_str << L"\"";

    for (auto& a : args) {
        cmd << L" ";
        std::wstring wa(a.begin(), a.end());
        bool needs_quote = wa.empty() || wa.find(L' ') != std::wstring::npos
            || wa.find(L'\t') != std::wstring::npos
            || wa.find(L'"') != std::wstring::npos;
        if (needs_quote) {
            cmd << L"\"";
            for (wchar_t c : wa) {
                if (c == L'"') cmd << L"\\\"";
                else cmd << c;
            }
            cmd << L"\"";
        } else {
            cmd << wa;
        }
    }

    return cmd.str();
}

// Build a null-terminated wide environment block for CreateProcessW
static auto build_env_block_wide(const std::vector<std::string>& entries) -> std::wstring {
    std::wstring block;
    for (auto& entry : entries) {
        std::wstring wide(entry.begin(), entry.end());
        block.append(wide);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

auto platform_spawn(SpawnParams params)
    -> std::expected<std::unique_ptr<RunningProcess::Impl>, SpawnError> {

    auto impl = std::make_unique<Win32ProcessImpl>();
    impl->on_stdout = std::move(params.on_stdout);
    impl->on_stderr = std::move(params.on_stderr);

    // Determine if interactive (all streams inherit, not detached)
    bool is_interactive = (params.stdout_mode == CommandConfig::OutputMode::inherit)
        && (params.stderr_mode == CommandConfig::OutputMode::inherit)
        && params.stdin_content.empty()
        && params.stdin_path.empty()
        && !params.stdin_closed
        && !params.detached;

    if (is_interactive)
        reset_console_for_interactive();

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // Stdin pipe
    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    bool pipe_stdin = !params.stdin_content.empty() || !params.stdin_path.empty();

    if (pipe_stdin) {
        if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0))
            return std::unexpected(SpawnError{SpawnError::pipe_creation_failed,
                                              static_cast<int>(GetLastError())});
        SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    }

    // Stdout pipe
    HANDLE stdout_read_h = nullptr, stdout_write = nullptr;
    if (params.stdout_mode != CommandConfig::OutputMode::inherit) {
        if (!CreatePipe(&stdout_read_h, &stdout_write, &sa, 0)) {
            if (stdin_read) { CloseHandle(stdin_read); CloseHandle(stdin_write); }
            return std::unexpected(SpawnError{SpawnError::pipe_creation_failed,
                                              static_cast<int>(GetLastError())});
        }
        SetHandleInformation(stdout_read_h, HANDLE_FLAG_INHERIT, 0);
    }

    // Stderr pipe (or merge with stdout)
    HANDLE stderr_read_h = nullptr, stderr_write = nullptr;
    if (params.stderr_merge && stdout_write) {
        // Duplicate stdout write handle for stderr
        DuplicateHandle(GetCurrentProcess(), stdout_write,
                        GetCurrentProcess(), &stderr_write,
                        0, TRUE, DUPLICATE_SAME_ACCESS);
    } else if (params.stderr_mode != CommandConfig::OutputMode::inherit) {
        if (!CreatePipe(&stderr_read_h, &stderr_write, &sa, 0)) {
            if (stdin_read) { CloseHandle(stdin_read); CloseHandle(stdin_write); }
            if (stdout_read_h) { CloseHandle(stdout_read_h); CloseHandle(stdout_write); }
            return std::unexpected(SpawnError{SpawnError::pipe_creation_failed,
                                              static_cast<int>(GetLastError())});
        }
        SetHandleInformation(stderr_read_h, HANDLE_FLAG_INHERIT, 0);
    }

    // NUL handle for discard mode
    HANDLE nul_handle = nullptr;
    bool need_nul = (params.stdout_mode == CommandConfig::OutputMode::discard && !stdout_write)
        || (params.stderr_mode == CommandConfig::OutputMode::discard && !stderr_write);
    if (need_nul || params.stdout_mode == CommandConfig::OutputMode::discard
        || params.stderr_mode == CommandConfig::OutputMode::discard) {
        nul_handle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                                 &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    // Setup STARTUPINFO
    STARTUPINFOW si = {};
    si.cb = sizeof(si);

    if (!is_interactive) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = pipe_stdin ? stdin_read
                       : params.stdin_closed ? nullptr
                       : GetStdHandle(STD_INPUT_HANDLE);

        if (params.stdout_mode == CommandConfig::OutputMode::capture)
            si.hStdOutput = stdout_write;
        else if (params.stdout_mode == CommandConfig::OutputMode::discard)
            si.hStdOutput = nul_handle;
        else
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);

        if (params.stderr_merge)
            si.hStdError = stderr_write ? stderr_write : stdout_write;
        else if (params.stderr_mode == CommandConfig::OutputMode::capture)
            si.hStdError = stderr_write;
        else if (params.stderr_mode == CommandConfig::OutputMode::discard)
            si.hStdError = nul_handle;
        else
            si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }

    // Build command line and env block
    auto cmd_line = build_command_line(params.needs_cmd_wrapper,
                                       params.resolved_program, params.args);
    auto env_block = build_env_block_wide(params.env_entries);

    const wchar_t* cwd = params.working_dir.empty()
        ? nullptr
        : params.working_dir.c_str();

    // Creation flags
    DWORD flags = 0;
    if (!is_interactive)
        flags |= CREATE_NO_WINDOW;
    if (params.detached)
        flags |= CREATE_NEW_PROCESS_GROUP;
    flags |= CREATE_UNICODE_ENVIRONMENT;

    // Create job object for tree kill support
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
        job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &job_info, sizeof(job_info));
    }

    // Spawn
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        params.needs_cmd_wrapper ? nullptr : params.resolved_program.c_str(),
        cmd_line.data(),
        nullptr, nullptr,
        TRUE,
        flags,
        env_block.data(),
        cwd,
        &si, &pi
    );

    // Cleanup write-side handles (child has them now)
    if (stdin_read) CloseHandle(stdin_read);
    if (stdout_write) CloseHandle(stdout_write);
    if (stderr_write) CloseHandle(stderr_write);
    if (nul_handle) CloseHandle(nul_handle);

    if (!ok) {
        int err = static_cast<int>(GetLastError());
        if (stdin_write) CloseHandle(stdin_write);
        if (stdout_read_h) CloseHandle(stdout_read_h);
        if (stderr_read_h) CloseHandle(stderr_read_h);
        if (job) CloseHandle(job);
        return std::unexpected(SpawnError{SpawnError::platform_error, err});
    }

    // Assign to job object
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);

    CloseHandle(pi.hThread);

    // Write stdin content (in a thread to avoid deadlock with stdout reading)
    if (stdin_write && !params.stdin_content.empty()) {
        std::thread([content = std::move(params.stdin_content), h = stdin_write] {
            DWORD written;
            WriteFile(h, content.data(),
                      static_cast<DWORD>(content.size()), &written, nullptr);
            CloseHandle(h);
        }).detach();
    } else if (stdin_write) {
        CloseHandle(stdin_write);
    }

    impl->process_handle = pi.hProcess;
    impl->job_handle = job;
    impl->process_id = static_cast<int>(pi.dwProcessId);
    impl->stdout_read = stdout_read_h;
    impl->stderr_read = stderr_read_h;

    return impl;
}

}  // namespace collab::process::detail
