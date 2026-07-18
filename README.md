# `baca`: TUI E-book Reader

![baca_screenshots](https://github.com/wustho/baca/assets/43810055/82d5beb0-d061-4e4c-82ed-a3bd84074d2f)

Meet `baca`, [epy](https://github.com/wustho/epy)'s lovely sister who lets you indulge
in your favorite e-books in the comfort of your terminal.
But with a sleek and contemporary appearance that's sure to captivate you!

## Features

- Formats supported: Epub, Epub3, Mobi & Azw
- Remembers last reading position
- Native images in Ghostty/Kitty with ANSI and text fallbacks
- Scroll animations
- Clean & modern looks
- Text justification
- Dark & light color scheme
- Regex search
- Hyperlinks

## Requirements

- A C23 compiler
- `pkg-config`, ncursesw, SQLite, libxml2, libzip, PCRE2, GdkPixbuf,
  Poppler GLib, and Cairo
- `mobitool` from [libmobi](https://github.com/bfabiszewski/libmobi) for
  MOBI and AZW-family books

## Installation

```sh
make
sudo make PREFIX=/usr/local install
```

## Usage

```sh
# to read an ebook
baca path/to/your/ebook.epub

# to read your last read ebook, just run baca without any argument
baca

# to see your reading history use -r as an argument
baca -r

# say you want to read an ebook from your reading history,
# but you forgot the path to your ebook
# just type any words you remember about your ebook
# and baca will try to match it to path or title+author
baca doc ebook.epub
baca alice wonder lewis carroll
```

## Opening an Image

`ImageMode=auto` uses the Kitty graphics protocol in Ghostty and Kitty, true-color
ANSI half blocks in other color terminals, and a text placeholder when neither is
available. Images are clipped as they scroll, so partially visible images do not
break the reading flow.

Click a visible image or its `IMAGE` fallback to open the original resource with
the configured system viewer. Corrupt, oversized, or unsupported images remain a
placeholder instead of preventing the book from opening.

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
# oversized, animated, or corrupt images are shown as placeholders
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
