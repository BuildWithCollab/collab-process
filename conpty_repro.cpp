// Minimal ConPTY repro — spawn `cmd /c echo HELLO_CONPTY` and see what lands
// in the output pipe. If HELLO_CONPTY appears in captured bytes, ConPTY is
// working. If only VT setup codes appear and HELLO_CONPTY bleeds to the
// parent's terminal, something about PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE isn't
// taking effect.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#pragma comment(lib, "user32.lib")

static void die(const char* msg, DWORD err = GetLastError()) {
    std::fprintf(stderr, "FAIL: %s (err=%lu)\n", msg, (unsigned long)err);
    std::exit(1);
}

int main() {
    // If we don't already have a real console (running through bash pipes etc),
    // allocate one. ConPTY attachment may silently fail without a host console.
    HANDLE probe = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(probe, &mode)) {
        std::fprintf(stderr, "no real console — FreeConsole then AllocConsole\n");
        FreeConsole();
        if (!AllocConsole()) {
            std::fprintf(stderr, "AllocConsole failed err=%lu\n",
                         (unsigned long)GetLastError());
            std::exit(1);
        }
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
        // Deliberately leave STD_ERROR_HANDLE alone so this repro's debug
        // fprintf(stderr, ...) still reaches the bash pipe and we can see it.
    } else {
        std::fprintf(stderr, "real console detected\n");
    }

    HANDLE input_read = nullptr, input_write = nullptr;
    HANDLE output_read = nullptr, output_write = nullptr;

    if (!CreatePipe(&input_read, &input_write, nullptr, 0))   die("CreatePipe input");
    if (!CreatePipe(&output_read, &output_write, nullptr, 0)) die("CreatePipe output");

    COORD size{80, 25};
    HPCON pty = nullptr;
    HRESULT hr = CreatePseudoConsole(size, input_read, output_write, 0, &pty);
    CloseHandle(input_read);
    CloseHandle(output_write);
    if (FAILED(hr)) die("CreatePseudoConsole", (DWORD)hr);

    std::fprintf(stderr, "PTY=%p\n", pty);

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    auto* attr_list = (PPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attr_size);
    if (!attr_list) die("HeapAlloc");
    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size)) die("Initialize");
    if (!UpdateProcThreadAttribute(attr_list, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pty, sizeof(pty), nullptr, nullptr))
        die("UpdateProcThreadAttribute");

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attr_list;

    std::wstring cmd = L"ping -t 127.0.0.1";

    // Read in a thread
    std::string captured;
    std::thread reader([&] {
        char buf[4096];
        DWORD n;
        while (ReadFile(output_read, buf, sizeof(buf), &n, nullptr) && n > 0) {
            captured.append(buf, n);
        }
    });

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(
        nullptr,
        cmd.data(),
        nullptr, nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        nullptr,
        &si.StartupInfo, &pi);
    if (!ok) die("CreateProcessW");

    std::fprintf(stderr, "child pid=%lu\n", (unsigned long)pi.dwProcessId);

    // Let ping run briefly, then deliver Ctrl+C via AttachConsole trick.
    std::this_thread::sleep_for(std::chrono::seconds{2});
    std::fprintf(stderr, "AttachConsole trick to pid=%lu...\n", (unsigned long)pi.dwProcessId);

    FreeConsole();
    BOOL attached = AttachConsole(pi.dwProcessId);
    std::fprintf(stderr, "AttachConsole: %d (err=%lu)\n", attached, (unsigned long)GetLastError());
    if (attached) {
        SetConsoleCtrlHandler(nullptr, TRUE);  // ignore
        BOOL gen = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        std::fprintf(stderr, "GenerateConsoleCtrlEvent: %d (err=%lu)\n", gen, (unsigned long)GetLastError());
        std::this_thread::sleep_for(std::chrono::milliseconds{200});
        FreeConsole();
        SetConsoleCtrlHandler(nullptr, FALSE);
    }
    AllocConsole();
    HWND hwnd = GetConsoleWindow();
    if (hwnd) ShowWindow(hwnd, SW_HIDE);

    DWORD wr = WaitForSingleObject(pi.hProcess, 5000);
    if (wr == WAIT_TIMEOUT) {
        std::fprintf(stderr, "ping did NOT exit after 5s — terminating\n");
        TerminateProcess(pi.hProcess, 1);
    } else {
        std::fprintf(stderr, "ping exited within 5s of Ctrl+C\n");
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    std::fprintf(stderr, "child exited %lu\n", (unsigned long)exit_code);

    // Match Microsoft's EchoCon sample: give ConPTY a moment to flush final output
    std::this_thread::sleep_for(std::chrono::milliseconds{500});

    ClosePseudoConsole(pty);
    CloseHandle(output_read);
    CloseHandle(input_write);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    reader.join();

    std::fprintf(stderr, "captured %zu bytes: [", captured.size());
    for (unsigned char c : captured) {
        if (c >= 32 && c < 127) std::fputc(c, stderr);
        else std::fprintf(stderr, "\\x%02x", c);
    }
    std::fprintf(stderr, "]\n");

    DeleteProcThreadAttributeList(attr_list);
    HeapFree(GetProcessHeap(), 0, attr_list);
}
