#include <windows.h>

namespace collab::process::detail {

// Reset console input/output modes for interactive child processes.
// Includes ENABLE_VIRTUAL_TERMINAL_INPUT for escape sequences (Ctrl+R, PSReadLine).
void reset_console_for_interactive() {
    HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    if (h_in != INVALID_HANDLE_VALUE) {
        SetConsoleMode(h_in,
            ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT
            | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT
            | ENABLE_VIRTUAL_TERMINAL_INPUT);
        FlushConsoleInputBuffer(h_in);
    }

    HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_out != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        GetConsoleMode(h_out, &mode);
        SetConsoleMode(h_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

}  // namespace collab::process::detail
