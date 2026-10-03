/*
 * ricoded-halfblock - minimal half-block terminal frontend for ricoded-ng.
 *
 * Word-timed songs: W timing selects the active chunk, while the source
 * lyric determines the complete word rendered with half-block glyphs.
 * Songs/lines without word timing fall back to centered line-by-line text.
 *
 * Controls: q quits.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define POLL_NS 100000000L   /* cmus poll interval: 100 ms */
#define NOTE_DISPLAY_DELAY 10.0 /* show notes after n seconds without a new line */

typedef struct {
    double t;
    char  *text;
} Word;

typedef struct {
    double  t;       /* < 0 => unsynchronized line */
    char   *text;
    Word   *w;
    size_t  nw, wcap;
} Line;

typedef struct {
    Line  *v;
    size_t n, cap;
    bool   synced;
} Lyrics;

typedef struct {
    char   file[4096];
    double position;
    int    playing;
    int    valid;
} CmusState;

typedef struct {
    double raw_position;
    double base_position;
    struct timespec base_time;
    int playing;
    int valid;
} PlaybackClock;

static volatile sig_atomic_t running = 1, resized = 1;

static void on_signal(int sig) { (void)sig; running = 0; }
static void on_winch(int sig) { (void)sig; resized = 1; }

static void sig_setup(int sig, void (*h)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* terminal                                                            */
/* ------------------------------------------------------------------ */

static struct termios old_termios;
static int termios_saved = 0;

static void cleanup_terminal(void)
{
    if (!termios_saved)
        return;
    fputs("\033[0m\033[?25h\033[?1049l", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
    termios_saved = 0;
}

static int terminal_init(void)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &old_termios) < 0)
        return -1;
    termios_saved = 1;
    t = old_termios;
    t.c_lflag &= ~(ECHO | ICANON);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) < 0) {
        termios_saved = 0;
        return -1;
    }
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fl >= 0)
        fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    fputs("\033[?1049h\033[?25l\033[2J\033[H", stdout);
    fflush(stdout);
    return 0;
}

static void terminal_size(int *cols, int *rows)
{
    struct winsize ws;
    *cols = 80;
    *rows = 24;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col) *cols = ws.ws_col;
        if (ws.ws_row) *rows = ws.ws_row;
    }
}

/* ------------------------------------------------------------------ */
/* lyrics storage (parsed from ricoded-ng L/W records)                 */
/* ------------------------------------------------------------------ */

static void line_free(Line *ln)
{
    for (size_t i = 0; i < ln->nw; i++)
        free(ln->w[i].text);
    free(ln->w);
    free(ln->text);
    memset(ln, 0, sizeof *ln);
}

static void lyrics_free(Lyrics *L)
{
    for (size_t i = 0; i < L->n; i++)
        line_free(&L->v[i]);
    free(L->v);
    memset(L, 0, sizeof *L);
}

static int lyrics_grow_to(Lyrics *L, size_t need)
{
    if (need <= L->cap) {
        if (L->n < need)
            L->n = need;
        return 0;
    }
    size_t ncap = L->cap ? L->cap : 16;
    while (ncap < need)
        ncap *= 2;
    Line *nv = realloc(L->v, ncap * sizeof *nv);
    if (!nv)
        return -1;
    memset(nv + L->cap, 0, (ncap - L->cap) * sizeof *nv);
    L->v = nv;
    L->cap = ncap;
    L->n = need;
    return 0;
}

static int word_push(Line *ln, double t, const char *text)
{
    if (ln->nw == ln->wcap) {
        size_t ncap = ln->wcap ? ln->wcap * 2 : 8;
        Word *nw = realloc(ln->w, ncap * sizeof *nw);
        if (!nw)
            return -1;
        ln->w = nw;
        ln->wcap = ncap;
    }
    ln->w[ln->nw].text = strdup(text);
    if (!ln->w[ln->nw].text)
        return -1;
    ln->w[ln->nw].t = t;
    ln->nw++;
    return 0;
}

/*
 * Parse one protocol record:
 *   L<TAB>index<TAB>time<TAB>text
 *   W<TAB>index<TAB>time<TAB>text
 * The lyric text is everything after the third TAB (tabs included).
 * Malformed records are skipped; allocation failure is fatal (-1).
 */
