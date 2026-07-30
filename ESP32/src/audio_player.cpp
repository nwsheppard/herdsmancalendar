#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>

#include "audio_player.h"

namespace {

// Verified directly against Elecrow's own V3.0 example -- see audio_player.h.
constexpr int i2s_dout_pin = 17;
constexpr int i2s_bclk_pin = 42;
constexpr int i2s_lrc_pin = 18;

// 44100 (CD-quality) was massive overkill for two simple sine beeps
// (880Hz/1318Hz -- Nyquist only needs >2636Hz) and, more importantly, was
// still contending with the RGB display's DMA even after the bounce
// buffer increase in Bus_RGB.cpp: confirmed directly on hardware that a
// *4 bounce buffer alone didn't stop the pixelation. I2S's DMA bandwidth
// footprint scales with sample rate, so 8000Hz cuts the actual contention
// by roughly 5.5x on top of that buffer increase, rather than just giving
// the display a bigger cushion to absorb the same amount of contention.
constexpr int sample_rate = 8000;
constexpr int16_t amplitude = 32767;

/** Generates one tone (or, for freq_hz == 0, silence) and writes it out over I2S. Blocks until fully written. */
void play_tone(int freq_hz, int duration_ms)
{
    const int samples_count = sample_rate * duration_ms / 1000;
    float phase = 0.0f;
    const float phase_increment = 2.0f * PI * freq_hz / sample_rate;

    size_t bytes_written;
    for (int i = 0; i < samples_count; i += 64) {
        int16_t samples[128];
        const int batch = min(64, samples_count - i);

        for (int j = 0; j < batch; ++j) {
            int16_t sample = 0;
            if (freq_hz != 0) {
                sample = static_cast<int16_t>(sinf(phase) * amplitude);
                phase += phase_increment;
                if (phase > 2.0f * PI) {
                    phase -= 2.0f * PI;
                }
            }
            samples[j * 2] = sample;
            samples[j * 2 + 1] = sample;
        }

        i2s_write(I2S_NUM_0, samples, batch * sizeof(int16_t) * 2, &bytes_written, portMAX_DELAY);
    }
}

/** Installs the I2S driver right before use -- see audio_player.h for why this isn't done once at boot. */
void i2s_begin()
{
    const i2s_config_t i2s_config = {
        .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };

    const i2s_pin_config_t pin_config = {
        .bck_io_num = i2s_bclk_pin,
        .ws_io_num = i2s_lrc_pin,
        .data_out_num = i2s_dout_pin,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_set_clk(I2S_NUM_0, sample_rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
}

} // namespace

void audio_player_play_alert()
{
    i2s_begin();

    // A short rising two-note beep -- distinct and attention-getting without
    // being a full melody (that's more "retro terminal" territory for a
    // future WAV-based version, per the brief's "worth considering" note).
    play_tone(880, 120); // A5
    play_tone(0, 30);
    play_tone(1318, 180); // E6

    // Tear the driver back down immediately -- see audio_player.h: leaving
    // it installed keeps I2S's master-mode clock/DMA running continuously,
    // contending with the RGB display's DMA for the rest of the device's
    // uptime, not just for this alert's actual duration.
    i2s_driver_uninstall(I2S_NUM_0);
}
