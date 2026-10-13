# foosonic

A [fooyin](https://github.com/fooyin/fooyin) plugin that browses and streams music from a
Subsonic-compatible server (Navidrome, Gonic, Airsonic...).

## Features

Legend: ✅ confirmed working, ⬜ not verified yet.

| Feature | Status |
|---|---|
| Library browser (**View → Foosonic Browser**) and layout widget (Internet category) | ✅ |
| Drag-and-drop / menu add to a fooyin playlist, Play Next, Add to Queue | ✅ |
| Subsonic playlists, and server playlist editing (create / add / rename / delete) | ✅ |
| Library search and playlist search | ✅ |
| Server setup; settings persist across restarts | ✅ |
| Track metadata + cover art in the browser | ✅ |
| Remote streams are seekable + pausable (HTTP Range) | ✅ |
| Seeking and pausing on remote streams (server must support HTTP Range) | ✅ |
| Seek fade (fade out/in) around remote seeks | ✅ |
| Instant seek from the cached file once a track has downloaded | ✅ |
| Stream while caching, then play from the local file on pause | ✅ |
| Prefetch ahead | ✅ |
| Download resume (HTTP Range) + retry | ✅ |
| Cache management — size limit + eviction, clear cache, cache info | ✅ |
| Restored-playlist self-heal (re-stream + re-cache a dead cache path) | ✅ |
| Transcoding (stream format + max bitrate) | ✅ |
| Download folder + cache-size-limit settings | ✅ |
| Scrobbling (now playing + play counts) | ✅ |
| Star / unstar, random + starred discovery | ✅ |
| Browser activity line (streaming, caching, switching, added to cache) | ✅ |
| "Stream while downloading" gate (hold playback until the track is cached) | ⬜ |

## Install

1. Download `fyplugin_foosonic.so` from the
[Releases](https://github.com/iljasrb/foosonic/releases) page (pre-release builds).
2. Install it in fooyin: Settings → Plugins → Install, or drop it
into fooyin's plugin directory:

```sh
mkdir -p ~/.local/lib/fooyin/plugins
cp fyplugin_foosonic.so ~/.local/lib/fooyin/plugins/
```

Restart fooyin, then open **View → Foosonic Browser**.

## Build from source

Requires fooyin development files (headers + `FooyinConfig.cmake`) and Qt 6.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Then copy `build/fyplugin_foosonic.so` as above, or install system-wide with
`sudo cmake --install build`.

## Usage

- **View → Foosonic Browser** opens it in a window.
- Or add the **Foosonic Browser** layout widget (under *Internet*) to embed it in a layout.

The browser has a **Filter** box that searches the server (Subsonic `search3`, falling back to
`search2`) for matching artists, albums and songs and shows them as grouped results (input is
debounced).

Expand an artist to load its albums and an album to load its songs. Drag a selection onto a playlist,
right-click for Play / Add to Playlist / Play Next / Add to Queue / Download to Library / Star /
Unstar, or double-click a song to play it. Right-click empty space for **Random songs** and
**Starred songs**, right-click songs to add them to a **server playlist** (or create one), and
right-click a playlist to rename or delete it. **Refresh** reloads the list.

**Download to Library** works on any selection (a song, album, artist, playlist or several of them),
saves each track into the library folder below, writes its tags and cover art, and adds it to the
fooyin library. Files are laid out as `AlbumArtist/Album/NN - Title.ext` and already-present files are
skipped. Downloaded (and pre-existing library) tracks are also used as the play cache, so they play
and seek from disk instead of streaming.

## Settings

All under **Settings → Foosonic**:

- **Server config** (URL, username, password, authentication mode / API key, SSL, music folder,
  cover-art size). Authentication can be token (default), plaintext password, or an OpenSubsonic
  API key.
- **Playback mode** — *Download first* (default: stream immediately and cache in the background) or
  *Stream* (play remotely, caching disabled). Once downloaded, seeks read straight from the cached
  file and pause switches playback to it (never a mid-track swap).
- **Stream while downloading** — play at once and switch to the cached file at the same position;
  off = wait until the track has downloaded.
- **Prefetch ahead** — how many upcoming tracks to fetch (default 1).
- **Stream format** (Original / MP3 / Opus / AAC) and **Max bitrate**.
- **Stream buffer** — in-memory buffer per remote stream (default 64 MB). At least the track size
  keeps the whole track in memory, which makes seeks instant.
- **Remote prebuffer** — audio buffered before playback resumes after a remote seek (default 400 ms).
- **Seek fade** — fade out/in around remote seeks, and the fade length (default 200 ms).
- **Scrobbling** — report now playing and play counts back to the server.
- **Play cached only** — skip stream-only tracks and play cached/downloaded copies only.
- **Download folder**, **Cache size limit**, and **Clear Cache**.
- **Library folder** — where **Download to Library** saves tracks (default `~/Music/Subsonic`). Point
  it at a folder inside one of your fooyin library paths so downloads become library tracks.

The browser's **Cache** column shows ✓ cached / ✓ **Downloaded copy** (a permanent library file) /
↓ downloading with a percentage. A second status line under the toolbar narrates what the player
is doing (streaming, caching, switching to the cached file, added to cache), with an animated dots
indicator while work is in progress. The settings page shows the cache size, file count, and the
active download.

## Notes

- A finished background download does not interrupt playback. The track keeps streaming; a seek to
  already-downloaded audio is served straight from the cached file (no network round-trip), and the
  local copy is used from the next track start or when you pause.
- The password (or API key) is stored in fooyin's settings file in plain text.
- Logs use the `fy.subsonic` category (`QT_LOGGING_RULES="fy.subsonic.debug=true"`).
