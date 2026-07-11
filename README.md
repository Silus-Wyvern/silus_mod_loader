# Mod / Skin Loader

A lightweight ASI plugin for PSOBB (annz1 / mteth client) that lets players load
replacement asset files (skins, textures, models, sounds, etc.) from a
`mods` folder **without modifying the game's real data directory**.

Install a mod = drop a folder in. Remove a mod = delete the folder. The original
game files are never touched.

---

## How it works (short version)

The game opens every asset through the Windows `CreateFileA` function. This DLL
hooks that call in the game's import table. When the game reads an existing file,
the loader checks whether a replacement with the same relative path exists inside
any installed mod folder — and if so, quietly opens that instead.

If no matching override exists, the call passes straight through. **With no mods
installed, the loader does nothing at all.** It never redirects file *writes*
(saves, configs) — only reads of existing files.

Because it hooks by function name rather than a hardcoded game address, it keeps
working across client updates.

---

## How the game loads it

The annz1 client loads `patch.dll` at startup and calls its `patch` export.
`patch.dll` is the ASI chainloader: it scans the `patches/` folder (and the game
root) for `.asi` files and calls each one's exported `void __stdcall load(void)`.
This plugin does its setup in that `load()` export.

Because it installs its hook by function name (not a hardcoded game address), it
does **not** care what base the client loads at, and needs no `/DYNAMICBASE:NO`.

---

## Folder layout

Put mod folders inside a `mods` directory next to `psobb.exe`. Inside each mod,
mirror the file's path **relative to the game folder**.

```
<game folder>\
├─ psobb.exe
├─ data\                      <- the REAL data dir (never modified)
├─ silus_mod_loader.asi
├─ silus_mod_loader.ini       (optional config)
└─ mods\
   ├─ load_order.txt          (optional priority list)
   ├─ blue_ranger_skin\
   │  └─ data\
   │     └─ some_texture.xvm  <- overrides <game>\data\some_texture.xvm
   └─ hd_ui\
      └─ data\
         └─ menu_font.prs
```

