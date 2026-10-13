# spotc

Spotify client for the terminal, written in C. Browse your library, control any
Spotify Connect device, and play locally through a built-in speed, reverb and
bass-boost effect chain.

Requires Spotify Premium.

```
┌ spotc ──────────────────────────────────────── ◈ spotc ┐
│ ♥ Liked Songs   │  Liked Songs · 212 tracks            │
│ ⊚ an album      │    1  Track one        Artist   3:41 │
│ ≡ a playlist    │    2  Track two        Artist   4:05 │
│ ...             │  ...                                 │
├─────────────────────────────────────────────────────────┤
│ ▶ Track one — Artist                       1:23 / 3:41  │
│ ━━━━━━━●─────────────────────────────────────────────── │
│ vol 80% · 1.35x fast · rev 18% · bass boost     ? keys  │
└─────────────────────────────────────────────────────────┘
```

## Features

- Liked songs, saved albums and playlists in a scrollable sidebar (paginated,
  not capped at the API's 50-per-page)
- Search for tracks and albums
- Full playback control over Spotify Connect: play, seek, volume, shuffle,
  repeat, queue, device switching
- Local playback device via librespot, so sound comes out of the machine
  spotc runs on
- Variable playback speed from 0.50x to 2.00x through a cubic-Hermite
  resampler (pitch follows speed), with 12 gradual levels and smooth live
  transitions
- Toggleable bass boost and a Freeverb implementation, both applied live to
  normal and reverse local audio; exact values can also be set in the config
- Vinyl record sound (`v` cycles off / mint / played / dusty / lo-fi): crackle,
  pops, scratches that tick once per revolution, wow & flutter tied to the
  platter speed, warm rolled-off top end with mono bass, hiss and rumble
- Rain sounds (`w` cycles off / drizzle / rain / storm): procedural noise bed,
  droplets, splats and distant thunder in a storm, mixed after the effects so
  they never slow down
- Local files: a "Local files" entry lists `music_dir` (default `~/Music`,
  scanned recursively; tags via ffprobe, cached); playback goes
  ffmpeg → spotc-fx → pacat, so every effect, seeking, next/prev and reverse
  work on your own files too
- Continuous exact-reverse mode for the local device: each track is decoded
  once, cached and played from its last sample to its first; the next track is
  then started automatically in reverse too
- Every key rebindable in a plain-text config, 256-color accent theming

## How it works

```
spotc (ncurses UI, Web API client)
└─ librespot --backend pipe  →  spotc-fx (DSP)  →  pacat (PulseAudio/PipeWire)
```

`spotc-fx` is a separate ~16KB binary that reads raw s16le audio on stdin and
writes it processed to stdout; spotc controls it at runtime through a FIFO.
The effects only apply to the local `spotc` device — Spotify's API does not
expose audio from other Connect devices.

Exact reverse needs the future audio before it can begin. Pressing `R` enables
reverse mode: `spotc-reverse-cache` decodes each complete track as fast as the
network allows, then `spotc-fx` reads its PCM file strictly from end to
beginning. At the beginning of one track, the next track is prepared and starts
in reverse automatically. Decoded tracks are reused from
`~/.cache/spotc/reverse/`; press `R` again to return to normal playback.

`spotc-ipv4.so` is preloaded into librespot to force IPv4 name resolution:
routers that advertise IPv6 without working upstream otherwise break
librespot's websocket connection (`Network is unreachable`) and the local
device never appears. Harmless when IPv6 works.

## Dependencies

- ncursesw, json-c, libcurl, openssl, Rust/Cargo (build)
- [librespot](https://github.com/librespot-org/librespot) ≥ 0.8 —
  `cargo install librespot --locked` (without `--locked` the build fails)
- `pacat` (pulseaudio-utils; PipeWire's pulse compatibility works fine)
- `ffmpeg` / `ffprobe` for local files (optional)

## Build

```sh
make
make install     # installs to ~/.local/bin
```

## Setup

Spotify removed shared-app access to the Web API, so you need your own (free)
API key — one-time, about two minutes:

1. Open <https://developer.spotify.com/dashboard> → Create app.
   Any name; Redirect URI: `http://127.0.0.1:5588/login`; check "Web API".
2. Run `spotc`, press `i`, paste the Client ID.
3. Press `l` and sign in in the browser.

For local audio (and the local effects), pair the device once: open
Spotify on your phone → Devices → pick `spotc`.

## Keys

| Key | Action | Key | Action |
|-----|--------|-----|--------|
| `j` `k` | navigate | `Enter` | play / open |
| `Tab` | switch pane | `Space` | play/pause |
| `n` `b` | next / previous | `,` `.` | seek ±10s |
| `-` `+` | volume | `a` | add to queue |
| `s` | shuffle | `r` | repeat |
| `R` | reverse mode on/off (all tracks) | `L` | reload library |
| `o` | faster (up to 2.00x) | `p` | slower (down to 0.50x) |
| `B` | bass boost on/off | `0` | reset fx |
| `/` | search | `d` | devices |
| `?` | help | `X` | logout |
| `q` | quit | | |

All of them can be changed in `~/.config/spotc/config`, which also holds
colors, bitrate, device name and persisted fx values.

## Notes

- Tokens are stored in `~/.config/spotc/tokens.json` (0600), librespot's cache
  in `~/.cache/spotc/`.
- `token-helper/` is an optional Rust experiment that mints Web API tokens
  through the librespot session. The main flow doesn't use it; it's kept for
  reference and not built by the Makefile.

## License

MIT
# spotc
