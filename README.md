# foonic

A [fooyin](https://github.com/fooyin/fooyin) plugin that browses and streams music from a
Subsonic-compatible server (Navidrome, Gonic, Airsonic...).

## Features

| Feature | Status |
|---|---|
| Library browser (**View → Foonic Browser**) and layout widget (Internet category) | Implemented |
| Drag-and-drop / menu add to a fooyin playlist, Play Next, Add to Queue | Implemented |
| Subsonic playlists, and server playlist editing (create / add / rename / delete) | Implemented |
| Library search and playlist search | Implemented |
| Server setup; settings persist across restarts | Implemented |
| Track metadata + cover art in the browser | Implemented |
| Remote streams are seekable + pausable (HTTP Range) | Implemented |
| Seeking and pausing on remote streams (server must support HTTP Range) | Implemented |
| Seek fade (fade out/in) around remote seeks | Implemented |
| Transcoding (stream format + max bitrate) | Unverified |
| Download to Library (tagged files added to the fooyin library) | Implemented |
| Scrobbling (now playing + play counts) | Implemented |
| Star / unstar, random + starred discovery | Unverified |

## Install

1. Download `fyplugin_foonic.so` from the
[Releases](https://github.com/iljasrb/foonic/releases) page.
2. Install it in fooyin: Settings → Plugins → Install, or drop it
into fooyin's plugin directory:

```sh
mkdir -p ~/.local/lib/fooyin/plugins
cp fyplugin_foonic.so ~/.local/lib/fooyin/plugins/
```

Restart fooyin, then open **View → Foonic Browser**.

## Build from source

Requires fooyin development files (headers + `FooyinConfig.cmake`) and Qt 6.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Then copy `build/fyplugin_foonic.so` as above, or install system-wide with
`sudo cmake --install build`.

## Usage

- **View → Foonic Browser** opens it in a window.
- Or add the **Foonic Browser** layout widget (under *Internet*) to embed it in a layout.

The browser has a **Filter** box that searches the server (Subsonic `search3`, falling back to
`search2`) for matching artists, albums, playlists and songs and shows them as grouped results.

Drag a selection onto a playlist,
right-click for Play / Add to Playlist / Play Next / Add to Queue / Download to Library / Star /
Unstar, or double-click a song to play it. Right-click empty space for **Random songs** and
**Starred songs**, right-click songs to add them to a **server playlist** (or create one), and
right-click a playlist to rename or delete it. **Refresh** reloads the list.

**Download to Library** works on any selection (a song, album, artist, playlist or several of them),
saves each track into the library folder below, writes its tags and cover art, and adds it to the
fooyin library. Files are laid out as `AlbumArtist/Album/NN - Title.ext` and already-present files are
skipped. Tracks that already exist in the library are played and seeked from disk.

## Settings

All under **Settings → Foonic**:

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
