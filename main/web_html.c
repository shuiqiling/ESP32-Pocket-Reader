/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#include "web_html.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static bool is_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_digit_c(unsigned char c)
{
    return c >= '0' && c <= '9';
}

/* Case-insensitive bounded search, like strstr but with an explicit len. */
static const char *find_ci(const char *hay, size_t hay_len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay_len < nlen) {
        return NULL;
    }
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        while (j < nlen && tolower((unsigned char)hay[i + j]) ==
               tolower((unsigned char)needle[j])) {
            j++;
        }
        if (j == nlen) {
            return hay + i;
        }
    }
    return NULL;
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && is_space((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
    char *p = s;
    while (*p && is_space((unsigned char)*p)) {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
}

/* ------------------------------------------------------------------ */
/* text writer used by the HTML-to-text state machine                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool last_nl;   /* last emitted character is a newline            */
    bool any;       /* at least one non-space/non-newline char seen   */
    bool truncated; /* output was longer than the caller's buffer     */
} text_writer_t;

static void w_init(text_writer_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->last_nl = false;
    w->any = false;
    w->truncated = false;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

static void w_nl(text_writer_t *w)
{
    if (w->cap == 0 || w->len + 1 >= w->cap) {
        w->truncated = true;
        return;
    }
    if (w->last_nl) {
        return;
    }
    if (w->len == 0) {
        return;                 /* avoid leading blank lines */
    }
    w->buf[w->len++] = '\n';
    w->buf[w->len] = '\0';
    w->last_nl = true;
}

static void w_char(text_writer_t *w, char c)
{
    if (w->cap == 0 || w->len + 1 >= w->cap) {
        w->truncated = true;
        return;
    }
    if (c == '\n' || c == '\r') {
        w_nl(w);
        return;
    }
    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
    w->last_nl = false;
    if (!is_space((unsigned char)c)) {
        w->any = true;
    }
}

static void w_utf8(text_writer_t *w, unsigned long cp)
{
    if (cp <= 0x7f) {
        w_char(w, (char)cp);
    } else if (cp <= 0x7ff) {
        w_char(w, (char)(0xc0 | (cp >> 6)));
        w_char(w, (char)(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        w_char(w, (char)(0xe0 | (cp >> 12)));
        w_char(w, (char)(0x80 | ((cp >> 6) & 0x3f)));
        w_char(w, (char)(0x80 | (cp & 0x3f)));
    } else if (cp <= 0x10ffff) {
        w_char(w, (char)(0xf0 | (cp >> 18)));
        w_char(w, (char)(0x80 | ((cp >> 12) & 0x3f)));
        w_char(w, (char)(0x80 | ((cp >> 6) & 0x3f)));
        w_char(w, (char)(0x80 | (cp & 0x3f)));
    }
}

/* Decode the entity that starts at p (which points to '&'). Returns number
 * of source bytes consumed; writes the decoded representation. */
static size_t w_entity(text_writer_t *w, const char *p, const char *end)
{
    size_t max = (size_t)(end - p);
    static const struct {
        const char *name;
        unsigned long cp;
    } named[] = {
        { "nbsp;", 0x20 },   { "amp;", '&' },   { "lt;", '<' },
        { "gt;", '>' },      { "quot;", '"' },  { "apos;", '\'' },
        { "mdash;", 0x2014 },{ "ndash;", 0x2013},
        { "hellip;", 0x2026 },{ "lsquo;", 0x2018},{ "rsquo;", 0x2019},
        { "ldquo;", 0x201c },{ "rdquo;", 0x201d},{ "middot;", 0x00b7},
    };
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        size_t need = strlen(named[i].name);
        if (max >= need + 1 &&
            strncmp(p + 1, named[i].name, need - 1) == 0 &&
            named[i].name[need - 1] == ';') {
            w_utf8(w, named[i].cp);
            return need + 1; /* leading '&' plus the complete named entity */
        }
    }

    if (max > 2 && p[1] == '#') {
        const char *q = p + 2;
        unsigned long cp = 0;
        bool hex = false;
        if (q < end && (*q == 'x' || *q == 'X')) {
            hex = true;
            q++;
        }
        const char *dig = q;
        while (q < end && q - dig <= 6) {
            unsigned v;
            if (*q >= '0' && *q <= '9') {
                v = (unsigned)(*q - '0');
            } else if (hex && *q >= 'a' && *q <= 'f') {
                v = (unsigned)(*q - 'a' + 10);
            } else if (hex && *q >= 'A' && *q <= 'F') {
                v = (unsigned)(*q - 'A' + 10);
            } else {
                break;
            }
            cp = cp * (hex ? 16 : 10) + v;
            q++;
        }
        if (q > dig && q < end && *q == ';' && cp <= 0x10ffff) {
            w_utf8(w, cp);
            return (size_t)(q - p) + 1;
        }
    }

    /* Unknown entity: emit it verbatim so we do not silently drop text. */
    size_t n = 0;
    while (p + n < end && p[n] != ';' && n < 32) {
        w_char(w, p[n]);
        n++;
    }
    if (p + n < end && p[n] == ';') {
        w_char(w, ';');
        n++;
    }
    return n ? n : 1;
}

/* ------------------------------------------------------------------ */
/* HTML-to-text core (used on the #nr1 region)                         */
/* ------------------------------------------------------------------ */

static bool tag_name_is(const char *tag, size_t len, const char *name)
{
    size_t nlen = strlen(name);
    if (len != nlen) {
        return false;
    }
    for (size_t i = 0; i < nlen; i++) {
        if (tolower((unsigned char)tag[i]) != tolower((unsigned char)name[i])) {
            return false;
        }
    }
    return true;
}


static bool html_to_text(const char *s, size_t len, char *out, size_t cap)
{
    text_writer_t w;
    w_init(&w, out, cap);

    const char *p = s;
    const char *end = s + len;
    bool in_script = false;
    bool in_style = false;

    while (p < end) {
        if (*p != '<') {
            if (!in_script && !in_style) {
                if (*p == '&') {
                    p += w_entity(&w, p, end);
                } else {
                    w_char(&w, *p);
                    p++;
                }
            } else {
                p++;
            }
            continue;
        }

        /* Tag */
        const char *q = p + 1;
        bool closing = false;
        if (q < end && *q == '/') {
            closing = true;
            q++;
        }
        const char *name = q;
        while (q < end && isalnum((unsigned char)*q)) {
            q++;
        }
        size_t name_len = (size_t)(q - name);

        /* find end of this tag */
        const char *gt = memchr(q, '>', (size_t)(end - q));
        const char *after = gt ? gt + 1 : end;

        if (!in_script && !in_style &&
            (tag_name_is(name, name_len, "script") && !closing)) {
            in_script = true;
            p = after;
            continue;
        }
        if (!in_script && !in_style &&
            (tag_name_is(name, name_len, "style") && !closing)) {
            in_style = true;
            p = after;
            continue;
        }
        if (in_script) {
            if (closing && tag_name_is(name, name_len, "script")) {
                in_script = false;
            }
            p = after;
            continue;
        }
        if (in_style) {
            if (closing && tag_name_is(name, name_len, "style")) {
                in_style = false;
            }
            p = after;
            continue;
        }

        /* Normal tags */
        if (tag_name_is(name, name_len, "br")) {
            w_nl(&w);
        } else if (closing && (tag_name_is(name, name_len, "p") ||
                               tag_name_is(name, name_len, "div") ||
                               tag_name_is(name, name_len, "li") ||
                               tag_name_is(name, name_len, "h1") ||
                               tag_name_is(name, name_len, "h2") ||
                               tag_name_is(name, name_len, "h3") ||
                               tag_name_is(name, name_len, "h4") ||
                               tag_name_is(name, name_len, "h5") ||
                               tag_name_is(name, name_len, "h6"))) {
            w_nl(&w);
        }
        /* Opening block tags are intentionally ignored here; the paragraph
         * boundary is produced by the matching closing tag. */
        p = after;
    }

    if (w.cap > 0) {
        if (w.len + 1 >= w.cap) {
            w.len = w.cap - 1;
        }
        w.buf[w.len] = '\0';
        while (w.len > 0 && w.buf[w.len - 1] == '\n') {
            w.buf[--w.len] = '\0';
        }
    }
    return !w.truncated;
}

/* A line is dropped if it is blank or contains a known ad/UI phrase. The
 * check uses only a small prefix, so no large per-line buffer is needed. */
static bool line_is_bad(const char *line, size_t len)
{
    bool has_text = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c != ' ' && c != '\t' && c != '\r') {
            has_text = true;
            break;
        }
    }
    if (!has_text) {
        return true;
    }

    char probe[192];
    size_t n = len;
    if (n >= sizeof(probe)) {
        n = sizeof(probe) - 1;
    }
    memcpy(probe, line, n);
    probe[n] = '\0';

    static const char *ads[] = {
        "请点击下一页继续阅读",
        "本章未完",
        "请翻页",
        "加入书签，方便阅读",
        "小主子，这个章节后面还有哦",
        "返回顶部",
        "手机用户请浏览",
        "m.zhaobiquge.com",
        "第(1/",
        "第(2/",
        "第(3/",
        "第(4/",
        "第(5/",
        "第(6/",
        "第(7/",
        "第(8/",
    };
    for (size_t i = 0; i < sizeof(ads) / sizeof(ads[0]); i++) {
        if (strstr(probe, ads[i])) {
            return true;
        }
    }
    return false;
}

/* In-place, single pass line filter. It never allocates a page-sized buffer. */
static void drop_ad_lines(char *text)
{
    if (!text || !*text) {
        return;
    }
    char *r = text;
    char *w = text;
    bool first = true;
    while (*r) {
        char *nl = strchr(r, '\n');
        size_t len = nl ? (size_t)(nl - r) : strlen(r);
        if (!line_is_bad(r, len)) {
            if (!first) {
                *w++ = '\n';
            }
            first = false;
            memmove(w, r, len);
            w += len;
        }
        if (!nl) {
            break;
        }
        r = nl + 1;
    }
    *w = '\0';
}

/* ------------------------------------------------------------------ */
/* public parser functions                                             */
/* ------------------------------------------------------------------ */

int web_html_parse_index(const char *html, size_t len,
                         web_html_chapter_t *out, int max)
{
    if (!html || !out || max <= 0) {
        return -1;
    }
    static const char attr[] = "href=\"";
    static const char marker[] = "/biquge/2/2031/";
    const char *p = html;
    const char *end = html + len;
    int count = 0;
    size_t mlen = sizeof(marker) - 1;
    size_t alen = sizeof(attr) - 1;

    while (p < end && count < max) {
        const char *attr_hit = find_ci(p, (size_t)(end - p), attr);
        if (!attr_hit) {
            break;
        }
        /* Make sure this href belongs to an opening <a ...> tag, not to
         * script text or ordinary body text. */
        const char *scan = attr_hit;
        while (scan > html && scan[-1] != '<' && scan[-1] != '>') {
            scan--;
        }
        if (scan == html || scan[-1] == '>') {
            p = attr_hit + 1;
            continue;
        }
        const char *tag_name_p = scan;
        if (*tag_name_p == '/') {
            tag_name_p++;
        }
        size_t tag_nlen = 0;
        while (tag_name_p + tag_nlen < end &&
               isalnum((unsigned char)tag_name_p[tag_nlen])) {
            tag_nlen++;
        }
        if (!tag_name_is(tag_name_p, tag_nlen, "a")) {
            p = attr_hit + 1;
            continue;
        }
        const char *value_start = attr_hit + alen;
        const char *quote_end = memchr(value_start, '"',
                                       (size_t)(end - value_start));
        if (!quote_end) {
            break;
        }
        /* Only real href="..." attributes are considered now. Find the
         * relative chapter path inside the href value. */
        size_t value_len = (size_t)(quote_end - value_start);
        const char *hit = find_ci(value_start, value_len, marker);
        if (!hit) {
            p = attr_hit + 1;
            continue;
        }
        const char *q = hit + mlen;
        const char *id_start = q;
        while (q < quote_end && is_digit_c((unsigned char)*q)) {
            q++;
        }
        /* Path must be marker + digits + ".html" and nothing after. */
        if (q == id_start || (size_t)(quote_end - q) != 5 ||
            strncmp(q, ".html", 5) != 0) {
            p = attr_hit + 1;
            continue;
        }

        /* Anchor text follows the closing quote of the href value. */
        const char *gt = memchr(quote_end + 1, '>',
                                (size_t)(end - (quote_end + 1)));
        if (!gt) {
            p = attr_hit + 1;
            continue;
        }
        const char *title_s = gt + 1;
        const char *close = find_ci(title_s, (size_t)(end - title_s), "</a>");
        if (!close) {
            p = attr_hit + 1;
            continue;
        }

        /* Candidate path is the exact /biquge/.../<digits>.html substring. */
        size_t path_len = (size_t)(quote_end - hit);
        bool dup = false;
        for (int i = 0; i < count; i++) {
            size_t stored = strlen(out[i].path);
            if (stored == path_len && strncmp(out[i].path, hit, path_len) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            web_html_chapter_t *c = &out[count];
            if (path_len >= sizeof(c->path)) {
                path_len = sizeof(c->path) - 1;
            }
            memcpy(c->path, hit, path_len);
            c->path[path_len] = '\0';

            size_t tlen = (size_t)(close - title_s);
            if (tlen >= sizeof(c->title)) {
                tlen = sizeof(c->title) - 1;
            }
            memcpy(c->title, title_s, tlen);
            c->title[tlen] = '\0';
            trim(c->title);
            count++;
        }
        p = close + 4;
    }
    return count;
}

int web_html_index_page_count(const char *html, size_t len)
{
    if (!html || len == 0) {
        return -1;
    }
    static const char marker[] = "/biquge/2/2031/index_";
    const char *p = html;
    const char *end = html + len;
    int highest = 1;

    while (p < end) {
        const char *hit = find_ci(p, (size_t)(end - p), marker);
        if (!hit) {
            break;
        }
        const char *q = hit + sizeof(marker) - 1;
        int page = 0;
        bool any = false;
        while (q < end && is_digit_c((unsigned char)*q)) {
            any = true;
            if (page < 10000) {
                page = page * 10 + (*q - '0');
            }
            q++;
        }
        if (any && page > highest && page <= 10000 &&
            (size_t)(end - q) >= 5 && strncmp(q, ".html", 5) == 0) {
            highest = page;
        }
        p = hit + sizeof(marker) - 1;
    }
    return highest;
}

/* Find <div id="nr1" ...> and the matching closing </div>. */
static bool find_div_id(const char *html, size_t len, const char *id,
                        const char **out_start, const char **out_end)
{
    char open[64];
    char open_single[64];
    snprintf(open, sizeof(open), "id=\"%s\"", id);
    snprintf(open_single, sizeof(open_single), "id='%s'", id);
    const char *hit = find_ci(html, len, open);
    if (!hit) {
        hit = find_ci(html, len, open_single);
    }
    if (!hit) {
        return false;
    }
    /* walk backwards to '<' */
    const char *tag = hit;
    while (tag > html && tag[-1] != '<') {
        tag--;
    }
    const char *gt = memchr(hit, '>', (size_t)(len - (size_t)(hit - html)));
    if (!gt) {
        return false;
    }
    const char *content_start = gt + 1;
    const char *p = content_start;
    const char *end = html + len;
    int depth = 1;
    while (p < end && depth > 0) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) {
            return false;
        }
        const char *q = lt + 1;
        bool closing = false;
        if (q < end && *q == '/') {
            closing = true;
            q++;
        }
        const char *name = q;
        while (q < end && isalnum((unsigned char)*q)) {
            q++;
        }
        size_t nlen = (size_t)(q - name);
        if (tag_name_is(name, nlen, "div")) {
            if (closing) {
                depth--;
                if (depth == 0) {
                    *out_start = content_start;
                    *out_end = lt;
                    return true;
                }
            } else {
                depth++;
            }
        }
        const char *gt2 = memchr(q, '>', (size_t)(end - q));
        p = gt2 ? gt2 + 1 : end;
    }
    return false;
}

