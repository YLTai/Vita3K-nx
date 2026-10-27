// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include "audio/impl/switch_audio.h"
#include "util/log.h"
#include "util/switch_thread.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <malloc.h>

namespace {
// Renderer voice budget. Vita titles rarely open more than a handful of output
// ports simultaneously; 24 is generous while keeping the renderer light.
constexpr int SWITCH_NUM_VOICES = 24;
// Wavebuf slots per port. Eight wavebufs give enough queue depth to avoid underruns
// without adding much latency.
constexpr int SWITCH_NUM_SLOTS = 8;
// How much audio a port keeps queued, and how long one wavebuf should play for. A renderer
// voice holds four wavebufs at a time, so with a small grain, one buffer per wavebuf leaves
// the hardware holding less audio than a renderer frame consumes and it falls behind real
// time. Grains at or above the threshold already cover a frame and are submitted unbatched.
constexpr uint64_t SWITCH_TARGET_QUEUE_US = 60000;
constexpr uint64_t SWITCH_TARGET_WAVEBUF_US = 20000;
constexpr uint64_t SWITCH_COALESCE_BELOW_US = 5000;

constexpr AudioRendererConfig AR_CONFIG = {
    .output_rate = AudioRendererOutputRate_48kHz,
    .num_voices = SWITCH_NUM_VOICES,
    .num_effects = 0,
    .num_sinks = 1,
    .num_mix_objs = 1,
    .num_mix_buffers = 2,
};

size_t align_up(size_t v, size_t a) {
    return (v + a - 1) & ~(a - 1);
}
} // namespace

SwitchAudioAdapter::SwitchAudioAdapter(AudioState &audio_state)
    : AudioAdapter(audio_state) {}

SwitchAudioAdapter::~SwitchAudioAdapter() {
    running = false;
    wake_all_ports();
    if (update_thread.joinable())
        update_thread.join();

    if (initialized) {
        // Give still-detaching pools a few renderer frames to complete.
        for (int attempt = 0; attempt < 8; attempt++) {
            {
                std::lock_guard<std::mutex> lock(driver_mutex);
                if (pending_pool_releases.empty())
                    break;
                audrvUpdate(&driver);
                reap_detached_pools();
            }
            audrenWaitFrame();
        }

        std::lock_guard<std::mutex> lock(driver_mutex);
        audrenStopAudioRenderer();
        audrvClose(&driver);
        audrenExit();
        // The service session is gone; whatever never detached can no longer be
        // read by anyone, so the buffers are safe to free now.
        for (const PendingPoolRelease &pending : pending_pool_releases)
            free(pending.pool);
        pending_pool_releases.clear();
        initialized = false;
    }
}

bool SwitchAudioAdapter::init() {
    Result rc = audrenInitialize(&AR_CONFIG);
    if (R_FAILED(rc)) {
        LOG_ERROR("audrenInitialize failed: 0x{:08x}", rc);
        return false;
    }

    rc = audrvCreate(&driver, &AR_CONFIG, 2);
    if (R_FAILED(rc)) {
        LOG_ERROR("audrvCreate failed: 0x{:08x}", rc);
        audrenExit();
        return false;
    }

    static const u8 sink_channels[] = { 0, 1 };
    const int sink = audrvDeviceSinkAdd(&driver, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_channels);
    if (sink < 0) {
        LOG_ERROR("audrvDeviceSinkAdd failed: {}", sink);
        audrvClose(&driver);
        audrenExit();
        return false;
    }

    audrvUpdate(&driver);

    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc)) {
        LOG_ERROR("audrenStartAudioRenderer failed: 0x{:08x}", rc);
        audrvClose(&driver);
        audrenExit();
        return false;
    }

    voice_used.assign(SWITCH_NUM_VOICES, false);
    initialized = true;
    running = true;
    update_thread = std::thread(&SwitchAudioAdapter::update_thread_func, this);
    LOG_INFO("Switch audren audio backend initialised ({} voices)", SWITCH_NUM_VOICES);
    return true;
}

