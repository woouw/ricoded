/*
 * ricoded-ng - the ricoded lyric engine.
 *
 * Queries cmus for the currently playing file, reads embedded lyrics
 * from FLAC Vorbis Comments (no external libraries: the FLAC container
 * and Vorbis Comment framing are parsed here), understands LRC line
 * timestamps and word-level <timestamp> markers, and prints a
 * machine-readable record protocol on stdout.  All diagnostics go to
 * stderr.
 *
 * Output protocol (one record per line, TAB-separated, documented in
 * README.md):
 *
 *   L<TAB>line-index<TAB>line-time<TAB>line-text
 *   W<TAB>line-index<TAB>word-time<TAB>word-text
 *
 * A line-time < 0 marks an unsynchronized lyric line.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* cmus                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char *file;     /* malloc'ed current file path, NULL if none */
} Info;

/*
 * Run "cmus-remote -Q" with fork/pipe/exec (never a shell) and copy
 * out the file path.  Returns 0 if cmus answered with a file, -1
 * otherwise (cmus not running, no track, or a too-long path).
 */
static int cmus_query(Info *in)
{
    int p[2];
    pid_t pid;
    FILE *fp;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int st;
    int rc = -1;

    memset(in, 0, sizeof *in);

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

    while ((n = getline(&line, &cap, fp)) > 0) {
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (strncmp(line, "file ", 5) == 0) {
            size_t plen = strlen(line + 5);
            free(in->file);
            in->file = NULL;
            /* PATH_MAX is not portable to test; reject absurd paths. */
            if (plen > 0 && plen < 4096) {
                in->file = strdup(line + 5);
                if (in->file)
                    rc = 0;
            }
        }
    }
    free(line);
    fclose(fp);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        free(in->file);
        in->file = NULL;
        return -1;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Lyrics data model                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    double t;        /* word start, seconds */
    char  *text;     /* word text incl. trailing separator space */
} Word;

typedef struct {
    double  t;       /* line start, seconds; < 0 => unsynchronized */
    char   *text;    /* line text with all <...> markers stripped  */
    Word   *w;
    size_t  nw, wcap;
} Line;

typedef struct {
    Line  *v;
    size_t n, cap;
    bool   synced;   /* at least one line carries a real timestamp */
} Lyrics;

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

/* Explicit-capacity growth; on failure the array is left unchanged. */
static int lyrics_reserve(Lyrics *L, size_t need)
{
    if (need <= L->cap)
        return 0;
    size_t ncap = L->cap ? L->cap : 16;
    while (ncap < need)
        ncap *= 2;
    Line *nv = realloc(L->v, ncap * sizeof *nv);
    if (!nv)
        return -1;
    memset(nv + L->cap, 0, (ncap - L->cap) * sizeof *nv);
    L->v = nv;
    L->cap = ncap;
    return 0;
}

/*
 * Append a line.  The text is copied; on any allocation failure the
 * lyric count is NOT incremented and -1 is returned, so no partially
 * initialized entry ever becomes visible to the caller.
 */
static int lyrics_push(Lyrics *L, double t, const char *text)
{
    if (lyrics_reserve(L, L->n + 1) < 0)
        return -1;
    Line *ln = &L->v[L->n];
    ln->text = strdup(text);
    if (!ln->text)
        return -1;
    ln->t = t;
    L->n++;
    return 0;
}

static int word_push(Line *ln, double t, const char *text, size_t len)
{
    if (ln->nw == ln->wcap) {
        size_t ncap = ln->wcap ? ln->wcap * 2 : 8;
        Word *nw = realloc(ln->w, ncap * sizeof *nw);
        if (!nw)
            return -1;
        ln->w = nw;
        ln->wcap = ncap;
    }
    ln->w[ln->nw].text = strndup(text, len);
    if (!ln->w[ln->nw].text)
        return -1;
    ln->w[ln->nw].t = t;
    ln->nw++;
    return 0;
}

/* Clone a parsed line (text + words) under a new timestamp. */
static int lyrics_clone(Lyrics *L, double t, const Line *src)
{
    if (lyrics_push(L, t, src->text) < 0)
        return -1;
    Line *dst = &L->v[L->n - 1];
    for (size_t i = 0; i < src->nw; i++) {
        size_t wl = strlen(src->w[i].text);
        if (word_push(dst, src->w[i].t, src->w[i].text, wl) < 0) {
            /* remove the half-built clone */
            L->n--;
            line_free(dst);
            return -1;
        }
    }
    return 0;
}

