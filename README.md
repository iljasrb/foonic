# foosonic

A [fooyin](https://github.com/fooyin/fooyin) plugin that browses and streams music from a
Subsonic-compatible server (Navidrome, Gonic, Airsonic...).

## Features

Legend: ✅ tested, ⬜ untested/unsure.

| Feature | Status |
|---|---|
| Library browser (**View → Foosonic Browser**) | ✅ |
| Browser to fooyin playlist song appending (with drag and drop) | ✅ |
| Subsonic playlists | ✅ |
| Library search + playlist search | ✅ |
| Stream while caching, then switch to the local file | ✅ |
| Resume after the switch (works, but needs polishing) | ⚠️ |
| Server setup | ✅ |
| Settings persist across restarts | ✅ |
| Seeking on cached tracks | ✅ |
| Stream mode (remote play doesn't support track resuming and seeking) | ⚠️ |
| "Stream while downloading" gate (hold playback until cached) | ⬜ |
| Prefetch ahead | ⬜ |
| Download resume (HTTP Range) + retry | ⬜ |
| Cache management — size limit + eviction, clear cache, cache info | ⬜ |
| Restored-playlist self-heal (register + cache remote tracks) | ⬜ |
| Transcoding — stream format + max bitrate | ⬜ |
| Download folder + cache-size-limit settings | ⬜ |
| Layout widget (Internet category) | ⬜ |
| Track metadata + cover art in the browser | ✅ |

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
right-click for Play / Add to Playlist / Play Next / Add to Queue / Download to Library, or
double-click a song to play it. **Refresh** reloads the list.

**Download to Library** works on any selection (a song, album, artist, playlist or several of them),
saves each track into the library folder below, writes its tags and cover art, and adds it to the
fooyin library. Files are laid out as `AlbumArtist/Album/NN - Title.ext` and already-present files are
skipped.

## Settings

All under **Settings → Foosonic**:

- **Server config**
- **Playback mode** — *Download first* (default: stream immediately, cache in the background, switch
  to the cached file in place) or *Stream* (never cache, very restricted).
- **Stream while downloading** — play at once and switch to the cached file at the same position;
  off = wait until the track has downloaded.
- **Prefetch ahead** — how many upcoming tracks to fetch (default 1).
- **Stream format** (Original / MP3 / Opus / AAC) and **Max bitrate**.
- **Download folder**, **Cache size limit**, and **Clear Cache**.
- **Library folder** — where **Download to Library** saves tracks (default `~/Music/Subsonic`). Point
  it at a folder inside one of your fooyin library paths so downloads become library tracks.

The browser's **Cache** column shows ✓ cached / ↓ downloading, and the settings page shows the cache
size, file count, and the active download.

## Notes

- The password is stored in fooyin's settings file in plain text.
- Logs use the `fy.subsonic` category (`QT_LOGGING_RULES="fy.subsonic.debug=true"`).