static int parse_record(Lyrics *L, char *line)
{
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
    if (len < 6 || line[1] != '\t')
        return 0;
    char type = line[0];
    char *p = line + 2;
    char *tab1 = strchr(p, '\t');
    if (!tab1)
        return 0;
    *tab1++ = '\0';
    char *tab2 = strchr(tab1, '\t');
    if (!tab2)
        return 0;
    *tab2++ = '\0';

    unsigned long idx = strtoul(p, NULL, 10);
    double t = strtod(tab1, NULL);
    if (idx > 1000000UL)
        return -1;

    if (type == 'L') {
        if (lyrics_grow_to(L, (size_t)idx + 1) < 0)
            return -1;
        Line *ln = &L->v[idx];
        line_free(ln);           /* defensive: drop any previous contents */
        ln->text = strdup(tab2);
        if (!ln->text)
            return -1;
        ln->t = t;
        if (t >= 0)
            L->synced = true;
    } else if (type == 'W') {
        if (idx >= L->n)
            return -1;           /* W without its parent L: protocol error */
        if (word_push(&L->v[idx], t, tab2) < 0)
            return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* cmus + engine                                                       */
/* ------------------------------------------------------------------ */

/*
 * Query cmus with fork/pipe/exec (never a shell).  A "file" line that
 * does not fit the buffer is rejected outright rather than truncated.
 */
static int cmus_query(CmusState *s)
{
    int p[2];
    pid_t pid;
    FILE *fp;
    char line[8192];
    int st;

    memset(s, 0, sizeof *s);
    if (pipe(p) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        close(p[0]);
        close(p[1]);
        execlp("cmus-remote", "cmus-remote", "-Q", (char *)0);
        _exit(127);
    }
    close(p[1]);
    fp = fdopen(p[0], "r");
    if (!fp) {
        close(p[0]);
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
            ;
        return -1;
    }

    while (fgets(line, sizeof line, fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] != '\n' && !feof(fp)) {
            /* line longer than the buffer: reject it, drain to EOL */
            int c;
            while ((c = fgetc(fp)) != EOF && c != '\n')
                ;
            continue;
        }
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (strncmp(line, "file ", 5) == 0) {
            size_t plen = strlen(line + 5);
            if (plen == 0 || plen >= sizeof s->file) {
                s->valid = 0;    /* too long: reject, do not truncate */
                continue;
            }
            memcpy(s->file, line + 5, plen + 1);
            s->valid = 1;
        } else if (strncmp(line, "position ", 9) == 0) {
            s->position = strtod(line + 9, NULL);
        } else if (strcmp(line, "status playing") == 0) {
            s->playing = 1;
        } else if (strncmp(line, "status ", 7) == 0) {
            s->playing = 0;
        }
    }
    fclose(fp);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        s->valid = 0;
        return -1;
    }
    return s->valid ? 0 : -1;
}

static double timespec_elapsed(const struct timespec *start,
                              const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec)
         + (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

/*
 * cmus reports playback position in coarse units.  Keep a monotonic local
 * clock between reports so word timestamps can be followed at sub-second
 * precision.  Re-anchor when cmus advances its reported position or when
 * playback changes between playing and paused/stopped.
 */
static double playback_position(PlaybackClock *c, const CmusState *s)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return s->position;

    if (!c->valid || c->raw_position != s->position || c->playing != s->playing) {
        c->raw_position = s->position;
        c->base_position = s->position;
        c->base_time = now;
        c->playing = s->playing;
        c->valid = 1;
    }

    if (!c->playing)
        return c->raw_position;

    return c->base_position + timespec_elapsed(&c->base_time, &now);
}

