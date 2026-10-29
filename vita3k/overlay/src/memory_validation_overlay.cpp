// SPDX-License-Identifier: GPL-2.0-or-later
#include <overlay/memory_validation_overlay.h>
#include <overlay/types.h>
#include <util/switch_memory_validation.h>

#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
#include <switch/arm/counter.h>
#endif

#include <algorithm>

namespace overlay {

memory_validation_overlay::memory_validation_overlay() {
    visible = true;
    m_panel.set_pos(kX, kY);
    m_panel.set_size(kWidth, kHeight);
    m_panel.back_color = { 0.02f, 0.025f, 0.035f, 0.88f };
    m_panel.border_radius = 6;
    constexpr std::array<int16_t, 5> rows{ 10, 32, 71, 91, 112 };
    for (size_t i = 0; i < m_lines.size(); ++i) {
        auto &line = m_lines[i];
        line.set_font(default_font_name, i == 0 ? 16 : 14);
        line.set_pos(kX + 10, kY + rows[i]);
        line.set_size(kWidth - 20, 22);
        line.set_wrap_text(false);
        line.back_color = { 0.f, 0.f, 0.f, 0.f };
        line.fore_color = { 1.f, 1.f, 1.f, 1.f };
    }
    m_track.set_pos(kX + 10, kY + 59);
    m_track.set_size(kWidth - 20, 6);
    m_track.back_color = { 0.22f, 0.24f, 0.28f, 1.f };
    m_progress.set_pos(kX + 10, kY + 59);
    m_progress.set_size(0, 6);
}

void memory_validation_overlay::update_status() {
#if defined(__SWITCH__) && defined(VITA3K_SWITCH_TRACELAB)
    const uint64_t now = armGetSystemTick();
    if (m_next_update_tick && now < m_next_update_tick)
        return;
    m_next_update_tick = now + std::max<uint64_t>(1, armGetSystemTickFreq() / 4);
    const auto status = switch_validation::status_snapshot();
    visible = status.stage != switch_validation::Stage::Disabled;
    if (!visible)
        return;
    const auto text = switch_validation::make_hud_text(status, now);
    const std::array<const char *, 5> lines{
        text.title.data(), text.timing.data(), text.tests.data(), text.samples.data(), text.action.data()
    };
    // No live per-store counters, font rebuild per draw, or HUD-driven presents.
    // Both benchmark variants display the same fixed-size native panel at 4 Hz.
    for (size_t i = 0; i < m_lines.size(); ++i) {
        m_lines[i].set_text(lines[i]);
        m_lines[i].refresh();
    }
    color4f accent{ 0.8f, 0.85f, 0.9f, 1.f };
    using Tone = switch_validation::HudText::Tone;
    switch (text.tone) {
    case Tone::Running: accent = { 0.4f, 0.75f, 1.f, 1.f }; break;
    case Tone::Ready: accent = { 0.45f, 1.f, 0.6f, 1.f }; break;
    case Tone::Warning: accent = { 1.f, 0.82f, 0.3f, 1.f }; break;
    case Tone::Error: accent = { 1.f, 0.45f, 0.45f, 1.f }; break;
    default: break;
    }
    m_lines[0].fore_color = accent;
    m_lines[4].fore_color = accent;
    m_progress.back_color = accent;
    const auto progress = std::min<uint32_t>(status.progress_per_mille, 1000);
    m_progress.set_size(static_cast<uint16_t>((kWidth - 20) * progress / 1000), 6);
    m_progress.refresh();
#else
    visible = false;
#endif
}

compiled_resource memory_validation_overlay::get_compiled() {
    compiled_resource result;
    if (!visible)
        return result;
    result.add(m_panel.get_compiled());
    result.add(m_track.get_compiled());
    if (m_progress.w)
        result.add(m_progress.get_compiled());
    for (auto &line : m_lines)
        result.add(line.get_compiled());
    return result;
}

} // namespace overlay