void SwitchAudioAdapter::update_thread_func() {
    // Same reasoning as the vblank tick: this thread spends its life blocked in
    // audrenWaitFrame and only does a short update per audio frame, but late
    // updates are heard as dropouts. Keep it off the cores the emulator saturates.
    switch_pin_to_helper_core("audio thread", 43);
    SwitchCpuProfileThread cpu_profile("switch-audio");

    while (running.load()) {
        audrenWaitFrame(); // blocks ~5 ms until the next renderer frame
        if (!running.load())
            break;
        {
            std::lock_guard<std::mutex> lock(driver_mutex);
            audrvUpdate(&driver);
            reap_detached_pools();
        }
        // A wavebuf may have completed; wake any port waiting for a free slot.
        wait_cond.notify_all();
        cpu_profile.checkpoint();
    }
}

int SwitchAudioAdapter::alloc_voice() {
    std::lock_guard<std::mutex> lock(voice_mutex);
    for (int i = 0; i < static_cast<int>(voice_used.size()); i++) {
        if (!voice_used[i]) {
            voice_used[i] = true;
            return i;
        }
    }
    return -1;
}

void SwitchAudioAdapter::free_voice(int voice_id) {
    if (voice_id < 0)
        return;
    std::lock_guard<std::mutex> lock(voice_mutex);
    if (voice_id < static_cast<int>(voice_used.size()))
        voice_used[voice_id] = false;
}

AudioOutPortPtr SwitchAudioAdapter::open_port(int nb_channels, int freq, int nb_sample) {
    if (!initialized)
        return nullptr;

    const int voice = alloc_voice();
    if (voice < 0) {
        LOG_ERROR("No free audren voice available for a new audio port");
        return nullptr;
    }

    auto port = std::make_shared<SwitchAudioOutPort>(*this);
    port->voice_id = voice;
    port->channels = nb_channels;
    port->freq = freq;
    port->len_bytes = nb_sample * nb_channels * static_cast<int>(sizeof(int16_t));
    port->len_microseconds = static_cast<uint64_t>(nb_sample) * 1'000'000ULL / freq;

    port->frames_per_wavebuf = 1;
    if (port->len_microseconds > 0 && port->len_microseconds < SWITCH_COALESCE_BELOW_US) {
        port->frames_per_wavebuf = static_cast<int>(
            (SWITCH_TARGET_WAVEBUF_US + port->len_microseconds - 1) / port->len_microseconds);
    }
    port->wavebuf_microseconds = port->len_microseconds * port->frames_per_wavebuf;

    port->num_slots = SWITCH_NUM_SLOTS;
    port->slot_bytes = align_up(port->len_bytes * port->frames_per_wavebuf, AUDREN_BUFFER_ALIGNMENT);
    port->pool_size = align_up(port->slot_bytes * port->num_slots, AUDREN_MEMPOOL_ALIGNMENT);
    port->pool = memalign(AUDREN_MEMPOOL_ALIGNMENT, port->pool_size);
    if (!port->pool) {
        LOG_ERROR("Failed to allocate {} byte audren mempool for audio port", port->pool_size);
        free_voice(voice);
        return nullptr;
    }
    memset(port->pool, 0, port->pool_size);

    port->wavebufs.assign(port->num_slots, AudioDriverWaveBuf{});
    for (int i = 0; i < port->num_slots; i++) {
        port->wavebufs[i].data_pcm16 = reinterpret_cast<s16 *>(static_cast<u8 *>(port->pool) + i * port->slot_bytes);
        port->wavebufs[i].state = AudioDriverWaveBufState_Free;
    }

    {
        std::lock_guard<std::mutex> lock(driver_mutex);
        port->mempool_id = audrvMemPoolAdd(&driver, port->pool, port->pool_size);
        if (port->mempool_id < 0 || !audrvMemPoolAttach(&driver, port->mempool_id)) {
            LOG_ERROR("audrvMemPoolAdd/Attach failed for audio port");
            free(port->pool);
            port->pool = nullptr;
            free_voice(voice);
            return nullptr;
        }

        if (!audrvVoiceInit(&driver, voice, nb_channels, PcmFormat_Int16, freq)) {
            LOG_ERROR("audrvVoiceInit failed (ch={} freq={})", nb_channels, freq);
            audrvMemPoolDetach(&driver, port->mempool_id);
            audrvMemPoolRemove(&driver, port->mempool_id);
            audrvUpdate(&driver);
            free(port->pool);
            port->pool = nullptr;
            free_voice(voice);
            return nullptr;
        }

        audrvVoiceSetDestinationMix(&driver, voice, AUDREN_FINAL_MIX_ID);
        if (nb_channels == 1) {
            // Mono voice fanned out to both output channels.
            audrvVoiceSetMixFactor(&driver, voice, 1.0f, 0, 0);
            audrvVoiceSetMixFactor(&driver, voice, 1.0f, 0, 1);
        } else {
            // Stereo identity mapping.
            audrvVoiceSetMixFactor(&driver, voice, 1.0f, 0, 0);
            audrvVoiceSetMixFactor(&driver, voice, 0.0f, 0, 1);
            audrvVoiceSetMixFactor(&driver, voice, 0.0f, 1, 0);
            audrvVoiceSetMixFactor(&driver, voice, 1.0f, 1, 1);
        }
        audrvVoiceSetVolume(&driver, voice, port->volume);
        audrvVoiceStart(&driver, voice);
        audrvUpdate(&driver);
    }

    LOG_INFO("Audio port opened on voice {}: {} ch @ {} Hz, {} samples, {} per wavebuf ({:.1f}ms), volume {}",
        voice, nb_channels, freq, nb_sample, port->frames_per_wavebuf,
        port->wavebuf_microseconds / 1000.0, port->volume);

    return port;
}