/* Run ricoded-ng once, parse its stdout, and reap it, checking status. */
static int engine_load(Lyrics *L)
{
    int p[2];
    pid_t pid;
    FILE *fp;
    char *line = NULL;
    size_t cap = 0;
    int st;
    int rc = 0;

    lyrics_free(L);
    if (pipe(p) < 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(p[1], STDOUT_FILENO);
        close(p[0]);
        close(p[1]);
        execlp("ricoded-ng", "ricoded-ng", (char *)0);
        _exit(127);
    }
    close(p[1]);
    fp = fdopen(p[0], "r");
    if (!fp) {
        close(p[0]);
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
            ;
        return -1;
    }
    while (getline(&line, &cap, fp) > 0) {
        if (parse_record(L, line) < 0) {
            rc = -1;
            break;
        }
    }
    free(line);
    fclose(fp);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
            ;
    if (rc != 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        lyrics_free(L);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* timing queries                                                      */
/* ------------------------------------------------------------------ */

/*
 * Index of the active line at pos, by binary search.
 * Deliberate choice: when playback is before the first timestamp we
 * clamp to line 0 (show the upcoming first line) rather than showing
 * nothing.  Callers must check L->n > 0 && L->synced first.
 */
static size_t current_lyric(const Lyrics *l, double pos)
{
    size_t lo = 0, hi = l->n;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (l->v[m].t <= pos)
            lo = m + 1;
        else
            hi = m;
    }
    return lo == 0 ? 0 : lo - 1;
}

/*
 * Index of the active word within the active line.  If the line is
 * active but precedes every word timestamp, word 0 is shown.
 */
static size_t current_word(const Line *ln, double pos)
{
    size_t lo = 0, hi = ln->nw;
    while (lo < hi) {
        size_t m = lo + (hi - lo) / 2;
        if (ln->w[m].t <= pos)
            lo = m + 1;
        else
            hi = m;
    }
    return lo == 0 ? 0 : lo - 1;
}

/* Show the note glyph before the first lyric timestamp and during any
 * gap at least NOTE_DISPLAY_DELAY seconds after the active line.  This
 * follows playback time itself, so the note remains stable while paused. */
static int should_show_notes(const Lyrics *l, double pos)
{
    size_t line;

    if (!l->synced || l->n == 0)
        return 0;
    if (pos < l->v[0].t)
        return 1;

    line = current_lyric(l, pos);
    return pos - l->v[line].t >= NOTE_DISPLAY_DELAY;
}

/*
 * Locate word wi's text inside the line text by a sequential scan and
 * return its offset and bold length (trailing whitespace excluded from
 * the bold span).  Words are ordered substrings of the line text, so
 * the scan always matches.
 */
static size_t word_offset(const Line *ln, size_t wi, size_t *bold_len)
{
    const char *cur = ln->text;
    const char *end = ln->text + strlen(ln->text);

    for (size_t i = 0; i <= wi && i < ln->nw; i++) {
        size_t wl = strlen(ln->w[i].text);

        /* Search only inside the remaining line.  This also handles
         * repeated words correctly because the cursor only moves forward. */
        while (cur + wl <= end && strncmp(cur, ln->w[i].text, wl) != 0)
            cur++;

        if (cur + wl > end) {
            *bold_len = 0;
            return 0;
        }

        if (i == wi) {
            *bold_len = wl;
            return (size_t)(cur - ln->text);
        }
        cur += wl;
    }

    *bold_len = 0;
    return 0;
}

/*
 * Find the complete source word containing timing chunk wi.  W records
 * may represent syllables or other smaller chunks rather than complete
 * orthographic words.  The lyric line itself remains authoritative for
 * word boundaries: after locating the W text, expand to the surrounding
 * non-whitespace span.
 */
static size_t source_word_span(const Line *ln, size_t wi, size_t *word_len)
{
    size_t chunk_len;
    size_t off = word_offset(ln, wi, &chunk_len);
    const char *text;
    size_t len;

    if (chunk_len == 0) {
        *word_len = 0;
        return 0;
    }

    text = ln->text;
    len = strlen(text);

    while (off > 0 && !isspace((unsigned char)text[off - 1]))
        off--;

    size_t end = off;
    while (end < len && !isspace((unsigned char)text[end]))
        end++;

    *word_len = end - off;
    return off;
}

/* ------------------------------------------------------------------ */
/* rendering                                                           */

/*
 * The halfblock frontend is deliberately mode-3-only.  The W timestamp
 * selects a timing chunk, but the lyric source determines the complete
 * orthographic word shown on screen.
 *
 * The word is drawn from a tiny monochrome bitmap font.  Two vertical
 * bitmap rows share one terminal cell through the Unicode half blocks:
 *   top only -> U+2580 (▀)
 *   bottom only -> U+2584 (▄)
 *   both -> U+2588 (█)
 *
 * There is no color layer or extra visualizer state: the terminal's normal
 * foreground/background are the only rendering attributes used.
 */

static const unsigned char font5x7[][7] = {
    /* A-Z */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    {0x0F,0x10,0x10,0x10,0x10,0x10,0x0F},
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0F,0x10,0x10,0x17,0x11,0x11,0x0F},
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},
    {0x01,0x01,0x01,0x01,0x11,0x11,0x0E},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    {0x11,0x19,0x19,0x15,0x13,0x13,0x11},
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},

    /* space, punctuation, digits, symbols */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /* . */
    {0x00,0x00,0x00,0x00,0x0C,0x0C,0x08}, /* , */
    {0x00,0x00,0x0C,0x00,0x00,0x0C,0x0C}, /* : */
    {0x04,0x04,0x04,0x04,0x00,0x04,0x04}, /* ! */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* ? */
    {0x04,0x04,0x04,0x00,0x00,0x00,0x00}, /* ' */
    {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* _ */
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, /* ( */
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, /* ) */
    {0x1C,0x10,0x10,0x10,0x10,0x10,0x1C}, /* [ */
    {0x1C,0x04,0x04,0x04,0x04,0x04,0x1C}, /* ] */
    {0x06,0x04,0x04,0x0C,0x04,0x04,0x06}, /* { */
    {0x0C,0x04,0x04,0x06,0x04,0x04,0x0C}, /* } */
    {0x01,0x02,0x04,0x08,0x10,0x00,0x00}, /* / */
    {0x10,0x08,0x04,0x02,0x01,0x00,0x00}, /* \\ */
    {0x0E,0x11,0x15,0x17,0x10,0x10,0x0F}, /* @ */
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, /* # */
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* $ */
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, /* % */
    {0x0C,0x12,0x0C,0x0D,0x12,0x12,0x0D}, /* & */
    {0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}, /* * */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* + */
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* = */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* < */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* > */
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, /* | */
    {0x00,0x00,0x12,0x15,0x08,0x00,0x00}, /* ~ */

    /* 0-9 */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    {0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},

    /* UTF-8 punctuation: … and · */
    {0x00,0x00,0x00,0x00,0x00,0x15,0x00}, /* … */
    {0x00,0x00,0x00,0x00,0x04,0x00,0x00}, /* · */
};

