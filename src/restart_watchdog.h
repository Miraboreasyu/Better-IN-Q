#pragma once
#include <cstdint>

namespace inq {

// Pure timing policy. The control thread owns this object; game hooks only
// publish restart. Unknown loading state is NOT evidence of non-loading.
class RestartWatchdog {
public:
    enum class Loading { Unknown, Busy, Idle };
    static constexpr std::uint64_t kTimeoutMs = 12000;

    void Reset() { stage_ = 0; since_ = 0; timing_ = false; }

    bool Step(std::uint64_t now, int stage, Loading loading, bool enabled)
    {
        if (!enabled || stage == 0) { Reset(); return false; }
        // Require a continuous, positively observed non-loading window. A
        // stalled disk load (or a disappearing UI chain) can last indefinitely.
        if (loading != Loading::Idle) {
            stage_ = stage;
            since_ = now;
            timing_ = false;
            return false;
        }
        if (!timing_ || stage != stage_ || now < since_) {
            stage_ = stage;
            since_ = now;
            timing_ = true;
            return false;
        }
        return now - since_ >= kTimeoutMs;
    }

private:
    int stage_ = 0;
    std::uint64_t since_ = 0;
    bool timing_ = false;
};

} // namespace inq
