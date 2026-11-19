# AutoKeyDown

A small Windows tray app that keeps keys held down for you.

1. Hold one or more keys (for example `W`, or `Shift`+`W`).
2. Press **Scroll Lock** while you're still holding them.
3. Let go. Windows and every app still see those keys as held down.

The held state ends when **either** of these happens:

- you press any one of the held keys again, or
- you turn Scroll Lock off.

When it ends, every held key is released.

Only keys that are physically down at the moment Scroll Lock turns on are held. Keys you press after that behave normally.

## Download

Get `autokeydown.exe` from the [latest release](https://github.com/jmasseo/autokeydown/releases/latest) and run it. Nothing needs installing. Windows SmartScreen may warn you because the file isn't signed; choose **More info → Run anyway**.

## Tray icon

- Gray: idle. Green: keys are being held. Hover over it to see which keys.
- Left- or right-click the icon for the menu:
  - **Turn Scroll Lock off when a held key is pressed** (on by default). This keeps the Scroll Lock light in step with the held state, so the next Scroll Lock press starts holding keys again. Turn it off if you want Scroll Lock left alone.
  - **Exit** releases any held keys and quits.

## Build

You need Visual Studio (with the C++ workload) and CMake.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The output is `build\Release\autokeydown.exe`. It's a single file that needs no runtime installed. To start it at login, put a shortcut to it in `shell:startup`.

## Test

```powershell
powershell -ExecutionPolicy Bypass -File tests\smoke-test.ps1
```

The test starts the app with `--accept-injected` and drives it with the unused `F24`/`F23` keys. It briefly toggles Scroll Lock and then puts it back the way it was.

Command-line flags:

- `--accept-injected`: for testing. Treats synthetic input as physical key presses.
- `--no-auto-scroll-off`: starts with the auto-off menu option unchecked.

## Notes and limitations

- The app uses a low-level keyboard hook (`WH_KEYBOARD_LL`). When you release a held key, the app swallows that key-up. The key just stays "down"; the app does not repeat it. In a text box you'll see one character, not a stream of them. Games and other apps that check whether a key is down will see it held.
- By default, key presses faked by other software (macro tools, remote desktop clients) are passed through and are never held.
- While an app running as administrator has focus, Windows hides its keystrokes from AutoKeyDown and blocks AutoKeyDown's key releases. Run AutoKeyDown as administrator too if you need it there. It never works on the secure desktop (Ctrl+Alt+Del, UAC prompts).
- Some anti-cheat systems block or flag injected or hooked input.
