#include "collab/process/running_process.hpp"
#include "running_process_impl.hpp"

namespace collab::process {

RunningProcess::RunningProcess(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

RunningProcess::RunningProcess(RunningProcess&&) noexcept = default;
auto RunningProcess::operator=(RunningProcess&&) noexcept -> RunningProcess& = default;

RunningProcess::~RunningProcess() {
    // RAII: kill the process tree on destruction if still alive.
    // Use detach() to release ownership if the child should outlive this handle.
    if (impl_ && impl_->is_alive())
        impl_->kill();
}

auto RunningProcess::pid() const -> int { return impl_->pid(); }
auto RunningProcess::is_alive() const -> bool { return impl_->is_alive(); }
auto RunningProcess::wait() -> std::expected<Result, SpawnError> { return impl_->wait(); }

auto RunningProcess::wait_for(std::chrono::milliseconds timeout)
    -> std::optional<Result> {
    return impl_->wait_for(timeout);
}

auto RunningProcess::stop(std::chrono::milliseconds grace) -> StopResult {
    return impl_->stop(grace);
}

auto RunningProcess::kill() -> bool { return impl_->kill(); }

auto RunningProcess::interrupt() -> bool { return impl_->interrupt(); }

auto RunningProcess::detach(this RunningProcess&& self) -> int {
    int pid = self.impl_->pid();
    self.impl_->release_for_detach();  // platform cleanup (e.g., remove kill-on-close job)
    self.impl_.reset();
    return pid;
}

}  // namespace collab::process
