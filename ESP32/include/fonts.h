#pragma once

#include <lvgl.h>

/**
 * Custom retro fonts, generated from Space Mono (assets/SpaceMono-Regular.ttf
 * -- Colophon Foundry/Google Fonts, SIL Open Font License 1.1, free to
 * embed/redistribute) via LVGL's own font converter (`npx lv_font_conv`).
 * Space Mono is a monospace font with a retro-modern, typewriter-adjacent
 * feel -- picked directly by the user from a side-by-side comparison of 8
 * candidates (VT323, Share Tech Mono, Space Mono, IBM Plex Mono, Courier
 * Prime, DotGothic16, Silkscreen, Major Mono Display) rendered at actual
 * grid/button sizes with a digit-legibility torture test. The prior custom
 * font (VT323) was dropped specifically because its 0 and 2 digits read as
 * too similar on real hardware.
 *
 * LVGL's built-in `unscii_8`/`unscii_16` were the very first attempt, ruled
 * out before VT323 even entered the picture: unscii only ships in two fixed
 * sizes with no in-between, and neither fit this app's layout (16px wrapped
 * the event grid's fixed-width columns, 8px was reported as too small to
 * read comfortably) -- see calendar_view.cpp's git history.
 *
 * Space Mono's letterforms are close to square, unlike VT323's noticeably
 * taller-than-wide proportions -- confirmed directly from the generated
 * glyph tables, it needs a wider advance width per pixel of visual height
 * than VT323 did, which is why the event grid's fixed columns
 * (`col_time_w`/etc. in calendar_view.cpp) had to widen again for this
 * swap. Still narrower than LVGL's built-in unscii_16 relative to its own
 * line height, so it still fits these buttons without reaching the sides:
 *   - unscii_8:      8px advance width,  8px line height
 *   - unscii_16:      16px advance width, 16px line height
 *   - spacemono_18:  11.0px advance width (`adv_w=176` @ 1/16px
 *                    fixed-point), 19px line height
 *   - spacemono_20:  12.25px advance width (`adv_w=196`), 21px line height
 *   - spacemono_32:  19.6px advance width (`adv_w=313`), 33px line height --
 *                    the calendar screen's "UPCOMING EVENTS" title only,
 *                    added after spacemono_20 was reported as still not
 *                    reading like a size change next to the WiFi/gear icons
 *
 * Regenerate with (from ESP32/assets):
 *   npx lv_font_conv --font SpaceMono-Regular.ttf --size <18|20|32> --bpp 4 \
 *     --format lvgl --range 0x20-0x7E --no-kerning --no-compress \
 *     --no-prefilter --lv-font-name lv_font_spacemono_<size> \
 *     -o ../src/fonts/lv_font_spacemono_<size>.c
 *
 * `--no-compress --no-prefilter` always, regardless of `--bpp`: confirmed
 * directly on hardware (during the VT323 conversion) that RLE compression
 * left on (`lv_font_conv`'s default) rendered every glyph as a solid
 * rectangle instead of the actual letterform -- this project's LVGL build
 * isn't decoding that compressed bitmap format correctly, confirmed
 * against LVGL's own built-in unscii fonts, which ship uncompressed
 * (`lv_font_unscii_16.c`'s header comment) and always rendered correctly
 * here.
 *
 * `--bpp 4` (antialiased), not `--bpp 1`: the first Space Mono conversion
 * used `--bpp 1` (matching unscii's settings exactly, out of caution after
 * the compression bug above), but reported back from hardware as looking
 * "washed out"/hard to read on the selected tab (dark text on a bright
 * amber fill -- thin 1-bit strokes read noticeably weaker dark-on-light
 * than the same weight reads light-on-dark) and "small and grainy" on the
 * calendar title. `--bpp 4` was untested at that point, not ruled out --
 * only the *compressed* `--bpp 4` conversion had ever actually failed.
 * Antialiasing directly targets both complaints (smooths the jagged 1-bit
 * edges causing the "grainy" read, and softens thin dark-on-light strokes
 * rather than leaving them as harsh single-pixel lines).
 */

/** Event grid cells/header, day separators, status/no-server messages, boot log. */
extern const lv_font_t lv_font_spacemono_18;

/** Button labels (theme_create_button()'s default -- see theme.cpp). */
extern const lv_font_t lv_font_spacemono_20;

/** The calendar screen's "UPCOMING EVENTS" title, and nothing else. */
extern const lv_font_t lv_font_spacemono_32;
