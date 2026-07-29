# Herdsman Trading Terminal — Project Brief

A retro-themed economic calendar / trading event terminal running on the Elecrow CrowPanel 5" HMI (ESP32-S3), housed in a custom CRT-style 3D-printed enclosure.

This document is meant as a starting brief for Claude Code — it captures the requirements, hardware constraints, and a suggested architecture, but leaves implementation decisions to be worked out during actual development.

---

## Hardware

- **Board:** Elecrow CrowPanel 5" HMI
- **MCU:** ESP32-S3-WROOM-1-N4R8 (dual-core Xtensa LX7 @ 240MHz, 4MB Flash, 8MB PSRAM)
- **Display:** 800×480 IPS TFT-LCD, capacitive touch
- **Display driver:** ILI6122 / ILI5960 (RGB parallel interface)
- **Connectivity:** WiFi 2.4GHz (802.11 b/g/n), Bluetooth 5.0/BLE — only WiFi needed here
- **Audio:** onboard speaker interface (for alert sounds)
- **Recommended graphics library:** LVGL (v8.3.x is the version Elecrow's own examples target) — this is the standard, well-documented path for this board and supports custom themes, images, and animations
- **Recommended dev environment:** Arduino IDE or PlatformIO (both supported; PlatformIO is generally nicer for a project this size)

## Core Concept

On boot, the device shows a custom retro splash screen, then connects to WiFi and pulls economic calendar / trading event data from a web source. Events are displayed in the device's own UI (not a literal web iframe — see note below) with a dark background and amber text, color-coded by event importance. As a timed event approaches, the device plays an alert sound.

### Important technical note on "iframe" requirement

A literal HTML iframe (i.e., rendering an actual webpage with its own CSS/JS) is **not feasible** on this hardware — ESP32 has no browser engine and nowhere near enough RAM to parse/render arbitrary HTML. The practical approach, and the one this brief assumes:

- Fetch the **underlying calendar data** (ideally JSON) via HTTP, not the rendered webpage
- Render that data in the device's **own native LVGL UI**, styled however we like (dark background, amber text — fully achievable this way)

This gives you full control over styling and is dramatically more reliable than trying to scrape/render live HTML on a microcontroller. If genuine "see the actual webpage" functionality turns out to be a hard requirement, that would need a different hardware approach entirely (e.g., a Raspberry Pi doing real browsing, streaming a video feed to the ESP32) — flag this if so.

---

## Feature Breakdown

### 1. Boot Splash Screen
- Custom retro 90s-themed image, "Herdsman Trading Terminal"
- Displayed immediately on boot, before WiFi connects / data loads
- Should be sized to the full 800×480 display
- Image needs to be converted to a format LVGL can use (typically a C byte array via LVGL's image converter, or loaded from SPIFFS/LittleFS/SD as a raw/binary image)
- *A starting image concept can be generated separately — see note at the end of this brief.*

### 2. WiFi + Data Fetching
- Connect to WiFi on boot (credentials via a config file, not hardcoded — consider a simple captive-portal setup flow for changing networks without reflashing)
- Poll a calendar data source on an interval (e.g. every 5–15 minutes is typical for economic calendars; don't hammer the source)
- **Candidate data source found:** Investing.com's economic calendar widget, embedded via:
  ```
  https://sslecal2.investing.com?columns=exc_actual,exc_forecast,exc_previous&category=_employment,_economicActivity,_inflation,_credit,_centralBanks,_confidenceIndex,_balance,_Bonds&importance=3&features=datepicker,timezone&countries=5&calType=week&timeZone=8&lang=1
  ```
  Useful parameter reference (from a community-documented mapping of this widget's params):
  - `columns` — which data columns to include (flags, currency, importance, actual/forecast/previous)
  - `importance=3` — filters to high-impact events only (the "red folder" equivalent; 1=low, 2=medium, 3=high)
  - `countries` — numeric country codes (a documented list exists; e.g. `5` = United States in at least one reference mapping — worth re-verifying codes at implementation time since these aren't officially published)
  - `calType` — `week`/`day` range
  - `timeZone`, `lang` — display prefs, less relevant once we're parsing data ourselves rather than rendering their page

  **Investigated: no public JSON/XHR endpoint exists.** This widget is server-rendered HTML, not backed by a JSON API — confirmed via an existing open-source project ([`andrevlima/economic-calendar-api`](https://github.com/andrevlima/economic-calendar-api)) that does exactly what we'd need: a small PHP script that fetches the widget HTML and scrapes it (via `HtmlDomParser`, targeting `tr[id*='eventRowId']` table rows) into clean JSON. This is the established community approach for this specific source — there isn't a cleaner path hiding underneath that a browser network inspection would reveal instead.

  **Recommended architecture:** don't scrape HTML on the ESP32 itself (too RAM-intensive, too fragile against markup changes for a microcontroller to handle gracefully). Instead:
  1. **Self-hosted middleware** — run a small scraper (adapt the PHP project above, or a Python equivalent) on a Raspberry Pi / home server / small cloud function you control. It re-serves clean JSON. The ESP32 then only ever does a simple JSON GET to *your* server — never touches Investing.com's HTML directly.
  2. **Paid third-party API** — services like Apify or Parse.bot already wrap Investing.com's calendar as clean JSON for a small per-call fee, if you'd rather not host/maintain a scraper yourself.

  Either path keeps the ESP32 firmware simple: one JSON GET, one JSON parse, done. All the fragility of dealing with Investing.com's actual markup lives in the middleware layer, which is much easier to fix/update than reflashing firmware whenever their HTML changes.

  Alternative: ForexFactory-mirroring JSON feeds remain a fallback option if this source doesn't pan out cleanly — worth keeping in mind as a plan B.
- Parse into a small in-memory structure: event name, time, currency/market, impact level, actual/forecast/previous values if available

### 3. Calendar UI
- Dark background, amber (`#FFB000`-ish, adjust to taste) text — classic amber-CRT terminal look, fits the enclosure's retro theme
- List/table view of upcoming events, sorted by time
- Color-code by impact level (e.g. red-folder = bright red accent, orange = orange accent, yellow/low = dim/gray) — using accent colors *within* the amber/dark theme rather than breaking it
- Highlight or visually distinguish the *next* upcoming event
- Touch support: CrowPanel has capacitive touch, so scrolling/tapping an event for detail view is realistic

### 4. Countdown / Alert Logic
- Track time-to-next-event continuously
- Trigger sound alert at a configurable threshold (e.g. 5 minutes before, or multiple staged alerts like 15/5/1 min)
- Consider a "snooze" or per-event mute, since not every event will matter to every user

### 5. Sound Alerts
- Use the board's onboard speaker interface
- Simple tone/beep is easiest to implement reliably; a short WAV clip is more "retro terminal" in character if you want to lean into the theme (e.g. a modem-era beep or teletype sound)
- Different sounds for different impact levels is a nice touch, worth considering once the base alert works

---

## Suggested Architecture

```
/src
  main.cpp              - setup/loop, boot sequence
  wifi_manager.*         - WiFi connect + reconnect handling
  calendar_client.*      - HTTP fetch + JSON parse of calendar data
  calendar_model.*        - in-memory event data structures
  ui/
    splash_screen.*       - boot splash
    calendar_view.*        - main LVGL calendar UI
    theme.*                 - shared dark/amber color palette, fonts
  alert_manager.*          - countdown tracking + sound trigger
  audio_player.*            - speaker output handling
/data
  boot_splash.bin (or .c)    - converted boot image
  alert_tones/                - sound assets
```

This is a starting suggestion, not a mandate — Claude Code should feel free to restructure once real implementation constraints show up.

---

## Open Questions to Resolve During Implementation

1. **Middleware hosting decision** — self-host a small scraper (Pi/home server/cloud function) vs. use a paid API (Apify/Parse.bot)? This affects ongoing cost and maintenance burden, not just initial setup.
2. **WiFi credential management** — hardcoded for now vs. a proper config flow (captive portal, SD card config file, etc.)?
3. **Alert timing** — single threshold, or staged alerts (15/5/1 min)?
4. **Touch interactions** — how much interactivity is wanted beyond passive display (tap for detail, manual refresh, settings screen)?
5. **Persistence** — should selected settings (WiFi, alert preferences) survive reboot? (Likely yes, via NVS/Preferences on ESP32.)

---

## Suggested First Steps

1. Get a basic LVGL "hello world" running on the actual hardware — confirms toolchain, display driver, and touch are all working before adding complexity
2. Get WiFi connecting and a simple HTTP GET working, printing raw response to serial — confirms networking before UI is layered on
3. Build the boot splash screen (visually simple win, good early milestone)
4. Build the calendar UI with static/mock data first — get the dark/amber styling and layout right before wiring up live data
5. Wire in the real data fetch once the UI shell is solid
6. Add countdown/alert logic last, once event data is flowing reliably

---

*Note: I can generate a starting concept image for the retro boot splash (800×480, 90s terminal aesthetic, "Herdsman Trading Terminal" text) separately if useful — just let me know and I'll put one together as a visual starting point for whoever converts it to the LVGL image format.*
