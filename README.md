# baca

a fast terminal ebook reader and library for linux.

![baca screenshot](https://github.com/wustho/baca/assets/43810055/82d5beb0-d061-4e4c-82ed-a3bd84074d2f)

## features

- reads epub, epub3, pdf, fb2, txt, markdown, cbz, cbr, cb7, png, jpeg, gif, webp, bmp, and svg
- reads mobi, prc, azw, azw3, and azw4 when `mobitool` is installed
- indexes calibre libraries and ordinary book folders with the bundled fff search engine
- groups formats for the same book and prefers epub when available
- remembers reading progress and bookmarks automatically
- opens local files and http(s) urls with a private offline cache
- renders images with kitty graphics, true-color ansi, or a text fallback
- provides regex document search, table of contents, metadata, links, themes, and configurable keys

## install

baca currently supports linux with a utf-8 locale. building requires gcc 14+ or clang 18+, rust 1.97, cargo, make, and `pkg-config`.

clone the repository with its pinned fff submodule:

```sh
git clone --recurse-submodules https://github.com/WhiteHades/baca.git
cd baca
rustup toolchain install 1.97.0 --profile minimal
```

if you already cloned without submodules, run:

```sh
git submodule update --init --recursive
```

### ubuntu and debian

```sh
sudo apt update
sudo apt install build-essential pkg-config libncurses-dev libsqlite3-dev \
  libglib2.0-dev libxml2-dev libzip-dev libarchive-dev libpcre2-dev \
  libgdk-pixbuf-2.0-dev libpoppler-glib-dev libcairo2-dev \
  libcurl4-openssl-dev librsvg2-common
```

### fedora

```sh
sudo dnf install gcc make pkgconf-pkg-config ncurses-devel sqlite-devel \
  glib2-devel libxml2-devel libzip-devel libarchive-devel pcre2-devel \
  gdk-pixbuf2-devel poppler-glib-devel cairo-devel libcurl-devel librsvg2
```

### arch linux

```sh
sudo pacman -S --needed base-devel pkgconf ncurses sqlite glib2 libxml2 \
  libzip libarchive pcre2 gdk-pixbuf2 poppler-glib cairo curl librsvg
```

install for the current user without root:

```sh
make doctor
make
make user-install
hash -r
baca --doctor
```

make sure `~/.local/bin` is in `PATH`. use `sudo make install` instead for a system-wide install under `/usr/local`.

the install includes `baca`, `libfff_c.so`, the default config, the `baca(1)` manual, and fff's license. rust is needed to build from source, but it is not needed to run an installed build.

uninstall with `make user-uninstall` or `sudo make uninstall`. personal config, history, bookmarks, and cached books are not deleted.

## usage

```sh
baca                              # open the library
baca book.epub                    # open a local book
baca "path with spaces/book.pdf" # quoted paths work
baca https://example.com/book.epub
baca -r                           # print reading history
baca 3                            # open history row 3
baca alice wonderland             # fuzzy-match history
baca --doctor                     # check the installation
```

run `man baca` for the complete command reference.

## library

set `LibraryPath` in `~/.config/baca/config.ini` to a calibre library or an ordinary folder of books. `LibraryPath = auto` checks `~/Calibre Library` and `~/Documents/Calibre Library`. `BACA_LIBRARY_PATH` overrides the config for one process.

a folder containing `metadata.db` is treated as a calibre library. baca displays authors first, then their books. ordinary folders display books directly. files with the same stem are grouped as formats of one book.

| key | action |
| --- | --- |
| `j`, `k`, arrows | move |
| `enter`, `l` | open the selected author or book |
| `/` | fuzzy filter through fff |
| `a` | switch between authors and all books |
| `f` | show formats for the selected book |
| `backspace`, `esc`, `h` | go back or clear the filter |
| `s` | cycle recent, title, and author sorting |
| `r` | rescan the library |
| `o` | open a path or url |
| `q` | quit |

epub is preferred over pdf, mobi/azw, and other supported formats. press `f` when you need a specific format.

## reader

| key | action |
| --- | --- |
| `j`, `k`, arrows | scroll |
| `ctrl-f`, `ctrl-b`, page keys | move by pages |
| `gg`, `G` | jump to the beginning or end |
| `/`, `?` | search forward or backward |
| `n`, `N` | repeat search |
| `ctrl-o`, `ctrl-i` | move through jump history |
| `tab` | open the table of contents |
| `b`, `B` | save or list bookmarks |
| `M` | show metadata |
| `v` | switch pdf fixed and reflow views |
| `f1` | show all configured keys |
| `q`, `esc` | cancel or quit |

numeric prefixes repeat movement commands, such as `12j`, `3n`, or `120G`.

## config

baca creates `~/.config/baca/config.ini` on first use. the important defaults are:

```ini
[General]
PreferredImageViewer = auto
LibraryPath = auto
MaxTextWidth = 80
TextJustification = justify
Pretty = no
PageScrollDuration = 0.2
ImageMode = auto

[Color Dark]
Background = #1e1e2e
Foreground = #cdd6f4
Accent = #cba6f7

[Color Light]
Background = #eff1f5
Foreground = #4c4f69
Accent = #8839ef
```

the shipped colors are catppuccin mocha and latte. see `resources/config.ini` for every key mapping.

## native fff integration

baca is written in c23. fff remains rust and is compiled from the pinned submodule into `libfff_c.so`. baca calls fff's c abi directly in the same process; it does not launch an fff command or require neovim.

the c adapter owns baca's copied paths and scores. rust owns the opaque index and temporary fff results, which are released through fff's matching c destructors.

## development

```sh
make test          # native and cli tests
make test-gcc      # clean gcc build
make test-clang    # clean clang build
make sanitize      # address and undefined behavior sanitizers
make installcheck  # staged install and uninstall
make format        # clang-format c sources and headers
```

fff is pinned as a git submodule so builds use a reviewed source revision and a locked cargo dependency graph.

## limits

- pdf reflow and search require an embedded text layer
- password-protected pdfs and encrypted comic archives are not supported
- document search works on laid-out lines, so a phrase split by wrapping may need a regex such as `word +next`
- direct image decoding is bounded; oversized or corrupt images remain placeholders

## license

gpl-3.0. the bundled fff component is mit licensed; its license is installed with baca.
