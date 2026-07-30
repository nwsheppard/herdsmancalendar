# Herdsman Trading Terminal — ESP32 Firmware

Firmware for the Elecrow CrowPanel 5.0" HMI (ESP32-S3-WROOM-1-N4R8, 800x480
RGB IPS touch). See [`herdsman-trading-terminal-brief.md`](herdsman-trading-terminal-brief.md)
for the full project brief.

## Hardware bring-up source

The RGB bus timings, pin mapping, and GT911/PCA9557 touch-reset sequence in
`src/main.cpp` and `src/touch.cpp` are copied from Elecrow's own verified
working example for this exact board
([`Elecrow-RD/CrowPanel-5.0-HMI-ESP32-Display-800x480`](https://github.com/Elecrow-RD/CrowPanel-5.0-HMI-ESP32-Display-800x480),
`example/V3.0/PlatformIO50`), not re-derived from datasheets. This is the
highest-risk, hardest-to-debug-remotely part of the project, so it's kept
byte-faithful to a known-working reference rather than rewritten.

Deliberate deviation from the brief: the brief suggested LVGL v8.3.x, but
Elecrow's current official example pins **LVGL 9.1.0** with the newer
display/indev API. This project follows the verified-current example
instead of the brief's guess.

`lib/LovyanGFX/` is vendored (not pulled from the PlatformIO registry):
Elecrow's example depends on a patched fork of LovyanGFX whose `Bus_RGB`
class has `getFrameBuffer()`/`presentFrameBuffer()` methods that don't exist
in stock upstream `lovyan03/LovyanGFX`, despite `library.json` claiming
version 1.2.21. This was discovered by actually trying the plain registry
version first and hitting a real compiler error (`no member named
'presentFrameBuffer'`) — not assumed. The vendored copy excludes
LovyanGFX's bundled CJK bitmap fonts (~120MB, unused here since all text
rendering goes through LVGL, not LovyanGFX's own font API), which is why
`lib/LovyanGFX/` is ~7.5MB rather than ~130MB.

## Building and flashing

Requires [PlatformIO](https://platformio.org/) (CLI or the VS Code
extension).

1. Open this `ESP32/` folder as a PlatformIO project.
2. Edit `upload_port` / `monitor_port` in `platformio.ini` to match whatever
   COM port (Windows) or `/dev/tty*` (Linux/Mac) the board enumerates as.
3. Build and upload (`pio run -t upload`, or the PlatformIO toolbar buttons).
4. Open the serial monitor at 115200 baud to see boot logs.

There's no `secrets.h`/build-time config file to fill in — WiFi credentials
and the calendar server's URL are both entered on-device (WiFi via
Milestone 4's setup screen, the server URL via Milestone 6's Settings
screen) and stored in NVS, since a sold unit can't ship with one customer's
network password or container address compiled into the firmware.

## Milestone 1: display + touch bring-up — done, confirmed on hardware

LVGL initializes the 800x480 RGB panel and confirmed real touch input via
GT911. Superseded visually by Milestone 3's splash artwork, but the
underlying bring-up code is unchanged.

## Milestone 2: WiFi + calendar API reachability — done, confirmed on hardware

On boot, after the splash renders, the firmware connects to WiFi and does
one `GET <server>/health` (via `calendar_client.*`). This is a one-shot
check on boot, not a poller yet — periodic polling and real calendar JSON
parsing is Milestone 5's job (`calendar_client.*` will grow into that, not
get replaced). Confirmed working end-to-end: WiFi connects, `/health`
returns `{"status":"ok",...}`.

(Both WiFi credentials and the calendar server's address, originally
compiled in via `secrets.h` when this milestone was built, were later moved
to on-device config -- Milestone 4 for WiFi, Milestone 6 for the server URL.
`secrets.h` no longer exists in this project.)

## Milestone 3: boot splash artwork — done