The rule: if the game would load `<game folder>\data\X`, an override goes at
`<game folder>\mods\<yourmod>\data\X`. Loose files that live in the game root
(not under `data\`) are mirrored the same way, straight under the mod folder.

---

## Load order (which mod wins)

When two mods override the same file, the **higher-priority** mod wins.

- **Default:** all folders in `mods\` are used, sorted alphabetically, and the
  one that comes **first** alphabetically has priority. Prefix names with numbers
  to be explicit — e.g. `10_base`, `20_override`, where `10_base` wins ties.
- **Explicit:** create `mods\load_order.txt`, one folder name per line, **top =
  highest priority**. Lines starting with `;` or `#` are comments. Any folders
  not listed are used afterward (lower priority), in alphabetical order.

```
; mods\load_order.txt
hd_ui
blue_ranger_skin
```

---

## Config (optional)

Create `silus_mod_loader.ini` next to the `.asi`:

```ini
[settings]
; folder (relative to the game dir) to scan for mods
mods_dir=mods
; 1 = write silus_mod_loader.log, 0 = off
log=1
; 1 = log every single redirected file (verbose/debug)
log_redirects=0
```

All keys are optional; the defaults above are used if the file is absent.

Comments go on their own line, starting with `;`. Inline comments after a value
(e.g. `mods_dir=mods   ; note`) are also tolerated, but keeping comments on
their own line is the safest habit.

---

## Building

Visual Studio → **empty C++ project** containing `silus_mod_loader.cpp`.

| Setting | Location | Value |
|---|---|---|
| Platform | top of properties | **Win32 / x86** (client is 32-bit; x64 won't load) |
| Configuration | | Release |
| Configuration Type | General | Dynamic Library (.dll) |
| Output File | Linker → General | `$(OutDir)$(TargetName).asi` |
| Runtime Library | C/C++ → Code Generation | Multi-threaded (`/MT`) |

The `load` export is produced by a linker pragma already in the source:

```cpp
#pragma comment(linker, "/EXPORT:load=_load@0")
```

`void __stdcall load(void)` decorates to `_load@0` under MSVC; the pragma
re-exports it under the plain name `load` so the loader finds it via
`GetProcAddress`. (A `silus_mod_loader.def` with `EXPORTS load` is included as an
alternative if you prefer — add it under Linker → Input → Module Definition
File and delete the pragma.)

Verify after building:

```
dumpbin /exports Release\silus_mod_loader.asi
```

You should see `load = _load@0`. If you only see `_load@0`, the loader won't
find it.

Then place `silus_mod_loader.asi` in the game's **`patches/`** folder, alongside
your other ASIs (Omnispawn, widescreen, etc.).

No external dependencies — pure Win32 / kernel32. No `.def` required, no
precompiled headers (make sure the VS template's generated `pch.h`/`dllmain.cpp`
aren't in the project, or you'll get duplicate-`DllMain` / missing-`pch.h`
errors — this file has its own `DllMain`).

---

## What actually gets loaded (which files, and "extra" files)

The loader is a **per-file, name-matched** override. It only ever swaps a file
when the game itself asks to open that exact relative path. That has three
practical consequences:

- **Loose replacement files work** — the common case. Most PSOBB skins replace
  loose files under `data\` (textures `.xvm`, `.prs`, models, etc.). The game
  opens those by name, so a matching file in your mod folder is loaded in their
  place.
- **"Extra" files the game never requests are ignored** — harmlessly. If a skin
  ships files the game doesn't open by that name (readmes, previews, spare
  variants), they just sit in the mod folder unused. Nothing breaks; they simply
  never load. So a skin with extra files works fine — you get the parts the game
  asks for, and the rest is inert.
- **Archive-packed assets need the whole archive** — the one caveat. Some PSOBB
  assets live *inside* container files (`.afs`, `.gsl`, `.nte`, etc.). The game
  opens the **container** by name and reads entries out of it by offset — it does
  not open the inner files by name. So loose, extracted inner files dropped in a
  mod folder will **not** be picked up. To override those, the skin must provide
  the whole modified container file (which the game opens by name → works). If a
  skin ships loose inner files meant to be injected into an archive, they need to
  be repacked into that archive first.

Rule of thumb: if the skin is drop-in loose files that mirror `data\`, it works
as-is. If it's a set of loose files that a tool is supposed to inject into an
archive, replace the archive instead.

---

## Testing checklist

1. Build as `.asi`, drop it in the game's `patches/` folder next to your other
   ASIs.
2. Launch the game once. Confirm `silus_mod_loader.log` appears and shows
   `status : ACTIVE`, your game dir, and the mods it found (in priority order).
3. Make a test mod: copy one real asset out of `data\`, edit it visibly, and
   place it at `mods\test\data\<same name>`.
4. Set `log_redirects=1` in the ini, relaunch, and load the part of the game that
   uses that asset. The log should show a `REDIRECT` line, and you should see
   your edited asset in-game.
5. Delete the `mods\test` folder — the game returns to stock, confirming nothing
   permanent changed.

---

## Troubleshooting

- **`status : FAILED` in the log** — the loader couldn't find `CreateFileA`. Make
  sure it's a 32-bit build and is actually being injected by the ASI loader.
- **No log file at all** — the `.asi` isn't being loaded. Check it's in
  `patches/`, that the export is named `load` (`dumpbin /exports`), and that your
  other ASIs in the same folder are working.
- **A mod doesn't apply** — turn on `log_redirects=1` and watch the log. If you
  see no `REDIRECT` for that file, the relative path inside your mod folder
  doesn't match the path the game actually requests. Compare against a
  `REDIRECT` line for a file that *does* work.
- **Some assets never redirect** — a small number of load paths may use
  `CreateFileW` or memory-mapping; the loader already handles `CreateFileW`. If
  something still slips through, note the exact filename from a stock run and it
  can be added.

---

## Safety notes

- Read-only: never redirects file creation or writes, so saves/configs are safe.
- Non-destructive: your real `data\` directory is never modified; mods are purely
  additive and fully reversible by deleting folders.
- No-op by default: with an empty or missing `mods` folder, behavior is identical
  to the unmodded client.

---

## Credits

- **Pixelated** — for the reverse-engineering of Ephinea's `ephinea.dll` and its
  asset-loading hooks, which identified the client's asset-open path and confirmed
  that skin/mod loading works by intercepting file opens rather than scanning a
  folder. That analysis informed this loader's approach.
  <https://github.com/therealpixelated/psobb-ephinea-re>
- **Ephinea / Sodaboy** — original authors of the Tethealla-derived client and the
  `ephinea.dll` patch module that the RE above studies.
- **Silus Wyvern** — this standalone mod loader: an independent implementation
  that hooks `CreateFileA` via the import table (address-free), a different and
  simpler approach than Ephinea's inline detours. No code was taken from the work
  above; it informed the design only.

---

## License

This loader's source is released under the MIT License — see [LICENSE](LICENSE).

**Not affiliated with SEGA or Ephinea.** *Phantasy Star Online* and its assets are
© SEGA; the Ephinea client and `ephinea.dll` are the property of the Ephinea
project. This project ships none of those files — it is an independent tool that
loads user-supplied replacement assets at runtime. Mods you use with it are the
responsibility of their respective authors, and you are responsible for having the
rights to any assets you distribute or install.
