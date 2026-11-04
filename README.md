# foonic

A [fooyin](https://github.com/fooyin/fooyin) plugin that browses and streams music from a
Subsonic-compatible server (Navidrome, Gonic, Airsonic...).

## Features

| Feature | Status |
|---|---|
| Library browser (**View → Foonic Browser**) and layout widget (Internet category) | Implemented |
| Home shelves (recently added/played, most played, top rated, favourites, random, genres) | Implemented |
| Drag-and-drop / menu add to a fooyin playlist, Play Next, Add to Queue | Implemented |
| Subsonic playlists, and server playlist editing (create / add / remove / rename / clear / delete) | Implemented |
| Add albums, artists, genres and shelves to a server playlist | Implemented |
| Library search, playlist-name search, optional playlist-track search | Implemented |
| Server setup; settings persist across restarts | Implemented |
| Track metadata + cover art in the browser | Implemented |
| Album cover grid and details panel (metadata, notes, biography, lyrics) | Implemented |
| Ratings and favourites (star/unstar songs, albums and artists; 1–5 stars) | Implemented |
| Remote streams are seekable + pausable (HTTP Range) | Implemented |
| Whole-track disk cache; any downloaded point seeks instantly | Implemented |
| Seek fade (fade out/in) around remote seeks | Implemented |
| Next-track prefetch once the current track is fully cached | Implemented |
| Transcoding (stream format + max bitrate) | Unverified |
| Download to Library (original file, tagged and added to the fooyin library) | Implemented |
| Scrobbling (now playing + play counts) | Implemented |
| Star / unstar, random + starred discovery | Implemented |

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

Run the self-checks with:

```sh
cmake -S . -B build -DFOONIC_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Then copy `build/fyplugin_foonic.so` as above, or install system-wide with
`sudo cmake --install build`.

## Usage

- **View → Foonic Browser** opens it in a window.
- Or add the **Foonic Browser** layout widget (under *Internet*) to embed it in a layout.

The browser opens on a **Home** section with shelves for recently added, recently played, most
played, top rated, favourite albums, random songs and genres. The **Filter** box searches the server
(Subsonic `search3`, falling back to `search2`) for matching artists, albums, playlists and songs and
shows them as grouped results. Playlist-track search is opt-in under **Filter options → Search in →
Playlist tracks** because it fetches every playlist from the server.

Drag a selection onto a playlist, right-click for Play / Add to Playlist / Play Next / Add to Queue /
Download to Library / Star / Unstar / Rating / Start Radio, or double-click a song to play it. Right-click empty
space for **Random songs** and **Starred songs**. Right-click songs, albums, artists, genres or Home
shelves to add them to a **server playlist** (or create one). Right-click a playlist to rename,
clear or delete it. Right-click a song inside an expanded playlist to remove it from that playlist.
**Refresh** reloads the list.

Use the **Grid** button for an album cover grid and the **Details** button for the side panel. The
panel shows cover art, metadata, server notes/biography and lyrics when the server provides them.
Selecting an item updates the panel; double-click an album in the grid to play it.

**Download to Library** works on any selection (a song, album, artist, playlist or several of them),
saves each track into the library folder below, writes its tags and cover art, and adds it to the
fooyin library. Downloads always request the original file, even when transcoding is enabled. Files
are laid out as `AlbumArtist/Album[/CDn]/NN - Title.ext` and already-present files are skipped.
Tracks that already exist in the library are played and seeked from disk.

## Settings

All under **Settings → Foonic**:

- **Server config** (URL, username, password, authentication mode / API key, SSL, music folder,
  cover-art size). Authentication can be token (default), plaintext password, or an OpenSubsonic
  API key.
- **Stream format** (Original / MP3 / Opus / AAC) and **Max bitrate**.
- **Remote prebuffer** — audio buffered before playback resumes after a remote seek (default 400 ms).
- **Seek fade** — fade out/in around remote seeks, and the fade length (default 200 ms).
- **Scrobbling** — report now playing and play counts back to the server.
- **Buffered range on the seekbar** — show the downloaded part of the current remote track.
- **Library folder** — where **Download to Library** saves tracks (default `~/Music/Subsonic`). Point
  it at a folder inside one of your fooyin library paths so downloads become library tracks.

The browser's **Downloaded** column marks songs that already exist as a tagged file in the library
folder.

## Notes

- The password (or API key) is stored in fooyin's settings file in plain text.
- While a remote track plays, the whole track is cached under fooyin's cache directory
  (`foonic/stream-cache`). The download continues in the background, so seeking anywhere in the
  track is instant once that part has arrived. The per-track file is removed when playback stops.
- Stream URLs contain the authentication parameters and are stored as track paths in fooyin
  playlists. Treat fooyin's playlist database with the same care as its settings file.
- Logs use the `fy.subsonic` category (`QT_LOGGING_RULES="fy.subsonic.debug=true"`).