void SwitchAudioAdapter::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (out_port.stopping)
        return;

    auto &port = static_cast<SwitchAudioOutPort &>(out_port);
    if (port.voice_id < 0 || !port.pool)
        return;

    // A Vita port is normally single-producer, but serialize it explicitly so
    // two guest threads can never claim the same reusable wavebuf slot.
    std::lock_guard<std::mutex> submit_lock(port.submit_mutex);

    // The hardware call returns once the previous buffer has played, so filling the
    // whole ring instead lets a guest audio thread run far ahead of the sound.
    const int max_in_flight = std::clamp(
        static_cast<int>((SWITCH_TARGET_QUEUE_US + port.wavebuf_microseconds - 1) / std::max<uint64_t>(port.wavebuf_microseconds, 1)),
        2, port.num_slots);

    // Find a reusable wavebuf slot, applying backpressure until one frees up so
    // the guest is paced by the renderer's consumption rate. Slot states live
    // under driver_mutex, so the wait must use that same lock with the search as
    // its predicate or a wakeup landing in between is lost.
    const auto find_slot = [&]() -> int {
        // A partly filled wavebuf is still this port's, so keep writing into it.
        if (port.fill_slot >= 0)
            return port.fill_slot;
        int in_flight = 0;
        int free_slot = -1;
        for (int i = 0; i < port.num_slots; i++) {
            const AudioDriverWaveBufState st = port.wavebufs[i].state;
            if (st == AudioDriverWaveBufState_Free || st == AudioDriverWaveBufState_Done) {
                if (free_slot < 0)
                    free_slot = i;
            } else {
                in_flight++;
            }
        }
        return in_flight < max_in_flight ? free_slot : -1;
    };

    std::unique_lock<std::mutex> dlock(driver_mutex);
    int slot = find_slot();
    while (slot < 0) {
        if (out_port.stopping)
            return;
        wait_cond.wait_for(dlock, std::chrono::microseconds(port.len_microseconds * 2 + 1000));
        if (out_port.stopping)
            return;
        slot = find_slot();
    }
    void *const slot_base = static_cast<u8 *>(port.pool) + slot * port.slot_bytes;
    void *const dst = static_cast<u8 *>(slot_base) + port.fill_offset;
    memcpy(dst, buffer, out_port.len_bytes);
    armDCacheFlush(dst, out_port.len_bytes);

    port.submitted_samples += out_port.len_bytes / (port.channels * static_cast<int>(sizeof(int16_t)));
    port.fill_offset += out_port.len_bytes;
    port.fill_count++;
    if (port.fill_count < port.frames_per_wavebuf) {
        // The batch is incomplete, so there is nothing to hand the renderer yet. Pacing the
        // guest once per wavebuf rather than once per buffer is also the slack it needs to
        // mix the rest of the batch.
        port.fill_slot = slot;
        return;
    }

    const size_t filled_bytes = port.fill_offset;
    const int nb_sample = static_cast<int>(filled_bytes) / (port.channels * static_cast<int>(sizeof(int16_t)));
    port.fill_slot = -1;
    port.fill_offset = 0;
    port.fill_count = 0;

    AudioDriverWaveBuf &wb = port.wavebufs[slot];
    wb = AudioDriverWaveBuf{};
    wb.data_pcm16 = static_cast<s16 *>(slot_base);
    wb.size = filled_bytes;
    wb.start_sample_offset = 0;
    wb.end_sample_offset = nb_sample;
    wb.state = AudioDriverWaveBufState_Free;

    if (!audrvVoiceAddWaveBuf(&driver, port.voice_id, &wb)) {
        LOG_WARN("audrvVoiceAddWaveBuf failed for voice {}", port.voice_id);
        return;
    }

    // Restart the voice if it stopped after draining its queue.
    if (!audrvVoiceIsPlaying(&driver, port.voice_id))
        audrvVoiceStart(&driver, port.voice_id);

}