static int line_cmp(const void *a, const void *b)
{
    double x = ((const Line *)a)->t, y = ((const Line *)b)->t;
    return (x > y) - (x < y);
}

/* ------------------------------------------------------------------ */
/* LRC parsing                                                         */
/* ------------------------------------------------------------------ */

/*
 * Strict timestamp: mm:ss or mm:ss.f, with 1-3 fraction digits.
 * Anything else (including the ambiguous mm:ss:xx form and fractions
 * with more than three digits) is rejected.
 */
static bool parse_ts(const char *s, size_t len, double *out)
{
    size_t i = 0, nd;
    unsigned long mm = 0, ss = 0, frac = 0;

    nd = 0;
    while (i < len && isdigit((unsigned char)s[i])) {
        mm = mm * 10 + (unsigned long)(s[i] - '0');
        nd++;
        i++;
    }
    if (!nd || mm > 999 || i >= len || s[i] != ':')
        return false;
    i++;
    nd = 0;
    while (i < len && isdigit((unsigned char)s[i])) {
        ss = ss * 10 + (unsigned long)(s[i] - '0');
        nd++;
        i++;
    }
    if (!nd || ss > 59)
        return false;
    double t = (double)mm * 60.0 + (double)ss;
    if (i < len) {
        if (s[i] != '.')
            return false;
        i++;
        nd = 0;
        while (i < len && isdigit((unsigned char)s[i])) {
            if (nd < 3)
                frac = frac * 10 + (unsigned long)(s[i] - '0');
            nd++;
            i++;
        }
        if (nd == 0 || nd > 3)
            return false;
        t += (double)frac / (nd == 1 ? 10.0 : nd == 2 ? 100.0 : 1000.0);
    }
    *out = t;
    return true;
}

/*
 * Strip <mm:ss.xx> word markers from raw into the line's clean text
 * and record each word's start time.  Word text runs from after its
 * marker to the next marker or to (and including) the first following
 * whitespace, so "I pull up in that ..." yields words "I ", "pull ",
 * "up " and leaves "in that ..." as untimed line text.  A '<' that is
 * not a valid marker is copied literally.  Returns 0 or -1 (OOM).
 */
static int line_set_text(Line *ln, const char *raw)
{
    size_t rlen = strlen(raw);
    char *clean = malloc(rlen + 1);
    if (!clean)
        return -1;

    size_t c = 0, i = 0;
    int rc = 0;
    while (i < rlen && rc == 0) {
        if (raw[i] == '<') {
            size_t j = i + 1;
            double t;
            while (j < rlen && raw[j] != '>')
                j++;
            if (j < rlen && parse_ts(raw + i + 1, j - i - 1, &t)) {
                i = j + 1;

                /* A marker belongs to the word that follows it.  Ignore
                 * separators before that word; keep separators in the clean
                 * line text so rendering still preserves the original text. */
                while (i < rlen && isspace((unsigned char)raw[i]))
                    i++;

                size_t wstart = c;
                while (i < rlen) {
                    if (raw[i] == '<') {
                        size_t k = i + 1;
                        double t2;
                        while (k < rlen && raw[k] != '>')
                            k++;
                        if (k < rlen && parse_ts(raw + i + 1, k - i - 1, &t2))
                            break;
                    }
                    if (isspace((unsigned char)raw[i]))
                        break;
                    clean[c++] = raw[i++];
                }

                if (c > wstart)
                    rc = word_push(ln, t, clean + wstart, c - wstart);
                continue;
            }
            /* not a valid marker: fall through, copy '<' literally */
        }
        clean[c++] = raw[i++];
    }
    clean[c] = '\0';
    ln->text = clean;   /* owned by the line either way */
    return rc;
}

/*
 * Parse LRC text into L.  Returns 1 if at least one timestamped line
 * was found, 0 if the text contains no timestamps, -1 on error.
 */
