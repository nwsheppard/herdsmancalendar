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

## Milestone 8: countdown/alert logic + value coloring — done (sound alert dropped)

Requested as an addition to Milestone 7 rather than waiting for its own
pass: a live countdown to the next event, a sound alert for high-impact
events, an automatic post-event refresh, and coloring actual/previous
values the same way forexfactory.com does. All four needed real
wall-clock time, which this firmware didn't have until now. **The sound
alert was ultimately dropped** after reliably corrupting the display on
real hardware and a full afternoon of escalating fixes -- see the hardware
debugging log and final outcome at the bottom of this section before
assuming `audio_player.*` still exists; it doesn't.

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
  - tracks the soonest event within 10 minutes and exposes both a countdown
    string as "MM:SS" (`alert_manager_get_countdown_text()`) and that
    event's id (`alert_manager_get_next_event_id()`)
  - calls `calendar_view_refresh()` 30-40 seconds after an event's
    scheduled time, once, to pick up its just-released actual value

  The refresh is tracked per event ID (Forex Factory's own stable
  `data-event-id`, now returned as `id` in `/calendar`'s JSON -- see
  `LXC/README.md`) so a threshold staying crossed for several ticks
  doesn't repeat it every tick. Originally also played a sound 5 minutes
  before red-folder events -- dropped; see below.
- **Countdown display lives on the event's own row**, not a separate
  banner. Originally it was a standalone label next to the Day/Week tabs,
  but the user asked to move it onto "the line that is upcoming" instead:
  each row is now tagged with its event id via `lv_obj_set_user_data()`
  (same pattern already used for checkboxes in
  `settings_filters_screen.cpp`/`settings_columns_screen.cpp`), and
  `calendar_view_tick()` -> `update_next_event_row()` swaps that row's TIME
  column from its scheduled clock time to the live "MM:SS" countdown and
  adds an amber border, reverting the row back to its plain time once it
  stops being "next" (event fires, drops out of the 10-minute window, or a
  refresh drops it). Only touches the row(s) that actually changed state --
  the common case (same event, next tick) is a single label update on the
  already-highlighted row, not a full-list rescan every second. A fresh
  `refresh_events()` call resets the tracked highlight id so the border
  gets reapplied to the new row objects rather than assuming a highlight
  from before the list rebuild survived (it doesn't -- `lv_obj_clean()`/
  `clear_list_yielding()` destroy the old row objects entirely).
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

**Build status (final, sound removed):** compiles cleanly, no warnings
(RAM 35.3%, 115,520 / 327,680 bytes; Flash 71.8%, 2,258,531 / 3,145,728
bytes). Backend fields verified end-to-end against the live site (real
better/worse/neutral distributions, real revision flags) -- see
`LXC/README.md`. Countdown display and the post-event auto-refresh were
confirmed working on real hardware via the `testalert` serial command
(see below); the sound alert was also confirmed working (it played), but
reliably corrupted the display doing so, and was removed -- see the full
debugging log below for why.

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
(not instead of) the larger bounce buffer.

