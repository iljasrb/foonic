# foosonic

A [fooyin](https://fooyin.org) plugin that browses and streams music from a
Subsonic-compatible server (Navidrome, Gonic, Airsonic, Subsonic, ...).

## Features

Legend: ✅ tested, ⬜ untested/unsure.

| Feature | Status |
|---|---|
| Browse artists → albums → songs (lazy tree, length/size/cache columns, cover thumbnails, refresh, status) | ✅ |
| Play / Add to Playlist / Play Next / Add to Queue / Play A Subsonic playlist (double-click, right-click, drag-and-drop) | ✅ |
| Download-first playback — stream while caching, then switch to the local file | ✅ |
| Seamless resume after the switch | ✅ |
| Server setup, **Test Connection**, token auth (verified on Navidrome + Gonic) | ✅ |
| Settings persist across restarts | ✅ |
| Browser window (**View → Foosonic Browser**) | ✅ |
| Seeking on cached tracks | ✅ |
| Stream mode (play remote, never cache) | ⬜ |
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

Download `fyplugin_foosonic.so` from the
[Releases](https://github.com/iljasrb/foosonic/releases) page (pre-release builds), then drop it
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
right-click for Play / Add to Playlist / Play Next / Add to Queue, or double-click a song to play it.
**Refresh** reloads the list.

## Settings

All under **Settings → Foosonic**:

- **Server URL / Username / Password** with **Test Connection**.
- **Playback mode** — *Download first* (default: stream immediately, cache in the background, switch
  to the cached file in place) or *Stream* (never cache). Changing it is applied to the current track
  and the songs already in the playlist: switching to *Stream* stops in-flight caching, switching to
  *Download first* starts caching whatever is playing.
- **Stream while downloading** — play at once and switch to the cached file at the same position;
  off = wait until the track has downloaded.
- **Prefetch ahead** — how many upcoming tracks to fetch (default 1).
- **Stream format** (Original / MP3 / Opus / AAC) and **Max bitrate**.
- **Download folder**, **Cache size limit**, and **Clear Cache**.

The browser's **Cache** column shows ✓ cached / ↓ downloading, and the settings page shows the cache
size, file count, and the active download.

## Notes

- The password is stored in fooyin's settings file in plain text.
- Logs use the `fy.subsonic` category (`QT_LOGGING_RULES="fy.subsonic.debug=true"`).