static int parse_lrc(const char *buf0, Lyrics *L)
{
    const char *buf = buf0;
    if ((unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF)
        buf += 3;   /* skip UTF-8 BOM */

    char *work = strdup(buf);
    if (!work)
        return -1;

    int rc = 0;
    char *save = NULL;
    for (char *s = strtok_r(work, "\n", &save); s && rc == 0;
         s = strtok_r(NULL, "\n", &save)) {
        size_t len = strlen(s);
        while (len && (s[len - 1] == '\r' || s[len - 1] == '\n'))
            s[--len] = '\0';

        char *p = s;
        double ts[8];
        int nt = 0;
        /* gather all leading [mm:ss.xx] timestamps */
        while (*p == '[' && nt < 8) {
            char *rb = strchr(p, ']');
            if (!rb || rb - p - 1 < 1)
                break;
            if (!parse_ts(p + 1, (size_t)(rb - p - 1), &ts[nt]))
                break;  /* metadata tag like [ti:..] ends the run */
            nt++;
            p = rb + 1;
        }
        if (!nt)
            continue;
        while (*p == ' ' || *p == '\t')
            p++;

        /* parse text + words once, then clone per extra timestamp */
        Line proto;
        memset(&proto, 0, sizeof proto);
        if (line_set_text(&proto, p) < 0) {
            line_free(&proto);
            rc = -1;
            break;
        }
        size_t first = L->n;    /* index of this line's first copy */
        if (lyrics_reserve(L, L->n + 1) < 0) {
            line_free(&proto);
            rc = -1;
            break;
        }
        L->v[L->n] = proto;     /* move proto into the array */
        L->v[L->n].t = ts[0];
        L->n++;
        for (int i = 1; i < nt; i++) {
            if (lyrics_clone(L, ts[i], &L->v[first]) < 0) {
                rc = -1;
                break;
            }
        }
        /* proto was moved into the array; nothing to free here */
    }
    free(work);

    if (rc == 0) {
        if (L->n == 0)
            return 0;
        qsort(L->v, L->n, sizeof *L->v, line_cmp);
        L->synced = true;
        return 1;
    }
    return -1;
}

/*
 * Unsynchronized text: one line per entry, t = -1, order preserved
 * (no sort).  Returns 1 if any line was stored, 0 for empty input,
 * -1 on error.
 */
static int parse_unsynced(const char *buf, Lyrics *L)
{
    char *work = strdup(buf);
    if (!work)
        return -1;
    int rc = 1;
    char *save = NULL;
    for (char *s = strtok_r(work, "\n", &save); s;
         s = strtok_r(NULL, "\n", &save)) {
        size_t len = strlen(s);
        while (len && (s[len - 1] == '\r' || s[len - 1] == '\n'))
            s[--len] = '\0';
        if (lyrics_push(L, -1.0, s) < 0) {
            rc = -1;
            break;
        }
    }
    free(work);
    if (rc == 1 && L->n == 0)
        rc = 0;
    L->synced = false;
    return rc;
}

/* ------------------------------------------------------------------ */
/* FLAC Vorbis Comments (native, no libFLAC)                           */
/* ------------------------------------------------------------------ */

/*
 * FLAC metadata block header: 1 bit last-flag, 7 bits type, 24 bits
 * big-endian length.  Vorbis Comment block (type 4) payloads use
 * little-endian 32-bit length fields.
 */

static uint32_t le32(const unsigned char *p)
{
    return ((uint32_t)p[0])       |
           ((uint32_t)p[1] << 8)  |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t be24(const unsigned char *p)
{
    return ((uint32_t)p[0] << 16) |
           ((uint32_t)p[1] << 8)  |
           (uint32_t)p[2];
}

static char *read_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > (64UL << 20)) { fclose(f); return NULL; }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(buf); return NULL; }
    buf[r] = '\0';
    if (size_out)
        *size_out = r;
    return buf;
}

static bool key_is(const char *comment, size_t len, const char *key)
{
    size_t klen = strlen(key);
    if (len <= klen || comment[klen] != '=')
        return false;
    return strncasecmp(comment, key, klen) == 0;
}

static char *vc_value(const char *comment, uint32_t clen, size_t keylen)
{
    size_t vlen = clen - keylen - 1;
    char *v = malloc(vlen + 1);
    if (v) {
        memcpy(v, comment + keylen + 1, vlen);
        v[vlen] = '\0';
    }
    return v;
}

enum {
    LYR_OK = 0,
    LYR_IOERR,        /* unreadable / not a readable file */
    LYR_NOT_FLAC,     /* bad magic */
    LYR_NO_COMMENTS,  /* no Vorbis Comment block */
    LYR_NO_LYRICS     /* comments present, none of the lyric keys */
};

/*
 * Extract the best embedded lyric comment.  Priority is independent
 * of the order comments appear in the file:
 *
 *   SYNCEDLYRICS > LYRICS > UNSYNCEDLYRICS
 *
 * On LYR_OK *out receives a malloc'ed copy (caller frees).
 */
