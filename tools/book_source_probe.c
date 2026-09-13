#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../main/web_html.h"

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = size > 0 ? malloc((size_t)size + 1) : NULL;
    if (!data) { fclose(f); return NULL; }
    *len = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[*len] = '\0';
    return data;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s SEARCH DETAIL CHAPTER\n", argv[0]);
        return 2;
    }
    size_t len = 0;
    char *html = read_all(argv[1], &len);
    web_html_book_t books[WEB_HTML_SEARCH_MAX];
    int count = html ? web_html_parse_book_search(html, len, books,
                                                   WEB_HTML_SEARCH_MAX) : -1;
    free(html);
    if (count <= 0) return 1;
    printf("results=%d first=%u %s / %s\n", count,
           (unsigned)books[0].book_id, books[0].title, books[0].author);

    html = read_all(argv[2], &len);
    uint32_t first = 0;
    bool has_first = html && web_html_book_start_id(html, len, 233, &first);
    free(html);
    if (!has_first) return 1;
    printf("first_chapter=%u\n", (unsigned)first);

    html = read_all(argv[3], &len);
    if (!html) return 1;
    char title[WEB_HTML_TITLE_SIZE];
    char *body = malloc(len + 1);
    uint32_t next = 0;
    char next_path[WEB_HTML_PATH_SIZE];
    size_t title_len = web_html_extract_biquge_title(html, len, title,
                                                      sizeof(title));
    bool has_next = web_html_biquguo_next_path(html, len, 233, next_path,
                                                sizeof(next_path), &next);
    size_t body_len = body ? web_html_extract_biquge_body(html, len, body,
                                                          len + 1) : 0;
    printf("title=%s body=%u next=%u path=%s\n", title, (unsigned)body_len,
           (unsigned)next, has_next ? next_path : "(none)");
    free(body);
    free(html);
    return title_len && body_len && has_next ? 0 : 1;
}