size_t web_html_extract_body(const char *html, size_t len,
                             char *out, size_t cap)
{
    if (!html || !out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    const char *start = NULL;
    const char *end = NULL;
    if (!find_div_id(html, len, "nr1", &start, &end)) {
        return 0;
    }
    /* No intermediate copy: html_to_text operates directly on the bounded
     * nr1 interval and writes into the caller-provided output buffer. */
    if (!html_to_text(start, (size_t)(end - start), out, cap)) {
        out[0] = '\0';
        return (size_t)-1; /* content does not fit the provided cap */
    }
    drop_ad_lines(out);
    return strlen(out);
}

static size_t decode_fragment(const char *start, const char *end,
                              char *out, size_t cap)
{
    if (!start || !end || end < start || !out || cap == 0) {
        return 0;
    }
    if (!html_to_text(start, (size_t)(end - start), out, cap)) {
        out[0] = '\0';
        return 0;
    }
    trim(out);
    return strlen(out);
}

static bool parse_uint32(const char *start, const char *end,
                         uint32_t *out)
{
    if (!start || !end || start >= end || !out) {
        return false;
    }
    uint64_t value = 0;
    const char *p = start;
    while (p < end && is_digit_c((unsigned char)*p)) {
        value = value * 10u + (unsigned)(*p - '0');
        if (value > UINT32_MAX) {
            return false;
        }
        p++;
    }
    if (p == start) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool chapter_id_from_href(const char *tag, const char *tag_end,
                                 uint32_t book_id, bool detail_path,
                                 uint32_t *out_id)
{
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "/book/%u%s", (unsigned)book_id,
             detail_path ? ".html" : "/");
    const char *hit = find_ci(tag, (size_t)(tag_end - tag), prefix);
    if (!hit) {
        return false;
    }
    if (detail_path) {
        if (out_id) {
            *out_id = book_id;
        }
        return true;
    }
    const char *id = hit + strlen(prefix);
    const char *p = id;
    while (p < tag_end && is_digit_c((unsigned char)*p)) {
        p++;
    }
    if (p == id || (size_t)(tag_end - p) < 5 ||
        strncmp(p, ".html", 5) != 0) {
        return false;
    }
    return parse_uint32(id, p, out_id);
}

int web_html_parse_book_search(const char *html, size_t len,
                               web_html_book_t *out, int max)
{
    if (!html || !out || max <= 0) {
        return -1;
    }
    const char *p = html;
    const char *end = html + len;
    int count = 0;
    while (p < end && count < max) {
        const char *klass = find_ci(p, (size_t)(end - p),
                                    "class=\"book-title\"");
        if (!klass) {
            break;
        }
        const char *tag = klass;
        while (tag > html && tag[-1] != '<' && tag[-1] != '>') {
            tag--;
        }
        if (tag == html || tag[-1] == '>') {
            p = klass + 1;
            continue;
        }
        tag--;
        const char *gt = memchr(klass, '>', (size_t)(end - klass));
        if (!gt) {
            break;
        }
        const char *close = find_ci(gt + 1, (size_t)(end - gt - 1), "</a>");
        if (!close) {
            break;
        }

        const char *href = find_ci(tag, (size_t)(gt - tag), "href=\"/book/");
        uint32_t book_id = 0;
        if (!href) {
            p = close + 4;
            continue;
        }
        href += strlen("href=\"/book/");
        const char *digits_end = href;
        while (digits_end < gt && is_digit_c((unsigned char)*digits_end)) {
            digits_end++;
        }
        if (!parse_uint32(href, digits_end, &book_id) ||
            (size_t)(gt - digits_end) < 6 ||
            strncmp(digits_end, ".html\"", 6) != 0) {
            p = close + 4;
            continue;
        }

        bool duplicate = false;
        for (int i = 0; i < count; i++) {
            if (out[i].book_id == book_id) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            p = close + 4;
            continue;
        }

        web_html_book_t *book = &out[count];
        memset(book, 0, sizeof(*book));
        book->book_id = book_id;
        if (decode_fragment(gt + 1, close, book->title,
                            sizeof(book->title)) == 0) {
            p = close + 4;
            continue;
        }

        const char *author = find_ci(close + 4,
                                     (size_t)(end - close - 4),
                                     "class=\"author\"");
        if (author) {
            const char *author_gt = memchr(author, '>', (size_t)(end - author));
            const char *author_lt = author_gt ?
                memchr(author_gt + 1, '<', (size_t)(end - author_gt - 1)) : NULL;
            if (author_gt && author_lt) {
                decode_fragment(author_gt + 1, author_lt, book->author,
                                sizeof(book->author));
            }
        }
        count++;
        p = close + 4;
    }
    if (count > 0) {
        return count;
    }

    /* Bootstrap clone layout: /<book_id / 1000>/<book_id>/ and an author
     * span in the same <dl>. This fallback deliberately keys off the h3/dd
     * result card so cover/latest-chapter links are never mistaken for books. */
    p = html;
    while (p < end && count < max) {
        const char *card = find_ci(p, (size_t)(end - p), "<dd><h3><a href=\"");
        if (!card) {
            break;
        }
        const char *href = card + strlen("<dd><h3><a href=\"");
        const char *q = href;
        if (q >= end || *q++ != '/') {
            p = card + 1;
            continue;
        }
        while (q < end && is_digit_c((unsigned char)*q)) {
            q++;
        }
        if (q >= end || *q++ != '/') {
            p = card + 1;
            continue;
        }
        const char *book_digits = q;
        while (q < end && is_digit_c((unsigned char)*q)) {
            q++;
        }
        uint32_t book_id = 0;
        if (!parse_uint32(book_digits, q, &book_id) || q >= end || *q++ != '/' ||
            q >= end || *q != '"') {
            p = card + 1;
            continue;
        }
        const char *gt = memchr(q, '>', (size_t)(end - q));
        const char *close = gt ? find_ci(gt + 1, (size_t)(end - gt - 1), "</a>") : NULL;
        const char *dl_end = close ? find_ci(close, (size_t)(end - close), "</dl>") : NULL;
        if (!gt || !close || !dl_end) {
            break;
        }
        bool duplicate = false;
        for (int i = 0; i < count; i++) {
            if (out[i].book_id == book_id) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            p = dl_end + 5;
            continue;
        }
        web_html_book_t *book = &out[count];
        memset(book, 0, sizeof(*book));
        book->book_id = book_id;
        if (decode_fragment(gt + 1, close, book->title, sizeof(book->title)) == 0) {
            p = dl_end + 5;
            continue;
        }
        if (book->title[0] == '[') {
            char *kind_end = strchr(book->title, ']');
            if (kind_end && kind_end[1]) {
                memmove(book->title, kind_end + 1, strlen(kind_end + 1) + 1);
                trim(book->title);
            }
        }
        const char *author = find_ci(close, (size_t)(dl_end - close), "作者：<span>");
        if (author) {
            author += strlen("作者：<span>");
            const char *author_end = find_ci(author, (size_t)(dl_end - author), "</span>");
            if (author_end) {
                decode_fragment(author, author_end, book->author, sizeof(book->author));
            }
        }
        count++;
        p = dl_end + 5;
    }
    return count;
}

bool web_html_book_start_id(const char *html, size_t len, uint32_t book_id,
                            uint32_t *out_chapter_id)
{
    if (!html || !out_chapter_id || book_id == 0) {
        return false;
    }
    const char *end = html + len;
    const char *start = find_ci(html, len, "id=\"start\"");
    if (!start) {
        start = find_ci(html, len, "id='start'");
    }
    if (!start) {
        /* Compact source: the complete directory is paged, but its first
         * page begins inside book_list2. Following next-chapter links later
         * avoids ever downloading the remaining directory pages. */
        char prefix[48];
        snprintf(prefix, sizeof(prefix), "/%u/%u/", (unsigned)(book_id / 1000u),
                 (unsigned)book_id);
        const char *list = find_ci(html, len, "class=\"book_list book_list2\"");
        if (!list) {
            return false;
        }
        const char *scan = list;
        uint32_t fallback_id = 0;
        for (int links = 0; links < 24 && scan < end; links++) {
            const char *hit = find_ci(scan, (size_t)(end - scan), prefix);
            if (!hit) {
                break;
            }
            const char *digits = hit + strlen(prefix);
            const char *digits_end = digits;
            while (digits_end < end && is_digit_c((unsigned char)*digits_end)) {
                digits_end++;
            }
            uint32_t candidate = 0;
            if ((size_t)(end - digits_end) < 5 ||
                strncmp(digits_end, ".html", 5) != 0 ||
                !parse_uint32(digits, digits_end, &candidate)) {
                scan = hit + 1;
                continue;
            }
            if (fallback_id == 0) {
                fallback_id = candidate;
            }
            const char *gt = memchr(digits_end + 5, '>',
                                    (size_t)(end - digits_end - 5));
            const char *close = gt ? find_ci(gt + 1,
                (size_t)(end - gt - 1), "</a>") : NULL;
            char heading[WEB_HTML_TITLE_SIZE] = "";
            if (gt && close) {
                decode_fragment(gt + 1, close, heading, sizeof(heading));
            }
            if (strstr(heading, "引子") || strstr(heading, "序章") ||
                strstr(heading, "楔子") || strstr(heading, "第一章") ||
                strstr(heading, "第1章")) {
                *out_chapter_id = candidate;
                return true;
            }
            scan = digits_end + 5;
        }
        if (fallback_id != 0) {
            *out_chapter_id = fallback_id;
            return true;
        }
        return false;
    }
    const char *close = find_ci(start, (size_t)(end - start), "</span>");
    if (!close) {
        close = end;
    }
    return chapter_id_from_href(start, close, book_id, false,
                                out_chapter_id);
}

size_t web_html_extract_biquge_body(const char *html, size_t len,
                                    char *out, size_t cap)
{
    if (!html || !out || cap == 0) {
        return 0;
    }
    const char *start = NULL;
    const char *end = NULL;
    out[0] = '\0';
    if (!find_div_id(html, len, "chaptercontent", &start, &end)) {
        const char *article = find_ci(html, len, "<article class=\"font_max\"");
        if (!article) {
            return 0;
        }
        const char *gt = memchr(article, '>', (size_t)((html + len) - article));
        const char *close = gt ? find_ci(gt + 1,
            (size_t)((html + len) - gt - 1), "</article>") : NULL;
        if (!gt || !close) {
            return 0;
        }
        start = gt + 1;
        end = close;
    }
    if (!html_to_text(start, (size_t)(end - start), out, cap)) {
        out[0] = '\0';
        return (size_t)-1;
    }
    drop_ad_lines(out);
    return strlen(out);
}

size_t web_html_extract_biquge_title(const char *html, size_t len,
                                     char *out, size_t cap)
{
    if (!html || !out || cap == 0) {
        return 0;
    }
    const char *end = html + len;
    const char *scope = find_ci(html, len, "class=\"book read\"");
    if (!scope) {
        scope = html;
    }
    const char *h1 = find_ci(scope, (size_t)(end - scope), "<h1>");
    if (!h1) {
        return 0;
    }
    h1 += 4;
    const char *close = find_ci(h1, (size_t)(end - h1), "</h1>");
    if (!close) {
        return 0;
    }
    size_t written = decode_fragment(h1, close, out, cap);
    char *part = strstr(out, "（1 / 1）");
    if (part) {
        *part = '\0';
        trim(out);
        written = strlen(out);
    }
    char *book_suffix = strstr(out, "-《");
    if (book_suffix) {
        *book_suffix = '\0';
        trim(out);
        written = strlen(out);
    }
    return written;
}

bool web_html_biquguo_next_path(const char *html, size_t len, uint32_t book_id,
                                char *out_path, size_t cap,
                                uint32_t *out_chapter_id)
{
    if (!html || !out_path || cap == 0 || !out_chapter_id || book_id == 0) {
        return false;
    }
    out_path[0] = '\0';
    *out_chapter_id = 0;
    const char *end = html + len;
    const char *id = find_ci(html, len, "id=\"next1\"");
    if (!id) {
        id = find_ci(html, len, "id='next1'");
    }
    if (!id) {
        return false;
    }
    const char *tag = id;
    while (tag > html && tag[-1] != '<' && tag[-1] != '>') {
        tag--;
    }
    if (tag == html || tag[-1] == '>') {
        return false;
    }
    tag--;
    const char *gt = memchr(id, '>', (size_t)(end - id));
    if (!gt) {
        return false;
    }
    const char *href_attr = find_ci(tag, (size_t)(gt - tag), "href=\"");
    if (!href_attr) {
        return false;
    }
    const char *href = href_attr + strlen("href=\"");
    const char *quote = memchr(href, '"', (size_t)(gt - href));
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "/%u/%u/", (unsigned)(book_id / 1000u),
             (unsigned)book_id);
    size_t prefix_len = strlen(prefix);
    if (!quote || (size_t)(quote - href) <= prefix_len + 5 ||
        strncmp(href, prefix, prefix_len) != 0) {
        return false;
    }
    const char *digits = href + prefix_len;
    const char *digits_end = digits;
    while (digits_end < quote && is_digit_c((unsigned char)*digits_end)) {
        digits_end++;
    }
    if (!parse_uint32(digits, digits_end, out_chapter_id)) {
        return false;
    }
    const char *suffix = digits_end;
    if (suffix < quote && *suffix == '_') {
        suffix++;
        if (suffix >= quote || !is_digit_c((unsigned char)*suffix)) {
            return false;
        }
        while (suffix < quote && is_digit_c((unsigned char)*suffix)) {
            suffix++;
        }
    }
    if ((size_t)(quote - suffix) != 5 || strncmp(suffix, ".html", 5) != 0) {
        return false;
    }
    size_t path_len = (size_t)(quote - href);
    if (path_len + 1 > cap) {
        return false;
    }
    memcpy(out_path, href, path_len);
    out_path[path_len] = '\0';
    return true;
}