void SwitchAudioAdapter::set_volume(AudioOutPort &out_port, float volume) {
    auto &port = static_cast<SwitchAudioOutPort &>(out_port);
    if (port.voice_id < 0)
        return;
    std::lock_guard<std::mutex> dlock(driver_mutex);
    audrvVoiceSetVolume(&driver, port.voice_id, volume);
}

int SwitchAudioAdapter::get_rest_sample(AudioOutPort &out_port) {
    auto &port = static_cast<SwitchAudioOutPort &>(out_port);
    if (port.voice_id < 0)
        return 0;
    std::lock_guard<std::mutex> dlock(driver_mutex);
    const uint32_t played_low = audrvVoiceGetPlayedSampleCount(&driver, port.voice_id);
    if (port.played_samples_initialized && played_low < port.last_played_samples)
        port.played_samples_epoch += uint64_t{ 1 } << 32;
    port.played_samples_initialized = true;
    port.last_played_samples = played_low;

    const uint64_t played = port.played_samples_epoch + played_low;
    const uint64_t pending = port.submitted_samples > played ? port.submitted_samples - played : 0;
    return static_cast<int>(std::min<uint64_t>(pending, std::numeric_limits<int>::max()));
}

void SwitchAudioAdapter::switch_state(const bool pause) {
    std::lock_guard<std::mutex> vlock(voice_mutex);
    std::lock_guard<std::mutex> dlock(driver_mutex);
    for (int i = 0; i < static_cast<int>(voice_used.size()); i++) {
        if (voice_used[i])
            audrvVoiceSetPaused(&driver, i, pause);
    }
    audrvUpdate(&driver);
}

void SwitchAudioAdapter::wake_all_ports() {
    wait_cond.notify_all();
}

void SwitchAudioAdapter::reap_detached_pools() {
    for (auto it = pending_pool_releases.begin(); it != pending_pool_releases.end();) {
        if (driver.in_mempools[it->mempool_id].state != AudioRendererMemPoolState_Detached) {
            ++it;
            continue;
        }
        audrvMemPoolRemove(&driver, it->mempool_id);
        free(it->pool);
        it = pending_pool_releases.erase(it);
    }
}

SwitchAudioOutPort::~SwitchAudioOutPort() {
    {
        std::lock_guard<std::mutex> dlock(adapter.driver_mutex);
        if (voice_id >= 0) {
            audrvVoiceStop(&adapter.driver, voice_id);
            audrvVoiceDrop(&adapter.driver, voice_id);
        }
        if (mempool_id >= 0) {
            audrvMemPoolDetach(&adapter.driver, mempool_id);
            adapter.pending_pool_releases.push_back({ mempool_id, pool });
            pool = nullptr;
        }
        audrvUpdate(&adapter.driver);
    }
    adapter.free_voice(voice_id);
    if (pool) {
        free(pool);
        pool = nullptr;
    }
}
