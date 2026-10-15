# foosonic

A [fooyin](https://github.com/fooyin/fooyin) plugin that browses and streams music from a
Subsonic-compatible server (Navidrome, Gonic, Airsonic...).

## Development goal

I am pushing a bunch of new untested features for now, and when the full functionality is achieved I will begin extensive testing and cleanup of the code. Stable version will be published afterwards.

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
| Transcoding (stream format + max bitrate) | ✅ |
| Download to Library (tagged files added to the fooyin library) | ✅ |
| Scrobbling (now playing + play counts) | ✅ |
| Star / unstar, random + starred discovery | ✅ |

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
skipped. Tracks that already exist in the library are played and seeked from disk.

## Settings

All under **Settings → Foosonic**:

- **Server config** (URL, username, password, authentication mode / API key, SSL, music folder,
  cover-art size). Authentication can be token (default), plaintext password, or an OpenSubsonic
  API key.
- **Stream format** (Original / MP3 / Opus / AAC) and **Max bitrate**.
- **Stream buffer** — in-memory buffer per remote stream (default 64 MB). At least the track size
  keeps the whole track in memory, which makes seeks instant.
- **Remote prebuffer** — audio buffered before playback resumes after a remote seek (default 400 ms).
- **Seek fade** — fade out/in around remote seeks, and the fade length (default 200 ms).
- **Scrobbling** — report now playing and play counts back to the server.
- **Library folder** — where **Download to Library** saves tracks (default `~/Music/Subsonic`). Point
  it at a folder inside one of your fooyin library paths so downloads become library tracks.

The browser's **Downloaded** column marks songs that already exist as a tagged file in the library
folder.

## Notes

- The password (or API key) is stored in fooyin's settings file in plain text.
- Logs use the `fy.subsonic` category (`QT_LOGGING_RULES="fy.subsonic.debug=true"`).