static int font_index(unsigned char c)
{
    if (c >= 'a' && c <= 'z')
        c = (unsigned char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return c - 'A';

    switch (c) {
    case ' ': return 26;
    case '-': return 27;
    case '.': return 28;
    case ',': return 29;
    case ':': return 30;
    case '!': return 31;
    case '?': return 32;
    case '\'': return 33;
    case '"': return 34;
    case '_': return 35;
    case '(': return 36;
    case ')': return 37;
    case '[': return 38;
    case ']': return 39;
    case '{': return 40;
    case '}': return 41;
    case '/': return 42;
    case '\\': return 43;
    case '@': return 44;
    case '#': return 45;
    case '$': return 46;
    case '%': return 47;
    case '&': return 48;
    case '*': return 49;
    case '+': return 50;
    case '=': return 51;
    case '<': return 52;
    case '>': return 53;
    case '|': return 54;
    case '~': return 55;
    default:
        if (c >= '0' && c <= '9')
            return 56 + (c - '0');
        return 32; /* '?' fallback */
    }
}

static int font_index_utf8(const unsigned char *p, int *bytes)
{
    if (p[0] == 0xE2 && p[1] == 0x80 && p[2] == 0xA6) {
        *bytes = 3;
        return 66; /* … */
    }
    if (p[0] == 0xC2 && p[1] == 0xB7) {
        *bytes = 2;
        return 67; /* · */
    }
    *bytes = 1;
    return font_index(p[0]);
}

static int word_bitmap_width(const char *s, int size)
{
    int width = 0;

    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        int bytes;
        (void)font_index_utf8(p, &bytes);
        if (width > INT_MAX - 6 * size)
            return INT_MAX;
        width += 6 * size;
        p += bytes;
    }
    return width ? width - size : 0;
}

