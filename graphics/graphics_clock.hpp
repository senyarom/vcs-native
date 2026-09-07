#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
namespace vcs {
// Unlike deterministic scheduling, uncapped gameplay must not credit a fixed
// PSP frame period every time a faster host completes a frame.
class GraphicsRealtimeClock {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    void reset(std::uint64_t guest, Time now = Clock::now()) noexcept {
        guest_anchor_ = guest; wall_anchor_ = now; anchored_ = true;
    }
    void update(std::uint64_t &guest, Time now = Clock::now()) noexcept {
        if (!anchored_) reset(guest, now);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - wall_anchor_).count();
        guest = std::max(guest, guest_anchor_ + std::uint64_t(std::max<std::int64_t>(0, elapsed)));
    }
    Time deadline(std::uint64_t guest) const noexcept {
        return wall_anchor_ + std::chrono::microseconds(guest > guest_anchor_ ? guest - guest_anchor_ : 0);
    }
private:
    bool anchored_{};
    std::uint64_t guest_anchor_{};
    Time wall_anchor_{};
};

// Vblank deadlines follow a stable wall-clock phase. A small scheduler or
// render overrun must not move every following vblank later: that compounds
// sleep jitter and slows the stock two-vblank (30 FPS) game loop. A missed
// whole period reanchors instead of replaying a backlog of expired vblanks.
class GraphicsFramePacer {
public:
    using Time=GraphicsRealtimeClock::Time;
    void reset() noexcept {anchored_=false;}
    Time next(Time now,std::uint64_t period_us) noexcept {
        if(!anchored_ || period_!=period_us) {deadline_=now;period_=period_us;anchored_=true;}
        const auto target=std::max(now,deadline_);
        deadline_+=std::chrono::microseconds(period_us);
        if(deadline_<=now) deadline_=now+std::chrono::microseconds(period_us);
        return target;
    }
    // Pass this deadline to the PSP scheduler rather than sleeping on the
    // host thread: lower-priority loader/audio workers must run meanwhile.
    std::uint32_t wait_microseconds(Time now, std::uint64_t period_us) noexcept {
        const auto wait = std::chrono::ceil<std::chrono::microseconds>(next(now, period_us) - now).count();
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(wait, 1, UINT32_MAX));
    }
private:
    bool anchored_{};
    std::uint64_t period_{};
    Time deadline_{};
};
} // namespace vcs
