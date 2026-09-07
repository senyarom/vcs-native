#pragma once

#include <cstdint>
#include <span>

namespace vcs {

// The host mix retains this scheduling margin. A guest audio thread waking a
// few ms late can still fill its original slot instead of adding a radio gap.
inline constexpr std::uint64_t kAudioSchedulingSlackUs = 60'000u;

// Host audio sink for the sceAudio HLE.  The PSP exposes eight regular PCM
// channels plus one SRC/Output2 channel; submissions are mixed on the guest's
// virtual-time line before they are handed to the native audio device.
[[nodiscard]] bool audio_output_enabled();

// Mix a buffer scheduled for `start_time_us`, which can be in the future.
// `guest_time_us` is the CURRENT guest clock: only this clock may seal the mix.
// Keeping the two separate lets simultaneous channels share a timeline even
// when a blocking PSP call has already supplied the next radio buffer.
void audio_output_submit(std::span<const std::int16_t> pcm, std::uint32_t frames,
                         bool stereo, std::uint32_t left, std::uint32_t right,
                         std::uint32_t source_rate, std::uint32_t channel,
                         std::uint64_t start_time_us, std::uint64_t guest_time_us);

struct AudioOutputStatus {
    std::uint64_t guest_frame{}, output_frame{}, device_frames{}, latency_frames{};
    std::uint64_t backlog_frames_discarded{}, late_frames{}, resyncs{}, underruns{};
};
// Host queue plus not-yet-submitted mix, excluding OS/device hardware latency.
[[nodiscard]] AudioOutputStatus audio_output_status();

// Seal and queue audio whose guest time is safely in the past.  Call this from
// the vblank path even on frames where the game submitted no new audio so the
// native device keeps receiving silence rather than underrunning.
void audio_output_advance(std::uint64_t guest_time_us);

// Forget stream/resampler continuity for one PSP channel (release/re-reserve).
void audio_output_reset_channel(std::uint32_t channel);

// Releases the device. Safe to call when nothing was ever opened.
void audio_output_shutdown();
// Pause the host device while the guest is stopped in the graphics panel.
void audio_output_pause(bool paused);

} // namespace vcs
