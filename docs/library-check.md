# Library check

Settings, Library, **Check library** looks at the games on the stick and lists what would
keep one from showing up or from starting. It only reports: no file is changed. Cross
checks again, Circle goes back. Source: `src/audit.c`; design: lumen-design
`mockups/library-audit.html`. The result also goes to `logs/lumen.log` (`audit:` lines).

It reads every `<home>/systems/<id>.ini` itself (a system whose games are all broken is
not loaded by the frontend, and that is the one to report) and walks its `paths` with the
scanner's own rules (`docs/systems.md`). The built-in games (`layout = stock`) are the
console's and are not checked.

| Problem | Layout | Meaning |
|---|---|---|
| The folder has no `Game.ini` | psc | `<path>/<n>/` holds files but no `Game.ini`: the game is not listed |
| `Game.ini` does not name the disc | psc | no `Discs=` line (or it is empty) |
| `Game.ini` names a disc that is not in the folder | psc | no `<disc>.<ext>` for an entry of `Discs=` (every disc, not only the first), with the system's `extensions` |
| The `.cue` points to a file that is not there | psc, files | a `FILE "..."` line names a file missing from the folder (also a path into another folder) |
| The `.cue` writes the name with different capitals | psc, files | the file is there under another case: works on FAT, fails on ext4. Reported only where it fails |
| The `.cue` names no file | psc, files | no `FILE` line (an empty or damaged `.cue`) |
| The file is empty | all | 0 bytes: a copy that never finished |
| Disc image without its `.cue` | files | a `.bin` / `.img` no `.cue` or `.m3u` of the folder points to (systems whose `extensions` include `cue`) |
| The `.m3u` list points to a file that is not there | psc, files | a line of the playlist |
| Protected game without its `.sbi` | psc | the disc is in `<home>/systems/<id>.protected` and `<disc>.sbi` is missing (below) |
| The `.sbi` is not named like the disc | psc | the core looks for `<disc>.sbi`: any other name is ignored |
| Empty game folder | psc, apps | |
| The system's core is missing | psc, files | `<home>/cores/<core>` of a system that has games |
| The folder has no `app.ini` / `app.ini` points to a program that is not there | apps | |
| Unfinished copy | psc, files | a `*.lumen-part` left by an interrupted USB transfer |

Each row is one problem (a game with two problems has two rows), sorted by system and
folder; the pane on the right gives the file and the folder.

## Copy-protected discs: `<id>.protected`

Some PAL PlayStation discs (LibCrypt) only work with their subchannel file, `<disc>.sbi`,
next to the image. The frontend cannot tell from an image whether a disc is protected, so
the check takes it from an optional list, `<home>/systems/<id>.protected`: one disc name
per line as written in `Discs=` (e.g. `SLES-01234`), `#` for comments. No list ships yet;
without it only a misnamed `.sbi` is reported.
