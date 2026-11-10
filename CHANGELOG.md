# Changelog

What's new in each version of Candela. Downloads are on the [releases page](https://github.com/Haise-d/candela/releases).

## 1.1.1 — 2026-11-10

### Fixed

- **Games → No popups over → + Add** could stay on "Open the game, then come back here" with a game running. Candela now remembers the app's name instead of its window, skips overlays (mouse, GPU and launcher pop-ups) and Windows' own windows so they can't replace the game, and checks again every 2 seconds in case a switch was missed.
- The Games page now follows the app in front while the settings are open: switch to a game and back, and **+ Add** offers it.
- `candela.log` now says which app Candela took as the one you're using, and why it passed over a window.
- Adding a running game to **Games → Anti-cheat games**, or taking it off the list, could crash Candela: the settings were rebuilt in the middle of the click.
- A quick **Alt+Tab** did nothing while an always-on-top window was open (a pinned video, Task Manager kept on top): the switcher picked the window you were already in. It now steps from the window in front, and from the desktop it goes to the window you last used.
- `candela.exe --dump` (developer screenshots) no longer replaces your settings file with its sample settings.
- Candela no longer starts while Candela mini is running, as mini already did the other way round: the two would fight over the colors and the hotkeys. It says so and asks you to quit mini first.
- With an admin window in front (Task Manager) and Candela not running as admin, Windows' own Alt+Tab opens, and a second Tab then opened Candela's switcher behind it, so neither worked. An Alt+Tab that Windows started now stays with Windows until Alt is let go. The same goes for one started over a game in exclusive fullscreen.
- `candela.log` now says why a popup wasn't shown over a game (it is in the list, or Windows reports exclusive fullscreen), once per game.
- **Games → Popups over games anyway** no longer promises popups over exclusive fullscreen: Candela tries, but Windows draws nothing on top of a game that really is in it.

### Known issues

- Far north (Tromsø, Reykjavik), a night start after midnight can land after the next sunrise, and night colors then stay on through the day. On days with no sunrise or sunset, Candela uses 07:00 and 19:00.
- Balatro gets no popups in its own fullscreen and borderless modes: Windows reports both as exclusive fullscreen. Windowed, made borderless by Candela (Ctrl+Alt+B), works.
- A game that runs as admin (Black Desert) hides the keyboard from Candela unless Candela runs as admin too. **Start with Windows** starts it that way when candela.exe is in a folder only admins can change, such as Program Files.

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
