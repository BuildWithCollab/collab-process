#include "../platform.hpp"
#include "../running_process_impl.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#pragma comment(lib, "user32.lib")  // GetConsoleWindow / ShowWindow

#include <chrono>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace collab::process::detail {

// Swallows any console ctrl event so our process doesn't get killed while
// we're temporarily attached to the child's console to deliver one.
static BOOL WINAPI swallow_ctrl(DWORD /*type*/) {
    return TRUE;
}


// ConPTY-backed Windows implementation — used when CommandConfig.interruptible
// is set. The child runs under a pseudoconsole, so CTRL_C_EVENT can be delivered
// by writing 0x03 to the console's input pipe. Trade-off: the pseudoconsole
// combines stdout and stderr into a single output stream.
struct ConPtyProcessImpl : RunningProcess::Impl {
    HANDLE process_handle = nullptr;
    HANDLE job_handle = nullptr;
    HPCON pty = nullptr;
    int process_id = 0;

    HANDLE input_write = nullptr;   // parent writes → child's console input
    HANDLE output_read = nullptr;   // parent reads ← child's console output

    PPROC_THREAD_ATTRIBUTE_LIST attr_list = nullptr;

    std::thread output_reader_thread;
    std::thread stdin_thread;

    collab::process::move_only_function<void(std::string_view)> on_stdout;
    // on_stderr is intentionally dropped — ConPTY has no separate stderr stream.

    std::string stdout_content;
    bool waited = false;

    void join_stdin_thread() {
        if (stdin_thread.joinable()) stdin_thread.join();
    }

    void join_output_reader() {
        if (output_reader_thread.joinable()) output_reader_thread.join();
    }

    void close_pty() {
        if (pty) {
            ClosePseudoConsole(pty);
            pty = nullptr;
        }
    }

    ~ConPtyProcessImpl() override {
        // Order matters: close PTY first so the output pipe hits EOF and the
        // reader thread can exit; then join threads; then close remaining
        // handles. ~RunningProcess already called kill() if the child was alive,
        // so ClosePseudoConsole won't block on a live process.
        close_pty();
        join_stdin_thread();
        join_output_reader();
        if (input_write) CloseHandle(input_write);
        if (output_read) CloseHandle(output_read);
        if (process_handle) CloseHandle(process_handle);
        if (job_handle) CloseHandle(job_handle);
        if (attr_list) {
            DeleteProcThreadAttributeList(attr_list);
            HeapFree(GetProcessHeap(), 0, attr_list);
        }
    }

    auto pid() const -> int override { return process_id; }

    auto is_alive() const -> bool override {
        if (!process_handle) return false;
        DWORD exit_code;
        GetExitCodeProcess(process_handle, &exit_code);
        return exit_code == STILL_ACTIVE;
    }

    void read_output() {
        char buf[4096];
        DWORD bytes_read;
        while (ReadFile(output_read, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
            std::string_view chunk(buf, bytes_read);
            if (on_stdout) on_stdout(chunk);
            stdout_content.append(chunk);
        }
    }

    auto build_result(bool timed_out = false) -> Result {
        auto trim = [](std::string& s) {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
        };
        trim(stdout_content);

        DWORD exit_code = 0;
        if (process_handle) GetExitCodeProcess(process_handle, &exit_code);

        return Result{
            .stdout_content = std::move(stdout_content),
            .stderr_content = {},  // combined into stdout under ConPTY
            .exit_code = timed_out
                ? std::optional<int>{}
                : std::optional<int>{static_cast<int>(exit_code)},
            .timed_out = timed_out,
        };
    }

    auto wait() -> std::expected<Result, SpawnError> override {
        if (!process_handle)
            return std::unexpected(SpawnError{SpawnError::platform_error, 0});

        if (!waited) {
            join_stdin_thread();
            WaitForSingleObject(process_handle, INFINITE);
            // ConPTY renders asynchronously — give it a moment to flush the
            // child's final output before we close the console and force EOF.
            // Matches Microsoft's own EchoCon sample (which uses Sleep(500)).
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            close_pty();
            join_output_reader();
            waited = true;
        }
        return build_result();
    }

    auto wait_for(std::chrono::milliseconds timeout) -> std::optional<Result> override {
        if (!process_handle) return std::nullopt;

        DWORD wait_ms = static_cast<DWORD>(timeout.count());
        DWORD wait_result = WaitForSingleObject(process_handle, wait_ms);
        if (wait_result == WAIT_TIMEOUT) return std::nullopt;

        if (!waited) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            close_pty();
            join_output_reader();
            waited = true;
        }
        return build_result();
    }

