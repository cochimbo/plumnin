# Windows installer

`plumnin.iss` is an [Inno Setup 6](https://jrsoftware.org/isinfo.php) script that builds
`plumnin-<version>-setup.exe`. The installer:

- Copies the emulator (`plumnin.exe`), `SDL2.dll` and any other file next to the executable.
- Creates the Start menu shortcut and, optionally, a desktop one.
- Can add plumnin to the "Open with" menu of Game Boy, NES and Super Nintendo ROMs
  (`.gb`, `.gbc`, `.nes`, `.sfc`, `.smc`, `.zip`) without changing the program that opens
  them by default.
- Installs without administrator rights (or for all users, if chosen).
- Is in Spanish and English, with the plumnin plum and marquee (`wizard.bmp`,
  `wizard-2x.bmp`) on the welcome and finish pages, the plum on the others
  (`wizard-small.bmp`, `wizard-small-2x.bmp`) and the plumnin icon
  (`src/frontend/assets/plumnin.ico`) on the installer itself.

## Automatic build

The `.github/workflows/windows-installer.yml` workflow builds the emulator with CMake +
MSVC, downloads SDL2, builds the installer and uploads it as an artifact of the run
(`plumnin-windows-installer`). When a `vX.Y.Z` tag is pushed, the installer is attached to
the release.

## Local build

1. Build the emulator in Release and copy `plumnin.exe` and `SDL2.dll` to a `dist\`
   folder at the root of the repository.
2. Run:

   ```bat
   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.0 installer\plumnin.iss
   ```

The installer ends up in `dist-installer\`. If the executable has another name, add
`/DAppExeName=other.exe`.
