# Changelog

What's new in each version of Candela. Downloads are on the [releases page](https://github.com/Haise-d/candela/releases).

## 1.1.0 — 2026-10-10

### New

- **Fades.** Turn on **Warmth → Fade color changes** and new colors come on gradually instead of all at once. Sunset, night and morning take the **Scheduled fade** time (20 seconds unless you change it, up to an hour), and the changes you make (hotkeys, the tray menu, the settings) take the **Hotkey fade** time (0.4 seconds; **Instant** turns it off for them). Off by default.
- Pressing a hotkey again mid-fade carries on from wherever the screen is, with no jump back.
- Starting Candela, waking the PC, unlocking it and plugging in a screen still put the colors on straight away.
- While warm colors are still locked, a fade stops where Windows does, at about 3400K.
- Inverted Afterglow never fades, because half way there the screen would be a flat red. It switches at once.

### Changed

- The **Afterglow** page in the settings is now **Warmth**. It has the fade settings at the top, then everything Afterglow had, under an Afterglow heading.

### Settings file

- New in `config.json`: `fade` (true / false), `fade_seconds` (1–7200) and `fade_hotkey_ms` (0–10000). An older settings file reads as before, with fades off.

## 1.0.0 — 2026-10-10

- First release.
