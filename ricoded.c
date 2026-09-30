/*
 * ricoded - the reference ricoded frontend ("distro").
 *
 * Polls cmus, runs ricoded-ng once per song, parses the L/W record
 * protocol, and renders lyrics in one of three modes:
 *
 *   1  line mode        - the complete current line
 *   2  word mode        - the complete line, active word in bold
 *   3  current-word     - only the active word
 *
 * Controls: 1, 2, 3 switch mode; q quits.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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
#define NOTE_DISPLAY_DELAY 5.0 /* show notes after 5 seconds without a new line */

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
 * five-second-or-longer gap after a timestamped line.  This is based on
 * playback time itself, so the note remains stable while paused. */
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

/* ------------------------------------------------------------------ */
/* rendering                                                           */
/* ------------------------------------------------------------------ */

static void put_centered(int y, const char *s, int cols)
{
    int x = (cols - (int)strlen(s)) / 2;   /* byte-based; see README */
    if (x < 0)
        x = 0;
    if (y < 1)
        y = 1;
    printf("\033[%d;%dH%s", y, x + 1, s);
}

static void draw(const Lyrics *L, size_t cur_line, size_t cur_word,
                 int mode, const char *status, int cols, int rows)
{
    int y = rows / 2;
    if (y < 3)
        y = 3;

    fputs("\033[2J\033[H", stdout);

    if (status) {
        put_centered(y, status, cols);
    } else if (!L->synced && L->n > 0) {
        /* unsynchronized lyrics: no timing to follow, show all lines */
        int start = y - (int)L->n / 2;
        if (start < 2)
            start = 2;
        for (size_t i = 0; i < L->n; i++) {
            if (start + (int)i > rows)
                break;
            put_centered(start + (int)i, L->v[i].text, cols);
        }
    } else if (mode == 3 && cur_word != (size_t)-1) {
        const Line *ln = &L->v[cur_line];
        const char *wt = ln->w[cur_word].text;
        while (isspace((unsigned char)*wt))
            wt++;
        size_t wl = strlen(wt);
        while (wl > 0 && isspace((unsigned char)wt[wl - 1]))
            wl--;
        int x = (cols - (int)wl) / 2;
        if (x < 0)
            x = 0;
        printf("\033[%d;%dH%.*s", y, x + 1, (int)wl, wt);
    } else if (mode == 2 && cur_word != (size_t)-1) {
        const Line *ln = &L->v[cur_line];
        size_t bl;
        size_t off = word_offset(ln, cur_word, &bl);
        if (bl == 0) {
            put_centered(y, ln->text, cols);
        } else {
            int x = (cols - (int)strlen(ln->text)) / 2;
            if (x < 0)
                x = 0;
            printf("\033[%d;%dH%.*s\033[1m%.*s\033[0m%s",
                   y, x + 1,
                   (int)off, ln->text,
                   (int)bl, ln->text + off,
                   ln->text + off + bl);
        }
    } else {
        put_centered(y, L->v[cur_line].text, cols);
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

enum { ST_OK, ST_NOTRACK, ST_NOLYRICS };

int main(void)
{
    CmusState cmus;
    PlaybackClock clock = {0};
    Lyrics lyrics;
    char previous_file[sizeof cmus.file];
    size_t cur_line = 0, cur_word = (size_t)-1;
    int mode = 1;
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
        fprintf(stderr, "ricoded: terminal init failed: %s\n", strerror(errno));
        return 1;
    }

    while (running) {
        /* input: mode keys + quit */
        char kb[64];
        ssize_t kn = read(STDIN_FILENO, kb, sizeof kb);
        for (ssize_t i = 0; i < kn; i++) {
            if (kb[i] == 'q')
                running = 0;
            else if (kb[i] >= '1' && kb[i] <= '3') {
                int m = kb[i] - '0';
                if (m != mode) {
                    mode = m;
                    redraw = 1;
                }
            }
        }
        if (!running)
            break;

        if (cmus_query(&cmus) < 0) {
            clock.valid = 0;
            if (state != ST_NOTRACK) {
                state = ST_NOTRACK;
                redraw = 1;
            }
        } else if (strcmp(previous_file, cmus.file) != 0) {
            /* new song: run ricoded-ng exactly once, cache the result */
            size_t plen = strlen(cmus.file);
            if (plen < sizeof previous_file) {
                memcpy(previous_file, cmus.file, plen + 1);
                clock.valid = 0;
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
            /* same song: follow a high-resolution local playback clock */
            double position = playback_position(&clock, &cmus);
            size_t nl = current_lyric(&lyrics, position);
            size_t nw = (size_t)-1;

            if (mode >= 2 && lyrics.v[nl].nw > 0)
                nw = current_word(&lyrics.v[nl], position);
            if (nw != cur_word) {
                cur_word = nw;
                if (mode >= 2)
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
                draw(&lyrics, 0, (size_t)-1, mode, "cmus: no track", cols, rows);
            } else if (state == ST_NOLYRICS) {
                draw(&lyrics, 0, (size_t)-1, mode, "no embedded lyrics", cols, rows);
            } else {
                double position = playback_position(&clock, &cmus);

                if (lyrics.synced) {
                    cur_line = current_lyric(&lyrics, position);
                    notes_visible = should_show_notes(&lyrics, position);
                } else {
                    notes_visible = 0;
                }

                if (mode >= 2 && lyrics.v[cur_line].nw > 0)
                    cur_word = current_word(&lyrics.v[cur_line], position);
                else
                    cur_word = (size_t)-1;
                draw(&lyrics, cur_line, cur_word, mode,
                     notes_visible ? "♫" : NULL, cols, rows);
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
