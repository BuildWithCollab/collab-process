#pragma once

namespace collab::process {

// Reconnect to a process by PID — e.g. from a database.
// Honest about limitations: no process group, no tree kill, no graceful stop.
class ProcessRef {
public:
    explicit ProcessRef(int pid);

    auto pid() const -> int;
    auto is_alive() const -> bool;
    auto kill() -> bool;  // best-effort single-PID kill

private:
    int pid_;
};

}  // namespace collab::process
