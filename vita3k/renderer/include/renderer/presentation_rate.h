// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace renderer {

// Counts successful presentations over wall time, independently of the game's
// FPS or the requested interpolation multiplier. The caller serializes access.
class PresentationRate {
public:
    using Clock = std::chrono::steady_clock;

    void reset() {
        window_start.reset();
        output_fps.reset();
        frame_count = 0;
    }

    void record(uint64_t frames, Clock::time_point now = Clock::now()) {
        if (frames == 0)
            return;

        if (!window_start || now - last_present > std::chrono::seconds(1)) {
            reset();
            window_start = now;
        } else {
            frame_count += frames;
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - *window_start).count();
            if (elapsed >= 1'000'000'000) {
                output_fps = static_cast<uint32_t>((frame_count * 1'000'000'000 + elapsed / 2) / elapsed);
                window_start = now;
                frame_count = 0;
            }
        }
        last_present = now;
    }

    std::optional<uint32_t> fps(Clock::time_point now = Clock::now()) const {
        if (!window_start || now - last_present > std::chrono::seconds(1))
            return std::nullopt;
        return output_fps;
    }

private:
    std::optional<Clock::time_point> window_start;
    Clock::time_point last_present{};
    uint64_t frame_count = 0;
    std::optional<uint32_t> output_fps;
};

} // namespace renderer