static int flac_lyrics(const char *path, char **out)
{
    size_t size;
    unsigned char *buf = (unsigned char *)read_file(path, &size);
    if (!buf)
        return LYR_IOERR;
    if (size < 4 || memcmp(buf, "fLaC", 4) != 0) {
        free(buf);
        return LYR_NOT_FLAC;
    }

    char *synced = NULL, *lyr = NULL, *unsynced = NULL;
    bool saw_vc = false;
    size_t off = 4;

    while (off + 4 <= size) {
        unsigned char h = buf[off];
        bool last = (h & 0x80) != 0;
        unsigned type = h & 0x7f;
        uint32_t len = be24(buf + off + 1);
        off += 4;
        if ((size_t)len > size - off)
            break;

        if (type == 4) {                        /* VORBIS_COMMENT */
            if (len >= 8)
                saw_vc = true;
            const unsigned char *p = buf + off;
            const unsigned char *end = p + len;
            if ((size_t)(end - p) >= 4) {
                uint32_t vendor_len = le32(p);
                p += 4;
                if (vendor_len <= (uint32_t)(end - p)) {
                    p += vendor_len;
                    if ((size_t)(end - p) >= 4) {
                        uint32_t count = le32(p);
                        p += 4;
                        for (uint32_t i = 0; i < count; i++) {
                            if ((size_t)(end - p) < 4)
                                break;
                            uint32_t clen = le32(p);
                            p += 4;
                            if (clen > (uint32_t)(end - p))
                                break;
                            const char *comment = (const char *)p;
                            if (!synced && key_is(comment, clen, "SYNCEDLYRICS"))
                                synced = vc_value(comment, clen, strlen("SYNCEDLYRICS"));
                            else if (!lyr && key_is(comment, clen, "LYRICS"))
                                lyr = vc_value(comment, clen, strlen("LYRICS"));
                            else if (!unsynced && key_is(comment, clen, "UNSYNCEDLYRICS"))
                                unsynced = vc_value(comment, clen, strlen("UNSYNCEDLYRICS"));
                            p += clen;
                        }
                    }
                }
            }
        }
        off += len;
        if (last)
            break;
    }
    free(buf);

    char *best;
    if (synced) {
        best = synced;
        free(lyr);
        free(unsynced);
    } else if (lyr) {
        best = lyr;
        free(unsynced);
    } else {
        best = unsynced;
    }
    if (!best)
        return saw_vc ? LYR_NO_LYRICS : LYR_NO_COMMENTS;
    *out = best;
    return LYR_OK;
}

/* ------------------------------------------------------------------ */
/* Output protocol                                                     */
/* ------------------------------------------------------------------ */

static void print_lyrics(const Lyrics *L)
{
    for (size_t i = 0; i < L->n; i++) {
        const Line *ln = &L->v[i];
        printf("L\t%zu\t%.3f\t%s\n", i, ln->t, ln->text);
        for (size_t w = 0; w < ln->nw; w++)
            printf("W\t%zu\t%.3f\t%s\n", i, ln->w[w].t, ln->w[w].text);
    }
}

int main(void)
{
    Info in;
    if (cmus_query(&in) < 0 || !in.file) {
        fprintf(stderr, "ricoded-ng: cmus: no track\n");
        return 1;
    }

    char *text = NULL;
    int fr = flac_lyrics(in.file, &text);
    if (fr != LYR_OK) {
        const char *what;
        switch (fr) {
        case LYR_IOERR:       what = "unable to read FLAC";        break;
        case LYR_NOT_FLAC:    what = "not a FLAC file";            break;
        case LYR_NO_COMMENTS: what = "no Vorbis Comments";         break;
        default:              what = "no embedded lyrics";         break;
        }
        fprintf(stderr, "ricoded-ng: %s: %s\n", what, in.file);
        free(in.file);
        return 1;
    }
    free(in.file);

    Lyrics L;
    memset(&L, 0, sizeof L);
    int pr = parse_lrc(text, &L);
    if (pr == 0)
        pr = parse_unsynced(text, &L);   /* LYRICS may be plain text */
    free(text);

    if (pr <= 0) {
        fprintf(stderr, "ricoded-ng: unable to parse lyrics\n");
        lyrics_free(&L);
        return 1;
    }

    print_lyrics(&L);
    lyrics_free(&L);
    return 0;
}
