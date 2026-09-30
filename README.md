# ricoded

ricoded is a small Unix lyrics viewer for cmus.  It shows the lyrics
embedded in your FLAC files - nothing is fetched from the network, no
sidecar `.lrc` files, no caches, no lyric databases.

## Philosophy

* Portability and minimal dependencies above everything: C99, POSIX
  APIs, libc.  If libc can do it, libc does it.
* No libFLAC, no ffmpeg, no ncurses, no metadata libraries.  The FLAC
  container and Vorbis Comment framing are parsed by ricoded-ng itself
  (only the metadata is read; audio is never decoded).
* No build ecosystem: a plain `make` with GCC.  No CMake, Meson, or Nix.
* Small, readable, Unix-like: programs that do one thing, text
  protocols, fork/pipe/exec instead of shells and daemons.

## Architecture: ricoded vs ricoded-ng

The project follows a Linux-like split:

```
ricoded
  |
  +-- ricoded-ng   the "kernel": lyric engine, no UI
  +-- ricoded      the reference "distro": terminal frontend
```

**ricoded-ng** is a one-shot engine.  It asks cmus for the current
file, reads embedded lyrics from the FLAC's Vorbis Comments, parses
LRC (including word-level timestamps), and prints a machine-readable
protocol on stdout.  It has no terminal code, no network code, and no
opinion about how lyrics should be displayed.  Anyone can build an
alternative frontend ("a ricoded distro") on top of it.

**ricoded** is the reference frontend.  It polls cmus (~10 Hz),
notices song changes, runs ricoded-ng exactly once per song, keeps the
parsed result in memory, and follows playback position with binary
searches.  It never re-runs the engine for a song it has already
loaded.

## Building and installing

Requires a C99 compiler, libc, and POSIX.  Builds with GNU make or BSD
make:

    make
    make install            # PREFIX defaults to /usr/local
    make install DESTDIR=/tmp/stage PREFIX=/usr

Compiles warning-free under:

    gcc -std=c99 -Wall -Wextra -Wpedantic -O2

Both binaries must be in `PATH` (the frontend execs `ricoded-ng` by
name).  cmus must be running with `cmus-remote` available.

## Where lyrics come from

Only embedded FLAC Vorbis Comments are read.  The first matching key
wins; priority is independent of the order tags appear in the file:

1. `SYNCEDLYRICS`  (LRC, may include word timestamps)
2. `LYRICS`
3. `UNSYNCEDLYRICS`

If the chosen value contains LRC timestamps it is treated as
synchronized lyrics; otherwise each line becomes an unsynchronized
lyric line (shown in full, no timing).

## LRC support

Line timestamps:  `[mm:ss.xx]text`  (1-3 fraction digits; `mm` up to
999, `ss` up to 59).  A line may carry several leading timestamps and
is then repeated at each time.  `[ti:...]`-style metadata tags are
ignored.

Word timestamps:  `[mm:ss.xx]<mm:ss.xx>word <mm:ss.xx>next ...`

The `<...>` markers never appear in output.  A word's text runs from
after its marker to the next marker, or to (and including) the first
following whitespace - so in

    [00:53.070]<00:53.078>I <00:53.233>pull <00:53.417>up in that Lamborghini

the words are `I `, `pull ` and `up `, and `in that Lamborghini`
remains part of the line with no invented timestamp.

Timestamps that are malformed (including fractions longer than three
digits and the ambiguous `mm:ss:xx` form) are rejected, not guessed.

## Engine output protocol (stable, machine-readable)

One record per line, TAB-separated.  Lyric text is everything after
the third TAB.

    L<TAB>line-index<TAB>line-time<TAB>line-text
    W<TAB>line-index<TAB>word-time<TAB>word-text

* Lines with word timing produce one `L` followed by its `W` records.
* Plain LRC produces only `L` records.
* A line-time or word-time printed as a negative value (`-1.000`)
  marks an unsynchronized line.

Example:

    L	0	53.070	I pull up in that Lamborghini just so you can see me
    W	0	53.078	I 
    W	0	53.233	pull 
    W	0	53.417	up 

Diagnostics (no track, unreadable FLAC, missing comments, parse
errors) go to stderr with a nonzero exit status; stdout carries
protocol records only.

## Frontend

    ricoded

Runs in the alternate screen, monochrome ANSI.  Header:

    ricoded [1] line mode [2] word mode [3] current word [q] quit

Controls: `1`, `2`, `3` switch mode, `q` quits.

* **Line mode** - the complete current lyric line.
* **Word mode** - the complete line with the active word in bold
  (ANSI SGR 1).  Falls back to line mode when the line has no word
  timing.
* **Current-word mode** - only the active word.  Falls back to line
  mode when no word timing exists.

Behavioral notes:

* If playback is before the first timestamp, the first line is shown
  (an explicit choice; the UI is never blank merely because no
  timestamp has elapsed yet).
* Unsynchronized lyrics are shown in full, all lines stacked.
* The screen is redrawn only when the active line, active word, mode,
  song, status, or terminal size changes.

## Limitations / current scope

* FLAC + Vorbis Comments only.  No Ogg, MP4, ID3, or sidecar files.
* cmus only (anything `cmus-remote -Q` reports).
* Centering is byte-based, not glyph-width-aware (CJK lyrics will be
  off-center).
* The planned bitmap/half-block glyph renderer does not exist yet.
* Lyric text is treated as opaque bytes; no charset conversion.
* No configuration file; the three modes and the hardcoded 100 ms poll
  interval are the whole interface.

## License

BSD-2-Clause, see LICENSE.
