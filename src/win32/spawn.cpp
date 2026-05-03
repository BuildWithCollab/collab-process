#include "../platform.hpp"
#include "../running_process_impl.hpp"

#include "collab/process/mode_error.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
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

    // Set before CreateProcess, tracks whether CREATE_NEW_PROCESS_GROUP was
    // applied (i.e. the child was spawned headless).
    // GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid) only has a valid
    // target when pid names a process-group leader, and Windows does *not*
    // reliably return zero for non-leaders — the call can succeed while
    // delivering the signal nowhere. terminate()/interrupt() throw
    // ModeError when this is false rather than lying about delivery.
    bool has_own_group = false;

    // Pipes for captured output
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;

    // Library-side write end of the child's stdin, retained for the
    // lifetime of the handle when StdinMode::pipe was selected. Guarded by
    // stdin_mutex so write_stdin / close_stdin from multiple threads (or
    // from inside an on_stdout callback racing with the main thread)
    // serialize cleanly. stdin_piped distinguishes "this handle never had
    // a writable stdin" from "it had one and we closed it".
    HANDLE stdin_write_handle = nullptr;
    bool stdin_piped = false;
    bool stdin_closed = false;
    std::mutex stdin_mutex;

    // Stdin writer thread — joined before reading pipes or destroying
    std::thread stdin_thread;

    // Background pipe readers — started at spawn time when we own at least
    // one read pipe. Fires on_stdout/on_stderr callbacks live and appends
    // to stdout_content / stderr_content as data arrives, so users can
    // observe output from a spawn()ed process without calling wait().
    //
    // Split per stream (not a single thread that fans out) so detach() can
    // wake each with CancelSynchronousIo on its native handle — necessary
    // because CancelIoEx returns ERROR_NOT_FOUND for synchronous ReadFile
    // on anonymous pipes.
    std::thread stdout_reader_thread;
    std::thread stderr_reader_thread;

    // Callbacks
    collab::process::move_only_function<void(std::string_view)> on_stdout;
    collab::process::move_only_function<void(std::string_view)> on_stderr;

    // Captured output (populated by wait)
    std::string stdout_content;
    std::string stderr_content;
    bool waited = false;

    void join_stdin_thread() {
        if (stdin_thread.joinable())
            stdin_thread.join();
    }

    void join_pipe_readers() {
        if (stdout_reader_thread.joinable())
            stdout_reader_thread.join();
        if (stderr_reader_thread.joinable())
            stderr_reader_thread.join();
    }

    ~Win32ProcessImpl() override {
        // Ensure I/O threads have finished before closing handles — the
        // readers' ReadFile must complete (via EOF when the process exits
        // or cancellation in release_for_detach) before we tear down.
        join_stdin_thread();
        join_pipe_readers();
        if (stdout_read) CloseHandle(stdout_read);
        if (stderr_read) CloseHandle(stderr_read);
        if (stdin_write_handle) {
            CloseHandle(stdin_write_handle);
            stdin_write_handle = nullptr;
        }
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

    static void read_one_pipe(
        HANDLE h, std::string& out,
        collab::process::move_only_function<void(std::string_view)>& cb) {
        if (!h) return;
        char buf[4096];
        DWORD bytes_read;
        while (ReadFile(h, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
            std::string_view chunk(buf, bytes_read);
            if (cb) cb(chunk);
            out.append(chunk);
        }
    }

    auto wait() -> std::expected<Result, SpawnError> override {
        if (!process_handle)
            return std::unexpected(SpawnError{SpawnError::platform_error, 0});

        if (!waited) {
            join_stdin_thread();
            WaitForSingleObject(process_handle, INFINITE);
            // Reader threads (if any) exit once the child closes its
            // write-ends of the pipes, which happens on process exit.
            join_pipe_readers();
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

    // Public wait_for: poll only, no kill. Returns nullopt on timeout.
    // Does NOT start a pipe reader — if the process fills the pipe buffer
    // and blocks, wait_for will still return after the timeout.
    auto wait_for(std::chrono::milliseconds timeout) -> std::optional<Result> override {
        if (!process_handle)
            return std::nullopt;

        DWORD wait_ms = static_cast<DWORD>(timeout.count());
        DWORD wait_result = WaitForSingleObject(process_handle, wait_ms);

        if (wait_result == WAIT_TIMEOUT)
            return std::nullopt;

        // Process exited — reader thread has seen / will imminently see EOF.
        if (!waited) {
            join_pipe_readers();
            waited = true;
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

    // Used by run() — kills the process on timeout. The background reader
    // started at spawn time prevents deadlock when the child fills the
    // pipe buffer (child blocks writing, parent blocks waiting).
    auto wait_for_and_kill(std::chrono::milliseconds timeout) -> std::expected<Result, SpawnError> override {
        if (!process_handle)
            return std::unexpected(SpawnError{SpawnError::platform_error, 0});

        DWORD wait_ms = static_cast<DWORD>(timeout.count());
        DWORD wait_result = WaitForSingleObject(process_handle, wait_ms);

        if (wait_result == WAIT_TIMEOUT) {
            // Kill the process tree so pipes close and the reader can finish
            if (job_handle)
                TerminateJobObject(job_handle, 1);
            else
                TerminateProcess(process_handle, 1);
            WaitForSingleObject(process_handle, 5000);

            join_pipe_readers();
            waited = true;

            auto trim = [](std::string& s) {
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                    s.pop_back();
            };
            trim(stdout_content);
            trim(stderr_content);

            return Result{
                .stdout_content = std::move(stdout_content),
                .stderr_content = std::move(stderr_content),
                .exit_code = std::nullopt,
                .timed_out = true,
            };
        }

        // Process exited within timeout — pipe reader finishes naturally
        join_pipe_readers();
        waited = true;

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

    auto terminate() -> bool override {
        // CTRL_BREAK_EVENT's second parameter is a process-group ID. A pid
        // only names a group when the process was spawned headless
        // (CREATE_NEW_PROCESS_GROUP). Interactive children share the
        // parent's console group — the terminal owns their signals, not
        // us. Calling terminate() on such a handle is a contract violation;
        // throw rather than silently returning false.
        if (!has_own_group)
            throw ModeError("collab::process: terminate() requires headless mode");
        if (!is_alive()) return false;
        return GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                        static_cast<DWORD>(process_id)) != 0;
    }

    auto interrupt() -> bool override {
        // Same mode contract as terminate(): interrupting an interactive
        // child is a programming error (terminal owns its signals).
        if (!has_own_group)
            throw ModeError("collab::process: interrupt() requires headless mode");
        // Even in headless mode, Windows offers no viable mapping —
        // CTRL_C_EVENT broadcasts to the whole console (would hit the
        // parent too) and is disabled for processes in a new process group
        // per MSDN. Honest "not delivered" return.
        return false;
    }

    auto kill() -> bool override {
        if (!is_alive()) return false;

        if (job_handle)
            return TerminateJobObject(job_handle, 1) != 0;
        return TerminateProcess(process_handle, 1) != 0;
    }

    auto write_stdin(std::string_view bytes)
        -> std::expected<void, WriteError> override {
        std::lock_guard lock(stdin_mutex);
        if (!stdin_piped)
            throw ModeError("collab::process: write_stdin requires StdinMode::pipe");
        if (stdin_closed)
            throw ModeError("collab::process: write_stdin called after close_stdin");
        if (bytes.empty()) return {};

        const char* p = bytes.data();
        size_t left = bytes.size();
        while (left > 0) {
            // WriteFile on a synchronous anonymous pipe blocks until either
            // every byte is delivered or the pipe breaks. A short return is
            // theoretically possible (e.g. the child closed the read end
            // mid-call); loop defensively.
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 0x7fffffff));
            DWORD written = 0;
            BOOL ok = WriteFile(stdin_write_handle, p, chunk, &written, nullptr);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE || err == ERROR_NO_DATA)
                    return std::unexpected(
                        WriteError{WriteError::broken_pipe, static_cast<int>(err)});
                return std::unexpected(
                    WriteError{WriteError::platform_error, static_cast<int>(err)});
            }
            if (written == 0) {
                // Defensive: WriteFile reporting success with 0 bytes on
                // a blocking pipe is unexpected. Treat as broken to avoid
                // an infinite loop.
                return std::unexpected(
                    WriteError{WriteError::broken_pipe, static_cast<int>(ERROR_BROKEN_PIPE)});
            }
            p    += written;
            left -= written;
        }
        return {};
    }

    void close_stdin() override {
        std::lock_guard lock(stdin_mutex);
        if (!stdin_piped)
            throw ModeError("collab::process: close_stdin requires StdinMode::pipe");
        if (stdin_closed)
            throw ModeError("collab::process: close_stdin called twice");
        if (stdin_write_handle) {
            CloseHandle(stdin_write_handle);
            stdin_write_handle = nullptr;
        }
        stdin_closed = true;
    }

    void release_for_detach() override {
        // Close the library-side stdin write end first so the detached
        // child sees EOF on stdin as part of being released. After detach
        // the user no longer has a handle to call write_stdin on
        // (RunningProcess is &&-consumed), so this is the last moment to
        // close cleanly.
        {
            std::lock_guard lock(stdin_mutex);
            if (stdin_write_handle) {
                CloseHandle(stdin_write_handle);
                stdin_write_handle = nullptr;
            }
            stdin_closed = true;
        }

        // Remove the kill-on-close flag so closing the job handle
        // doesn't kill the child process.
        if (job_handle) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info = {};
            // Clear the kill-on-close flag
            SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation,
                                    &job_info, sizeof(job_info));
        }

        // Wake the reader threads by closing the pipe read handles.
        //
        // Anonymous-pipe synchronous ReadFile cannot be cancelled by either
        // CancelIoEx (returns ERROR_NOT_FOUND — it only covers async I/O)
        // or CancelSynchronousIo (also ERROR_NOT_FOUND — the pipe driver
        // does not register sync reads in the per-thread cancel table).
        // Closing the handle from this thread is technically UB per MSDN,
        // but in practice on Windows the in-flight ReadFile returns with
        // an error and the reader thread exits cleanly — which is exactly
        // what we need so ~Win32ProcessImpl can join and destroy safely.
        //
        // Null out the members before closing so the destructor's
        // CloseHandle(…) calls don't double-close.
        HANDLE out_closing = stdout_read;
        HANDLE err_closing = stderr_read;
        stdout_read = nullptr;
        stderr_read = nullptr;
        if (out_closing) CloseHandle(out_closing);
        if (err_closing) CloseHandle(err_closing);

        if (stdout_reader_thread.joinable()) stdout_reader_thread.join();
        if (stderr_reader_thread.joinable()) stderr_reader_thread.join();
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
    impl->has_own_group = params.headless;

    // Console-inherit path: all streams inherit AND the child is interactive
    // (not headless) — the child shares the parent's console and process
    // group, so Ctrl+C routes naturally. Headless children get
    // CREATE_NEW_PROCESS_GROUP, which breaks shared-console setup
    // (foreground group tracking etc.).
    bool is_interactive = (params.stdout_mode == CommandConfig::OutputMode::inherit)
        && (params.stderr_mode == CommandConfig::OutputMode::inherit)
        && (params.stdin_mode == CommandConfig::StdinMode::inherit)
        && !params.headless;

    if (is_interactive)
        reset_console_for_interactive();

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // Stdin pipe
    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    bool pipe_stdin = (params.stdin_mode == CommandConfig::StdinMode::content)
                   || (params.stdin_mode == CommandConfig::StdinMode::file)
                   || (params.stdin_mode == CommandConfig::StdinMode::pipe);

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

    // NUL handles for discard mode and stdin_closed
    HANDLE nul_write = nullptr;
    HANDLE nul_read = nullptr;
    if (params.stdout_mode == CommandConfig::OutputMode::discard
        || params.stderr_mode == CommandConfig::OutputMode::discard) {
        nul_write = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE,
                                &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (params.stdin_mode == CommandConfig::StdinMode::closed) {
        nul_read = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ,
                               &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    // Setup STARTUPINFOEX — we use the extended form so we can scope
    // inheritance to an explicit handle list below. Without that list,
    // CreateProcessW(bInheritHandles=TRUE) hands the child every inheritable
    // handle in *our* address space — including pipes our own parent opened
    // for us. Those grandparent pipes then keep the grandparent's pipe
    // readers waiting on EOF until the grandchild exits, which manifests as
    // wait() blocking on unrelated long-running detached processes.
    STARTUPINFOEXW siex = {};
    siex.StartupInfo.cb = sizeof(siex);
    STARTUPINFOW& si = siex.StartupInfo;

    if (!is_interactive) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = pipe_stdin ? stdin_read
                       : nul_read ? nul_read
                       : GetStdHandle(STD_INPUT_HANDLE);

        if (params.stdout_mode == CommandConfig::OutputMode::capture)
            si.hStdOutput = stdout_write;
        else if (params.stdout_mode == CommandConfig::OutputMode::discard)
            si.hStdOutput = nul_write;
        else
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);

        if (params.stderr_merge)
            si.hStdError = stderr_write ? stderr_write : stdout_write;
        else if (params.stderr_mode == CommandConfig::OutputMode::capture)
            si.hStdError = stderr_write;
        else if (params.stderr_mode == CommandConfig::OutputMode::discard)
            si.hStdError = nul_write;
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
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT;
    // CREATE_NO_WINDOW hides the child's console window — but it also
    // effectively detaches the child's console enough that a parent
    // GenerateConsoleCtrlEvent cannot reach it. Headless children need
    // that console reachability to receive CTRL_BREAK later, so keep the
    // shared console in that case.
    if (!is_interactive && !params.headless)
        flags |= CREATE_NO_WINDOW;
    if (params.headless)
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

    // Build the handle-inheritance whitelist.
    //
    // For the non-interactive path every handle the child needs is one we
    // put in STARTUPINFO explicitly — list exactly those. For the
    // interactive path the child shares our console and we don't set
    // STARTF_USESTDHANDLES; still need to list our std handles so they
    // survive the attribute-list filter (an empty list would inherit
    // nothing and the child would see no stdin/stdout/stderr).
    std::vector<HANDLE> inherit_handles;
    auto add_handle = [&](HANDLE h) {
        if (h && h != INVALID_HANDLE_VALUE)
            inherit_handles.push_back(h);
    };
    if (is_interactive) {
        add_handle(GetStdHandle(STD_INPUT_HANDLE));
        add_handle(GetStdHandle(STD_OUTPUT_HANDLE));
        add_handle(GetStdHandle(STD_ERROR_HANDLE));
    } else {
        add_handle(si.hStdInput);
        add_handle(si.hStdOutput);
        add_handle(si.hStdError);
    }
    // Deduplicate — UpdateProcThreadAttribute rejects duplicates (common
    // when stderr_merge routes stderr through the stdout pipe or both
    // streams point at the same NUL handle).
    std::sort(inherit_handles.begin(), inherit_handles.end());
    inherit_handles.erase(
        std::unique(inherit_handles.begin(), inherit_handles.end()),
        inherit_handles.end());

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    auto attr_buf = std::make_unique<BYTE[]>(attr_size);
    auto attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.get());
    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) {
        int err = static_cast<int>(GetLastError());
        if (stdin_read) CloseHandle(stdin_read);
        if (stdout_write) CloseHandle(stdout_write);
        if (stderr_write) CloseHandle(stderr_write);
        if (nul_write) CloseHandle(nul_write);
        if (nul_read) CloseHandle(nul_read);
        if (stdin_write) CloseHandle(stdin_write);
        if (stdout_read_h) CloseHandle(stdout_read_h);
        if (stderr_read_h) CloseHandle(stderr_read_h);
        if (job) CloseHandle(job);
        return std::unexpected(SpawnError{SpawnError::platform_error, err});
    }
    if (!inherit_handles.empty()) {
        UpdateProcThreadAttribute(
            attr_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherit_handles.data(),
            inherit_handles.size() * sizeof(HANDLE),
            nullptr, nullptr);
    }
    siex.lpAttributeList = attr_list;

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
        &siex.StartupInfo, &pi
    );

    DeleteProcThreadAttributeList(attr_list);

    // Cleanup write-side handles and NUL handles (child has them now)
    if (stdin_read) CloseHandle(stdin_read);
    if (stdout_write) CloseHandle(stdout_write);
    if (stderr_write) CloseHandle(stderr_write);
    if (nul_write) CloseHandle(nul_write);
    if (nul_read) CloseHandle(nul_read);

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

    // Write stdin content in a thread to avoid deadlock with stdout reading.
    // Thread is stored in impl and joined before reading pipes or destroying.
    if (stdin_write && params.stdin_mode == CommandConfig::StdinMode::content) {
        impl->stdin_thread = std::thread(
            [content = std::move(params.stdin_content), h = stdin_write] {
                DWORD written;
                WriteFile(h, content.data(),
                          static_cast<DWORD>(content.size()), &written, nullptr);
                CloseHandle(h);
            });
    } else if (stdin_write && params.stdin_mode == CommandConfig::StdinMode::file) {
        impl->stdin_thread = std::thread(
            [path = std::move(params.stdin_path), h = stdin_write] {
                HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    char buf[4096];
                    DWORD bytes_read;
                    while (ReadFile(file, buf, sizeof(buf), &bytes_read, nullptr) && bytes_read > 0) {
                        DWORD written;
                        WriteFile(h, buf, bytes_read, &written, nullptr);
                    }
                    CloseHandle(file);
                }
                CloseHandle(h);
            });
    } else if (stdin_write && params.stdin_mode == CommandConfig::StdinMode::pipe) {
        // Retain the write handle on the impl for live writes via
        // RunningProcess::write_stdin.
        impl->stdin_write_handle = stdin_write;
        impl->stdin_piped        = true;
    } else if (stdin_write) {
        CloseHandle(stdin_write);
    }

    impl->process_handle = pi.hProcess;
    impl->job_handle = job;
    impl->process_id = static_cast<int>(pi.dwProcessId);
    impl->stdout_read = stdout_read_h;
    impl->stderr_read = stderr_read_h;

    // Live pipe reader — fulfills the IoCallbacks contract that callbacks
    // fire "as data arrives" (not "when wait() happens to drain"). Only
    // start it when the child will actually write to one of our pipes:
    // discard mode creates a pipe too but routes the child's stdout/stderr
    // to NUL, so a reader would block on ReadFile forever.
    // One thread per captured stream so each can be cancelled individually
    // via CancelSynchronousIo (see release_for_detach).
    Win32ProcessImpl* raw = impl.get();
    if (params.stdout_mode == CommandConfig::OutputMode::capture) {
        impl->stdout_reader_thread = std::thread([raw] {
            Win32ProcessImpl::read_one_pipe(raw->stdout_read, raw->stdout_content, raw->on_stdout);
        });
    }
    if (params.stderr_mode == CommandConfig::OutputMode::capture) {
        impl->stderr_reader_thread = std::thread([raw] {
            Win32ProcessImpl::read_one_pipe(raw->stderr_read, raw->stderr_content, raw->on_stderr);
        });
    }

    return impl;
}

}  // namespace collab::process::detail
