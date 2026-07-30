#pragma once

/**
 * I2S audio output for alert sounds, via the CrowPanel 5" HMI's onboard I2S
 * amplifier. Pins verified directly against Elecrow's own V3.0 hardware
 * example (Elecrow-RD/CrowPanel-5.0-HMI-ESP32-Display-800x480,
 * example/V3.0/Arduino/Course/Example2_Play_music) -- the same hardware
 * revision this project's display/board config was already sourced from --
 * not guessed: DOUT=GPIO17, BCLK=GPIO42, LRC=GPIO18. None of these overlap
 * the display/touch/backlight pins already in use in main.cpp/touch.cpp.
 *
 * Raw sine-wave tone generation over the ESP-IDF I2S driver, matching
 * Elecrow's own example's approach, rather than a full audio-file decoder
 * (e.g. ESP32-audioI2S) -- alert sounds are short synthesized beeps, not
 * music/speech playback, so a decoder would be unused complexity.
 *
 * No persistent begin()/setup call: the I2S driver is installed only for
 * the duration of audio_player_play_alert() and uninstalled immediately
 * after, rather than once at boot. ESP32-S3's RGB LCD peripheral shares
 * GDMA/PSRAM bandwidth with I2S, and I2S master mode generates a
 * continuous bit clock (and DMA feed) from the moment it's installed --
 * not just while a tone is actually playing -- so an always-installed I2S
 * driver contends with the display's DMA for the device's entire uptime.
 * Confirmed directly on hardware: the display corrupted (white/black,
 * scrolling garbage) immediately after boot once I2S was installed
 * persistently, well before any alert had ever fired. Installing only
 * around the brief (well under a second) moment a tone actually plays
 * confines that contention to alerts themselves instead of all the time.
 */

/**
 * Play a short, retro-terminal-style alert tone (a rising two-note beep).
 * Blocks for its duration -- acceptable here the same way other brief
 * blocking calls are elsewhere in this codebase (HTTP fetches, etc.), and
 * this one is rare (at most once per high-impact event).
 */
void audio_player_play_alert();
