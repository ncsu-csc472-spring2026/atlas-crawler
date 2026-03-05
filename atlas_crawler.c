#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <curl/curl.h>
#include <stdbool.h>
#include <unistd.h>

#define NOB_IMPLEMENTATION
#define STB_DS_IMPLEMENTATION
#include "thirdparty/nob.h"
#include "thirdparty/stb_ds.h"

typedef struct {
    char *target_url;
    char *output_file;
    int max_concurrent;
    int max_pages;
} CrawlerConfig;

typedef struct {
    char *url;
    String_Builder html_body;
} ConnContext;

typedef struct {
    char *key;  // Link
    bool value; // visted
} Links_ht;

static size_t write_mem_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    String_Builder *sb = (String_Builder *)userp;
    sb_append_buf(sb, contents, realsize);
    return realsize;
}

static int find_hrefs(Links_ht **links, char *text, regex_t *regex) {
    regmatch_t pmatch[2]; // pmatch[0] is the whole match, pmatch[1] is the URL inside the quotes
    char *cursor = text;
    while (regexec(regex, cursor, 2, pmatch, 0) == 0) {
        // Calculate the length of the captured URL
        int start = pmatch[1].rm_so;
        int end = pmatch[1].rm_eo;
        int length = end - start;

        char *link = strndup(cursor + start, length);
        char *question_mark = strchr(link, '?');
        if (strchr(link, '?') != NULL) {
            *question_mark = '\0';
        }
        char *hash_mark = strchr(link, '#');
        if (hash_mark != NULL) {
            *hash_mark = '\0';
        }
        if (strstr(link, "chrome-extension") != NULL ||
        strstr(link, "file://") != NULL ||
        strstr(link, "C:/") != NULL ||
        strstr(link, "facebook.com") != NULL ||
        strstr(link, "instagram.com") != NULL ||
        strstr(link, "twitter.com") != NULL ||
        strstr(link, "x.com") != NULL ||
        strstr(link, "youtube.com") != NULL ||
        strstr(link, "linkedin.com") != NULL ||
        strstr(link, "pinterest.com") != NULL) {
            free(link);
            cursor += pmatch[0].rm_eo;
            continue;
        }
        if (shgeti(*links, link) == -1) {
            if (strstr(link, "gcs") != NULL ||
            strstr(link, "gaston") != NULL) {
                shput(*links, link, false);
            } else {
                shput(*links, link, true);
           }
        } else {
            free(link);
        }
        // Move cursor forward
        cursor += pmatch[0].rm_eo;
    }
    return 0;
}

