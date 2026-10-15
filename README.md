# navipod-sync

Sync an iPod Classic with a Navidrome library (no Rockbox required). navipod-sync works with the stock iPod firmware (writes the iPod's normal database).
Previously iTunes-synced music, videos, podcasts, the Photos folder/Photo Database is never modified.

## Build

Clone with submodules (libgpod-ng is built together with navipod):

```sh
git clone --recursive https://github.com/sidchil/navipod-sync
cd navipod-sync
```

Dependencies:

```sh
# Debian / Ubuntu
sudo apt install cmake ninja-build pkg-config g++ python3 libglib2.0-dev libplist-dev libsqlite3-dev \
                 libgdk-pixbuf-2.0-dev libxml2-dev zlib1g-dev libcurl4-openssl-dev nlohmann-json3-dev
# Arch Linux
sudo pacman -S base-devel cmake ninja pkgconf python glib2 libplist sqlite gdk-pixbuf2 libxml2 zlib curl nlohmann-json

# macOS (Homebrew)
brew install cmake ninja pkg-config glib pcre2 libffi libplist sqlite gdk-pixbuf libxml2 nlohmann-json
export PKG_CONFIG_PATH="$(brew --prefix pcre2)/lib/pkgconfig:$(brew --prefix glib)/lib/pkgconfig:$(brew --prefix libffi)/lib/pkgconfig:$(brew --prefix libxml2)/lib/pkgconfig:$(brew --prefix sqlite)/lib/pkgconfig"
```

Build and test:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
cmake --install build --prefix ~/.local
```

To use an installed libgpod instead of the submodule, add `-DNAVIPOD_SYSTEM_LIBGPOD=ON`.
Windows is not supported.

## Usage

```sh
navipod pair --server https://music.example.com --user me    # only needs to be run once per computer/iPod
navipod info
navipod sync --all --max-bitrate 256 --eject
navipod sync --playlist "Road trip" --artist "Daft Punk" --starred --fill
navipod status                                                # dry run of your saved settings
```

Settings are remembered per iPod in `iPod_Control/navipod/state.json`. Run `navipod help` for all options.

| iTunes | navipod |
|---|---|
| Link the iPod to a library | `pair`. Sync refuses to run against a different server or user unless you use `pair --force` |
| Entire library, or selected playlists, artists, genres | `--all`, `--playlist`, `--artist`, `--album`, `--genre`, `--starred`, `--recent N` |
| Automatically fill free space | `--fill` |
| Convert higher bitrate songs | `--max-bitrate N` (Navidrome transcodes) |
| Manually manage music | `--manual` (never deletes) |
| Play counts and ratings | Written back to Navidrome by default (`--no-scrobble` to disable) |
| Artwork | On by default (`--no-artwork`) |
| Eject | `eject`, or `--eject` after a sync |
| Remove synced content | `unpair --erase` |

navipod only touches songs it added itself. Music synced by iTunes, videos, podcasts, and the Photos folder are left
alone. The iTunesDB is backed up to `iPod_Control/navipod/backup/` before each sync, and the last three are kept.

## Edited serial numbers

The Classic checks a signature on its database that is computed from the iPod's FireWire GUID, and libgpod picks
the signature type from the model number, which it looks up from the serial number. If you changed the serial (to allow for higher-capacity storage or iPod classic "7th gen" exclusive features):

- `navipod info` must show a recognized model and the GUID. If not, pass `--model` (for example `MB565`) and
  `--guid` (16 hex digits). Both are remembered.
- A wrong GUID gives a database the iPod rejects, so the library looks empty. Restore the file in
  `iPod_Control/navipod/backup/`, or sync once with iTunes.
- navipod will not write if the model is unknown or the GUID is missing.

The GUID is the iPod's USB serial number: `system_profiler SPUSBDataType` on macOS, `lsusb -v -d 05ac: | grep iSerial`
on Linux.

## Notes

- The iPod plays MP3, AAC/ALAC (.m4a), WAV and AIFF. FLAC, OGG and Opus are transcoded by Navidrome, to 320 kbps
  MP3 by default. Navidrome's stock "aac" transcoding is raw ADTS, which the iPod cannot play.
- Songs already on the iPod from iTunes are not matched against the Navidrome library, so a song can end up on the
  iPod twice.
- Release builds need the libraries from the dependency list above installed (glib, libplist, sqlite3, gdk-pixbuf,
  libxml2, curl).
