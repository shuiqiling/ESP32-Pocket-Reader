/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_HTML_INDEX_PAGE_SIZE 50
#define WEB_HTML_PATH_SIZE    48
#define WEB_HTML_TITLE_SIZE   96
#define WEB_HTML_AUTHOR_SIZE  48
#define WEB_HTML_SEARCH_MAX   8
typedef struct {
    char path[WEB_HTML_PATH_SIZE];      /* /biquge/2/2031/7393038.html */
    char title[WEB_HTML_TITLE_SIZE];    /* text inside the <a> tag       */
} web_html_chapter_t;

typedef struct {
    uint32_t book_id;
    char title[WEB_HTML_TITLE_SIZE];
    char author[WEB_HTML_AUTHOR_SIZE];
} web_html_book_t;

/* Parses the index page. Returns number of links found (at most max) or <0
 * on error. Only chapter links matching /biquge/2/2031/<digits>.html are
 * accepted; duplicates are removed and directory order is preserved. */
int web_html_parse_index(const char *html, size_t len,
                         web_html_chapter_t *out, int max);

/* Returns the highest index_N.html page referenced by the directory. A
 * directory with no explicit pagination is one page. */
int web_html_index_page_count(const char *html, size_t len);

/* Extracts the visible text from <div id="nr1"> ... </div>.
 * Returns bytes written (not counting the terminating NUL). The output is
 * always NUL terminated when cap > 0. */
size_t web_html_extract_body(const char *html, size_t len,
                             char *out, size_t cap);

/* Extracts the chapter heading from <div class="nr_title" id="nr_title">.
 * Returns bytes written (not including NUL). */
size_t web_html_extract_title(const char *html, size_t len,
                              char *out, size_t cap);

/* If the current chapter page is split (7393039_2.html / _3.html ...), fills
 * out_path with the page whose numeric id equals base_id AND whose suffix is
 * exactly current_part + 2. current_part is 0 for the main page, 1 for
 * page _2, 2 for page _3, etc. This rejects current/back links and never
 * jumps to another chapter. base_id is the plain decimal id, e.g. "7393039". */
bool web_html_next_part_path(const char *html, size_t len,
                             const char *base_id, int current_part,
                             char *out_path, size_t cap);

/* Parser helpers for the compact online source. Search results are deliberately
 * capped by the caller so an ESP32 never retains a full result page. */
int web_html_parse_book_search(const char *html, size_t len,
                               web_html_book_t *out, int max);

/* Extracts the first chapter id from the book detail page's #start link. */
bool web_html_book_start_id(const char *html, size_t len, uint32_t book_id,
                            uint32_t *out_chapter_id);

/* Extracts #chaptercontent and the h1 chapter heading used by biquge.tw. */
size_t web_html_extract_biquge_body(const char *html, size_t len,
                                    char *out, size_t cap);
size_t web_html_extract_biquge_title(const char *html, size_t len,
                                     char *out, size_t cap);

/* Returns the rel=next chapter id when it still belongs to the same book.
 * False means the current chapter is the end of the novel. */
bool web_html_biquge_next_id(const char *html, size_t len, uint32_t book_id,
                             uint32_t *out_chapter_id);

/* Extracts the source's authoritative "next" link. Multipart pages keep the
 * same chapter id (for example 253194_2.html); the last part points at the next
 * chapter id. False means there is no usable next link. */
bool web_html_biquguo_next_path(const char *html, size_t len, uint32_t book_id,
                                char *out_path, size_t cap,
                                uint32_t *out_chapter_id);

#ifdef __cplusplus
}
#endif
