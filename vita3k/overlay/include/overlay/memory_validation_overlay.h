// Native, non-interactive memory-validation HUD.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <overlay/controls.h>
#include <overlay/overlay.h>
#include <array>

namespace overlay {

class memory_validation_overlay final : public overlay {
public:
    memory_validation_overlay();
    // Called on the render thread, independently of the optional FPS overlay.
    void update_status();
    compiled_resource get_compiled() override;

private:
    static constexpr uint16_t kWidth = 520;
    static constexpr uint16_t kHeight = 142;
    static constexpr int16_t kX = virtual_width - kWidth - 12;
    static constexpr int16_t kY = virtual_height - kHeight - 12;
    rounded_rect m_panel;
    overlay_element m_track, m_progress;
    std::array<label, 5> m_lines;
    uint64_t m_next_update_tick = 0;
};

} // namespace overlay