    auto wait_for_and_kill(std::chrono::milliseconds timeout)
        -> std::expected<Result, SpawnError> override {
        if (!process_handle)
            return std::unexpected(SpawnError{SpawnError::platform_error, 0});

        DWORD wait_ms = static_cast<DWORD>(timeout.count());
        DWORD wait_result = WaitForSingleObject(process_handle, wait_ms);

        if (wait_result == WAIT_TIMEOUT) {
            if (job_handle) TerminateJobObject(job_handle, 1);
            else TerminateProcess(process_handle, 1);
            WaitForSingleObject(process_handle, 5000);
            close_pty();
            join_output_reader();
            waited = true;
            return build_result(true);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        close_pty();
        join_output_reader();
        waited = true;
        return build_result();
    }

    auto stop(std::chrono::milliseconds /*grace*/) -> StopResult override {
        if (!is_alive()) return StopResult::not_running;
        // Under ConPTY, Ctrl+Break isn't cleanly targeted at the child group
        // the way it is for a bare CREATE_NEW_PROCESS_GROUP child. Go straight
        // to tree kill — callers who want the polite path should use interrupt()
        // first and then fall back to stop().
        if (job_handle) TerminateJobObject(job_handle, 1);
        else TerminateProcess(process_handle, 1);
        WaitForSingleObject(process_handle, 5000);
        return StopResult::killed;
    }

    auto kill() -> bool override {
        if (!is_alive()) return false;
        if (job_handle) return TerminateJobObject(job_handle, 1) != 0;
        return TerminateProcess(process_handle, 1) != 0;
    }

    auto interrupt() -> bool override {
        if (!is_alive()) return false;
        // The child runs in its own console (the ConPTY's conhost). Attach
        // *ourselves* to that console, signal the group, then detach and
        // re-attach to our own hidden console. This is the classic
        // AttachConsole Ctrl+C dance.

        // Install a swallowing handler BEFORE detaching so we never go
        // through a window where the default action (terminate) is active.
        SetConsoleCtrlHandler(swallow_ctrl, TRUE);

        FreeConsole();
        bool sent = false;
        if (AttachConsole(process_id)) {
            // Re-register on the new console — handler table is per-console.
            SetConsoleCtrlHandler(swallow_ctrl, TRUE);
            sent = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != 0;
            // Give the child's handler a moment to run before we detach.
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
            FreeConsole();
        }

        // Restore our own hidden console so subsequent spawns still work.
        if (AllocConsole()) {
            HWND hwnd = GetConsoleWindow();
            if (hwnd) ShowWindow(hwnd, SW_HIDE);
            HANDLE conout = CreateFileW(L"CONOUT$",
                GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);
            HANDLE conin = CreateFileW(L"CONIN$",
                GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);
            if (conout != INVALID_HANDLE_VALUE)
                SetStdHandle(STD_OUTPUT_HANDLE, conout);
            if (conin != INVALID_HANDLE_VALUE)
                SetStdHandle(STD_INPUT_HANDLE, conin);
        }
        SetConsoleCtrlHandler(swallow_ctrl, FALSE);
        return sent;
    }

    void release_for_detach() override {
        if (job_handle) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
            SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation,
                                    &job_info, sizeof(job_info));
        }
    }
};