static CURL *prepare_curl_handle(const char *url) {
    CURL *eh = curl_easy_init();
    if (!eh) return NULL;

    ConnContext *ctx = malloc(sizeof(ConnContext));
    ctx->url = strdup(url);
    memset(&ctx->html_body, 0, sizeof(String_Builder));

    curl_easy_setopt(eh, CURLOPT_URL, ctx->url);
    curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, write_mem_callback);
    curl_easy_setopt(eh, CURLOPT_WRITEDATA, &ctx->html_body);
    curl_easy_setopt(eh, CURLOPT_USERAGENT, "Atlas-Sentinel/1.0");
    curl_easy_setopt(eh, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(eh, CURLOPT_PRIVATE, ctx);
    curl_easy_setopt(eh, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(eh, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(eh, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(eh, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
    curl_easy_setopt(eh, CURLOPT_NOSIGNAL, 1L);
    return eh;
}

static void save_links_to_file(Links_ht *links, const char *filename) {
    FILE *file = fopen(filename, "w");
    if (!file) {
        fprintf(stderr, "Could not open %s", filename);
        return;
    }

    size_t links_count = shlen(links);
    for (size_t i = 0; i < links_count; i++) {
        fprintf(file, "%s\n", links[i].key);
    }

    fclose(file);
    printf("Successfully saved %zu links to %s\n", links_count, filename);
}
void usage(const char *prog_name) {
    printf("ATLAS Crawler\n");
    printf("Usage: %s [OPTIONS] <Target URL>\n\n", prog_name);
    printf("Options:\n");
    printf("  -o <file>   Output file for discovered assets (default: output.txt)\n");
    printf("  -c <num>    Max concurrent network sockets (default: 50)\n");
    printf("  -m <num>    Max pages to crawl (default: 100)\n");
    printf("  -h          Print this help menu\n");
}

static CrawlerConfig parse_arguments(int argc, char **argv) {
    CrawlerConfig config = {
        .target_url = NULL,
        .output_file = "output.txt",
        .max_concurrent = 50,
        .max_pages = 100
    };

    int opt;
    while ((opt = getopt(argc, argv, "o:c:m:h")) != -1) {
        switch (opt) {
            case 'o':
                config.output_file = optarg;
                break;
            case 'c':
                config.max_concurrent = atoi(optarg); // Convert string to integer
                break;
            case 'm':
                config.max_pages = atoi(optarg);
                break;
            case 'h':
                usage(argv[0]);
                exit(0);
            default:
                usage(argv[0]);
                exit(1);
        }
    }

    // 3. Grab the positional argument (The Target URL)
    // optind is the index of the first non-flag argument
    if (optind < argc) {
        config.target_url = argv[optind];
    } else {
        fprintf(stderr, "[ERROR] Missing Target URL.\n\n");
        usage(argv[0]);
        exit(1);
    }

    return config;
}

int main(int argc, char **argv) {
    CrawlerConfig config = parse_arguments(argc, argv);
    curl_global_init(CURL_GLOBAL_ALL);
    CURLM *multi_handle = curl_multi_init();
    Links_ht *links = NULL;

    shput(links, strdup(config.target_url), false);
    regex_t regex;
    const char *pattern = "href=\"(https?://[^\"]+)\"";
    if (regcomp(&regex, pattern, REG_EXTENDED | REG_ICASE) != 0) {
        fprintf(stderr, "Failed to compile regex.\n");
        return -1;
    }
    size_t head = 0;
    int active_transfers = 0;
    int pages_crawled = 0;

    while (active_transfers > 0 || (head < (size_t)shlen(links) && pages_crawled < config.max_pages)) {
        while (active_transfers < config.max_concurrent && head < (size_t)shlen(links) && pages_crawled < config.max_pages) {
            if (links[head].value == false) {
                links[head].value = true; // Mark as visited
                CURL *eh = prepare_curl_handle(links[head].key);
                if (eh) {
                    curl_multi_add_handle(multi_handle, eh);
                    active_transfers++;
                    pages_crawled++;
                    printf("[INFO] Curling: %s\n", links[head].key);
                }
            }
            head++;
        }
        curl_multi_perform(multi_handle, &active_transfers);
        curl_multi_poll(multi_handle, NULL, 0, 1000, NULL);
        int msgs_left;
        CURLMsg *msg;
        while ((msg = curl_multi_info_read(multi_handle, &msgs_left))) {
            if (msg->msg == CURLMSG_DONE) {
                CURL *eh = msg->easy_handle;
                ConnContext *ctx = NULL;
                long http_code = 0;

                curl_easy_getinfo(eh, CURLINFO_PRIVATE, &ctx);
                curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &http_code);

                if (msg->data.result == CURLE_OK && http_code == 200) {
                    sb_append_null(&ctx->html_body);
                    printf("[INFO] Parsing %lu bytes from %s\n", ctx->html_body.count, ctx->url);

                    find_hrefs(&links, ctx->html_body.items, &regex);
                } else {
                    if (head == 1) {
                        fprintf(stderr, "[ERROR] Invalid URL %s (HTTP %ld)\n", ctx->url, http_code);
                        return 1;
                    }
                    fprintf(stderr, "[ERROR] Failed %s (HTTP %ld)\n", ctx->url, http_code);
                }
                curl_multi_remove_handle(multi_handle, eh);
                curl_easy_cleanup(eh);
                da_free(ctx->html_body);
                free(ctx->url);
                free(ctx);
            }
        }
    }
    save_links_to_file(links, config.output_file);
    size_t total_found = shlen(links);
    for (size_t i = 0; i < total_found; i++) {
        free(links[i].key);
    }
    regfree(&regex);
    shfree(links);
    curl_multi_cleanup(multi_handle);
    curl_global_cleanup();
    return 0;
}
