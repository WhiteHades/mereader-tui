# `baca`: TUI E-book Reader

![baca_screenshots](https://github.com/wustho/baca/assets/43810055/82d5beb0-d061-4e4c-82ed-a3bd84074d2f)

Meet `baca`, [epy](https://github.com/wustho/epy)'s lovely sister who lets you indulge
in your favorite e-books in the comfort of your terminal.
But with a sleek and contemporary appearance that's sure to captivate you!

## Features

- Formats supported: EPUB, EPUB3, MOBI, AZW, PDF, CBZ, CBR, CB7, PNG, JPEG,
  GIF, WebP, BMP, and SVG
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

- A C23 compiler
- `pkg-config`, ncursesw, SQLite, GLib, libxml2, libzip, libarchive, PCRE2,
  GdkPixbuf, Poppler GLib, and Cairo
- `mobitool` from [libmobi](https://github.com/bfabiszewski/libmobi) for
  MOBI and AZW-family books

## Installation

```sh
make
sudo make PREFIX=/usr/local install
```

## Usage

```sh
# open the library
baca

# open an ebook, PDF, or image directly
baca path/to/your/ebook.epub

# print reading history without opening the TUI
baca -r

# open a 1-based history row or fuzzy-match path/title/author
baca 3
baca alice wonder lewis carroll
```

### Library keys

- `Enter` or `l`: open the selected book; `j`/`k` or arrows move
- `gg`/`G`, Home/End: jump; Ctrl-f/Ctrl-b or Page keys: page
- `/`: literal case-insensitive filter; `Esc`: cancel or clear it
- `s`: cycle recent, title, and author sorting; `r`: refresh and remove missing books
- `o`: open a local path; `q`: quit

Quitting a reader opened from the library returns to the refreshed library.

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
Home = home,g
End = end,G
OpenToc = tab
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

- Compared to [epy](https://github.com/wustho/epy), currently `baca` has some missing features.
  But these are planned to be implemented to `baca` in the near future:

  - [ ] **TODO** Bookmarks
  - [ ] **TODO** FictionBook support
  - [ ] **TODO** URL reading support

## Credits

- [ncurses](https://invisible-island.net/ncurses/)
- [libmobi](https://github.com/bfabiszewski/libmobi)
- [libxml2](https://gitlab.gnome.org/GNOME/libxml2)

## License

GPL-3
