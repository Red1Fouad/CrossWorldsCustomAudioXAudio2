# CrossWorlds Custom Audio (XAudio2)

A BGM replacement mod for **Sonic Racing CrossWorlds** that lets you play your own music files in-game. The mod hooks into the game's CRI Middleware audio system via DLL injection, intercepts background music cues, and plays user-supplied audio instead.

## Requirements

- [UE4SS (Unreal Engine 4/5 Scripting System)](https://gamebanana.com/tools/20876) — required for loading the mod DLL into the game.

## Installation

1. Install UE4SS into the game directory following its own instructions.
2. Place the mod files so the structure looks like this:

```
SonicRacingCrossWorlds/
  UNION/
    Binaries/
      Win64/
        ue4ss/
          Mods/
            AudioDLL/
              main.dll
              settings.txt        (auto-generated on first run)
              music/              (race BGM)
              music_lobby/        (lobby BGM)
              music_title/        (title screen BGM)
```

3. Drop your audio files into the appropriate folder(s).
4. Launch the game. The mod loads automatically via UE4SS.

## Supported Audio Formats

| Format | Extension |
|--------|-----------|
| WAV    | `.wav`    |
| MP3    | `.mp3`    |
| Ogg Vorbis | `.ogg` |
| FLAC   | `.flac`   |
| AAC    | `.aac`    |
| M4A    | `.m4a`    |
| AAX    | `.aax`    |
| ADX    | `.adx`    |
| BRSTM  | `.brstm`  |

## Music Folders

The mod has three independent music pools that replace different parts of the game's soundtrack:

| Folder | Replaces | Triggered by |
|--------|----------|--------------|
| `music/` | In-race BGM | `BGM_LAP1`, `BGM_LAP2_FORCE`, `BGM_GP_*_FINAL_1_2` cues |
| `music_lobby/` | Lobby music | `BGM_LOBBY_*` cues |
| `music_title/` | Title screen music | `BGM_TITLE_*` cues |

Place your audio files directly into the corresponding folder. Files from all three pools are **preloaded into memory** at startup.

## Shuffle & Playback

- Each pool uses a **weighted shuffle**: every track is played once before any repeats.
- You can control per-track shuffle probability and volume via an optional `tracks.txt` file inside each music folder.

### tracks.txt format

Place a `tracks.txt` file inside `music/`, `music_lobby/`, or `music_title/`:

```
mysong.mp3 : 1.0, 1.0
anothersong.flac : 2.0, 0.8
chill.wav : 0.5, 1.2
```

Each line: `filename : weight, volumeMul`
- **weight** — higher values make the track more likely to be picked (default: `1.0`).
- **volumeMul** — multiplies the volume for this specific track (default: `1.0`).

## Settings

A `settings.txt` file is auto-generated next to `main.dll` after you first adjust volume. You can also create it manually:

```
//Play another music file after one is finished instead of looping? true or false
PlayNewMusic: false
//Mute custom BGM when game window loses focus
MuteOnUnfocus: false
//Custom BGM volume (0.0 - 5.0)
Volume: 1.0
```

| Setting | Description |
|---------|-------------|
| `PlayNewMusic` | `false` = loop current track. `true` = play next shuffled track when the current one finishes. |
| `MuteOnUnfocus` | Mutes custom BGM when the game window loses focus. |
| `Volume` | Global volume multiplier for custom BGM (`0.0` to `5.0`). |

## Hotkeys

| Shortcut | Action |
|----------|--------|
| `Ctrl + Up` | Volume up (+10%) |
| `Ctrl + Down` | Volume down (-10%) |
| `Ctrl + Right` | Skip to next track |
| `Ctrl + Left` | Restart current track |

## How It Works

The mod is a DLL that hooks into the game at runtime:

1. **Hooks `XAudio2Create`** to capture the game's own XAudio2 instance, so custom audio plays through the same audio device.
2. **Pattern-scans** the game executable for CRI Atom Ex functions (`criAtomExPlayer_SetCueName`, `criAtomExPlayer_Start`, etc.) using byte signatures.
3. **Intercepts `criAtomExPlayer_Start`** — when the game tries to play a BGM cue, the mod checks the cue name and replaces the audio with a track from the matching pool.
4. **Mutes the game's original BGM** category while custom audio is playing.
5. **Restores original audio** when a race finishes (`SE_FINISH` cue).

## Credits

### Mod Author

- **Red1Fouad** — [GitHub](https://github.com/Red1Fouad)

### Libraries

| Library | Author(s) | License |
|---------|-----------|---------|
| [dr_flac](https://github.com/mackron/dr_libs) v0.13.4 | David Reid | Public Domain / MIT No Attribution |
| [dr_mp3](https://github.com/mackron/dr_libs) v0.7.3 | David Reid (based on [minimp3](https://github.com/lieff/minimp3) by lieff) | Public Domain / MIT No Attribution |
| [stb_vorbis](https://github.com/nothings/stb) v1.22 | Sean Barrett | Public Domain / MIT |
| [libhelix-aac](http://www.helixcommunity.org) | Jon Recker, Ken Cooke (RealNetworks) | RPSL / RCSL |
| [MinHook](https://github.com/TsudaKageyu/minhook) | Tsuda Kageyu | BSD 2-Clause |

### Dependencies

- [UE4SS](https://gamebanana.com/tools/20876) — required to load the mod.
- Microsoft XAudio2 — audio output (included with Windows).