static void halfblock_cell(int top, int bottom)
{
    if (top && bottom)
        fputs("\xE2\x96\x88", stdout); /* █ */
    else if (top)
        fputs("\xE2\x96\x80", stdout); /* ▀ */
    else if (bottom)
        fputs("\xE2\x96\x84", stdout); /* ▄ */
    else
        fputc(' ', stdout);
}

static void put_halfblock_word(const char *word, int cols, int rows, int size)
{
    int width = word_bitmap_width(word, size);
    int half_rows = 7 * size;
    int text_rows = (half_rows + 1) / 2;
    int x = (cols - width) / 2;
    int y = (rows - text_rows) / 2;

    if (y < 1)
        y = 1;
    if (x < 0)
        x = 0;

    for (int band = 0; band < text_rows; band++) {
        int row = y + band;

        if (row > rows)
            break;

        printf("\033[%d;%dH", row, x + 1);
        for (const unsigned char *p = (const unsigned char *)word; *p; ) {
            int bytes;
            int glyph = font_index_utf8(p, &bytes);
            int top_halfrow = band * 2;
            int bottom_halfrow = top_halfrow + 1;

            int top_row = top_halfrow / size;
            int bottom_row = bottom_halfrow / size;
            int top = top_halfrow < half_rows;
            int bottom = bottom_halfrow < half_rows;

            if (top)
                top = (font5x7[glyph][top_row] != 0);
            if (bottom)
                bottom = (font5x7[glyph][bottom_row] != 0);

            /* Each source pixel becomes SIZE terminal columns. */
            for (int bit = 4; bit >= 0; bit--) {
                int on_top = top && ((font5x7[glyph][top_row] >> bit) & 1);
                int on_bottom = bottom && ((font5x7[glyph][bottom_row] >> bit) & 1);

                for (int repeat = 0; repeat < size; repeat++)
                    halfblock_cell(on_top, on_bottom);
            }

            for (int repeat = 0; repeat < size; repeat++)
                fputc(' ', stdout);

            p += bytes;
        }
    }
}

static void put_centered(int y, const char *s, int cols)
{
    int x = (cols - (int)strlen(s)) / 2;
    if (x < 0)
        x = 0;
    if (y < 1)
        y = 1;
    printf("\033[%d;%dH%s", y, x + 1, s);
}

static void draw(const Lyrics *L, size_t cur_line, size_t cur_word,
                 const char *status, int notes_visible,
                 int cols, int rows, int size)
{
    int y = rows / 2;

    if (y < 1)
        y = 1;

    fputs("\033[2J\033[H", stdout);

    if (status) {
        put_centered(y, status, cols);
    } else if (notes_visible) {
        put_centered(y, "♫", cols);
    } else if (L->synced && cur_line < L->n) {
        const Line *ln = &L->v[cur_line];

        /* Prefer the halfblock word renderer whenever this line has W
         * records.  If word timing is absent, fall back to the ordinary
         * centered line renderer.  This also makes mixed L/W lyrics
         * degrade gracefully line-by-line. */
        if (cur_word != (size_t)-1 && ln->nw > 0) {
            size_t wl;
            size_t off = source_word_span(ln, cur_word, &wl);

            if (wl > 0) {
                char word[256];
                size_t n = wl < sizeof word - 1 ? wl : sizeof word - 1;
                memcpy(word, ln->text + off, n);
                word[n] = '\0';
                put_halfblock_word(word, cols, rows, size);
            } else {
                put_centered(y, ln->text, cols);
            }
        } else {
            put_centered(y, ln->text, cols);
        }
    } else if (!L->synced && L->n > 0) {
        /* Preserve the reference frontend's behavior for unsynchronized
         * lyrics: show the complete lyric block as ordinary centered text. */
        int start = y - (int)L->n / 2;
        if (start < 1)
            start = 1;
        for (size_t i = 0; i < L->n; i++) {
            if (start + (int)i > rows)
                break;
            put_centered(start + (int)i, L->v[i].text, cols);
        }
    }

    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* main                                                                */

enum { ST_OK, ST_NOTRACK, ST_NOLYRICS };

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [-s SIZE]\n", argv0);
}