**Same issue a third time, with a different symptom, confirmed the real
fix needed Kconfig access.** Reported from hardware: elements ("Upcoming
Events" title, gear icon) visually shifted/dropped toward the bottom of
the screen, not just pixelated -- a different, more specific ESP32-S3
RGB LCD failure mode than plain corruption, known as "frame shift" (a
DMA stall desyncs the panel driver's internal read pointer from the
display's actual VSYNC timing). This has one documented fix,
`CONFIG_LCD_RGB_RESTART_IN_VSYNC`, which is an ESP-IDF Kconfig option --
unreachable from `framework = arduino` (prebuilt libraries), only
reachable by switching to the combined `framework = arduino, espidf`
build (Arduino compiled as an ESP-IDF component from source).

**That switch was attempted on a `esp32-espidf-build-mode` branch (work
backed up via a commit on `main` first) and ultimately abandoned.** In
order, hit and resolved:
1. A corrupted `tool-cmake` package (PlatformIO's own extraction silently
   dropped `cmake.exe` from the installed package -- not antivirus, a
   manual re-extraction confirmed the binary persisted and ran fine once
   placed by hand).
2. ESP-IDF's CMake tooling hard-rejects any project path containing a
   space -- and this repository's parent folder was named "CRT Terminal".
   Neither a `subst` drive letter nor a proper NTFS junction fully masked
   this: a component (`libsodium`, pulled in transitively by Arduino's
   mandatory RainMaker/Zigbee/Modbus/Insights dependencies for the esp32s3
   target -- none of which this project uses) still baked the real,
   space-containing absolute path into its build commands regardless of
   which alias the build ran from. Resolved only by actually renaming the
   folder to `CRT-Terminal` (done with the user's explicit confirmation,
   since it's a real filesystem change outside the repo itself).
3. `CONFIG_FREERTOS_HZ=1000` required (Arduino's timing assumes a 1ms
   tick; ESP-IDF's plain default is 100Hz).
4. `CONFIG_AUTOSTART_ARDUINO=y` required (without it, nothing provides
   `app_main()`, so `setup()`/`loop()` never get called at all).

Then hit a fifth issue with no quick documented fix: `NetworkClientSecure`
(part of Arduino's WiFi library, unused by this project -- everything
here is plain `http://`) failed to link with undefined references into
`ssl_client.cpp` (`start_ssl_client`, `send_ssl_data`, etc.) -- the file
exists in `arduino-esp32` but wasn't part of whatever source list
PlatformIO's Arduino-as-component integration compiles in this
configuration. At that point -- four unrelated build-system issues
already resolved, a fifth with no clear answer in hand, and still not
having reached the point of even testing whether
`CONFIG_LCD_RGB_RESTART_IN_VSYNC` fixes the original bug -- the decision
was made to stop rather than keep absorbing open-ended risk for a
nice-to-have feature. Reverted to the `main` checkpoint;
`esp32-espidf-build-mode` remains as a branch if this is ever worth
revisiting (a repo without "CRT Terminal"'s space in its path is one
fewer issue to solve next time).

**Final decision (at the time): the sound alert is dropped, not fixed.**
`audio_player.h`/`.cpp` deleted; `alert_manager.*` no longer references
impact level or plays anything, just tracks the countdown and triggers
the post-event refresh. `main.cpp`'s `testalert` command lost its
`<impact_level>` argument accordingly (it never did anything without the
sound to gate).

**The I2S diagnosis above was wrong.** Reported from hardware afterward:
the exact same "frame shift" symptom (header/gear icon dropping toward
the bottom of the screen) still happened on a plain calendar refresh --
with the sound alert, `audio_player.*`, and all I2S code completely
removed from the project. That ruled out every fix attempted above
(bounce buffer size, I2S sample rate, and the abandoned
`CONFIG_LCD_RGB_RESTART_IN_VSYNC` build-mode detour) as ever having
addressed the actual cause -- they were chasing a coincidence: the
`testalert` runs that "confirmed" I2S as the cause always also triggered
a `calendar_view_refresh()` (the post-event auto-refresh, 30-40s after
the test event's simulated time), and that refresh was the real trigger
all along.

**Real root cause:** `populate_events()` (`calendar_view.cpp`) calls
`lv_obj_clean(list)` to clear the old event rows, then loops building new
ones -- each row is 8 LVGL objects (the row + an impact bar + 6 labels).
A week view can be 50-100+ events, so a single refresh is hundreds of
object creations and flex-layout recalculations in one uninterrupted
burst, with no call back to `lv_timer_handler()` anywhere in that loop.
LVGL's own rendering *and* this project's RGB panel flush
(`display_flush()` -> `presentFrameBuffer()` in `main.cpp`) only run when
`lv_timer_handler()` runs -- so a long enough gap between calls starves
the panel's bounce-buffer refill exactly the way sustained I2S DMA was
suspected of doing, except the actual cause was this project's own UI
code blocking the timer loop, not a peripheral contention issue at all.

**Fix:** `populate_events()` now calls `lv_timer_handler()` once right
after `lv_obj_clean(list)`, and again every 8 rows while building the new
list, so no single blocking stretch runs long enough to trigger the
symptom. No build-system changes, no ESP-IDF Kconfig access needed --
this was fixable entirely in application code once correctly diagnosed.

**Every-8-rows wasn't tight enough margin.** Confirmed on hardware: fixed
on the Day tab (short list), still reproduced on the Week tab
specifically (50-100+ events -- the one case where an 8-row gap is still
a lot of uninterrupted object creation). Tightened twice:
1. `lv_obj_clean(list)` (a single call that can destroy 100+ objects left
   over from the previous fetch) never got a chance to yield mid-teardown
   at all, only once after it finished. Replaced with
   `clear_list_yielding()`, which deletes one child at a time and calls
   `lv_timer_handler()` after each.
2. The creation loop's "every 8 rows" became every single row.

More `lv_timer_handler()` calls than strictly necessary is cheap
insurance here -- this only runs during an explicit refresh (tab switch,
periodic poll, post-Settings, post-event), not every frame, so the extra
call overhead doesn't cost anything that matters.

**Still recurring after the every-row fix, including brief self-correcting
"blips" (screen shifts, then recovers on its own)** -- reported from
hardware. That self-correcting behavior is actually a useful clue: it
reads more like the RGB panel's VSYNC-vs-DMA-readpointer desync
(`CONFIG_LCD_RGB_RESTART_IN_VSYNC`'s territory -- see above) recovering on
its own once a later `lv_timer_handler()` call catches back up, rather
than a hard corruption that only clears on reset. `populate_events()`'s
row-building loop was the confirmed cause of the original, more severe
symptom, but per-row yielding doesn't cover every stall in this codebase --
notably, `calendar_client_get_calendar()`'s `deserializeJson()` call plus
the loop building each `CalendarEvent`'s `String` fields (`calendar_client.cpp`)
runs with **no** `lv_timer_handler()` call anywhere in it, same hazard as
the row-building loop, and for a week's worth of events (100+) is a
plausible-sized stall on its own.

**Added instead of another guess: logging to pin down exactly when and
where a stall happens**, rather than continuing to patch specific call
sites one at a time against symptoms alone:
- `calendar_view.cpp`'s `timed_timer_handler(context)` wraps every
  `lv_timer_handler()` call in this file (`clear_list_yielding()`,
  `populate_events()`'s per-row yield, `refresh_events()`'s "Loading
  events..." repaint) and logs to Serial (with an absolute `millis()`
  timestamp) whenever a single call takes more than 20ms -- roughly a
  couple of frames' worth at 60fps, well past routine per-frame variance.
- `main.cpp`'s `loop()` does the same for its own `lv_timer_handler()`
  call, *and* separately logs whenever the gap between one loop()
  iteration ending and the next starting exceeds the same 20ms threshold
  -- a catch-all for any stall not individually instrumented elsewhere
  (WiFi reconnects, NVS/Preferences access, touch I2C hiccups, etc.),
  since it fires on elapsed wall-clock time regardless of which line
  caused it.
- `calendar_client.cpp`'s `http_get()` already logged each GET's elapsed
  time; now also logs an absolute ending timestamp alongside it, and
  `calendar_client_get_calendar()` separately logs how long parsing the
  response body took (event count + elapsed + ending timestamp),
  unconditionally (it only runs once per refresh, not once per frame, so
  there's no noise concern).

None of this fixes anything by itself -- it's purely diagnostic. Compiles
clean (RAM 35.3%, Flash 71.8%).

**The logging paid off immediately.** With no countdown active, no
stalls logged at all over normal use -- consistent with the user's report
that the original frame-shift symptom hadn't recurred since the yield
fixes. But testing the new row-based countdown (`testalert`, targeted at
a real displayed row via `calendar_view_get_first_event_id()` -- seebelow)
turned up a **different, previously invisible problem**: once a countdown
was actively showing on a row, `lv_timer_handler() (main loop)` logged a
~66-67ms stall on essentially *every single* `loop()` iteration,
continuously, for the entire time the countdown was up (with periodic
~102ms spikes on top, once a second) -- not an occasional glitch, a
sustained ~15x/second cost the whole feature was quietly paying.

**Root cause:** `calendar_view_tick()` -> `update_next_event_row()` runs
every `loop()` iteration (unrated), and its "same event still counting
down" fast path called `lv_label_set_text()` on the row's TIME label every
single time, even though `alert_manager_get_countdown_text()`'s return
value only actually *changes* once a second (alert_manager rate-limits its
own tick internally). LVGL's `lv_label_set_text()` invalidates the label
regardless of whether the new text equals the old text, and this
project's display is configured `LV_DISPLAY_RENDER_MODE_FULL` -- any
invalidation forces a full 800x480 frame redraw and `presentFrameBuffer()`
call, which at this panel's 12MHz pixel clock costs ~66ms. So for as long
as a countdown was visible, the main loop was silently doing a full,
unnecessary frame redraw around 15 times a second instead of once. This is
a very plausible explanation for the intermittent "screen shifts, then
recovers on its own" blips reported from hardware -- a sustained,
self-inflicted near-continuous redraw load is exactly the kind of thing
that could intermittently starve the bounce buffer/DMA timing without
being severe enough to cause the harder, non-recovering corruption from
Milestone 8's original bug.

**Fix:** `calendar_view.cpp` now tracks `last_shown_countdown_text` and
only calls `lv_label_set_text()` when the countdown string actually
changed since the last tick, both in the "same event" fast path and when a
new row first gets highlighted. Compiles clean (RAM 35.3%, Flash 71.9%).

**Confirmed on hardware.** Re-running `testalert` after this fix: the
constant ~66-67ms stall on every single `loop()` iteration is gone,
replaced by a single elevated `lv_timer_handler()` line roughly once a
second (~69-105ms) -- matching the countdown label legitimately changing
once a second and triggering one real full-frame redraw, not a bug. Visual
confirmation from hardware: no shift/pixelation seen while the countdown
was live on a row. One open, minor curiosity not yet explained: that
once-a-second stall's duration drifts upward over about 7 cycles (69 -> 74
-> 79 -> 83 -> 88 -> 94 -> 100ms) before resetting -- much smaller and less
severe than the original bug, not investigated further since the visible
symptom is gone.

Also fixed alongside this: `alert_manager_inject_test_event()` originally
always used a synthetic, decrementing id (e.g. `-1`) that has no
corresponding row in the currently-displayed list -- harmless when the
countdown was a standalone banner (any text would do), but once the
countdown moved onto the row itself, a synthetic id meant
`update_next_event_row()` had nothing to find and highlight, so
`testalert` silently did nothing visible. `alert_manager_inject_test_event()`
gained an optional `reuse_event_id` parameter, and `main.cpp`'s
`handle_serial_command()` now passes `calendar_view_get_first_event_id()`
(a new accessor exposing the first currently-displayed event's real id) so
the test countdown actually lands on a visible row.

**Two small UI changes alongside the retest:**
- **Day Events is now the default launch screen**, not Week (`current_range`
  in `calendar_view.cpp` now starts `"day"`, and `build_events_ui()`
  initializes the tab styling to match via `set_active_tab(day_tab_button,
  week_tab_button)`). Purely a user preference -- `calendar_api.py`'s own
  `/calendar` endpoint still defaults to `week` when no `range` param is
  given, this only changes what the firmware requests first.
- **Font swapped app-wide from Montserrat to LVGL's built-in `unscii_16`**,
  a monospace bitmap font modeled on classic PC/VGA text-mode glyphs --
  fits this project's retro-CRT-terminal styling much better than a
  proportional sans-serif, and reads as "more VGA" per the request. Every
  `lv_font_montserrat_14`/`_20`/`_40` usage across the whole app (calendar
  view, boot splash, settings and its sub-screens, WiFi setup) now points
  at `&lv_font_unscii_16` -- one consistent retro font everywhere rather
  than swapping only the calendar screen and leaving other screens
  mismatched. `platformio.ini`'s `LV_FONT_MONTSERRAT_*` flags were
  initially replaced outright with a single `LV_FONT_UNSCII_16=1`.

  **Wrong on the glyph metrics, caught immediately from hardware.** The
  original writeup here claimed unscii's glyphs are a fixed 8px wide,
  narrower than Montserrat's proportional metrics -- backwards. Checked
  directly against the generated font source
  (`.pio/libdeps/esp32s3/lvgl/src/font/lv_font_unscii_16.c`): every glyph's
  `adv_w` is `256` in LVGL's 1/16px fixed-point encoding, i.e. **16px
  wide** -- unscii_16 is a 16x16 monospace font, not 8 wide x16 tall. Real
  hardware promptly reported exactly what that implies: grid cells
  wrapping (a fixed-width TIME/ACT/FCST/PREV column budgeted for
  Montserrat 14's much narrower proportional digits couldn't hold text at
  16px/character), button labels unaffected (`theme_create_button()` never
  set an explicit font on its label to begin with, silently falling
  through to `LV_FONT_DEFAULT` the whole time -- the app-wide swap missed
  every single button), and the WiFi icon rendering as a blank box (unscii
  has no `LV_SYMBOL_*` glyphs at all; only Montserrat's tables bundle
  LVGL's icon range).

  **Fix, three parts:**
  1. Added `LV_FONT_UNSCII_8=1` (8x8, confirmed `adv_w=128`/8px from the
     same source file) for anything living in a tight, fixed-width space:
     the event grid's cells and header row, the day-separator label, and
     the two message labels with no explicit width set
     (`no_server_message`, `status_label` -- both have long lines that
     would've run close to or past the 800px screen width at 16px/char)
     and the boot splash's boot-log/copyright text (hand-padded to a fixed
     28-char width in `make_boot_line()`, sized for the box at
     `box_text_width=440px`). `unscii_16` stays for standalone text with
     no tight width budget: the calendar/settings/wifi-setup screen titles.
  2. `theme_create_button()` (`theme.cpp`) now explicitly sets
     `&lv_font_unscii_16` on every button's label instead of silently
     inheriting `LV_FONT_DEFAULT` -- this is what actually fixes "button
     fonts are still the same," in one place rather than at each of the
     ~15 call sites. The two call sites whose "text" is actually an
     `LV_SYMBOL_*` icon (the settings gear, the WiFi-setup password
     eye-toggle) explicitly override the label back to `&lv_font_montserrat_20`
     immediately after creation, same reasoning as the WiFi status icon fix.
  3. Both WiFi status icon labels (`main.cpp`'s boot splash,
     `calendar_view.cpp`'s calendar screen) reverted from unscii back to
     `&lv_font_montserrat_20` -- an icon glyph, not a word, needs a font
     that actually contains it.

  `platformio.ini` now enables `LV_FONT_UNSCII_8`, `LV_FONT_UNSCII_16`,
  `LV_FONT_MONTSERRAT_14` (restored, kept as `LV_FONT_DEFAULT` so anything
  not explicitly touched by this change -- checkboxes, dropdowns, textarea
  placeholders on the filters/columns/server settings screens -- keeps its
  original pre-font-swap appearance rather than silently inheriting
  whatever LVGL's fallback logic picks with Montserrat 14 no longer
  enabled) and `LV_FONT_MONTSERRAT_20` (icons + button-adjacent Montserrat
  needs). Also added `LV_LABEL_LONG_DOT` to every fixed-width grid label
  (previously only the flexible EVENT-name column had a long_mode set at
  all -- the fixed columns silently defaulted to LVGL's wrap-by-default,
  which is what actually broke the row's fixed 56px height): belt-and-
  suspenders so a still-too-long value in the future ellipsizes instead of
  wrapping and blowing out the row, regardless of font. Compiles clean
  (RAM 35.3%, Flash 69.7%). **Not yet re-confirmed on hardware** as of this
  writing.

**Countdown extended to multiple simultaneous events.** Raised directly
from a design question about the row-based countdown: the original
implementation only ever tracked a single "soonest" event
(`alert_manager_get_next_event_id()`), so if two or more events shared (or
nearly shared) a scheduled time -- routine for a trading calendar, e.g.
several US indicators all releasing at 8:30am -- only the first one the
scan happened to see got a countdown; the others got nothing despite being
equally "next." Reworked to highlight all of them:
- `alert_manager.*`: `next_event_id`/`alert_manager_get_countdown_text()`
  (singular) replaced with `active_events` (every `{id, seconds_left}`
  currently inside the 10-minute window, recomputed every tick, not just
  the minimum) and two accessors: `alert_manager_get_active_event_ids()`
  (all of them) and `alert_manager_get_countdown_text_for(event_id)` (that
  specific one's "MM:SS", or `""` if it's not active).
- `calendar_view.cpp`: `highlighted_event_id`/`last_shown_countdown_text`
  (singular) replaced with `highlighted_rows` (a
  `vector<{id, last_shown_countdown_text}>`). `update_next_event_row()`
  diffs the previous tick's highlighted set against the current active
  set: rows that dropped out get unhighlighted
  (`unhighlight_row()`, factored out since both the old single-event code
  path and this one needed it), rows newly active get the border +
  initial countdown text, and rows still active only get a
  `lv_label_set_text()` call if their own countdown string actually
  changed -- same per-row anti-thrash guard as the single-event version,
  just tracked per id now instead of once globally.

`testalert`'s serial command gained a second optional argument to test
this -- `testalert <seconds> <row_index>` targets the Nth currently-
displayed row (0 = first, the previous always-first behavior) instead of
always the first, via a new `calendar_view_get_event_id_at(index)`
accessor. Two overlapping calls with different row indices (e.g.
`testalert 90 0` then `testalert 95 1`) let both be verified at once
without waiting for a real simultaneous release. **Confirmed on hardware**:
both rows highlighted independently, each with its own live countdown.

- The settings screen's big "SETTINGS" header (previously Montserrat 40px,
  no same-size monospace unscii equivalent exists) now also uses
  `unscii_16`, smaller than before -- kept for font-family consistency with
  the rest of the app over preserving that one header's original size.

**Third round: unscii's two fixed sizes weren't enough, so a custom
scalable font replaced it.** Reported back from hardware: the button font
(the whole-app default from `theme_create_button()`, `unscii_16`) reached
the sides of the Day/Week Events tabs, and the grid font (`unscii_8`, from
the wrap fix above) was too small to read comfortably. The request was
specific -- buttons down one size, cells and the rest of the small-tier
text up two -- which exposed the real limit of `unscii_8`/`_16`: LVGL's
built-in version only ships in those two fixed sizes, nothing in between,
so neither "down one" nor "up two" is actually satisfiable by picking
between them. Worse, checked directly: jumping the grid to `unscii_16`
outright to solve "too small" would have shrunk the flexible EVENT-name
column from ~45 characters of headroom to ~6-7 before ellipsizing --
trading one readability problem for a worse one, not fixing anything.

Solved with a real scalable font instead of another guess at which of two
fixed sizes to pick:
- **`assets/VT323-Regular.ttf`** (Peter Hull/The VT323 Project Authors,
  SIL Open Font License 1.1, free to embed) -- a monospace font modeled on
  vintage terminal displays, sourced from Google Fonts' own
  `github.com/google/fonts` repo. Converted to two custom LVGL bitmap
  fonts via `npx lv_font_conv` (Node/npm were already available locally):
  `src/fonts/lv_font_vt323_22.c` and `src/fonts/lv_font_vt323_28.c`
  (`--size 22`/`28`), both over the full printable ASCII range
  (`0x20-0x7E`) with kerning dropped (monospace doesn't need it). Declared
  in a new `include/fonts.h`, which also documents the exact regeneration
  command.
  **First attempt rendered every glyph as a solid rectangle on real
  hardware** -- generated with `--bpp 4` (antialiased) and RLE compression
  left on (`lv_font_conv`'s default), this project's LVGL build wasn't
  decoding that compressed bitmap format correctly. Fixed by regenerating
  both with `--bpp 1 --no-compress --no-prefilter` -- the exact settings
  LVGL's own built-in `unscii` fonts ship with (confirmed from
  `lv_font_unscii_16.c`'s own header comment) and which render correctly
  in this project. Costs the antialiasing `--bpp 4` would have given, but
  crisp 1-bit edges fit a retro/VGA look at least as well anyway.
- **Why this actually solves the tradeoff, not just "a third size":**
  checked directly from each generated font's glyph table (`adv_w`, LVGL's
  1/16px fixed-point advance width) -- VT323's letterforms are
  proportioned taller than they are wide, so its advance width per pixel
  of visual height is much narrower than unscii's square 1:1 glyphs.
  `vt323_22` has a 19px line height (more than double `unscii_8`'s 8px --
  a real readability jump) but only an ~8.8px advance width (`adv_w=141`),
  barely wider than `unscii_8`'s 8px. `vt323_28` (22px line height,
  ~11.2px advance width, `adv_w=179`) reads clearly bigger than
  `unscii_16` ever did on a button without touching the sides, because
  it's still narrower per character than unscii_16's 16px.
- **`vt323_22`** replaces every previous `unscii_8` use: the event grid's
  cells and header row, the day-separator label, `status_label`,
  `no_server_message`, and the boot splash's boot-log/copyright text.
  `col_time_w`/`col_act_w`/`col_fcst_w`/`col_prev_w` (`calendar_view.cpp`)
  widened modestly (70 -> 85/80/80/85; `col_ccy_w` unchanged at 55, already
  had room) for the slightly wider advance width, computed against
  worst-case content ("Tentative", an 8-digit value, PREV plus a revision
  `*`) -- `LV_LABEL_LONG_DOT`'s ellipsis fallback (added during the wrap
  fix) still covers anything past that budget.
- **`vt323_28`** replaces `theme_create_button()`'s per-button default
  (`unscii_16` before this) -- fixes every button app-wide in the one
  place, same as the earlier button-font fix. `settings_screen.cpp`'s
  `make_menu_button()` no longer needs its own explicit font override to
  look bigger than a plain button -- that was pointing at the exact same
  `unscii_16` the default itself now used, a redundant no-op once the
  earlier button-font fix landed; removed rather than left as dead code
  pointing at a font that no longer exists in this codebase.
- **Untouched by this pass, on purpose:** every screen title *except* the
  calendar screen's own (see below) stays on `unscii_16` -- not part of
  what was reported broken, and changing them wasn't asked for. The WiFi
  status icon, settings gear, and WiFi-setup password eye-toggle stay on
  Montserrat, same reasoning as before -- VT323 is a text font like
  unscii, no `LV_SYMBOL_*` icon glyphs either.

Compiles clean (RAM 35.3%, Flash 70.0%).

**Two more rounds of hardware feedback, both resolved:**
1. **Every glyph rendered as a solid rectangle**, not the actual
   letterform -- see `fonts.h`'s doc comment. Root cause: the first
   conversion used `--bpp 4` (antialiased) with RLE compression left on
   (`lv_font_conv`'s default), and this project's LVGL build doesn't
   decode that compressed bitmap format correctly. Fixed by regenerating
   both fonts with `--bpp 1 --no-compress --no-prefilter` -- confirmed
   from `lv_font_unscii_16.c`'s own header comment that these are the
   exact settings LVGL's built-in fonts ship with, and unlike the custom
   ones, those always rendered correctly here. Costs the antialiasing
   `--bpp 4` would have given; crisp 1-bit edges fit a retro/VGA look at
   least as well anyway.
2. **Readable, but asked to read "just a bit more."** `vt323_22`
   (19px line height) regenerated at size 26 instead (22px line height,
   `adv_w=166` -> ~10.4px advance width, up from ~8.8px) -- renamed
   `lv_font_vt323_26` throughout (file, symbol, `fonts.h` declaration,
   every call site) rather than leave a `_22` name pointing at a 26px
   font. `col_time_w`/`col_act_w`/`col_fcst_w`/`col_prev_w` widened again
   for the new advance width (85/80/80/85 -> 100/90/90/95;
   `col_ccy_w` still unchanged at 55).

   Also fixed in the same pass: the calendar screen's own "UPCOMING
   EVENTS" title, reported as reading small next to the WiFi/gear icons
   sharing its top strip -- and since those icons anchor to the top-right
   corner rather than the title's own position, the title had to change
   rather than the layout. Bumped from `unscii_16` to `vt323_28` (the
   button tier) -- a deliberate one-off scoped to just this title, not an
   app-wide title bump, since it's the only title that shares a row with
   icons in the first place.

Compiles clean (RAM 35.3%, Flash 69.9%).

**Reported back from hardware: the title change above wasn't visible at
all** -- not "still too small," but pixel-identical to before the edit,
even after a clean rebuild and reflash (confirmed the object code was
current: a no-op rebuild showed nothing left to recompile). Left as an
open question rather than guessed at further -- see the next entry, which
incidentally re-tests this same code path with a completely different
font.

**VT323 dropped for Space Mono, picked directly by the user from a
side-by-side comparison.** Rather than keep iterating blind on font
choice, built an HTML comparison page (self-contained, fonts embedded as
base64 data URIs) showing 8 candidates -- VT323, Share Tech Mono, Space
Mono, IBM Plex Mono, Courier Prime, DotGothic16, Silkscreen, Major Mono
Display, sourced from Google Fonts' own `github.com/google/fonts` repo --
each rendering the same three lines (a real grid row, a digit-legibility
torture test, a button label) at the actual on-device sizes. Prompted by
the user separately flagging that VT323's `0` and `2` read as too similar,
which the torture test line was built specifically to surface across
every candidate at once. User picked Space Mono.

`assets/VT323-Regular.ttf` and `src/fonts/lv_font_vt323_26.c`/`_28.c`
removed; replaced with `assets/SpaceMono-Regular.ttf` (Colophon
Foundry/Google Fonts, SIL OFL 1.1) and `src/fonts/lv_font_spacemono_18.c`/
`_20.c`, generated with the same proven-safe settings
(`--bpp 1 --no-compress --no-prefilter`) established during the VT323
rectangle-rendering bug above. `fonts.h` rewritten (declarations, doc
comment, regeneration command) rather than patched -- see its current
comment for the full font history and exact metrics.

Space Mono's letterforms are close to square, unlike VT323's noticeably
taller-than-wide proportions -- anticipated directly by the user before
even trying it ("I don't think we will be able to keep the same size").
Calibrated the same way as VT323 (checking `adv_w` from a range of test
conversions): landed on 18px for the grid tier (19px line height, `adv_w=176`
-> 11.0px advance width) and 20px for the button/title tier (21px line
height, `adv_w=196` -> 12.25px advance width) -- both a step down in pixel
size from the VT323 tiers (26px/28px) for a comparable visual weight,
confirming the expected tradeoff. Every `vt323_26`/`vt323_28` reference
renamed to `spacemono_18`/`spacemono_20` throughout (`calendar_view.cpp`,
`main.cpp`, `theme.cpp`), including the button-tier calendar-screen title
from the entry above -- this reflash also serves as a second, independent
test of whether that title actually updates, since it's now a completely
different font family rather than a bigger size of the same one.

`col_time_w`/`col_act_w`/`col_fcst_w`/`col_prev_w` widened again for the
wider advance width (100/90/90/95 -> 105/95/95/100; `col_ccy_w` still
unchanged at 55, "USD" never got close to that budget under any font
tried so far).

Compiles clean (RAM 35.3%, Flash 69.9%).

**Reported back: cells look good, but the selected tab button reads as
"washed out," and the title still doesn't clearly look like a different
font -- "could just be it's small and grainy."** Both trace to the same
choice: the initial Space Mono conversion used `--bpp 1` (no
antialiasing), matching unscii's settings exactly out of caution left over
from the earlier rectangle-rendering bug -- but that bug was specifically
about *compression*, never actually about antialiasing itself; `--bpp 4`
had never been tried uncompressed. Regenerated both fonts `--bpp 4
--no-compress --no-prefilter` (confirmed `bitmap_format=0`, same
uncompressed layout as before, just with grayscale antialiasing data now
present) -- smooths the jagged 1-bit edges causing the "grainy" read, and
should make thin dark-on-amber strokes (the selected tab's text/background
swap) read as an actual soft-edged glyph instead of a harsh single-pixel
line that optically looks weaker than the same weight reads light-on-dark.
`adv_w`/line-height are unchanged (font metrics, not bitmap depth), so no
column-width changes needed this round. Compiles clean (RAM 35.3%, Flash
70.1% -- antialiasing bitmap data is larger than 1-bit, expected).

**Title still didn't read as bigger, reported once more.** Two attempts at
sharing an existing tier (`unscii_16`, then `spacemono_20`, the button
size) both landed as "doesn't look changed." Rather than try a third
shared tier, gave the title its own dedicated size: `spacemono_32`
(33px line height, `adv_w=313` -> 19.6px advance width), used nowhere
else. At `x=20, y=10` this puts the title's bottom edge around `y=43`,
comfortably clear of the Day/Week tabs starting at `y=58`. Compiles clean
(RAM 35.3%, Flash 70.6%).

**Calendar screen's WiFi icon recolored off the boot splash's neon
green.** `THEME_COLOR_SPLASH_TERMINAL_GREEN` was sampled from the boot
splash artwork specifically (see its own doc comment in `theme.h`) and
reused for the calendar screen's WiFi icon too, since both were driven by
the same `apply_pending_wifi_icon_state()` call with one shared color per
state -- reported back as reading too bright against the calendar
screen's amber theme. `main.cpp`'s `apply_pending_wifi_icon_state()` now
passes different colors to the two icons for "connected"/"connecting":
the boot splash keeps green/dim-grey (matches its own artwork), the
calendar icon gets `THEME_COLOR_AMBER`/`_AMBER_DIM` (matches its own
screen's text). Both keep the same red "disconnected" color/strike --
that's a status signal, not a theme choice, and reads the same either
way. Compiles clean (RAM 35.3%, Flash 70.6%). **Not yet seen on real
hardware** as of this writing.

**Day-separator text had no space between weekday and month** ("FriJul
31"), noticed directly on the display and asked about rather than assumed
broken. Root cause was server-side, in `calendar_api.py` -- see
`LXC/README.md`'s Maintenance notes for the full writeup (a BeautifulSoup
`get_text()` concatenation quirk against Forex Factory's actual nested
markup, confirmed directly against the live site). `alert_manager.cpp`'s
`parse_event_timestamp()` had its own matching fix: it used to skip
exactly 3 characters to get past the weekday ("MonJul 27" ->
`event.day.substring(3)` -> "Jul 27"), which the backend fix would have
silently broken (the corrected "Mon Jul 27" would leave a leading space
before "Jul", shifting the parse and making every event's day/time
unparseable) -- changed to skip to the first space instead, which works
correctly against both the old and new backend format. Compiles clean
(RAM 35.3%, Flash 70.6%). **Backend fix requires redeploying
`calendar_api.py` to the LXC container** (`pct enter <ctid>`, then
`update` -- see `LXC/README.md`'s Updating section) -- not something
this session can do directly.

**Grid widened to shift ACT/FCST/PREV right, PREV asked to line up under
the WiFi icon.** First attempt just widened `list`/`header` from 760 to a
hand-picked 780 (flush to the screen's right edge) -- reported back as
"shifted some, but not all the way." Rather than guess a third pixel
value, computed the grid's width at runtime instead: `build_events_ui()`
now calls `lv_obj_update_layout(wifi_icon_label)` (forces its
`lv_obj_align()` to resolve immediately -- alignment is otherwise lazily
applied on the next layout pass, not synchronous) and reads its actual
rendered right edge via `lv_obj_get_coords()`, then sizes `header`/`list`
so the grid's own right edge matches it exactly. `make_header_row()`
gained a `width` parameter instead of a hardcoded constant, since it now
needs whatever `build_events_ui()` computes rather than a fixed number.
This should be correct regardless of font metrics, rather than another
hand-tuned guess. Compiles clean (RAM 35.3%, Flash 70.6%). **Reported back:
"right back under the gear icon"** -- still not landing exactly where
intended. Rather than keep hand-guessing the grid's absolute right edge a
fourth time with no hardware feedback in the loop, the request that
followed this report shifted to something more concretely implementable
(below) -- the exact-icon-alignment question is left open; `grid_width`'s
computation (still keyed off `wifi_icon_label`'s position) wasn't touched
again in the work below, since none of it depends on exactly where the
grid's right edge lands, only on how the width it has is divided up
internally. Worth another look with the user actually watching, not
another blind guess.

## Overnight worklist (four requests, done unattended -- see the checklist at the end)

Requested as an explicit list to complete without stopping for
confirmation (no hardware access until morning), with an explicit
instruction not to ask clarifying questions. Where a judgment call was
needed, the reasoning is written out below so it can be revisited/reversed
easily if it guessed wrong.

**1. "PREV right justified, FCST/ACT not too wide past their biggest
numbers, all the space I can get for EVENT."** Previously ACT/FCST/PREV
were fixed widths (95/95/100) sized for a worst case that's rarely
actually all on screen at once. Replaced with `compute_column_layout()`
(`calendar_view.cpp`): scans whatever `events` actually came back from
the current fetch, measures the longest `actual`/`forecast`/`previous`
value (the `previous` measurement includes the trailing `*` a revised
value gets), and sizes each column to `chars * 11px + 10px` -- 11px is
spacemono_18's confirmed advance width (`adv_w=176` in 1/16px
fixed-point, see `fonts.h`), 10px is a small padding margin so tight-fit
text doesn't touch its neighbors. Never narrower than the column's own
header text ("ACT"/"FCST"/"PREV"), so the header can't itself get
clipped by an unusually narrow data column (e.g. a range with no actual
values released yet). Since EVENT is the row's only `flex_grow` column,
narrower ACT/FCST/PREV automatically hands EVENT the freed space --
nothing separate needed to "give EVENT more room."

`PREV` right-justified (`lv_obj_set_style_text_align(label,
LV_TEXT_ALIGN_RIGHT, 0)`, both the header label and each row's) -- reads
like a real table's numeric column, flush against the grid's own right
edge, rather than left-hanging text in a now-tightly-sized column. ACT/FCST
weren't asked to be right-justified and were left as-is (left-aligned).

Because column widths now depend on the *current* fetch (and, see below,
the current currency filter), the header can no longer be a build-once
static row -- `make_header_row()` became `rebuild_header_row()`: deletes
any existing `header` object and builds a fresh one every call, tracked
in a new namespace `header` pointer (parallel to `list`, which already
worked this way). Called once in `build_events_ui()` with fallback widths
(before the first fetch resolves, so the screen isn't blank while
"Loading events..." shows), then again from `populate_events()` every
single refresh with the real measurement. Header rebuild cost is trivial
(7 objects) next to the up-to-100+ row list that already gets fully
rebuilt the same way every refresh.

**2. "Drop CCY if only 1 currency is selected; EVENT starts there. Most
people are going to use it for USD."** `refresh_events()` now also calls
`calendar_client_get_filters()` (a small extra `/filters` GET each
refresh, alongside the existing `/calendar` fetch) and checks
`filters.currency_codes.size() == 1`. If so, `show_ccy=false` flows
through to both `rebuild_header_row()` and every `make_event_row()` call
for that refresh, and the CCY label is skipped entirely -- not shrunk,
not blanked, just never created, so EVENT's `flex_grow` absorbs that
space exactly like it does for ACT/FCST's freed width above. Defaults to
showing CCY (the previous, safe behavior) if the filter fetch itself
fails, matching this file's existing fallback direction elsewhere. This
re-evaluates on every refresh (tab switch, periodic poll, post-Settings
return), so changing the currency filter in Settings and coming back
updates it without needing a reboot.

**3. "All EVENT cells word-wrapped by default, not the Week tab's `...`
after visiting it once."** The `add_label()` lambda in `make_event_row()`
used `LV_LABEL_LONG_DOT` (ellipsis) for the flex-grow EVENT column --
added earlier as the fix for a real bug (a *fixed*-height row plus
unbounded wrap could blow out past the row's height and corrupt the
display, confirmed on hardware -- see the "wraps the cells" writeup
above). Changed to `LV_LABEL_LONG_WRAP`, made safe to re-enable by the
row-height change below rather than reverting into the same bug.
Fixed-width columns (TIME/CCY/ACT/FCST/PREV) still ellipsize -- short
single-value cells, not free text, so multi-line wrapping would look
wrong there even though the row can now grow to fit it.

**Row height changed from a fixed 56px to `LV_SIZE_CONTENT`**, specifically
to make wrap safe to turn back on: a row that can grow to fit wrapped
text can't overflow it. LVGL sizes a content-fit flex container to its
tallest child -- `impact_bar`'s own fixed 40px height sets a sensible
floor for an ordinary single/double-line row (both comfortably under 40px
at spacemono_18's 19px line height), and a row wrapping to 3+ lines
simply grows taller than that instead of clipping, with `impact_bar`
staying its original size, vertically centered. Added explicit
`pad_top`/`pad_bottom` (6px each) to the row, since content-fit sizing
with zero padding would hug the text with no breathing room at all.

Wasn't in the original three-item list but follows directly from item 3 --
without it, turning wrap back on would have reintroduced the exact bug it
was originally added to fix, just with a different trigger (a single very
long event name instead of many rows built too fast). Flagged here as a
judgment call made without hardware to check it against, not something
explicitly requested.

Compiles clean (RAM 35.3%, Flash 70.6%). **None of this has been seen on
real hardware** -- built and self-reviewed only (checked for stale
references to renamed functions/constants, confirmed the row's TIME label
is still always child index 1 regardless of whether CCY is present, since
`update_next_event_row()`/`unhighlight_row()` depend on that), since no
hardware access was available overnight. See the checklist below for what
to verify first.

**Overnight checklist (verify each on hardware, then check off):**
- [x] PREV reads right-justified, flush against the grid's right edge --
      **confirmed on hardware**, and also now lines up under the WiFi
      icon (the "under the gear icon instead" report from before this
      worklist resolved itself once these changes landed -- not
      separately touched).
- [x] ACT/FCST columns are visibly narrower than before, roughly hugging
      their actual widest value rather than a lot of empty padding --
      confirmed, but see the immediate follow-up fix just below.
- [ ] EVENT column visibly wider than before (more of a long event name
      fits before truncating)
- [ ] With exactly one currency selected in Settings -> Filters, the CCY
      column is gone entirely and EVENT starts where it used to be
- [ ] With two or more currencies selected, CCY still shows as before
- [ ] Day tab: event names wrap across multiple lines instead of showing "..."
- [ ] Week tab: same -- wraps instead of ellipsizing, including after
      switching away to Day and back (the original inconsistency reported)
- [ ] Rows with wrapped (multi-line) event names don't visually overlap
      the row below them -- confirms the row auto-height actually works
      as intended
- [ ] No recurrence of the frame-shift/pixelation display bug from
      Milestone 8, especially on Week (many rows, most likely to have a
      long name wrap to 3+ lines)

**Immediate follow-up, same session:** ACT/FCST's *values* were still
left-aligned within their new (narrower) columns -- only PREV had actually
been right-justified the first time through, per how the original request
was worded ("I need the PREV right justified, with the FCST and ACT
columns not too wide"). Reported back as reading misaligned against their
own headers once the columns were narrow enough for the difference to be
obvious. Both the ACT/FCST data labels and their header labels
(`add_label`/`add_header_label` calls in `make_event_row()`/
`rebuild_header_row()`) now pass `right_align=true`, matching PREV.
Compiles clean (RAM 35.3%, Flash 70.6%). Not yet re-confirmed on hardware
as of this specific change.

**Reported still misaligned after that fix, before it had actually been
flashed yet.** Investigated without hardware access (no native compiler
available in this environment to build a literal LVGL render test):
checked the live backend directly for hidden whitespace in
`actual`/`forecast`/`previous` (`repr()` against the raw JSON -- none
found), then traced LVGL's own alignment code
(`lv_label.c`'s `calculate_x_coordinate()`: for `LV_TEXT_ALIGN_RIGHT`,
`x += label_width - text_pixel_width`) to confirm the already-applied
`right_align=true` fix above is correct against LVGL's actual
implementation, not just this project's assumption about how it should
work. No bug found in either the data or the alignment code -- most
likely explanation is the report described the build from before that
fix, not a failure of the fix itself. Still **not confirmed on real
hardware**.

**Screensaver: researched, not built.** Asked about feasibility (idle
detection, wake-on-touch, time-of-day wake, a custom candlestick-chart
screensaver) -- explicitly deferred to a future session rather than
built tonight, so this is a research note, not a change:
- Idle detection: `lv_display_get_inactive_time(NULL)`, wake-on-touch is
  automatic (any touch already resets it). Time-of-day wake is
  straightforward given NTP is already wired up (`configTzTime()` in
  `main.cpp`).
- Not really solving a burn-in problem on this hardware (RGB TFT, not
  OLED/plasma) -- would be a visual flourish, not hardware protection.
- LVGL's built-in `lv_chart` widget has no candlestick/OHLC series type
  (checked directly: only `LINE`/`BAR`/`SCATTER`) -- a real candlestick
  chart needs either `lv_canvas` (a manually-allocated pixel buffer --
  confirmed `LV_USE_CANVAS`/`LV_USE_CHART` both default-enabled given
  this project's build flags, but a half-screen-sized RGB565 canvas
  alone is ~456KB, well over this chip's ~327KB internal RAM, so it'd
  need a PSRAM-backed buffer) or plain LVGL objects (a colored rect per
  candle body + a thin rect per wick, positioned programmatically --
  recommended instead, since it reuses the exact pattern `impact_bar`
  already uses successfully in the event grid, with zero extra pixel
  buffer needed).
- The real risk if this gets built: this display only redraws in full
  800x480-frame mode (~66ms per invalidation, confirmed earlier during
  the countdown-stall investigation) -- an animated screensaver that
  updates too often would reintroduce that exact class of bug. Whatever
  gets built needs the same discipline already established for the
  countdown (update on a slow tick, skip the redraw if nothing changed).

**The frame-shift/pixelation glitch is back, and this time the `[stall]`
logging cleared the app of suspicion.** Reported again from hardware, with
the serial log captured right around the actual occurrence for the first
time (earlier captures kept missing it) -- every `[stall]` line nearby was
in the same 55-81ms range this project has seen constantly, including on
refreshes with no visible glitch at all. No outlier, nothing pointing at a
specific blocking call. That rules out application-level blocking as the
cause here, which is what all of this project's fixes so far (the
per-row/per-delete yielding, the countdown label-invalidation fix) have
been targeting -- the remaining cause is lower-level than anything
`lv_timer_handler()` scheduling can reach.

Checked whether `CONFIG_LCD_RGB_RESTART_IN_VSYNC` (this project's own
documented fix for genuine RGB-LCD DMA contention, see the
`bounce_buffer_size_px` comment in `Bus_RGB.cpp`) is reachable any other
way than the abandoned ESP-IDF Kconfig build mode: confirmed directly
against this exact build's own vendored header
(`esp_lcd_panel_rgb.h`, from `framework-arduinoespressif32-libs`) that
`esp_lcd_rgb_panel_config_t.flags` has no such bit -- it's genuinely a
compile-time Kconfig-gated behavior inside the driver implementation, not
a runtime toggle, so there's still no way to reach it short of that
abandoned build mode.

That same header search turned up something new, though:
`flags.bb_invalidate_cache` -- a real runtime bit on the exact same
`esp_lcd_rgb_panel_config_t` this project already configures, previously
left unset (defaulting to 0 via the struct's `= {}` zero-init). Per its
own doc comment, it invalidates the CPU cache's view of data GDMA just
read into the bounce buffer -- only meaningful because bounce-buffer mode
is already on (`fb_in_psram`/`bounce_buffer_size_px` above); without it,
nothing sits between GDMA and PSRAM for a cache to be stale about. A
plausible, previously-untested explanation for an intermittent visual
desync that isn't explained by any single slow function call: a narrow
cache-coherency window where the display pipeline could read stale data
that GDMA had already overwritten. Enabled in `Bus_RGB.cpp`, right after
`fb_in_psram`. Compiles clean (RAM 35.3%, Flash 70.6%). Cheaper and lower-
risk to try first than reopening the ESP-IDF combined build mode -- a
one-flag runtime change on infrastructure already in place, no
build-system change required. **Not yet confirmed on hardware** -- next
step is exactly the same wait-and-watch process that caught the glitch
this time (keep the serial monitor running, note whether it recurs at
all, and if it does, whether it's now rarer/gone or unchanged).

**`bb_invalidate_cache` didn't help -- reported back after another clean
capture** (same unremarkable 55-81ms `[stall]` range, nothing correlated,
two clean passes in a row now). Rules out cache coherency alongside
application-level blocking; both of this project's own instrumented
layers have now been cleared. Also notable: `[stall]` logging can only
ever catch a *slow function call* -- a genuine VSYNC/DMA desync doesn't
need anything to block the CPU long enough to cross even a tight
threshold, since it's the panel driver's own internal read pointer losing
sync with the timing signal, plausibly from a bus-arbitration hiccup
(WiFi's own DMA traffic, for instance) far too brief to ever show up in a
`millis()`-based measurement. Consistent with everything ruled out so far
pointing at something below what this project's own diagnostics can see.

**Next experiment, chosen over reopening the ESP-IDF build mode
immediately:** `bounce_buffer_size_px` bumped from `*40` to `*80` (see
its own updated comment in `Bus_RGB.cpp` -- also corrected there: `*40`
was never actually confirmed to matter, since it was originally a fix
attempt against the *misdiagnosed* I2S-contention theory, not this
glitch). Cheap, reversible, no build-system change. Compiles clean (RAM
35.3%, Flash 70.6% -- unchanged, since the bounce buffer is a runtime
heap allocation inside `esp_lcd_new_rgb_panel()`, not a static global the
linker's own RAM figure accounts for; if `*80` is too aggressive for
available internal RAM, that would show up as an obvious boot-time
failure on first flash, not a subtle runtime symptom).

**Predicted failure mode happened exactly as flagged: `*80` didn't
boot.** Reverted immediately back to `*40` (confirmed-working; still
includes `bb_invalidate_cache`, harmless on its own even though it didn't
fix the glitch). This effectively closes off "just make the bounce
buffer bigger" as a cheap lever -- internal SRAM on this chip is a small,
already-committed budget (LVGL's own buffers, FreeRTOS task stacks,
WiFi's stack, and the bounce buffer needing space for its own
double-buffering all draw from the same pool), and `*40` is apparently
already close to what's actually available, not conservative headroom.
Smaller increments (`*50`/`*60`) remain technically available but
weren't tried -- doubling was meant to be a decisive test, and it was:
decisively no. Compiles clean (RAM 35.3%, Flash 70.6%). **Confirmed
back to a bootable state on hardware** was the only thing actually
verified here -- the glitch itself is exactly where it was before this
detour: application-level blocking and cache coherency both ruled out,
buffer size now also ruled out, and the two remaining options
(reopening the ESP-IDF build mode, or accepting this as a known rare
quirk) are unchanged from above.

If sound is ever wanted again, `CONFIG_LCD_RGB_RESTART_IN_VSYNC` (via the
combined `arduino, espidf` build mode explored and abandoned above) is
still the documented fix for genuine RGB-LCD DMA contention in general --
that avenue wasn't invalidated by this discovery, it just turned out to
be solving a problem this project didn't actually have.

**Separately, reported after reflashing the reverted (`*40`) firmware:
WiFi connected initially (reached `loop()`, so `setup()`'s connect
succeeded) but then dropped and stayed stuck on `loop()`'s "WiFi still
down, retrying..." backstop, on a network with nothing else reported
unusual. `wifi_on_event()` logged nothing about *why* it disconnected --
only `ARDUINO_EVENT_WIFI_STA_DISCONNECTED` -> a boolean icon-state
change, no detail. Switched to `WiFi.onEvent()`'s
`arduino_event_info_t`-carrying overload and log
`info.wifi_sta_disconnected.reason`/`.rssi` on that event -- the ESP-IDF
WiFi stack's own disconnect reason code (auth failure, AP not found,
beacon timeout, etc.) and signal strength at the moment of the drop,
neither previously captured anywhere. Doesn't fix anything by itself
(`loop()`'s blind retry-every-10s backstop is unchanged) -- purely
diagnostic, same spirit as the `[stall]` logging above, so the *next*
occurrence actually says something instead of just "still down."
Compiles clean (RAM 35.3%, Flash 70.6%). **Not yet seen on real
hardware.**

**Third capture of the frame-shift glitch actually found something real.**
A Week-tab refresh's first `populate_events row` `[stall]` logged 114ms --
noticeably above the ~66-70ms baseline every other capture (including
this one's own second line, 68ms) had shown so far. Root cause:
`rebuild_header_row()` (called every single refresh since a few sessions
ago, to keep ACT/FCST/PREV widths and CCY visibility current -- see the
worklist entries above) deletes the old header and creates up to 7 new
objects (the header container + TIME/CCY/EVENT/ACT/FCST/PREV labels) with
*no yield of its own*. That whole backlog was silently bundling into
whichever `lv_timer_handler()` call happened to run next -- the first row
in the loop right below it -- inflating just that one call specifically,
exactly matching the 114-then-68 pattern in the log. Fixed with one more
`timed_timer_handler()` call, right after `rebuild_header_row()` and
before the row loop starts, same pattern already used everywhere else in
this function. Compiles clean (RAM 35.3%, Flash 70.6%). This is the
first time one of these captures has actually shown a real outlier
instead of routine noise -- **not yet confirmed fixed on hardware**, but
unlike the previous two dead ends (cache coherency, bounce buffer size),
this one has an actual, specific, previously-unaccounted-for burst of
object creation to point to.

**Fourth capture: glitch recurred with the header-rebuild fix in place,
and this time the log is unambiguous -- the fix worked, but didn't help
the glitch.** Every `[stall]` line in the capture (`clear_list_yielding`
at 86/71/71/71ms, `populate_events header rebuild` at 69ms, `populate_events
row` at 68ms x3) sits inside the routine 55-86ms range; no 114ms-style
outlier this time, meaning the header-rebuild yield is doing exactly what
it was added to do. The glitch still happened anyway. That makes three
out of three real captures now where the visible glitch has no
correlated slow `lv_timer_handler()` call to blame -- the one time a real
outlier did show up (the 114ms capture above), fixing it didn't stop the
glitch from recurring on a later, perfectly ordinary-looking capture.
Combined with `bb_invalidate_cache` (cache coherency) and bounce buffer
size both already ruled out, this closes off application-level blocking
as the cause with about as much confidence as `[stall]` logging can ever
provide -- see the "genuine VSYNC/DMA desync" note above on why this
class of cause wouldn't show up in a `millis()`-based measurement at all.
The two options from before stand unchanged: reopen the abandoned
`arduino, espidf` combined build mode for
`CONFIG_LCD_RGB_RESTART_IN_VSYNC`, or treat this as a known rare
hardware-level quirk this project's own instrumentation can't reach.

**Follow-up investigation into both options -- reading the vendored
driver source itself, not just its header -- found the ceiling on what's
actually reachable here, and it's lower than hoped.**

Checked whether the chip itself can log the underlying hardware event:
`esp_lcd_panel_rgb.c` has an `ESP_EARLY_LOGE(TAG, "LCD underrun")` line,
gated behind `#if LCD_LL_EVENT_UNDERRUN`. Checked this exact build's own
`hal/esp32s3/include/hal/lcd_ll.h`: that event bit is only defined for
the P4 (`LCD_LL_EVENT_VSYNC_END`/`LCD_LL_EVENT_TRANS_DONE` are the only
two on the S3). That log line is compiled out entirely on this chip --
not a reachable lever.

More significantly: the driver already runs its own automatic recovery
every VBlank, independent of `CONFIG_LCD_RGB_RESTART_IN_VSYNC`.
`lcd_rgb_panel_try_restart_transmission()` resets the GDMA channel
whenever it detects `bb_eof_count < expect_eof_count` (a bounce-buffer
underrun it tracked itself) -- the Kconfig flag only changes this from
"restart when a mismatch is detected" to "restart unconditionally every
single VBlank." Its own comment describes precisely this project's
symptom as a known side effect of the *recovery itself*: "if this
interrupt is late enough, the display will shift ... the single-frame
desync this leads to is preferable to the permanent desync that could
otherwise happen." In other words, the glitch may well be this exact
safety net doing its job, not an unrecovered failure -- which means
enabling the Kconfig flag would likely make an already-firing recovery
path fire *more* often, not fix anything.

Looked for a way to get a direct signal on that recovery path without
the build-mode switch: `esp_lcd_rgb_panel_event_callbacks_t` has an
`on_bounce_empty` hook, called every time the driver refills a bounce
buffer. Checked `lcd_rgb_panel_fill_bounce_buffer()`'s actual gating,
though, and it only invokes that callback when `panel->num_fbs == 0` (the
driver's "no internal framebuffer, caller supplies pixels manually"
mode) -- otherwise it takes the plain `memcpy()` path. `Bus_RGB.cpp` sets
`panel_config.num_fbs = 2`, so `on_bounce_empty` would never fire in this
project's configuration; registering it would be dead code, not a
diagnostic. Ruled out without touching hardware.

The actual recovery function is `static` and passes no event data even
to the callbacks that do exist (`esp_lcd_rgb_panel_event_data_t` is an
empty struct) -- there is no application-reachable way to instrument it.
The only way to get a real signal (a counter, incremented inside
`lcd_rgb_panel_try_restart_transmission()` itself) would be compiling the
LCD driver component from source, which is the same combined
`arduino, espidf` build mode again -- now valuable only for that counter,
independent of whether `CONFIG_LCD_RGB_RESTART_IN_VSYNC` itself helps.

Re-examined what that switch would actually take, per the abandoned
`esp32-espidf-build-mode` branch's own account above. That branch is a
stale pointer at an old `main` commit, not a WIP diff -- nothing to
resume, the same steps would need repeating. Of the five issues hit last
time, one is now moot (the repo's parent folder was renamed from
`CRT Terminal` to `CRT-Terminal` for unrelated reasons since that
attempt, which removes the CMake space-in-path blocker), two more have
known one-line fixes (`CONFIG_FREERTOS_HZ=1000`,
`CONFIG_AUTOSTART_ARDUINO=y`), but the fifth -- `NetworkClientSecure`
(unused by this project; everything here is plain `http://`) failing to
link with undefined references into `ssl_client.cpp` -- is exactly where
the previous attempt stopped, still unresolved, with no known fix.

**Decision: not pursuing the build-mode switch for now.** The linker
issue that stopped the previous attempt is still open and would need
fresh investigation to solve, and even solving it would very likely
enable a flag that (per the analysis above) probably doesn't fix the
actual glitch. Both diagnostic avenues this project's own code can reach
(`[stall]` logging, `bb_invalidate_cache`) are exhausted, and the
avenues that remain (chip-level underrun IRQ, `on_bounce_empty`) are
confirmed dead ends on this hardware/configuration. This is being left
as a known, rare, low-severity artifact of the RGB panel driver's own
DMA-desync recovery path -- below the ceiling of what this project's
instrumentation, or the currently-reachable ESP-IDF driver surface, can
observe or influence.

**2026-08: new data point on what triggers the recovery path, from the
FlareSolverr-related fetch work above.** Reported directly: the glitch
landed exactly during a `/calendar` fetch that was failing with a
read-timeout (the `setTimeout()` `uint16_t` overflow above) -- 4.5s of the
WiFi radio actively waiting on a stalled connection. WiFi radio activity
is the standard ESP32 suspect for the kind of interrupt-latency spike that
would make `lcd_rgb_panel_try_restart_transmission()`'s recovery visible
(per its own comment, quoted above: "if this interrupt is late enough, the
display will shift"), and this is the first time this project has had a
concrete WiFi-activity window to correlate a real occurrence against,
rather than routine background noise. Not a fix -- the recovery path
itself is still confirmed unreachable from application code (see above) --
but it sharpens *why* FlareSolverr made this worse independent of fetch
duration alone: every `/calendar` fetch now keeps the radio meaningfully
busy for several seconds (previously well under one), so each refresh has
a bigger window for this to land in, on top of just taking longer overall.
The `poll_interval_ms` throttle (Milestone 8, above) already mitigates via
frequency; there's no equivalent lever for shortening a single fetch's own
radio-active window, since that's dictated by FlareSolverr's own solve
time, not anything this firmware controls.

**Confirmed on hardware:** the glitch recurred on a fetch that succeeded
and loaded events normally -- not just on the earlier failing/retrying
one. That rules out "specific to failed/retrying requests" as the
distinguishing factor: it's the fetch's WiFi-radio-active window itself
(several seconds either way, success or failure) that correlates with the
glitch, not anything about the request erroring. Still not an
application-level fix available -- same conclusion as the rest of this
section, just now on firmer evidence. The only lever this project has is
`poll_interval_ms`'s refresh frequency (already throttled to hourly),
since there's no way to shorten FlareSolverr's own solve time from the
firmware side.

## 2026-08: persistent /calendar+/filters failures, needing a reboot to clear

With the 65000/8000ms timeouts above in place, reported directly: both
`/calendar` and `/filters` started failing with read-timeouts (status
-11) on *every single retry* for 11+ minutes straight (the
`poll_retry_interval_ms` 2-minute backoff, already in place from an
earlier session, kept firing correctly the whole time). Two facts ruled
out the obvious explanations: `journalctl -u herdsman-calendar-api`
covering that exact window showed **zero record** of any of these
requests ever arriving at the LXC (the last successful entry from the
ESP32's IP is well before the failures start, then total silence until
long after they stop) -- if `calendar_api.py` itself were stuck, uvicorn
would still eventually log something. And WiFi never reported
disconnected (`wifi_on_event()`'s reason/rssi logging, added in an
earlier session, stayed silent) -- confirmed directly, not a radio drop.
It also **needed a reboot to recover**, not just time -- later retries
after the failure window didn't start working again on their own.

That combination -- works fine, then every new connection silently fails
with the request never reaching the server, only a reboot clears it --
doesn't point at the server or the WiFi radio. It's the classic signature
of a leaked resource on the ESP32 side itself: most likely lwIP's own
small, fixed socket table, or heap fragmentation from the repeated
`HTTPClient`/`String` churn across retries (a new pattern that didn't
exist before this session's move to a background-task fetch -- see the
core-pinning and timeout entries above). Added unconditional
(`Serial.printf`) free-heap/largest-free-block logging right before each
`refresh_task()` fetch attempt (`calendar_view.cpp`) -- a steady decline
across retries during the next occurrence would confirm a heap leak; flat
heap with fetches still failing would point at socket-table exhaustion
instead (a different fixed resource, not visible from heap alone).
Compiles clean (RAM 35.3%, Flash 70.6%). **Not yet confirmed on
hardware** -- this is purely diagnostic, waiting on the next occurrence
to actually narrow down which resource is leaking.

**Follow-up, confirmed on hardware: the heap logging ruled out a leak,
and turned up a different failure mode too.** Free heap stayed perfectly
flat (9064 bytes, `largest_free_block=7668`) across every single retry
over 27+ minutes straight -- not declining at all, so heap fragmentation
isn't the cause. One retry in that same window also failed with a
*different* HTTPClient status (-5, "connection lost", after 45s) instead
of the usual -11 full-timeout -- a response that started arriving and
then dropped, not just never arriving at all. Combined with the earlier
`journalctl` evidence (zero record of any of these requests reaching the
LXC) and WiFi never reporting disconnected, this doesn't look like a
resource leak or a server-side stall -- it looks like a stale low-level
network path on the ESP32 itself (most likely an ARP cache entry gone
bad), which `WiFi.status()` has no way to detect since the 802.11
association itself never actually dropped. That's also why nothing in
`loop()`'s existing `WiFi.reconnect()` backstop ever caught it -- that
only fires when WiFi reports disconnected, which it never did here. Only
a full device reboot cleared it.

Added `wifi_force_reconnect()` (`wifi_manager.cpp`/`.h`) -- a plain
`WiFi.disconnect()` + `WiFi.reconnect()` cycle, forcing fresh 802.11
association and a fresh DHCP lease without powering down the radio or
needing credentials passed again. `calendar_view.cpp`'s
`apply_ready_refresh_result()` now counts consecutive `/calendar` fetch
failures (reset on any success) and calls it once the count reaches
`consecutive_failure_reconnect_threshold` (3, roughly 6-8 minutes at the
2-minute retry cadence) instead of waiting on a manual reboot to clear
whatever's stuck. This is a workaround for the symptom, not a root cause
fix -- there's no way to inspect the ESP32's live ARP table or ferry that
diagnosis further without local hardware access, and a forced reconnect
is a reasonable, low-risk recovery path regardless of the exact
underlying cause. Compiles clean (RAM 35.3%, Flash 70.7%). **Not yet
confirmed on hardware** whether this actually clears the stuck state when
it recurs, or whether it needs a longer per-attempt wait to let the new
association/DHCP lease actually settle before the next fetch retries.

**Mitigation instead of a fix: reported directly that the glitch shows up
shortly after a refresh specifically, and that constant refreshing isn't
actually needed -- only around the times events are scheduled.** Can't
fix the underlying DMA-desync-recovery mechanism (see above -- below the
ceiling of what this project or the currently-reachable ESP-IDF driver
surface can reach), but can reduce how often the display does the one
thing that's been observed to trigger it: rebuilding the whole event list
(`clear_list_yielding()` + `populate_events()`). `calendar_view_poll()`
was refreshing unconditionally every 10 minutes (`calendar_view.cpp`) --
up to ~144 rebuilds a day regardless of whether anything calendar-
relevant was actually happening, each one a chance to trigger the glitch.
`alert_manager_tick()` (`alert_manager.cpp`) already refreshes 30-40s
after each event's own scheduled time independent of that periodic poll
-- freshness right around real events was never depending on the 10-
minute cadence to begin with. Lengthened `poll_interval_ms` to an hour:
keeps a backstop for newly-added/updated events and rolling the Day tab
over at midnight, while cutting unconditional rebuilds by roughly 6x.
Compiles clean. **Not yet confirmed on hardware whether this
meaningfully reduces how often the glitch is seen** -- it's a reduction
in exposure to the trigger, not a fix for the trigger itself, so it's
expected to make the glitch rarer, not eliminate it.

## 2026-08: calendar fetch moved off the LVGL task

Forex Factory's Cloudflare JS challenge (see `LXC/calendar_api.py`'s
docstring) means `calendar_api.py` now routes `/calendar` through
FlareSolverr instead of scraping directly -- a real fetch now takes
several seconds, and up to ~30s the first time a session needs its
timezone auto-fixed, instead of the well-under-a-second responses this
used to get. `refresh_events()` (`calendar_view.cpp`) called
`calendar_client_get_filters()`/`calendar_client_get_calendar()` straight
from `loop()`'s own task -- the same task that runs `lv_timer_handler()` --
with no yield inside either blocking HTTP call. Reported directly: the
screen now visibly freezes (no redraws, no clock, no alert countdowns) for
however long a refresh takes, which went from unnoticeable to several
seconds (worst case ~30s) purely because of the backend change above, not
anything in the firmware itself.

Fixed by moving the fetch onto its own FreeRTOS task (`refresh_task()`,
pinned to core 0, opposite `loopTask`'s core 1) so `loop()`/
`lv_timer_handler()` keep running while it's in flight. The result comes
back over a single-slot queue (`refresh_result_queue`) and gets applied to
the UI from `apply_ready_refresh_result()`, called every `loop()` iteration
via `calendar_view_tick()` -- that function is the only thing that ever
touches an `lv_obj_t` from this flow, keeping LVGL access on the one task
it's actually safe on. A tab switch (or the periodic poll) that lands while
a fetch is already in flight no longer stacks a second concurrent
HTTPClient/task on top of it -- it just sets a flag
(`pending_refresh_requested`) to kick a fresh fetch once the current one
lands; a result whose range no longer matches the current tab (the user
switched away mid-fetch) is discarded rather than flashing stale data for
the wrong tab. Compiles clean (RAM 35.3%, Flash 70.6% -- unchanged from
before this).

**Follow-up, confirmed on hardware:** `refresh_task()` was initially
pinned explicitly to core 0 (`loopTask` runs on core 1), on the theory
that keeping it off the LVGL task's own core was the safest way to
guarantee it could never block `lv_timer_handler()`. That backfired --
the very first background fetch after flashing failed to even connect
(`GET .../filters failed, status/error: -1` and same for `/calendar`,
each after ~5000ms -- a connect-timeout signature, not a slow response),
with no WiFi disconnect logged around it, meaning the radio link itself
was fine. ESP32 Arduino's own WiFi/lwIP internal tasks also default to
core 0, and pinning our fetch task there put it in contention with them.
Changed to `tskNO_AFFINITY` (let the scheduler place it) -- doesn't need
core separation from `loopTask` to keep the screen responsive anyway,
since this task spends nearly all its time blocked in socket `recv()`,
not burning CPU, so preemption keeps `lv_timer_handler()` running
regardless of which core either task lands on. Compiles clean, same
RAM/Flash.

**Confirmed on hardware:** the `tskNO_AFFINITY` change fixed the connect
failure -- the next boot connected to the calendar API fine. But it
surfaced a second, distinct problem in the same log: `GET .../calendar
failed, status/error: -11` after exactly 8042ms -- HTTPClient's
read-timeout code, hit right at `calendar_client.cpp`'s hardcoded 8s
timeout (`http_get()`). That 8s budget was sized for calendar_api.py's own
local work (filters/health/etc, no upstream dependency) -- `/calendar`
specifically routes through FlareSolverr server-side and calendar_api.py
budgets up to 65s for that (two round trips back-to-back the first time a
session needs its timezone auto-fixed), so 8s was cutting off a fetch that
was still legitimately in progress, not actually stuck. `http_get()` now
takes an optional per-call timeout (default unchanged at 8s);
`calendar_client_get_calendar()` passes 70s. Safe to do now specifically
*because* the fetch runs on its own background task (previous entry) --
before that change, an 70s timeout on the LVGL task would have meant a
70s frozen screen instead of an 8s one. Compiles clean, same RAM/Flash.

**Follow-up, confirmed on hardware:** the 70s timeout didn't take --
`/calendar` failed again, this time after only ~4.5s (still status -11,
read-timeout). Root cause: `HTTPClient::setTimeout()` takes a `uint16_t`,
which tops out at 65535 -- passing 70000 silently wrapped to
`70000 % 65536 = 4464`, and `http_get()`'s own parameter was a `uint32_t`,
so nothing caught the overflow before it reached `setTimeout()`. Fixed
two ways: the actual value passed for `/calendar` is now 65000 (the
largest round number that fits, comfortably above the ~30s worst case
confirmed during `calendar_api.py`'s timezone self-heal testing), and
`http_get()`'s parameter type is now `uint16_t` too, so a future caller
passing an out-of-range value gets a compiler warning at the call site
instead of a silent runtime wraparound. Compiles clean, same RAM/Flash.
**Not yet confirmed on hardware** whether a real `/calendar` fetch now
completes successfully within the corrected budget.

**Follow-up, confirmed on hardware: a sharper root cause, from timing.**
The boot-time fetch worked instantly (7.5s, normal). The very next fetch
-- the first one after a full hour of zero network activity
(`poll_interval_ms`) -- failed to even connect (-1, a connect-timeout,
not a read-timeout). `WiFi.status()` stayed `WL_CONNECTED` the whole time
with no disconnect event, and RSSI at the following forced reconnect was
-33 -- a strong signal, ruling out a weak/degraded RF link. Solid signal,
no disconnect event, connection quietly stops working after sitting idle,
only a fresh reassociation fixes it: that matches a known behavior on
some routers/APs -- silently dropping a station's association state after
an idle period without ever sending a real deauth frame the client could
react to. There's no way to detect that from the ESP32's side; nothing
tells it anything changed.

Also surfaced in this same log: `wifi_force_reconnect()` raced
`WiFi.setAutoReconnect(true)`'s own automatic reconnect --
`WiFi.disconnect()` kicked off auto-reconnect immediately, and the
explicit `WiFi.reconnect()` 100ms later got rejected by ESP-IDF (`sta is
connecting, return error`), so the forced reconnect didn't actually take
effect that time. Fixed by sharing one cooldown timestamp between
`wifi_force_reconnect()` and `loop()`'s own down-detection backstop
(`wifi_reconnect_if_down()`, `wifi_manager.cpp`/`.h`) instead of two
independent timers -- see that function's own header comment for the full
mechanism.

For the idle-association root cause itself: rather than shortening
`poll_interval_ms` (deliberately lengthened to reduce frame-shift glitch
exposure -- see Milestone 8 above) and giving back that reduction, added
a much cheaper keep-alive instead. `wifi_keepalive_poll()` (`main.cpp`)
pings `/health` every 5 minutes -- a plain local check on
`calendar_api.py`'s own LXC with no FlareSolverr/upstream round trip
involved, normally well under 100ms -- specifically to keep the AP
association itself active between the real hourly calendar fetches,
without adding meaningfully to the radio-active time the frame-shift
glitch correlates with. Runs off the LVGL task via its own background
task (`wifi_keepalive_task`), same reasoning as `calendar_view.cpp`'s
`refresh_task()` -- a zombied connection could still take up to
`/health`'s own timeout to fail, which would otherwise freeze the screen
for that long. Compiles clean (RAM 35.3%, Flash 70.7%). **Not yet
confirmed on hardware** whether keeping the connection warm actually
prevents the idle-association drop from happening in the first place.

**Follow-up, confirmed on hardware: the keep-alive disproved its own
premise, and pointed at the real bug.** Every 5-minute `/health` ping
succeeded for the full hour, including the one just 5 minutes before the
failure -- ruling out an idle-association drop entirely, since the
connection was never actually idle. At the exact same `t=3600070ms` mark
as every previous occurrence, everything broke again -- and this time
`/health` failed too, not just `/filters`/`/calendar`. The tell:
`wifi_keepalive_interval_ms` (5 min) divides evenly into
`poll_interval_ms` (1 hour), so the keep-alive task and the hourly
calendar-refresh task are *guaranteed* to fire in the same instant every
hour -- both are separate FreeRTOS tasks, each opening its own
`HTTPClient` connection concurrently. `HTTPClient`/`WiFiClient` were never
designed for concurrent use from multiple tasks; two simultaneous
connection attempts colliding at the WiFiClient/lwIP layer corrupting
shared, non-reentrant state explains everything observed across every
occurrence of this bug: works fine under normal (serialized) use, an
exact-interval collision breaks it, and the corruption persists across
many subsequent (individually serial, but now permanently broken) retries
until a reboot resets the whole stack.

Fixed at the actual chokepoint: added a mutex (`http_mutex()`,
`calendar_client.cpp`) around the entirety of `http_get()` and
`calendar_client_save_filters()`'s own `HTTPClient` POST -- every HTTP
call this project makes, from every caller (both background tasks, and
Settings screens calling `calendar_client_*` synchronously from the LVGL
task). Two calls that land at the same instant now serialize instead of
running concurrently, regardless of whether their trigger intervals
happen to line up -- fixes the actual bug (unsynchronized concurrent
access) rather than the specific symptom (this one exact collision).
Function-local static `SemaphoreHandle_t`, not a namespace-scope global,
so its one-time initialization is C++11-guaranteed thread-safe without
depending on static-initialization-order relative to FreeRTOS's own
scheduler startup. Compiles clean (RAM 35.3%, Flash 70.7%). **Not yet
confirmed on hardware** whether this actually prevents the corruption on
the next hourly collision.

**Follow-up, confirmed on hardware: the mutex wasn't the whole story.**
The very next boot failed the exact same way -- `/calendar` timed out
once (-11 at 65s), and every request after it (including plain `/filters`)
kept failing too, all before the WiFi keep-alive had even fired once (its
first tick isn't due until 5 minutes in) -- so this specific occurrence
had no concurrent second task involved at all. Tested the LXC directly
while the ESP32 was stuck: `/health`, `/filters`, and `/calendar` all
answered instantly and correctly. The server was never the problem, and
neither, this time, was concurrent access -- a *single* request that runs
all the way to its own read-timeout is apparently enough on its own to
break every request after it.

Read through the vendored `HTTPClient.cpp` for why: `disconnect()`
(called from `end()`) only forces the underlying socket closed
(`_client->stop()`) when `_canReuse` is false. `_canReuse` gets set `true`
as soon as a response's headers start arriving -- meaning a request that
times out *after* headers begin (exactly what a read-timeout is) leaves
`disconnect()` believing the connection should be kept open for reuse
instead of torn down. The destructor does still call `_client->stop()` as
a backstop when the `HTTPClient` object itself goes out of scope, so this
isn't a proven leak, but it's a real gap given the observed symptom.
Addressed two ways: `http.setReuse(false)` on both `http_get()` and
`calendar_client_save_filters()`'s POST, forcing a hard socket close every
time regardless of `_canReuse` -- costs nothing, since every call here
already constructs a brand-new `HTTPClient` object with no actual
keep-alive reuse happening across our own requests to begin with. And
`consecutive_failure_reconnect_threshold` (`calendar_view.cpp`) dropped
from 3 to 1 -- waiting for a clearer pattern before self-healing no longer
makes sense once a single failure has repeatedly been shown to already
carry the same signal three would; it was only ever costing several more
minutes of guaranteed-doomed retries first. Compiles clean (RAM 35.3%,
Flash 70.7%). **Not yet confirmed on hardware** whether `setReuse(false)`
actually addresses the underlying cause, or whether the more aggressive
self-heal threshold is just recovering faster from something still not
fully understood.

## 2026-08: the real fix was on the server side

Every entry above treated the symptom from the ESP32 side -- reasonably,
since that's where the failures showed up, but reported directly as
"this feels very fragile" once it kept recurring even after the mutex,
`setReuse(false)`, and the aggressive self-heal threshold. The actual
underlying problem was architectural: `calendar_api.py`'s `/calendar`
was answering every request by fetching from Forex Factory through
FlareSolverr *inline*, and the ESP32, as the primary (often only) client,
was the one consistently left holding that multi-second-to-65s cost, no
matter how much the firmware defended itself against the fallout.

Restructured `calendar_api.py` (see its own README/module docstring) so a
background thread on the LXC refreshes the calendar cache on its own
schedule, and `/calendar` always just reads from it -- a plain local dict
lookup, same cost profile as `/health`/`/filters` always had. Nothing
that answers a client request talks to FlareSolverr anymore.

With that in place, three ESP32-side changes followed directly:
- `calendar_client_get_calendar()`'s special 65s timeout reverted to the
  plain `default_timeout_ms` (8s, same as every other endpoint) --
  `/calendar` no longer has a reason to need longer than that.
- `poll_interval_ms` (`calendar_view.cpp`) reverted from an hour back
  to 10 minutes, the brief's own original suggested cadence. It was only
  ever lengthened to reduce how often a multi-second FlareSolverr fetch
  could give the frame-shift glitch a window to trigger in -- with
  `/calendar` fast again, that justification is gone.
- Removed `wifi_keepalive_poll()`/`wifi_keepalive_task()` (`main.cpp`)
  entirely -- it was built on the idle-association theory from earlier in
  this same section, which the keep-alive itself ended up disproving (the
  connection was never actually idle when the failure recurred). With
  `poll_interval_ms` back to 10 minutes, the real calendar poll exercises
  the network at least that often anyway, making a separate keep-alive
  redundant -- and removing it also removes one more source of the
  concurrent-HTTPClient-access pattern the mutex above now has to guard
  against, for no remaining benefit.

The mutex (`http_mutex()`), `setReuse(false)`, and the
`consecutive_failure_reconnect_threshold` of 1 are all left in place --
still reasonable defense-in-depth for whatever genuinely rare hiccup
might still happen (an LXC restart, a real WiFi drop), just no longer
compensating for a backend design that guaranteed the ESP32 would
regularly hit multi-second-to-65s waits. Compiles clean (RAM 35.3%,
Flash 70.7%). **Not yet confirmed on hardware** -- this is the fix
expected to actually resolve the whole saga above, rather than another
layer of defense against it.

## Roadmap (from the brief, plus Milestones 4 and 6 which weren't in it)

1. ✅ LVGL hello world on real hardware (display + touch bring-up)
2. ✅ WiFi connect + HTTP GET to the calendar API, raw response to serial
3. ✅ Boot splash screen (custom retro image)
4. ✅ On-device WiFi setup (not in the original brief, added for productization)
5. ✅ Calendar UI with static/mock data (dark/amber styling, layout)
6. ✅ Settings screen: calendar filters (impact/currency/columns, via Forex Factory) + WiFi reconfigure (not in the original brief)
7. ✅ Real data fetch, wired to `LXC/calendar_api.py`'s `/calendar` endpoint (day/week tabs, periodic poll, Settings-triggered refresh)
8. ✅ Countdown/alert logic (10-min countdown, +30s post-event refresh) + actual/previous value coloring. Sound alert attempted, reliably corrupted the display, dropped -- see write-up above.
9. Retro-style screensaver, to consider at the very end. The CrowPanel is
   an IPS LCD, not OLED, so it isn't at risk of the classic permanent
   pixel burn-in OLED/plasma panels get -- this is about backlight wear
   and (on a cheap panel) temporary image persistence from running static
   high-contrast content 24/7, not a defect that needs fixing. Lower
   priority than 7/8, but a fun one -- something like a CRT-style
   scanline/phosphor-fade effect, or a bouncing amber logo, after N minutes
   of no touch input.

## 2026-08: periodic polling replaced with long-polling

Reported directly as the actual goal, after the whole FlareSolverr/
WiFi-stability saga above: the ESP32 should be "just the display of the
webpage," not something that decides on its own schedule when to go
looking for updates. Considered WebSockets for genuine server push, but
decided against it -- it would mean adding a new client library (nothing
like it in this project currently) and managing a long-lived connection's
own lifecycle, exactly the kind of thing that's been fragile all session
(a persistent connection has more ways to end up in a broken state than a
short request/response). Long-polling gets the same practical outcome
with none of that: still a plain `HTTPClient` GET, just pointed at an
endpoint (`calendar_api.py`'s new `/calendar/wait`) that doesn't
necessarily answer instantly.

`calendar_client_wait_for_calendar()` (`calendar_client.cpp`, replacing
`calendar_client_get_calendar()`) calls `/calendar/wait?range=X&since=Y`.
The server holds the response open until `range`'s data actually changes
from version `Y`, or its own ~25s timeout elapses -- either way it always
eventually answers. `since=-1` is a sentinel meaning "don't wait, give me
whatever's current right now," used for a first load, a tab switch, a
Settings change, or `alert_manager_tick()`'s post-event refresh -- every
case that already called `refresh_events()` before. What's new:
`apply_ready_refresh_result()` now keeps the loop itself alive --
whenever a result lands that's a genuine success, not superseded by a
tab switch/Settings change, and not stale, it immediately calls
`start_refresh(current_calendar_version)` to wait for whatever comes
*next*, with no periodic timer driving any of it. `calendar_view_poll()`
(still called every `loop()` iteration) now only handles one thing: a
short delayed retry after a failed fetch, rather than the periodic
full-refresh check it used to be -- `poll_interval_ms`/
`poll_retry_interval_ms`/`last_poll_ms` are gone entirely.

One real tradeoff, accepted deliberately: `calendar_client.cpp`'s
`http_mutex()` still serializes every HTTP call this project makes (see
its own git history for why that's necessary), so a tab switch or
Settings change landing while a long-poll request is genuinely blocked
server-side has to wait for that request to resolve -- up to
`calendar_api.py`'s own ~25s timeout, not instant. Considered letting the
long-poll run on a separate, unsynchronized connection to avoid that, but
rejected it: that's exactly the kind of concurrent `HTTPClient` access
already confirmed (earlier in this same investigation) to corrupt
something at the WiFiClient/lwIP layer badly enough to break every
request afterward. A bounded worst-case delay on an uncommon interaction
is a much better trade than reopening that failure mode.

Compiles clean (RAM 35.3%, Flash 70.7% -- unchanged). **Not yet confirmed
on hardware** -- this replaces a lot of the plumbing built up over the
rest of this section's investigation, so it's worth specifically
re-watching for: the screen staying responsive, a real calendar change
(an actual/forecast value landing) showing up promptly without a manual
refresh, and whether the earlier idle-gap-shaped flakiness (a request
right after a long quiet stretch having trouble) still shows up now that
the connection is essentially never idle for more than a few seconds at a
time.

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
