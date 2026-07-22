# `baca`: TUI E-book Reader

![baca_screenshots](https://github.com/wustho/baca/assets/43810055/82d5beb0-d061-4e4c-82ed-a3bd84074d2f)

Meet `baca`, [epy](https://github.com/wustho/epy)'s lovely sister who lets you indulge
in your favorite e-books in the comfort of your terminal.
But with a sleek and contemporary appearance that's sure to captivate you!

## Features

- Formats supported: EPUB, EPUB3, MOBI, AZW, PDF, FB2, TXT, Markdown, CBZ, CBR,
  CB7, PNG, JPEG, GIF, WebP, BMP, and SVG
- Opens HTTP(S) document URLs with a private offline cache
- Remembers last reading position
- Searchable, sortable library home
- Native images in Ghostty/Kitty with ANSI and text fallbacks
- Scroll animations
- Clean & modern looks
- Text justification
- Dark & light color scheme
- Regex search
- Hyperlinks

## Requirements

- Linux with a UTF-8 locale
- GCC 14 or newer, or Clang 18 or newer, with C23 support
- `pkg-config`, ncursesw, SQLite, GLib, libxml2, libzip, libarchive, PCRE2,
  GdkPixbuf, Poppler GLib, Cairo, and libcurl
- `mobitool` from [libmobi](https://github.com/bfabiszewski/libmobi) for
  MOBI and AZW-family books

## Installation

Download a source archive or clone this repository, then install the development
packages for your distribution.

### Debian and Ubuntu

```sh
sudo apt update
sudo apt install build-essential pkg-config libncurses-dev libsqlite3-dev \
  libglib2.0-dev libxml2-dev libzip-dev libarchive-dev libpcre2-dev \
  libgdk-pixbuf-2.0-dev libpoppler-glib-dev libcairo2-dev \
  libcurl4-openssl-dev
```

### Fedora

```sh
sudo dnf install gcc make pkgconf-pkg-config ncurses-devel sqlite-devel \
  glib2-devel libxml2-devel libzip-devel libarchive-devel pcre2-devel \
  gdk-pixbuf2-devel poppler-glib-devel cairo-devel libcurl-devel
```

### Arch Linux

```sh
sudo pacman -S --needed base-devel pkgconf ncurses sqlite glib2 libxml2 \
  libzip libarchive pcre2 gdk-pixbuf2 poppler-glib cairo curl
```

`mobitool` is required only for MOBI, PRC, AZW, AZW3, and AZW4 files. Install
libmobi with its XML writer tools enabled and ensure `mobitool` is in `PATH`.
All other listed formats work without it.

If your distribution does not package `mobitool`, use libmobi's supported
Autotools build. The Git checkout requires Autoconf, Automake, and Libtool from
your distribution:

```sh
git clone https://github.com/bfabiszewski/libmobi.git
cd libmobi
./autogen.sh
./configure
make
make test
sudo make install
sudo ldconfig
```

### User-local install

This does not require root and installs into `~/.local`:

```sh
make doctor
make
make user-install
hash -r
baca --doctor
```

Ensure `~/.local/bin` is in `PATH`. The current shell can use:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

### System-wide install

The default prefix is `/usr/local`:

```sh
make doctor
make
sudo make install
baca --doctor
```

Installation includes the executable, default configuration reference, and the
`baca(1)` manual. Packaging tools can stage files safely with `DESTDIR`, for
example `make install DESTDIR="$pkgdir" PREFIX=/usr`.

### Uninstall

```sh
make user-uninstall       # user-local installation
sudo make uninstall       # system-wide installation
```

Uninstalling does not delete personal configuration, reading history, bookmarks,
or downloaded documents under `~/.config/baca` and `~/.cache/baca`.

## Usage

```sh
# open the library
baca

# open an ebook, document, comic, or image directly
baca path/to/your/ebook.epub
baca "/path/with spaces/book.pdf"

# download and open an HTTP(S) document
baca https://example.com/ebook.epub

# print reading history without opening the TUI
baca -r

# open a 1-based history row or fuzzy-match path/title/author
baca 3
baca alice wonder lewis carroll

# check installation paths and MOBI/AZW availability
baca --doctor

# read the complete command manual
man baca
```

### Library keys

- `Enter` or `l`: open the selected book; `j`/`k` or arrows move
- `gg`/`G`, Home/End: jump; Ctrl-f/Ctrl-b or Page keys: page
- `/`: literal case-insensitive filter; `Esc`: cancel or clear it
- `s`: cycle recent, title, and author sorting; `r`: refresh and remove missing books
- `o`: open a local path or HTTP(S) URL; `q`: quit

Quitting a reader opened from the library returns to the refreshed library.

### Reading URLs

HTTP and HTTPS documents are cached privately under `~/.cache/baca/downloads`.
The normalized URL remains the reading-history and bookmark identity, and a cached
document can be reopened while offline. URL fragments are ignored. URLs containing
credentials are rejected, TLS certificates and hostnames are verified, HTTPS
redirects cannot downgrade to HTTP, and downloads are limited to 256 MiB.

### Reader keys

- `j`/`k` or arrows scroll; Ctrl-f/Ctrl-b, `l`/`h`, or Page keys move by pages
- `gg`/`G` or Home/End jump to the start/end; `{count}gg` and `{count}G` jump to a 1-based layout line
- Numeric prefixes repeat scrolling, paging, and `n`/`N` search motions, such as `12j` or `3n`
- Ctrl-o goes back through jumps; Ctrl-i goes forward. Tab opens the TOC when there is no forward jump
- `/` and `?` search; `n`/`N` repeat; Enter clears highlights; `q` or Esc cancels a search or quits
- `b` saves the current position; `B` lists bookmarks; Enter jumps and Delete removes
- `M` opens metadata; F1 opens all configured keys

## Opening an Image

PNG, JPEG, GIF, WebP, BMP, and SVG files can be opened directly as one-image
documents when the corresponding GdkPixbuf loader is available. PNG, JPEG, GIF,
WebP, and BMP signatures are recognized from file contents. SVG markup is
recognized within a bounded 1 KiB prefix after an optional UTF-8 BOM, whitespace,
XML declaration, and comments. The extension is retained as a fallback. The
filename is used as the document title in reading history.

`ImageMode=auto` uses the Kitty graphics protocol in Ghostty and Kitty, true-color
ANSI half blocks in other color terminals, and a text placeholder when neither is
available. Images are clipped as they scroll, so partially visible images do not
break the reading flow.

Click a visible image or its `IMAGE` fallback to open the original resource with
the configured system viewer. Corrupt, oversized, animated, or otherwise
undecodable files in a supported format remain a placeholder instead of preventing
the document from opening. A direct image up to the 16 MiB decode limit is captured
at open and decoded from an immutable in-session compressed-byte snapshot, so
replacing its path or editing the file in place does not change later probes or
renders. Larger files do not retain compressed bytes and remain placeholders, but
their original descriptor stays open. Opening any direct image writes a stable,
private named copy under Baca's temporary directory for the configured viewer;
files above the 16 MiB snapshot limit are streamed in bounded chunks. Exports are
limited to 256 MiB to prevent temporary-storage exhaustion. A non-PDF image whose
aspect ratio would exceed 1,024 terminal rows is shown as a one-row `IMAGE`
placeholder rather than failing or being squashed.

## Reading Comics

CBZ (ZIP), CBR (RAR/RAR5), and CB7 (7-Zip) archives are read directly through
libarchive without extracting the book to disk. Image pages use a case-insensitive
natural filename order, so `2.png` precedes `10.png`. Directories, links, unsafe
paths, and non-image members are ignored. The table of contents uses the original
page filenames.

Comic archives are limited to 20,000 members, 10,000 image pages, 16 MiB per page,
1 GiB of declared image data, and 8 MiB of retained member names. Encrypted archives
are not supported. The opened archive descriptor remains authoritative for the
reading session, so replacing or deleting the path does not redirect page loads to
another file.

## Reading Text and FictionBook

TXT files are decoded as UTF-8, UTF-8 with a byte-order mark, or BOM-marked
UTF-16LE/UTF-16BE. CRLF and CR line endings are normalized to LF. Markdown files
use the same reader and remain literal plain text; Baca does not render Markdown
syntax. Text input and decoded output are each limited to 64 MiB. Invalid text,
embedded null characters, and incomplete UTF-16 code units are rejected.

FictionBook 2 (`.fb2`) files provide book and document metadata, nested section
navigation, inline emphasis/strong/code styles, internal and external links,
additional bodies such as notes, cover images, and lazy embedded image decoding.
FB2 files are limited to 64 MiB of XML and 64 MiB of generated markup. Metadata
fields are limited to 1 MiB. Up to 10,000 supported embedded images may be retained,
with 16 MiB per image, 1 GiB of declared image data, and 8 MiB of image identifiers.
DTD subsets are rejected and XML parsing never loads network resources. Missing or
unsupported image references remain placeholders instead of preventing the book
from opening.

## Reading PDFs

PDFs open in fixed-page view when the terminal has a usable Kitty or ANSI image
mode. When terminal graphics are unavailable and `ImageMode` resolves to a text
placeholder, PDFs open in extracted, reflowable text instead. Use the
`TogglePdfView` mapping (`v` by default) to switch views. Search works in both
views; a fixed-view match jumps to the matching page.

Click a PDF link to follow it. Clicking elsewhere on a rendered page opens the
original PDF in the system viewer. Reflow and search require an embedded text
layer, so scanned image-only pages may have no reflowable text. Password-protected
PDFs are not supported.

PDF input is limited to 10,000 pages, 1 MiB of extracted text per page and
16 MiB in total, 100,000 links with 8 MiB of retained link targets, and 100,000
outline nodes at up to 64 levels. Layout is limited to 262,144 lines, 1,024 rows
per image, and 65,536 aggregate extra image rows. Fixed-page renders are limited
to 32,768 pixels per dimension, 10 million target pixels, and 16 MiB of encoded
image data. A PDF that cannot preserve page aspect ratios within these limits
fails with an explicit error rather than shortening page images.

Poppler 26.07 does not expose page rotation through its GLib API. For a detected
90- or 270-degree page, Baca follows a destination to the page top instead of
using a vertical coordinate that may be wrong; normal-page XYZ, FitH, FitBH, and
FitR coordinates retain their vertical position.

## Configurations

![pretty_yes_no_cap](https://user-images.githubusercontent.com/43810055/228417623-ac78fb84-0ee0-4930-a843-752ef693822d.png)

Configuration file available at `~/.config/baca/config.ini` for linux users. Here is the default:

```ini
[General]
# pick your favorite image viewer
PreferredImageViewer = auto

# integer columns or a terminal-width percentage
MaxTextWidth = 80

# 'justify', 'center', 'left', 'right'
TextJustification = justify

Pretty = no

PageScrollDuration = 0.2

# auto, kitty, ansi, or placeholder
# oversized, over-tall, animated, or corrupt images are shown as placeholders
# PDF fixed-page rendering follows this mode; press v for reflowable text
ImageMode = auto

[Color Dark]
Background = #1e1e1e
Foreground = #f5f5f5
Accent = #0178d4

[Color Light]
Background = #f5f5f5
Foreground = #1e1e1e
Accent = #0178d4

[Keymaps]
ToggleLightDark = c
TogglePdfView = v
ScrollDown = down,j
ScrollUp = up,k
PageDown = ctrl+f,pagedown,l,space
PageUp = ctrl+b,pageup,h
Home = home,gg
End = end,G
OpenToc = tab
AddBookmark = b
OpenBookmarks = B
OpenMetadata = M
OpenHelp = f1
SearchForward = slash
SearchBackward = question_mark
NextMatch = n
PreviousMatch = N
Confirm = enter
CloseOrQuit = q,escape
Screenshot = f12
```

## Known Limitations

- When searching for specific phrases in `baca`,
  keep in mind that it may not be able to find them if they span across two lines,
  much like in the search behavior of editor vi(m).

  For example, `baca` won't be able to find the phrase `"for it"` because it is split into two lines
  in this example.

  ```
  ...
  she had forgotten the little golden key, and when she went back to the table for
  it, she found she could not possibly reach it: she could see  it  quite  plainly
  ...
  ```


  Additionally, `baca` may struggle to locate certain phrases due to adjustments made for text justification.
  See the example above, `"see_it"` may become `"see__it"` due to adjusted spacing between words.
  In this case, it may be more effective to use a regex search for `"see +it"` or simply search for the word `"see"` alone.

  Overall, `baca`'s search feature is most effective for locating individual words
  rather than phrases that may be split across multiple lines or impacted by text justification.

## Credits

- [ncurses](https://invisible-island.net/ncurses/)
- [libmobi](https://github.com/bfabiszewski/libmobi)
- [libarchive](https://www.libarchive.org/)
- [libxml2](https://gitlab.gnome.org/GNOME/libxml2)
- [libcurl](https://curl.se/libcurl/)

## License

GPL-3