static auto build_command_line(const std::filesystem::path& program,
                               const std::vector<std::string>& args) -> std::wstring {
    std::wostringstream cmd;
    auto prog_str = program.wstring();
    cmd << L"\"" << prog_str << L"\"";
    for (auto& a : args) {
        cmd << L" ";
        std::wstring wa(a.begin(), a.end());
        bool needs_quote = wa.empty()
            || wa.find(L' ') != std::wstring::npos
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

// ConPTY requires the *parent* to have a real console — without one, the
// PTY routes the child's stdio back to an invalid handle and output is lost.
// Ensure we have one, invisibly, before creating the pseudoconsole. Safe to
// call repeatedly; AllocConsole no-ops when a console already exists.
static void ensure_parent_console() {
    DWORD mode = 0;
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode))
        return;  // already have a real console — nothing to do

    // Fully detach whatever we may have inherited and allocate a fresh hidden
    // console. Without FreeConsole first, AllocConsole fails if we're already
    // attached to one (e.g. bash's terminal subsystem).
    FreeConsole();
    if (!AllocConsole())
        return;

    HWND hwnd = GetConsoleWindow();
    if (hwnd) ShowWindow(hwnd, SW_HIDE);

    HANDLE conout = CreateFileW(L"CONOUT$",
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE conin = CreateFileW(L"CONIN$",
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (conout != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_OUTPUT_HANDLE, conout);
        SetStdHandle(STD_ERROR_HANDLE, conout);
    }
    if (conin != INVALID_HANDLE_VALUE)
        SetStdHandle(STD_INPUT_HANDLE, conin);
}

auto spawn_conpty(SpawnParams params)
    -> std::expected<std::unique_ptr<RunningProcess::Impl>, SpawnError> {

    ensure_parent_console();

    auto impl = std::make_unique<ConPtyProcessImpl>();
    impl->on_stdout = std::move(params.on_stdout);

    // Two pipes: input (we write → child reads) and output (child writes → we read).
    HANDLE input_read = nullptr, input_write = nullptr;
    HANDLE output_read = nullptr, output_write = nullptr;
    if (!CreatePipe(&input_read, &input_write, nullptr, 0))
        return std::unexpected(SpawnError{SpawnError::pipe_creation_failed,
                                          static_cast<int>(GetLastError())});
    if (!CreatePipe(&output_read, &output_write, nullptr, 0)) {
        int err = static_cast<int>(GetLastError());
        CloseHandle(input_read); CloseHandle(input_write);
        return std::unexpected(SpawnError{SpawnError::pipe_creation_failed, err});
    }

    COORD size{80, 25};
    HPCON pty = nullptr;
    HRESULT hr = CreatePseudoConsole(size, input_read, output_write, 0, &pty);
    // CreatePseudoConsole takes ownership of the read/write halves it needs.
    CloseHandle(input_read);
    CloseHandle(output_write);
    if (FAILED(hr)) {
        CloseHandle(input_write); CloseHandle(output_read);
        return std::unexpected(SpawnError{SpawnError::platform_error, static_cast<int>(hr)});
    }

    // Build the startup info with the PSEUDOCONSOLE attribute.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    auto* attr_list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attr_size));
    if (!attr_list) {
        ClosePseudoConsole(pty);
        CloseHandle(input_write); CloseHandle(output_read);
        return std::unexpected(SpawnError{SpawnError::platform_error, ERROR_NOT_ENOUGH_MEMORY});
    }
    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
        int err = static_cast<int>(GetLastError());
        HeapFree(GetProcessHeap(), 0, attr_list);
        ClosePseudoConsole(pty);
        CloseHandle(input_write); CloseHandle(output_read);
        return std::unexpected(SpawnError{SpawnError::platform_error, err});
    }
    if (!UpdateProcThreadAttribute(attr_list, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pty, sizeof(pty),
            nullptr, nullptr)) {
        int err = static_cast<int>(GetLastError());
        DeleteProcThreadAttributeList(attr_list);
        HeapFree(GetProcessHeap(), 0, attr_list);
        ClosePseudoConsole(pty);
        CloseHandle(input_write); CloseHandle(output_read);
        return std::unexpected(SpawnError{SpawnError::platform_error, err});
    }

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attr_list;

    auto cmd_line = build_command_line(params.resolved_program, params.args);
    auto env_block = build_env_block_wide(params.env_entries);
    const wchar_t* cwd = params.working_dir.empty() ? nullptr
                                                    : params.working_dir.c_str();

    DWORD flags = EXTENDED_STARTUPINFO_PRESENT;
    // Skip detached + unicode-env flags for now while debugging the attach.

    // Job object for tree kill
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
        job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &job_info, sizeof(job_info));
    }

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        params.needs_cmd_wrapper ? nullptr : params.resolved_program.c_str(),
        cmd_line.data(),
        nullptr, nullptr,
        FALSE,
        flags,
        nullptr,  // inherit parent's env for now
        cwd,
        &si.StartupInfo, &pi);

    if (!ok) {
        int err = static_cast<int>(GetLastError());
        DeleteProcThreadAttributeList(attr_list);
        HeapFree(GetProcessHeap(), 0, attr_list);
        ClosePseudoConsole(pty);
        CloseHandle(input_write); CloseHandle(output_read);
        if (job) CloseHandle(job);
        return std::unexpected(SpawnError{SpawnError::platform_error, err});
    }

    if (job) AssignProcessToJobObject(job, pi.hProcess);
    CloseHandle(pi.hThread);

    impl->process_handle = pi.hProcess;
    impl->job_handle = job;
    impl->process_id = static_cast<int>(pi.dwProcessId);
    impl->pty = pty;
    impl->input_write = input_write;
    impl->output_read = output_read;
    impl->attr_list = attr_list;

    // Stdin writer — content / file modes only. We keep input_write open after
    // the writer finishes so interrupt() can still use it.
    if (params.stdin_mode == CommandConfig::StdinMode::content) {
        impl->stdin_thread = std::thread(
            [content = std::move(params.stdin_content), h = input_write] {
                if (!content.empty()) {
                    DWORD written = 0;
                    WriteFile(h, content.data(),
                              static_cast<DWORD>(content.size()), &written, nullptr);
                }
            });
    } else if (params.stdin_mode == CommandConfig::StdinMode::file) {
        impl->stdin_thread = std::thread(
            [path = std::move(params.stdin_path), h = input_write] {
                HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    char buf[4096];
                    DWORD bytes_read;
                    while (ReadFile(file, buf, sizeof(buf), &bytes_read, nullptr)
                           && bytes_read > 0) {
                        DWORD written;
                        WriteFile(h, buf, bytes_read, &written, nullptr);
                    }
                    CloseHandle(file);
                }
            });
    }

    // Output reader — always on, so callbacks fire live and nothing can
    // deadlock on pipe-buffer backpressure.
    auto* raw = impl.get();
    impl->output_reader_thread = std::thread([raw] { raw->read_output(); });

    return impl;
}

}  // namespace collab::process::detail
