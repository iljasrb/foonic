# foonic

A [fooyin](https://github.com/fooyin/fooyin) plugin for browsing and streaming music from a
Subsonic-compatible server (Navidrome, Gonic, Airsonic, ...).

## Install

1. Download `fyplugin_foonic.so` from the
   [Releases](https://github.com/iljasrb/foonic/releases) page.
2. Install it with **Settings → Plugins → Install**, or copy it to fooyin's plugin directory:

   ```sh
   mkdir -p ~/.local/lib/fooyin/plugins
   cp fyplugin_foonic.so ~/.local/lib/fooyin/plugins/
   ```

3. Restart fooyin and open **View → Foonic Browser**.

## Use

- Browse artists, albums, playlists and search results; double-click a song to play it.
- Drag tracks onto a fooyin playlist, or use the context menu to Play, Play Next, Add to Queue,
  Download to Library, star/unstar, or add to a server playlist.
- The **Foonic Browser** layout widget can be embedded under **Internet**.
- **Download to Library** saves the original file with tags and cover art under the configured
  library folder.

## Settings

Settings are under **Settings → Foonic**:

- **Server**: URL, username/password or API key, SSL handling, music folder and cover-art size.
- **Stream format / max bitrate**: play the original file (default) or transcode.
- **Remote prebuffer**: decoded audio buffered before resuming after a remote seek.
- **Seek fade**: fade out/in around remote seeks.
- **Session cache**: disk space used for the current fooyin session. The current track and the
  next track are cached; playback and prefetch share the same file. The cache is deleted when
  fooyin exits.
- **Library folder**: destination for **Download to Library**.
- **Buffered range on the seekbar**: show which parts of the current track are cached.

## Notes

- The password or API key is stored in fooyin's settings file in plain text.
- Subsonic stream URLs contain authentication parameters and are stored in fooyin playlists.
  Protect fooyin's playlist database like its settings file.
- Debug logs: `QT_LOGGING_RULES="fy.subsonic.debug=true"`.

## Build

Requires fooyin development files and Qt 6:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run the tests with:

```sh
cmake -S . -B build -DFOONIC_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
