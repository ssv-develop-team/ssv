#pragma once

#include <functional>

namespace ssv {

class SsvWindowLifecycle {
public:
    virtual ~SsvWindowLifecycle() = default;

    /// Both operations must run on the thread executing the runner main loop.
    virtual void show(std::function<void()> on_close) = 0;
    virtual void close() noexcept = 0;
};

} // namespace ssv