The boot splash is the user-supplied retro synthwave artwork
(`assets/terminal-800x480.png`, already sized to the panel's 800x480), shown
full-screen, with live status text overlaid on top of it:

- A boot log inside the artwork's terminal box (`>> DISPLAY`, `>> TOUCH
  (GT911)`, `>> WIFI`, `>> CALENDAR API`, each updated in place with
  `[ OK ]`/`[FAIL]`/the actual WiFi IP as that subsystem resolves — not
  decorative placeholder text). Drawn twice with a 1px horizontal offset
  (faux-bold) since LVGL's built-in bitmap fonts have no bold variant.
- "(c) 2026 Herdsman Corp." bottom-left.
- A `LV_SYMBOL_WIFI` icon bottom-right whose color reflects connection state
  (dim while idle/connecting, green once connected, red on failure) —
  replaces the earlier text-based WiFi status. On failure/disconnect a
  small red bar rotates across the icon (LVGL's built-in symbol set has no
  "no-wifi" glyph, so this is drawn manually rather than a font glyph).
  This isn't just a boot-time check: `WiFi.onEvent()` is registered so a
  later drop or reconnect (`WiFi.setAutoReconnect(true)` is enabled in
  `wifi_manager.cpp`) updates the icon for the life of the program, not
  only at boot. The event callback only sets a flag, since it runs on the
  WiFi task, not the task LVGL was initialized on — `loop()` applies the
  actual icon/label change.

**Known-hardware-confirmed fix:** `WiFi.setAutoReconnect(true)` alone
turned out not to be reliable on real hardware — disconnecting and
restoring WiFi showed the red icon correctly but never reconnected on its
own (a known arduino-esp32 quirk, not specific to this code). `loop()` now
backstops it: while `WiFi.status() != WL_CONNECTED`, it calls
`WiFi.reconnect()` explicitly every 10s (`wifi_reconnect_interval_ms`)
rather than trusting the built-in mechanism alone.

**Image pipeline:** `assets/terminal-800x480.png` (RGBA) is flattened to RGB
and converted to a C byte array with LVGL's own v9 converter
([`lvgl/lvgl` `scripts/LVGLImage.py`](https://github.com/lvgl/lvgl/blob/v9.1.0/scripts/LVGLImage.py),
matched to our LVGL 9.1.0 / RGB565 config — LVGL 9 changed the image
descriptor format from v8, so an old/generic converter would have produced
an incompatible header), landing in `src/assets/terminal_800x480.c`
(`lv_image_dsc_t terminal_800x480`, declared in
`include/boot_splash_image.h`). Uncompressed RGB565, no SPIFFS/runtime
decode involved — it's linked straight into flash, which is why Flash usage
jumped to 68% (see below). `LV_LVGL_H_INCLUDE_SIMPLE` had to be added to
`build_flags` for the generated file's `#include` to resolve correctly
against the registry LVGL layout. The terminal box's interior coordinates
(`box_text_x/y/width` in `main.cpp`) were measured from the image by
detecting its cyan border pixels, not eyeballed.

**Attribution:** the dog logo in the artwork is the user's own company
logo/asset, used with permission; not something generated or redistributed
by this project.

**Build status:** compiles cleanly (RAM 35.1%, 114,968 / 327,680 bytes;
Flash 68.0%, 2,139,687 / 3,145,728 bytes). Layout verified by overlaying the
actual boot-log text onto the source image offline (to check line lengths —
e.g. the WiFi line with a full IP address — actually fit inside the box)
since hardware isn't accessible from this side; not yet flashed/tested on
the real panel.

## Milestone 4: on-device WiFi setup — done

Not in the original brief's roadmap, but needed once this became a
"might sell it" product rather than a one-off build: `secrets.h` obviously
can't hold a customer's WiFi password. WiFi credentials now live in NVS
(`wifi_credentials_store.*`, via ESP32's `Preferences` library), entered
on-device rather than baked into the firmware:

- On boot, stored credentials (if any) are tried first via `wifi_connect()`.
- If there are none, or they fail to connect (new router, changed
  password), `wifi_setup_screen_run()` (`wifi_setup_screen.*`) takes over:
  scans for networks (`WiFi.scanNetworks()`), shows a tappable list, an
  on-screen keyboard for the password if the network is secured, and
  retries on a failed connect attempt. It only returns once a connection
  actually succeeds — there's nothing useful the device can do without
  WiFi, so there's no cancel/skip.
- Once connected, the credentials that worked are saved to NVS so every
  later boot skips straight past setup.

**Deliberately out of scope for now** (per your call): no way to
deliberately re-enter setup once WiFi is already working (e.g. to switch to
a new router) — that's deferred to a future Settings screen rather than
built as a hidden gesture now.

**Hardware-confirmed fixes from real testing:**
- The password field had no way to check what you'd actually typed. Added
  an eye-icon toggle button (`LV_SYMBOL_EYE_OPEN`/`EYE_CLOSE`) next to the
  password field that flips `lv_textarea_set_password_mode()`; it resets to
  hidden each time a new network is selected rather than carrying a
  previous "show password" choice forward.
- The Back button rendered on top of the Connect button. Root cause: their
  layout used `lv_obj_align_to()` against a button's position/size
  immediately after creating it but before adding its label —
  content-driven sizing resolves on the next layout pass, not
  synchronously, so the alignment target's size was still stale/default at
  that point. Fixed by giving every button an explicit size and adding its
  label *before* using it as an alignment target, rather than relying on
  auto-sizing timing.

**Build status:** compiles cleanly (RAM 35.1%, 115,160 / 327,680 bytes;
Flash 68.3%, 2,149,511 / 3,145,728 bytes). The core scan-list-tap-keyboard-
connect flow and the WiFi drop/reconnect behavior are now confirmed working
on real hardware; these two UI fixes address issues found during that
testing and haven't been re-flashed/re-confirmed from this side yet.

## Milestone 5: calendar UI with mock data — done

The dark/amber "Calendar UI" from the brief's section 3, built against
static mock data (`calendar_model.*`) rather than live data -- Milestone 6
swaps the data source without touching this screen, per the brief's
suggested build order ("get the dark/amber styling and layout right before
wiring up live data").

- `calendar_view.*` builds each event as a flex row (impact-color bar +
  time + currency + event name + actual/forecast/previous), not an
  `lv_table`: LVGL 9's table widget only supports per-cell color overrides
  through custom draw callbacks, which is real complexity to get right
  without hardware to iterate against. Plain `lv_obj`/`lv_label` flex rows
  give full per-row styling through ordinary style calls instead.
- Impact level colors reuse `THEME_COLOR_IMPACT_HIGH/MEDIUM/LOW` (already
  defined in `theme.h` back in Milestone 1, unused until now).
- The event flagged `is_next` in the mock data gets an amber border +
  lighter background. There's no live-clock "what's actually next" logic
  yet — Milestone 7 (countdown/alerts) is where real time comparison
  belongs; for mock data it's just a flag on one entry.
- Rows are tappable (`LV_OBJ_FLAG_CLICKABLE`); tapping one logs the event
  name to serial for now. The brief's "tap for detail view" is real but
  what a detail view should actually show isn't specified yet, so it's not
  built as a guess.
- After the boot log finishes (Milestone 2-4's flow, unchanged), `setup()`
  holds it on screen for 1.5s, then loads the calendar screen.

**Deliberately not carried over:** the WiFi status icon from the splash
screen isn't duplicated onto this screen. Worth adding later (any
always-on display should probably show connectivity status somewhere) but
that's scope beyond "get the calendar layout right," so it's flagged here
rather than added unasked.

**Build status:** compiles cleanly (RAM 35.1%, 115,160 / 327,680 bytes;
Flash 68.4%, 2,151,335 / 3,145,728 bytes). Column-width arithmetic checked
by hand (longest mock event name, "Core Durable Goods Orders (MoM)" at 32
characters, fits the ~367px name column at 14pt without truncation) since
there's no LVGL simulator set up here to render it against — this is the
least-verified milestone so far and worth a close look on the actual panel.

**Hardware-confirmed readability fix:** flashed and found hard to read.
Not a light-gray-background situation (a light background would fight the
retro amber-CRT-glow look this project is going for -- amber-on-light-gray
reads as a faded printout, not a glowing terminal) — the real problem was
two-fold. First, `THEME_COLOR_AMBER_DIM` (driving most of a row: currency,
actual/forecast/previous, headers) was too low-contrast against near-black
in practice, even though its WCAG contrast ratio was technically fine on
paper — warm dark hues read as muddy rather than legible, especially on a
panel with reduced black levels. Brightened `0x8A5E00` → `0xC98A00` and
lifted the background `0x0D0D0D` → `0x141414` slightly. Second (and
probably the bigger factor): `calendar_api.py`'s impact-level detection was
silently broken (see that repo's README) — every event came back
`impact_level: 0`, so every impact bar rendered the same dim gray instead
of the intended red/orange/gray variety, making the whole screen look flat
regardless of the text palette. Fixing that scraper bug should visibly help
independent of the color tweak.

## Milestone 6: Settings screen (filters + WiFi) — done

Not in the original brief either, but a direct consequence of Milestone 4's
productization discussion: different customers of the same hardware want
different things (some want low-impact events too, some want UK/China
alongside the US). `calendar_api.py` grew `GET`/`POST /filters` and
`GET /countries` for exactly this (see `LXC/README.md`) — this milestone is
the on-device UI for it.

- **Restructured into a menu + three dedicated sub-screens** after the
  first version (WiFi status, the server URL fields, impact checkboxes, and
  a 107-country list all on one page) turned out cluttered in practice.
  `settings_screen.*` is now just a menu -- "WiFi" / "Calendar Server" /
  "Calendar Filters" -- each opening its own screen
  (`settings_wifi_screen.*`, `settings_server_screen.*`,
  `settings_filters_screen.*`). None of these block (unlike
  `wifi_setup_screen_run()`); each one's Back/Save returns to whichever
  screen opened it -- a sub-screen's Save/Back returns to the Settings
  menu, and the menu's own "Back to Calendar" is what actually leaves
  Settings. Splitting the Calendar Server screen out on its own also meant
  it needed its own reachability check on Save (`calendar_client_check_health()`)
  that the combined screen got for free before (via the filters POST).
- On open, each sub-screen fetches its own current state from the API
  (blocking HTTP calls, same pattern used elsewhere in this codebase) and
  pre-fills to match -- not built from scratch each time blind. The
  107-country list is cached in memory (in `settings_filters_screen.cpp`)
  after first fetch so reopening that screen later in the same session
  doesn't re-fetch it.
- Bundles the WiFi-reconfigure option deferred twice already (Milestones 4
  and 5): "Reconfigure WiFi" on the WiFi sub-screen calls the *existing*
  `wifi_setup_screen_run()` unchanged, passing that sub-screen itself as
  the screen to return to afterward.
- Added `ArduinoJson` (`bblanchon/ArduinoJson@^7.2.0`) as a new dependency
  for parsing `/filters`/`/countries` and building the `POST /filters`
  body -- the first JSON parsing this project has needed; everything
  before this read `/health`'s raw string or built simple query strings by
  hand.
- `calendar_client.*` gained `calendar_client_get_filters()`,
  `calendar_client_get_countries()`, and `calendar_client_save_filters()`
  alongside the existing `calendar_client_check_health()`.

**Every button re-themed.** `lv_button_create()` renders LVGL's default
theme by default -- a glossy blue button -- which every button in this
project (the settings gear, Connect/Back/Rescan in the WiFi setup flow,
Save/Back everywhere) had been using unstyled up to this point. Flashed and
noticed on the gear icon specifically ("looks out of place for a 90s retro
terminal"), but it was really a systemic gap, not a one-icon problem.
Added `theme_create_button()` (`theme.h`/`theme.cpp`, the first `.cpp` this
header has needed) -- dark fill, amber border and text, no drop shadow --
and every button-creation call site across `calendar_view.cpp`,
`wifi_setup_screen.cpp`, and all four `settings*.cpp` files now goes
through it instead of `lv_button_create()` directly.

**`secrets.h` is gone entirely.** It originally held WiFi credentials (moved
to NVS earlier in this same milestone group) and, until now, the calendar
server's URL as a compile-time `CALENDAR_API_BASE_URL` macro — the last
thing standing between this firmware and being genuinely
build-once-flash-to-any-customer. That's now a Settings screen field,
backed by `calendar_server_store.*`/NVS via `calendar_client_set_base_url()`.
If no server URL has ever been set (a brand new device), boot skips the
`/health` check rather than requesting against an empty URL — the boot log
shows `CALENDAR API [NOT SET]` instead of `[FAIL]`, and Settings is where
it gets configured. `calendar_client.*`'s functions all silently no-op
(return false/empty) until a URL is set, so nothing crashes or hangs
waiting on a request that was never going to have anywhere to go.

The server address is entered as three separate fields, not one free-text
URL: a protocol dropdown (`http://`/`https://`, defaulting to `http://`), a
host/IP text field, and a port field (defaulting to `8080`). Typing a full
URL by hand on the on-screen keyboard is genuinely tedious, and the vast
majority of setups are plain `http://` on port `8080` — so the common case
is just tapping the host field and typing an IP, with protocol/port left
alone. Saving reassembles the three into one `protocol + host + ":" + port`
string for `calendar_client`/NVS, and reopening Settings later splits a
previously-saved URL back into the three fields via the same logic in
reverse (`split_server_url()`), so it round-trips correctly either way.

**Not built yet:** changing filters (or the server URL) here doesn't
refresh the calendar view's mock data (Milestone 7 is where the calendar
view fetches and re-polls live data — a natural point to also refresh
after a Settings save, rather than building that plumbing against mock
data now).

**Calendar view now has an empty state for "no server configured"**, from
a follow-up question about first-boot UX: unlike WiFi, a customer's
`calendar_api.py` container might not be running yet when they first power
on the terminal, so forcing server setup during initial WiFi setup would
risk stalling them on a step they can't complete yet. The chosen
alternative was to leave first-boot setup WiFi-only and instead make the
Events screen fail clearly: `calendar_view_create()` checks
`calendar_server_url_load()` and, if nothing's been saved, shows "No
calendar server configured. Tap the gear icon above to set one up in
Settings -> Calendar Server." instead of the mock event list. This also
quietly fixes the scenario the mock-data note above glossed over: once
Milestone 7 replaces mock data with a live fetch, a device with no server
configured genuinely has nothing to show, so it needs this regardless.

**Checkbox/textarea/dropdown/list text was unreadable** (hardware feedback:
"the text for the filters is unreadable, I can't see the impact or
countries"). Root cause was the same class of bug as the blue-button
issue above, just for different widgets: `lv_checkbox_create()`,
`lv_textarea_create()`, `lv_dropdown_create()`, and `lv_list_create()`/
`lv_list_add_button()` all render with LVGL's default (light) theme unless
styled explicitly, and nothing in this project had done that for them --
their text (and, for the WiFi network list, the whole list/button
background) rendered in colors meant for a light background, next to
invisible against `THEME_COLOR_BACKGROUND`. Fixed with five more
`theme.h`/`theme.cpp` helpers alongside `theme_create_button()`:
`theme_create_checkbox()` (recolors both the label and the indicator
box/checkmark), `theme_style_textarea()` (fill/border/text/placeholder/
cursor), `theme_style_dropdown()` (the closed control and its options list
-- the list is created eagerly alongside the dropdown, just hidden until
opened, so it's safe to style right away rather than needing an on-open
callback), and `theme_style_list()`/`theme_style_list_button()`. Applied
everywhere those widgets appear: the impact/country checkboxes in
`settings_filters_screen.cpp`, the protocol dropdown and host/port
textareas in `settings_server_screen.cpp`, and the password textarea plus
network list/buttons in `wifi_setup_screen.cpp`.

**Categories and Columns filters added**, from direct hardware feedback:
"I need to have filters for Columns and for Categories. There are some
that aren't relevant to me." `calendar_api.py`'s `category` param (which
investing.com event categories to include -- Employment, Inflation,
Central Banks, etc.) and a new `columns` concept (which optional per-event
fields -- Impact/Actual/Forecast/Previous -- to return) were both static
before this; both are now part of `filters.json`/`GET`+`POST /filters`
exactly like `importance`/`countries` already were, with `GET /categories`
and `GET /columns` added for building picker UIs (see `LXC/README.md` for
the full endpoint details, including why `exc_currency` can't be one of
the optional columns). Two more dedicated Settings sub-screens were added
rather than extending `settings_filters_screen.cpp` further -- that screen
is already tight on space with impact level plus a 107-entry country list:
`settings_categories_screen.*` (8 checkboxes) and
`settings_columns_screen.*` (4 checkboxes), both following the exact same
shape as the country-checkbox list (fetch-on-open, cache in memory,
index-based checkbox `user_data` into the cached options vector rather
than a pointer into it, so the cache can safely outlive any single screen
open/close). The Settings menu grew from three buttons to five (renamed
"Calendar Filters" to "Impact & Countries" for clarity alongside the new
"Event Categories"/"Data Columns" entries) and menu buttons shrank
slightly (400x70 → 360x56) to fit five in the same vertical space.

Each of these three filter sub-screens now only edits its own slice of the
one combined `/filters` object -- Save on any of them fetches the current
full state on open and carries the *other* two dimensions through
unchanged, rather than resetting them to empty/default (a real risk once
there were three screens sharing one API object instead of one screen
owning all of it).

**Calendar view layout fixed + Day/Week tabs added (UI only).** Hardware
feedback: the header row/event list overlapped the gear icon in the top
right. Cause was the same one already seen elsewhere in this milestone --
`theme_create_button()`'s content-fit sizing rendered the settings button
taller on hardware than the header row's hardcoded y-offset assumed. Given
an explicit size (44x44) instead, and the header row/list shifted down
(header 46→108, list 74→136, list height 400→338 to keep the same bottom
margin) to clear it with room to spare. Also added "Day Events"/"Week
Events" toggle buttons above the header row -- UI only for now (both show
the same mock list; clicking one just recolors it as the active tab via
`set_active_tab()`), deliberately scoped down from wiring in the real
`range=day`/`range=week` fetch, which is Milestone 7's job.

**Build status:** compiles cleanly (RAM 35.2%, 115,480 / 327,680 bytes;
Flash 71.6%, 2,251,095 / 3,145,728 bytes -- still comfortable headroom).
The server side (`/filters`, `/countries`, `/categories`, `/columns`,
validation, persistence, backward-compat with a `filters.json` predating
the new keys) is verified end-to-end against the live site — see
`LXC/README.md`.

**Resolved from hardware testing:** "Could not load country list -- check
the connection" when opening Calendar Filters turned out not to be a code
bug at all. Serial log showed `nvs_open failed: NOT_FOUND` followed by
`Calendar server not configured (Settings) -- skipping request` -- the
calendar server URL had been wiped by an NVS erase done earlier to force
the WiFi setup screen to reappear for testing (see "Known quirks" below:
erasing NVS wipes the calendar server URL along with WiFi credentials,
since both live in the same partition). `calendar_server_url_load()`
correctly detected nothing was stored and `calendar_client` correctly
refused to make a request with no server configured, exactly as designed
-- the fix was just re-entering the address in Settings → Calendar Server,
no code changes needed.

**Data source switched from investing.com to Forex Factory** (see
`LXC/README.md` for the full rationale/technical details). This changed
the filter schema `calendar_client`/the Settings screens talk to, so
several files here changed to match:
- `calendar_client.h`/`.cpp`: `CountryOption` is gone (replaced by the
  already-existing `StringOption`, since currency codes are strings like
  `category`/`columns` codes were, not investing.com's numeric country
  codes). `CalendarFilters.country_codes`/`category_codes` are gone;
  `currency_codes` replaces `country_codes`. Added `impact_holiday` for
  Forex Factory's grey/non-economic impact tier, which investing.com had
  no equivalent of. `calendar_client_get_countries()`/`get_categories()`
  are gone; `calendar_client_get_currencies()` replaces the former.
- `settings_categories_screen.*` deleted outright -- Forex Factory has no
  category concept to filter by, so unlike the countries→currencies
  rename, there was nothing to adapt here.
- `settings_filters_screen.cpp` renamed "Countries" to "Currencies" (title
  now "IMPACT & CURRENCIES") and added a 4th "Holiday" impact checkbox
  alongside Low/Medium/High. The currency list shrank from a 107-entry
  scrollable country list to ~10 currencies, so its container shrank too
  (320px tall → 110px) rather than leaving a mostly-empty box.
- `settings_columns_screen.*` needed no code changes at all -- it's fully
  data-driven from `GET /columns`, so the column codes changing shape
  server-side (`exc_actual` → `actual`, etc.) required zero ESP32-side
  updates.
- `settings_screen.cpp`'s menu dropped back from five buttons to four
  (WiFi / Calendar Server / Impact & Currencies / Data Columns) now that
  Event Categories is gone, and buttons grew back out a bit (spacing
  33px apart from center rather than five squeezed in at 66px apart).

**Follow-up hardware feedback after the Forex Factory switch:**
- **Currency box grown so "US Dollar" doesn't need scrolling to see.**
  `GET /currencies` sorts alphabetically by name, so "US Dollar" -- the
  default selection -- lands last; at the box's old 110px height (sized
  for ~10 currencies with room to spare) it still needed a scroll to
  reach. Grown to 190px, comfortably fitting all of them without scrolling.
- **Checkbox indicator hardened against LVGL's default (blue-accented)
  theme leaking through in less-common states.** The checked/unchecked
  fill and border were already overridden (see the earlier
  `theme_create_checkbox()` entry above), but pressed and keyboard-focus
  states weren't -- `theme_create_checkbox()` now explicitly styles the
  indicator's default/checked/pressed/focus-key states individually
  rather than relying on state fallback, plus a small explicit radius so
  it reads as a deliberately-styled retro box rather than a leftover
  default one. Also added `theme_style_scrollbar()` (amber-dim, applied to
  every scrollable container project-wide: the currency/column checkbox
  lists, the WiFi network list, and the main calendar event list) --
  LVGL's default scrollbar is a plain grey pill, another default-theme
  leftover nothing had touched yet.
- **The currency/column list boxes themselves were still showing a
  default-theme border**, reported as "still blue" after the above.
  `currency_list`/`column_list` are plain `lv_obj_create()` containers --
  LVGL's default theme applies its "card" style to any unstyled generic
  object, which includes a border color/width neither screen had
  explicitly overridden (only their background color was set). Every
  *other* plain container in this project either strips all styling via
  `lv_obj_remove_style_all()` or is a screen root (which gets a plain
  background-only style, no border, from the theme) -- these two were the
  only ones left exposed. Consolidated into a new
  `theme_style_checkbox_wrap()` helper (dark fill, no border, themed
  scrollbar) rather than patching both call sites ad hoc.
- **WiFi status icon added to the calendar screen itself.** Previously the
  dim/green/red(+strikethrough) WiFi icon only existed on the boot splash
  screen -- once `calendar_view_create()` replaces it, that icon is still
  updated on every `WiFi.onEvent()` callback, just invisibly, since
  nothing ever deletes the boot splash screen object. A WiFi drop after
  boot had nowhere on-screen to show up. Fixed by giving `calendar_view.cpp`
  its own copy of the same icon (`calendar_view_set_wifi_icon()`, same
  color_hex/show_strike shape as main.cpp's boot-splash
  `set_wifi_icon_state()`), called from the same `apply_pending_wifi_icon_state()`
  in main.cpp so both stay in sync from one event source. Takes the
  corner spot the gear icon used to occupy; the gear moved left (-20 →
  -72) to make room, rather than shrinking either icon to fit.

**"Impact & Currencies screen is slow, seems like currencies get re-pulled
each time"** -- reported after the above. They don't: `GET /currencies`
in `calendar_api.py` already just returns the static, hardcoded
`CURRENCY_NAMES` dict (no Forex Factory scraping happens for that
endpoint at all), and `settings_filters_screen.cpp` already caches the
result in RAM after the first fetch each power-on
(`if (currencies_cache.empty())`), so a repeat visit in the same session
shouldn't request `/currencies` again. What *does* happen on every visit
is a `GET /filters` round-trip (needed, since saved filters could have
changed elsewhere) -- likely the real cost, but unconfirmed without
hardware access. Rather than guess further, `calendar_client.cpp`'s
`http_get()` (the one shared helper behind every GET request) now logs
each request's URL, status, and elapsed time to Serial. Next visit to
this screen will show, definitively: whether `/currencies` is really
being requested again (cache bug) or not (as the code implies), and how
long `/filters` alone actually takes -- that log is the next thing to
check before changing anything else here.

**Build status:** compiles cleanly (RAM 35.2%, 115,400 / 327,680 bytes;
Flash 71.4%, 2,247,443 / 3,145,728 bytes -- still comfortable headroom).

## Milestone 7: live data fetch — done

Mock data (`calendar_model.*`) is gone; the calendar view now fetches and
renders real events from `LXC/calendar_api.py`'s `/calendar` endpoint.

- `CalendarEvent` (`calendar_model.h`) switched from `const char *` fields
  (fine for static string-literal mock data) to `String` (necessary once
  the struct is populated by parsing a live HTTP response it doesn't own
  the storage for). Dropped `is_next` (highlight the upcoming event) --
  computing that needs a real wall-clock time to compare against, and this
  firmware has no NTP sync yet. That's Milestone 8's problem (countdown/
  alert logic), not this one's; re-added there once there's an actual
  mechanism to set it, rather than carrying a field nothing sets.
- `calendar_client_get_calendar(range, out)` added alongside the existing
  filters/currencies/columns functions -- same shape, GET + `ArduinoJson`
  parse. Field names in the response (`day`/`time`/`currency`/`impact`/
  `impact_level`/`name`/`actual`/`forecast`/`previous`) mirror
  `calendar_api.py`'s output directly, confirmed against it rather than
  assumed.
- **Day/Week tabs are now real**, not the UI-only placeholder from the
  earlier layout-fix pass -- clicking one sets which range to request and
  re-fetches. Events are grouped with a day-separator label (e.g.
  "MonJul 27") wherever the day changes between consecutive events,
  matching `calendar_api.py`'s own day-carry-forward parsing.
- **Handles three distinct states** the mock-data version never had to:
  no calendar server configured (existing message, unchanged), a
  configured server that fails to respond (new: "Could not load events --
  check the connection and try again"), and a successful fetch that
  matches zero events against the current filters (new: "No events match
  your current filters for this range" -- previously impossible with a
  fixed 6-event mock list).
- **Periodic re-poll** (`calendar_view_poll()`, called from `loop()`) every
  10 minutes, inside the brief's own suggested 5-15 minute range ("don't
  hammer the source"). Rate-limiting is handled in `calendar_view_poll()`
  itself rather than inside the refresh function it calls, specifically so
  the no-server-configured case (where the refresh function returns
  immediately without doing anything) still only gets checked once per
  interval instead of on every single `loop()` iteration once the first
  interval has elapsed.
- **Settings changes now reflect immediately, not after up to 10 minutes**
  -- closes the gap flagged back in Milestone 6 ("changing filters (or the
  server URL) here doesn't refresh the calendar view['s data]"). Returning
  from Settings (`settings_screen.cpp`'s "Back to Calendar") now calls
  `calendar_view_refresh()`.
- **A server configured after boot works without a reboot.** If no server
  was set when `calendar_view_create()` ran, only the "no server
  configured" message exists -- no tabs/header/list. `calendar_view_refresh()`
  checks for exactly this case and, if Settings has since gained a server
  address, deletes that message and builds the rest of the UI for the
  first time before fetching, rather than requiring a restart to notice.

**Build status:** compiles cleanly, no warnings (RAM 35.2%,
115,432 / 327,680 bytes; Flash 71.5%, 2,250,615 / 3,145,728 bytes).
Backend side unchanged from the Forex Factory switch above and already
verified end-to-end there; this milestone is untested on real hardware
(no device access) -- the JSON field names were checked directly against
`calendar_api.py`'s actual output rather than assumed, but the UI/timing
behavior (loading state, tab switching, periodic poll, post-Settings
refresh) hasn't been confirmed on the device yet.

## Milestone 8: countdown/alert logic + value coloring — done

Requested as an addition to Milestone 7 rather than waiting for its own
pass: a live countdown to the next event, a sound alert for high-impact
events, an automatic post-event refresh, and coloring actual/previous
values the same way forexfactory.com does. All four needed real
wall-clock time, which this firmware didn't have until now.

- **NTP + timezone, not just NTP.** Forex Factory's `day`/`time` text
  carries no timezone of its own, and turned out not to be in the ESP32's
  local timezone by default -- confirmed directly (not assumed): an
  anonymous request's page displayed a clock exactly 4 hours behind real
  UTC in July, and its own JS explicitly declares
  `timezone_name: 'America/New_York'`. `main.cpp` calls
  `configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov")`
  once after WiFi connects -- the POSIX TZ string's DST rule (2nd Sunday in
  March to 1st Sunday in November) lets `getLocalTime()`/`mktime()` handle
  EST/EDT transitions correctly year-round via the standard C library,
  rather than hand-rolling DST math that would drift wrong for half the
  year. Non-blocking; everything downstream treats "time not synced yet"
  (`getLocalTime()` returning false) as "nothing to do yet," not an error.
- **`alert_manager.*` (new)**, matching the brief's suggested architecture.
  `alert_manager_set_events()` is called by `calendar_view.cpp` after every
  successful fetch; it parses each event's `day`+`time` text into a real
  timestamp (skipping "All Day"/"Tentative" -- nothing to count down to)
  and keeps the parsed list. `alert_manager_tick()`, called every `loop()`
  iteration but rate-limited internally to once a second, then:
  - tracks the soonest event within 10 minutes and exposes it as countdown
    text (`alert_manager_get_countdown_text()`, e.g. "NEXT: NFP in 09:47")
  - plays a sound 5 minutes before, but only for `impact_level == 3`
    (red-folder/high-impact) events
  - calls `calendar_view_refresh()` 30-40 seconds after an event's
    scheduled time, once, to pick up its just-released actual value

  Each of the sound/refresh actions is tracked per event ID (Forex
  Factory's own stable `data-event-id`, now returned as `id` in
  `/calendar`'s JSON -- see `LXC/README.md`) so a threshold staying crossed
  for several ticks doesn't repeat the action every tick.
- **`audio_player.*` (new)**: I2S tone generation for the alert sound.
  Pins (DOUT=17, BCLK=42, LRC=18) are verified directly against Elecrow's
  own V3.0 hardware example
  (`example/V3.0/Arduino/Course/Example2_Play_music`) -- the same hardware
  revision this project's display/board config already came from -- not
  guessed, and confirmed not to overlap any pin already used for display/
  touch/backlight. Raw sine-wave synthesis over the legacy ESP-IDF
  `driver/i2s.h`, matching Elecrow's own example's approach exactly, rather
  than a full audio-file decoder library -- alert sounds are short
  synthesized beeps, not music/speech, so a decoder would be unused
  complexity. The legacy `driver/i2s.h` header does emit a build-time
  deprecation warning (ESP-IDF suggests `driver/i2s_std.h` instead); left
  as-is since it matches the verified-working vendor reference rather than
  rewriting to an unverified newer API.
- **Countdown display** sits in the calendar screen's existing free space
  to the right of the Day/Week tabs (x=410, next to tabs ending at x=390
  and the WiFi/settings icons starting around x=728) rather than requiring
  the whole layout to shift down again for a dedicated row. Empty unless
  `alert_manager` has a timed event within its 10-minute window.
  `calendar_view_tick()` (new, called every `loop()` iteration right after
  `alert_manager_tick()`) is the only thing that touches it.
  `calendar_view_refresh()`/`alert_manager_set_events()` call each other
  across module boundaries (calendar_view triggers alert tracking after
  each fetch; alert_manager triggers a refetch 30-40s after an event) --
  an intentional two-way dependency between these two files, not
  accidental coupling, since the feature inherently needs both directions.
- **Actual/previous value coloring**, from `calendar_api.py`'s new
  `actual_state`/`previous_state`/`previous_revised` fields (see
  `LXC/README.md`): `THEME_COLOR_VALUE_BETTER`/`_WORSE` (green/red, matching
  forexfactory.com's own CSS exactly, not a retro-palette color) replace
  the flat `THEME_COLOR_AMBER_DIM` those two columns used before. A
  revised previous value gets a trailing `*` in the label text itself
  (simplest way to flag it without a second icon/glyph asset).
- **`CalendarEvent` gained `id`/`actual_state`/`previous_state`/
  `previous_revised`**, parsed in `calendar_client.cpp` alongside the
  existing fields -- same shape as the JSON, nothing derived client-side.

**Build status:** compiles cleanly (RAM 35.3%, 115,568 / 327,680 bytes;
Flash 72.4%, 2,277,791 / 3,145,728 bytes -- still comfortable headroom).
One expected warning (the legacy I2S deprecation notice above), no others.
Backend fields verified end-to-end against the live site (real
better/worse/neutral distributions, real revision flags) -- see
`LXC/README.md`. The alert timing/audio/countdown behavior itself is
untested on real hardware (no device access) -- the logic was written
against real, verified inputs (actual DST rules, actual I2S pins, actual
event ID stability) rather than guessed, but hasn't been confirmed on the
device yet.

**Resolved from hardware testing: display corrupted (white/black, garbage
scrolling down the screen) immediately after boot, right after WiFi
connected.** Not a crash -- touch events kept logging normally the whole
time, confirming the firmware itself was alive and running; only the
display output was corrupted. Root cause: `audio_player_begin()` called
`i2s_driver_install()` once at boot and left it installed for the rest of
the device's uptime. ESP32-S3's RGB LCD peripheral (`Bus_RGB`) shares
GDMA/PSRAM bandwidth with I2S, and I2S master mode generates a continuous
bit clock -- and therefore continuous DMA traffic -- from the moment it's
installed, not just while a tone is actively playing. A persistently
installed I2S driver was contending with the display's DMA for the whole
session, not just during an alert, which is exactly why corruption started
right at boot rather than only around the first alert (none had even
fired yet). Confirmed against the underlying board hardware -- SKU
DIS07050H -- via Elecrow's own published Arduino tutorial for this same
5" HMI display, which uses these identical I2S pins (17/42/18), not just
the GitHub example repo cited above; two independent Elecrow sources agree
on the pin mapping, so the pins themselves were never in question. Fixed
by removing `audio_player_begin()` entirely: `audio_player_play_alert()`
now installs the I2S driver immediately before playing its tone and calls
`i2s_driver_uninstall()` immediately after, confining the DMA contention
to the brief (well under a second) duration of an actual alert instead of
the device's entire uptime.

Pins were subsequently confirmed a third way, directly from the actual
schematic for this board (SKU DIS07050H): the SCH+PCB zip linked from
Elecrow's product page (`ESP32_Dispaly_5.0_module-SCH+PCB_schematic.zip`)
contains `CrowPanel ESP32 Display-5.0-inch-V3.0-20240314.sch`, whose
ESP32-S3 pin connector net labels read `IO17_I2S_SDIN`, `IO42-I2S_BCLK`,
`IO18_I2S_LRCLK` verbatim -- an exact match to what was already in
`audio_player.cpp`. The onboard amp is an NS4168 (mono Class-D I2S amp/DAC).

**Serial test command added** (`main.cpp`'s `handle_serial_command()`) so
the countdown/sound/refresh pipeline can be exercised on real hardware
without waiting for an actual calendar event to approach: type
`testalert` (red-folder test event 400s/6:40 out -- clears the 5-minute
alert threshold with room to spare, so the sound fires for real once the
countdown crosses 5:00 rather than immediately) into the serial monitor,
or `testalert <seconds>` / `testalert <seconds> <impact_level>` for other
timings. `alert_manager_inject_test_event()` appends a synthetic event
(negative, decrementing IDs so repeated injections and real Forex Factory
IDs can never collide) alongside whatever's already tracked.

**Removing the persistent I2S install wasn't sufficient on its own.**
Confirmed via a `testalert` run on real hardware: the display survived
fine while idle (the original fix), but corrupted again -- backlight
staying on, screen heavily pixelated, recoverable only with a manual
reset -- the moment the alert tone actually started playing. Root cause is
the same GDMA/PSRAM bandwidth contention as before, just triggered by a
much heavier competitor: actively streaming real audio samples through
I2S is sustained, substantial DMA traffic, unlike an idle I2S peripheral
(register-level clock generation only) or WiFi's bursty usage.

The vendored LovyanGFX fork's `Bus_RGB.cpp` already uses the ESP-IDF
RGB LCD driver's "bounce buffer" mode specifically to insulate the panel
from this class of problem -- the frame buffer lives in PSRAM
(`flags.fb_in_psram = 1`), and GDMA feeds the panel from a smaller
internal-SRAM staging buffer instead of contending for PSRAM directly --
but it was sized at only `width * 10` (~10 scanlines), not enough margin
to absorb a full ~300ms tone's worth of competing I2S DMA without
underrunning. Increased to `width * 40` (~4x). This is a runtime
parameter (`esp_lcd_rgb_panel_config_t::bounce_buffer_size_px`), not an
ESP-IDF Kconfig/sdkconfig setting, so it doesn't require switching
`platformio.ini` off `framework = arduino` (prebuilt libraries) -- if
there's ever not enough internal SRAM for a larger buffer,
`esp_lcd_new_rgb_panel()` simply fails and logs
`ESP_LOGE("Bus_RGB", "esp_lcd RGB init failed: ...")` rather than
corrupting silently, so this was safe to try more aggressively rather
than incrementally. **Untested on hardware yet** -- if pixelation during
an alert still happens after this, the next lever is ESP-IDF's
`CONFIG_LCD_RGB_RESTART_IN_VSYNC`/`CONFIG_SPIRAM_FETCH_INSTRUCTIONS`/
`CONFIG_SPIRAM_RODATA` sdkconfig options (documented on the ESP32 forum
for this exact WiFi/BT-vs-RGB-LCD contention class of issue, likely
applicable to I2S too) -- but those need the bigger `arduino, espidf`
combined build mode, not attempted yet since the simpler runtime lever
hadn't been tried first.

**The `*4` bounce buffer alone wasn't enough either** -- confirmed via
another `testalert` run: same pixelation, still needing a manual reset.
That result is itself informative: giving the display four times the
local cushion and still losing tells us this isn't "the buffer runs dry
occasionally, give it more runway" so much as genuine bandwidth
contention on the shared bus for the entire time I2S is actively
streaming -- a bigger cushion just delays, rather than prevents, hitting
the same wall. The lever not yet pulled was reducing how much bandwidth
I2S actually demands in the first place: `audio_player.cpp` was running
I2S at 44100Hz (CD quality) for two plain sine beeps (880Hz/1318Hz) whose
Nyquist requirement is under 2700Hz -- massive, unnecessary headroom that
was pure added contention with nothing to show for it. Dropped to 8000Hz,
roughly a 5.5x cut in I2S's actual DMA bandwidth footprint, on top of
(not instead of) the larger bounce buffer. **Untested on hardware yet.**
If pixelation still happens after *both* of these, that's a stronger
signal the ESP-IDF sdkconfig levers (`CONFIG_LCD_RGB_RESTART_IN_VSYNC`
etc., which need the `arduino, espidf` combined build mode) are actually
necessary rather than optional -- worth treating that as the next step in
sequence, not a third parallel guess.

## Roadmap (from the brief, plus Milestones 4 and 6 which weren't in it)

1. ✅ LVGL hello world on real hardware (display + touch bring-up)
2. ✅ WiFi connect + HTTP GET to the calendar API, raw response to serial
3. ✅ Boot splash screen (custom retro image)
4. ✅ On-device WiFi setup (not in the original brief, added for productization)
5. ✅ Calendar UI with static/mock data (dark/amber styling, layout)
6. ✅ Settings screen: calendar filters (impact/currency/columns, via Forex Factory) + WiFi reconfigure (not in the original brief)
7. ✅ Real data fetch, wired to `LXC/calendar_api.py`'s `/calendar` endpoint (day/week tabs, periodic poll, Settings-triggered refresh)
8. ✅ Countdown/alert logic (10-min countdown, 5-min red-folder sound, +30s post-event refresh) + actual/previous value coloring
9. Retro-style screensaver, to consider at the very end. The CrowPanel is
   an IPS LCD, not OLED, so it isn't at risk of the classic permanent
   pixel burn-in OLED/plasma panels get -- this is about backlight wear
   and (on a cheap panel) temporary image persistence from running static
   high-contrast content 24/7, not a defect that needs fixing. Lower
   priority than 7/8, but a fun one -- something like a CRT-style
   scanline/phosphor-fade effect, or a bouncing amber logo, after N minutes
   of no touch input.

## Known quirks

- `esp32-s3-devkitc-1-myboard.json` is copied from Elecrow's example
  unmodified. Its `upload.maximum_size` says 8MB, but this board's actual
  flash is 4MB (matches `huge_app.csv`'s partition table, which fits exactly
  in 4MB) — likely a copy-paste leftover from an N8 variant board definition
  in Elecrow's own file. Left as-is since it's the verified-working
  reference; PlatformIO's actual flash layout comes from
  `board_build.partitions` (`huge_app.csv`), not this field.
- **Erasing NVS wipes WiFi credentials AND the calendar server URL
  together, not just WiFi.** To force the WiFi setup screen to reappear for
  testing (rather than waiting on a future Settings option or a full chip
  erase + reflash), the whole NVS partition can be erased directly:
  `esptool --port <COM> erase_region 0x9000 0x5000` (offset/size from
  `huge_app.csv`). That erases the *entire* NVS partition, which holds both
  `wifi_cfg` (WiFi credentials) and `server_cfg` (calendar server URL) as
  separate namespaces — wiping one wipes both. Missing WiFi credentials
  force the (blocking) WiFi setup screen on next boot, so that's hard to
  miss; a missing server URL doesn't block anything (the calendar view
  still shows mock data regardless), so it's easy to not notice until
  something that actually calls `calendar_client.*` (e.g. opening Settings
  → Calendar Filters) fails with "not configured". If you erase NVS to
  retest WiFi setup, remember to also revisit Settings → Calendar Server
  afterward.
- Seeing `[E][Preferences.cpp:47] begin(): nvs_open failed: NOT_FOUND` in
  the serial log the first time `wifi_credentials_load()` or
  `calendar_server_url_load()` runs after a fresh flash or an NVS erase is
  **expected, not a bug** — ESP-IDF's NVS logs exactly this when a
  namespace is opened read-only before anything has ever been written to
  it. Both `*_load()` functions already handle it correctly (return false,
  caller falls back to its own default/setup flow); the scary-looking `[E]`
  is just how the underlying library reports "nothing here yet."
