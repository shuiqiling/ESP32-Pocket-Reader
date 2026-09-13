#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/web_html.h"

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    *len = rd;
    return buf;
}

int main(void)
{
    int failed = 0;
    size_t len = 0;

    char *idx = read_file("index.html", &len);
    if (!idx) { fprintf(stderr, "cannot read index.html\n"); return 1; }
    web_html_chapter_t chapters[30];
    int n = web_html_parse_index(idx, len, chapters, 20);
    int page_count = web_html_index_page_count(idx, len);
    printf("index parse count=%d\n", n);
    for (int i = 0; i < n && i < 3; i++) {
        printf("  [%d] %s | %s\n", i + 1, chapters[i].path, chapters[i].title);
    }
    if (n != 20) { fprintf(stderr, "FAIL: expected 20 chapters, got %d\n", n); failed++; }
    if (page_count != 15) {
        fprintf(stderr, "FAIL: expected 15 index pages, got %d\n", page_count);
        failed++;
    }
    if (strcmp(chapters[0].path, "/biquge/2/2031/7393038.html") != 0) {
        fprintf(stderr, "FAIL: first path wrong: %s\n", chapters[0].path); failed++;
    }
    if (strcmp(chapters[1].path, "/biquge/2/2031/7393039.html") != 0) {
        fprintf(stderr, "FAIL: second path wrong: %s\n", chapters[1].path); failed++;
    }
    free(idx);

    char *chap = read_file("chap.html", &len);
    if (!chap) { fprintf(stderr, "cannot read chap.html\n"); return 1; }

    char body[20000];
    size_t blen = web_html_extract_body(chap, len, body, sizeof(body));
    printf("body length=%u\n", (unsigned)blen);
    if (blen == 0) { fprintf(stderr, "FAIL: no body\n"); failed++; }
    const char *expect = "斗罗大陆，天斗帝国西南，法斯诺行省";
    if (!strstr(body, expect)) {
        fprintf(stderr, "FAIL: body missing first paragraph\n"); failed++;
    }
    if (strstr(body, "加入书签") || strstr(body, "请点击下一页") || strstr(body, "本章未完")) {
        fprintf(stderr, "FAIL: ad/page-control text leaked into body\n"); failed++;
    }

    const char *entity_html =
        "<html><div id=\"nr1\">甲&nbsp;乙&amp;丙&#x2014;丁</div></html>";
    char entity_body[64];
    size_t entity_len = web_html_extract_body(entity_html, strlen(entity_html),
                                               entity_body, sizeof(entity_body));
    if (entity_len == 0 || strcmp(entity_body, "甲 乙&丙—丁") != 0) {
        fprintf(stderr, "FAIL: entity decoding left/dropped bytes: [%s]\n",
                entity_body);
        failed++;
    }

    char title[128];
    size_t tlen = web_html_extract_title(chap, len, title, sizeof(title));
    printf("title=[%s] len=%u\n", title, (unsigned)tlen);
    if (strstr(title, "第一章 斗罗大陆，异界唐三（一）") == NULL) {
        fprintf(stderr, "FAIL: title extraction\n"); failed++;
    }

    char next[64];
    bool has_next = web_html_next_part_path(chap, len, "7393039", 0, next, sizeof(next));
    printf("next part=%s\n", has_next ? next : "(none)");
    if (!has_next || strcmp(next, "/biquge/2/2031/7393039_2.html") != 0) {
        fprintf(stderr, "FAIL: same-entry pagination not detected\n"); failed++;
    }

    /* The chapter page must NOT report a different chapter as a next part. */
    char bad[64];
    bool cross = web_html_next_part_path(chap, len, "7393040", 0, bad, sizeof(bad));
    if (cross) { fprintf(stderr, "FAIL: cross chapter pagination false positive: %s\n", bad); failed++; }

    /* ---- synthetic: duplicate hrefs and non-href look-alikes ---- */
    const char *synth_index =
        "<html><body><ul>"
        "<li><a href=\"/biquge/2/2031/111.html\">甲</a></li>"
        "<li><a href=\"/biquge/2/2031/111.html\">甲重复</a></li>"
        "<li><a href=\"/biquge/2/2031/222.html\">乙</a></li>"
        "</ul>"
        "<script>var x = '/biquge/2/2031/333.html';</script>"
        "<p>正文里写 /biquge/2/2031/444.html</p>"
        "</body></html>";
    web_html_chapter_t synth_ch[10];
    int sn = web_html_parse_index(synth_index, strlen(synth_index), synth_ch, 10);
    printf("synthetic index count=%d\n", sn);
    if (sn != 2) { fprintf(stderr, "FAIL: synthetic duplicate/non-href count=%d\n", sn); failed++; }
    if (strcmp(synth_ch[0].path, "/biquge/2/2031/111.html") != 0 ||
        strcmp(synth_ch[1].path, "/biquge/2/2031/222.html") != 0) {
        fprintf(stderr, "FAIL: synthetic index order/dedupe\n"); failed++;
    }
    const char *synth_pages =
        "<option value=\"/biquge/2/2031/index_1.html\">1</option>"
        "<option value=\"/biquge/2/2031/index_27.html\">27</option>"
        "<a href=\"/biquge/2/2031/index_9.html\">next</a>";
    if (web_html_index_page_count(synth_pages, strlen(synth_pages)) != 27) {
        fprintf(stderr, "FAIL: index page count parser\n"); failed++;
    }

    /* ---- synthetic: strict pagination order + loop protection ---- */
    const char *page2 =
        "<a href=\"/biquge/2/2031/555_2.html\">当前页</a>"
        "<a href=\"/biquge/2/2031/555_2.html\">上一页</a>"
        "<a href=\"/biquge/2/2031/555_3.html\">下一页</a>";
    char p3[64];
    bool to3 = web_html_next_part_path(page2, strlen(page2), "555", 1, p3, sizeof(p3));
    printf("page2 -> part3=%s\n", to3 ? p3 : "(none)");
    if (!to3 || strcmp(p3, "/biquge/2/2031/555_3.html") != 0) {
        fprintf(stderr, "FAIL: page2 must pick _3 not _2\n"); failed++;
    }
    bool to2 = web_html_next_part_path(page2, strlen(page2), "555", 0, p3, sizeof(p3));
    if (!to2 || strcmp(p3, "/biquge/2/2031/555_2.html") != 0) {
        fprintf(stderr, "FAIL: base page must pick _2\n"); failed++;
    }

    const char *loop = page2; /* repeats _2/_3; never returns _2 when on part 1 */
    bool loop_back = web_html_next_part_path(loop, strlen(loop), "555", 1, p3, sizeof(p3));
    if (!loop_back || strcmp(p3, "/biquge/2/2031/555_3.html") != 0) {
        fprintf(stderr, "FAIL: loop test must advance, not go back\n"); failed++;
    }
    /* A page that only links the same/previous part must not produce a next. */
    const char *only_cur =
        "<a href=\"/biquge/2/2031/555_2.html\">当前</a>"
        "<a href=\"/biquge/2/2031/555_2.html\">重复</a>";
    bool no_next = web_html_next_part_path(only_cur, strlen(only_cur), "555", 1, p3, sizeof(p3));
    if (no_next) { fprintf(stderr, "FAIL: no _3 should mean no next page\n"); failed++; }

    /* ---- synthetic: searchable books + sequential biquge reader ---- */
    const char *search_html =
        "<div class=\"result-card\"><a href=\"/book/9322.html\" "
        "class=\"book-title\">斗破苍穹.2</a><span class=\"author\">柴老五</span></div>"
        "<div><a href=\"/book/199325.html\" class=\"book-title\">斗破苍穹</a>"
        "<span class=\"author\">天蚕土豆</span></div>";
    web_html_book_t books[4];
    int book_count = web_html_parse_book_search(search_html,
                                                 strlen(search_html), books, 4);
    if (book_count != 2 || books[0].book_id != 9322 ||
        strcmp(books[0].title, "斗破苍穹.2") != 0 ||
        strcmp(books[1].author, "天蚕土豆") != 0) {
        fprintf(stderr, "FAIL: search parser\n");
        failed++;
    }

    const char *detail_html =
        "<span id=\"start\"><a href=\"/book/9322/391457.html\">开始阅读</a></span>";
    uint32_t chapter_id = 0;
    if (!web_html_book_start_id(detail_html, strlen(detail_html), 9322,
                                &chapter_id) || chapter_id != 391457) {
        fprintf(stderr, "FAIL: book start parser\n");
        failed++;
    }

    const char *biquge_chapter =
        "<div class=\"book read\"><h1>第一章 测试（1 / 1）</h1>"
        "<a href=\"/book/9322/391467.html\" rel=\"next\">下一章</a>"
        "<div class=\"read-content\" id=\"chaptercontent\"><p>甲&amp;乙</p>"
        "<p>第二段</p></div></div>";
    char biquge_title[64];
    char biquge_body[64];
    if (web_html_extract_biquge_title(biquge_chapter, strlen(biquge_chapter),
                                      biquge_title, sizeof(biquge_title)) == 0 ||
        strcmp(biquge_title, "第一章 测试") != 0) {
        fprintf(stderr, "FAIL: biquge title parser [%s]\n", biquge_title);
        failed++;
    }
    if (web_html_extract_biquge_body(biquge_chapter, strlen(biquge_chapter),
                                     biquge_body, sizeof(biquge_body)) == 0 ||
        strcmp(biquge_body, "甲&乙\n第二段") != 0) {
        fprintf(stderr, "FAIL: biquge body parser [%s]\n", biquge_body);
        failed++;
    }
    chapter_id = 0;
    if (!web_html_biquge_next_id(biquge_chapter, strlen(biquge_chapter),
                                 9322, &chapter_id) || chapter_id != 391467) {
        fprintf(stderr, "FAIL: next chapter parser\n");
        failed++;
    }

    /* ---- synthetic: compact source search/detail/multipart chapter ---- */
    const char *compact_search =
        "<div><dl><dd><h3><a href=\"/0/233/\">[玄幻]斗罗大陆</a></h3></dd>"
        "<dd class=\"book_other\">作者：<span>唐家三少</span></dd></dl></div>"
        "<div><dl><dd><h3><a href=\"/97/97847/\">斗罗大陆5重生唐三</a></h3></dd>"
        "<dd class=\"book_other\">作者：<span>唐家三少</span></dd></dl></div>";
    memset(books, 0, sizeof(books));
    book_count = web_html_parse_book_search(compact_search,
                                             strlen(compact_search), books, 4);
    if (book_count != 2 || books[0].book_id != 233 ||
        strcmp(books[0].title, "斗罗大陆") != 0 ||
        strcmp(books[0].author, "唐家三少") != 0 ||
        books[1].book_id != 97847) {
        fprintf(stderr, "FAIL: compact search parser [%d, %u, %s, %s]\n",
                book_count, (unsigned)books[0].book_id, books[0].title,
                books[0].author);
        failed++;
    }
    const char *compact_detail =
        "<div class=\"book_list book_list2\"><ul>"
        "<li><a href=\"/0/233/253190.html\">其他作品外篇</a></li>"
        "<li><a href=\"/0/233/253194.html\">引子</a></li></ul></div>";
    chapter_id = 0;
    if (!web_html_book_start_id(compact_detail, strlen(compact_detail), 233,
                                &chapter_id) || chapter_id != 253194) {
        fprintf(stderr, "FAIL: compact book start parser\n");
        failed++;
    }
    const char *compact_part =
        "<h1>引子 穿越的唐家三少-《斗罗大陆》</h1>"
        "<a id=\"next1\" href=\"/0/233/253194_2.html\">下一章</a>"
        "<article class=\"font_max\"><br>第(1/3)页<br>甲&amp;乙<br>第二段"
        "<br>第(1/3)页</article>";
    char compact_title[64];
    char compact_body[64];
    if (web_html_extract_biquge_title(compact_part, strlen(compact_part),
                                      compact_title, sizeof(compact_title)) == 0 ||
        strcmp(compact_title, "引子 穿越的唐家三少") != 0) {
        fprintf(stderr, "FAIL: compact title parser [%s]\n", compact_title);
        failed++;
    }
    if (web_html_extract_biquge_body(compact_part, strlen(compact_part),
                                     compact_body, sizeof(compact_body)) == 0 ||
        strcmp(compact_body, "甲&乙\n第二段") != 0) {
        fprintf(stderr, "FAIL: compact body parser [%s]\n", compact_body);
        failed++;
    }
    char compact_next[64];
    chapter_id = 0;
    if (!web_html_biquguo_next_path(compact_part, strlen(compact_part), 233,
                                    compact_next, sizeof(compact_next),
                                    &chapter_id) || chapter_id != 253194 ||
        strcmp(compact_next, "/0/233/253194_2.html") != 0) {
        fprintf(stderr, "FAIL: compact multipart next parser\n");
        failed++;
    }
    const char *compact_last =
        "<a id=\"next1\" href=\"/0/233/253195.html\">下一章</a>";
    chapter_id = 0;
    if (!web_html_biquguo_next_path(compact_last, strlen(compact_last), 233,
                                    compact_next, sizeof(compact_next),
                                    &chapter_id) || chapter_id != 253195) {
        fprintf(stderr, "FAIL: compact next chapter parser\n");
        failed++;
    }

    free(chap);
    if (failed) {
        fprintf(stderr, "SELFTEST FAILED (%d)\n", failed);
        return 1;
    }
    printf("SELFTEST PASSED\n");
    return 0;
}