bool web_html_biquge_next_id(const char *html, size_t len, uint32_t book_id,
                             uint32_t *out_chapter_id)
{
    if (!html || !out_chapter_id || book_id == 0) {
        return false;
    }
    const char *end = html + len;
    const char *rel = find_ci(html, len, "rel=\"next\"");
    if (!rel) {
        rel = find_ci(html, len, "rel='next'");
    }
    if (!rel) {
        return false;
    }
    const char *tag = rel;
    while (tag > html && tag[-1] != '<' && tag[-1] != '>') {
        tag--;
    }
    if (tag == html || tag[-1] == '>') {
        return false;
    }
    tag--;
    const char *gt = memchr(rel, '>', (size_t)(end - rel));
    if (!gt) {
        return false;
    }
    return chapter_id_from_href(tag, gt, book_id, false, out_chapter_id);
}

size_t web_html_extract_title(const char *html, size_t len,
                              char *out, size_t cap)
{
    if (!html || !out || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    static const char open[] = "id=\"nr_title\"";
    const char *hit = find_ci(html, len, open);
    if (!hit) {
        hit = find_ci(html, len, "id='nr_title'");
    }
    if (!hit) {
        return 0;
    }
    const char *gt = memchr(hit, '>', (size_t)(len - (size_t)(hit - html)));
    if (!gt) {
        return 0;
    }
    const char *title_s = gt + 1;
    const char *lt = memchr(title_s, '<', (size_t)(len - (size_t)(title_s - html)));
    size_t tlen = lt ? (size_t)(lt - title_s) : (size_t)(len - (size_t)(title_s - html));
    if (tlen >= cap) {
        tlen = cap - 1;
    }
    memcpy(out, title_s, tlen);
    out[tlen] = '\0';
    /* decode common entities in title */
    if (strchr(out, '&')) {
        char tmp[WEB_HTML_TITLE_SIZE];
        text_writer_t w;
        w_init(&w, tmp, sizeof(tmp));
        const char *p2 = out;
        const char *e2 = out + strlen(out);
        while (p2 < e2) {
            if (*p2 == '&') {
                p2 += w_entity(&w, p2, e2);
            } else {
                w_char(&w, *p2);
                p2++;
            }
        }
        w.buf[w.len] = '\0';
        snprintf(out, cap, "%s", tmp);
    }
    trim(out);
    return strlen(out);
}

bool web_html_next_part_path(const char *html, size_t len,
                             const char *base_id, int current_part,
                             char *out_path, size_t cap)
{
    if (!html || !base_id || !out_path || cap == 0 || current_part < 0) {
        return false;
    }
    static const char attr[] = "href=\"";
    static const char marker[] = "/biquge/2/2031/";
    const char *p = html;
    const char *end = html + len;
    size_t alen = sizeof(attr) - 1;
    size_t mlen = sizeof(marker) - 1;
    int expected = current_part + 2; /* base=0 -> _2, _2 page=1 -> _3 ... */

    while (p < end) {
        const char *attr_hit = find_ci(p, (size_t)(end - p), attr);
        if (!attr_hit) {
            break;
        }
        const char *value_start = attr_hit + alen;
        const char *quote_end = memchr(value_start, '"',
                                       (size_t)(end - value_start));
        if (!quote_end) {
            break;
        }
        size_t value_len = (size_t)(quote_end - value_start);
        const char *hit = find_ci(value_start, value_len, marker);
        if (hit) {
            const char *q = hit + mlen;
            size_t blen = strlen(base_id);
            if ((size_t)(quote_end - q) >= blen + 2 &&
                strncmp(q, base_id, blen) == 0 && q[blen] == '_') {
                const char *d = q + blen + 1;
                int suffix = 0;
                bool any = false;
                while (d < quote_end && is_digit_c((unsigned char)*d)) {
                    any = true;
                    suffix = suffix * 10 + (*d - '0');
                    d++;
                }
                if (any && d < quote_end &&
                    (size_t)(quote_end - d) == 5 &&
                    strncmp(d, ".html", 5) == 0 &&
                    suffix == expected) {
                    size_t total = (size_t)(quote_end - hit);
                    if (total >= cap) {
                        return false;
                    }
                    memcpy(out_path, hit, total);
                    out_path[total] = '\0';
                    return true;
                }
            }
        }
        p = attr_hit + 1;
    }
    return false;
}