int main(int argc, char **argv)
{
    CmusState cmus;
    PlaybackClock clock = {0};
    int size = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            char *end;
            long v;

            if (++i >= argc) {
                usage(argv[0]);
                return 2;
            }
            errno = 0;
            v = strtol(argv[i], &end, 10);
            if (errno || *argv[i] == '\0' || *end != '\0' || v < 1 || v > 64) {
                fprintf(stderr, "ricoded-halfblock: invalid size: %s\n", argv[i]);
                return 2;
            }
            size = (int)v;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    Lyrics lyrics;
    char previous_file[sizeof cmus.file];
    size_t cur_line = 0, cur_word = (size_t)-1;
    int state = ST_NOTRACK;
    int redraw = 1;
    int notes_visible = 0;
    int cols = 80, rows = 24;

    memset(&lyrics, 0, sizeof lyrics);
    previous_file[0] = '\0';

    sig_setup(SIGINT, on_signal);
    sig_setup(SIGTERM, on_signal);
    sig_setup(SIGHUP, on_signal);
    sig_setup(SIGWINCH, on_winch);

    if (terminal_init() < 0) {
        fprintf(stderr, "ricoded-halfblock: terminal init failed: %s\n",
                strerror(errno));
        return 1;
    }

    while (running) {
        /* mode 3 is the only mode in this frontend; q is the only key. */
        char kb[64];
        ssize_t kn = read(STDIN_FILENO, kb, sizeof kb);
        for (ssize_t i = 0; i < kn; i++) {
            if (kb[i] == 'q')
                running = 0;
        }
        if (!running)
            break;

        if (cmus_query(&cmus) < 0) {
            clock.valid = 0;
            notes_visible = 0;
            if (state != ST_NOTRACK) {
                state = ST_NOTRACK;
                redraw = 1;
            }
        } else if (strcmp(previous_file, cmus.file) != 0) {
            size_t plen = strlen(cmus.file);

            if (plen < sizeof previous_file) {
                memcpy(previous_file, cmus.file, plen + 1);
                clock.valid = 0;
                cur_line = 0;
                cur_word = (size_t)-1;
                notes_visible = 0;
                if (engine_load(&lyrics) < 0 || lyrics.n == 0)
                    state = ST_NOLYRICS;
                else
                    state = ST_OK;
            } else {
                state = ST_NOLYRICS;
            }
            redraw = 1;
        } else if (state == ST_OK) {
            double position = playback_position(&clock, &cmus);
            size_t nl = current_lyric(&lyrics, position);
            size_t nw = (size_t)-1;

            if (lyrics.synced && nl < lyrics.n && lyrics.v[nl].nw > 0)
                nw = current_word(&lyrics.v[nl], position);

            if (nw != cur_word) {
                cur_word = nw;
                redraw = 1;
            }
            if (nl != cur_line) {
                cur_line = nl;
                redraw = 1;
            }

            {
                int show_notes = should_show_notes(&lyrics, position);
                if (show_notes != notes_visible) {
                    notes_visible = show_notes;
                    redraw = 1;
                }
            }
        }

        if (resized) {
            resized = 0;
            redraw = 1;
        }

        if (redraw) {
            terminal_size(&cols, &rows);

            if (state == ST_NOTRACK) {
                draw(&lyrics, 0, (size_t)-1,
                     "cmus: no track", 0, cols, rows, size);
            } else if (state == ST_NOLYRICS) {
                draw(&lyrics, 0, (size_t)-1,
                     "no embedded lyrics", 0, cols, rows, size);
            } else {
                double position = playback_position(&clock, &cmus);

                if (lyrics.synced) {
                    cur_line = current_lyric(&lyrics, position);
                    notes_visible = should_show_notes(&lyrics, position);
                    if (cur_line < lyrics.n && lyrics.v[cur_line].nw > 0)
                        cur_word = current_word(&lyrics.v[cur_line], position);
                    else
                        cur_word = (size_t)-1;
                } else {
                    cur_word = (size_t)-1;
                    notes_visible = 0;
                }

                draw(&lyrics, cur_line, cur_word,
                     NULL, notes_visible, cols, rows, size);
            }
            redraw = 0;
        }

        {
            struct timespec ts = { 0, POLL_NS };
            nanosleep(&ts, NULL);
        }
    }

    lyrics_free(&lyrics);
    cleanup_terminal();
    return 0;
}
