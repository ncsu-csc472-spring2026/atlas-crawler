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
    int max_depth;
    bool verbose;
    char *blocklist_file;
    char *allowlist_file;
} CrawlerConfig;

typedef struct {
    char *url;
    String_Builder html_body;
    int depth;
} ConnContext;

typedef struct {
    char *key;  // Link
    bool value; // visted
    int depth;
} Link_ht;

static size_t write_mem_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    String_Builder *sb = (String_Builder *)userp;
    sb_append_buf(sb, contents, realsize);
    return realsize;
}

static int find_hrefs(Link_ht **links, CrawlerConfig *config, char *text, regex_t *regex, int curr_depth, char **allowlist, char **blocklist) {
    regmatch_t pmatch[2]; // pmatch[0] is the whole match, pmatch[1] is the URL inside the quotes
    char *cursor = text;
    int next_depth = curr_depth + 1;
    while (regexec(regex, cursor, 2, pmatch, 0) == 0) {
        // Calculate the length of the captured URL
        int start = pmatch[1].rm_so;
        int end = pmatch[1].rm_eo;
        int length = end - start;

        char *link = strndup(cursor + start, length);
        // Strip ? and # off links
        char *question_mark = strchr(link, '?');
        if (strchr(link, '?') != NULL) {
            *question_mark = '\0';
        }
        char *hash_mark = strchr(link, '#');
        if (hash_mark != NULL) {
            *hash_mark = '\0';
        }
        // Blocklist
        if (blocklist != NULL) {
            bool is_blocked = false;
            for (int i = 0; i < arrlen(blocklist); i++) {
                if (strstr(link, blocklist[i]) != NULL) {
                    is_blocked = true;
                    break;
                }
            }
            if (is_blocked) {
                free(link);
                cursor += pmatch[0].rm_eo;
                continue; // Drop it completely
            }
        }
        if (shgeti(*links, link) == -1) {
            if (allowlist != NULL) {
                // Allowlist
                bool is_in_scope = false;
                if (arrlen(allowlist) > 0) {
                    for (int i = 0; i < arrlen(allowlist); i++) {
                        if (strstr(link, allowlist[i]) != NULL) {
                            is_in_scope = true;
                            break;
                        }
                    }
                } else {
                    // Fallback: If no allowlist is provided, assume it's external by default
                    is_in_scope = true;
                }
                if (is_in_scope && next_depth <= config->max_depth) {
                    shput(*links, link, false);
                } else {
                    shput(*links, link, true);
                }
            } else {
                shput(*links, link, false);
            }
            // Save the depth level we found it at
            (*links)[shgeti(*links, link)].depth = next_depth;
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

static void save_links_to_file(Link_ht *links, const char *filename) {
    FILE *file;
    if (filename == NULL){
        file = stdout;
    } else {
        file = fopen(filename, "w");
    }
    if (!file) {
        fprintf(stderr, "Could not open %s", filename);
        return;
    }
    size_t links_count = shlen(links);
    for (size_t i = 0; i < links_count; i++) {
        fprintf(file, "%s\n", links[i].key);
    }

    fclose(file);
}

void usage(const char *prog_name) {
    printf("ATLAS Crawler\n");
    printf("Usage: %s [OPTIONS] <Target URL>\n\n", prog_name);
    printf("Options:\n");
    printf("  -v          Enable verbose output\n");
    printf("  -o <file>   Output file for discovered assets\n");
    printf("  -a <file>   Allowlist file for allowing strings\n");
    printf("  -b <file>   Blocklist file for blocking strings\n");
    printf("  -c <num>    Max concurrent network sockets (default: 50)\n");
    printf("  -d <num>    Max depth (default: 3)\n");
    printf("  -m <num>    Max pages to crawl (default: 1000)\n");
    printf("  -h          Print this help menu\n");
}

static CrawlerConfig parse_arguments(int argc, char **argv) {
    CrawlerConfig config = {
        .target_url = NULL,
        .allowlist_file = NULL,
        .blocklist_file = NULL,
        .output_file = NULL,
        .max_concurrent = 50,
        .max_pages = 1000,
        .max_depth = 4,
        .verbose = false
    };

    int opt;
    while ((opt = getopt(argc, argv, "vo:c:m:ha:b:d:")) != -1) {
        switch (opt) {
        case 'v':
            config.verbose = true;
            break;
        case 'a':
            config.allowlist_file = optarg;
            break;
        case 'b':
            config.blocklist_file = optarg;
            break;
        case 'o':
            config.output_file = optarg;
            break;
        case 'c':
            config.max_concurrent = atoi(optarg); // Convert string to integer
            break;
        case 'd':
            config.max_depth = atoi(optarg); // Convert string to integer
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

    if (optind < argc) {
        config.target_url = argv[optind];
    } else {
        fprintf(stderr, "[ERROR] Missing Target URL.\n\n");
        usage(argv[0]);
        exit(1);
    }

    return config;
}
static char **load_wordlist(const char *filename) {
    if (!filename) return NULL;
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "[ERROR] Warning: Could not open wordlist %s\n", filename);
        return NULL;
    }
    char **list = NULL;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = 0; // Strip the newline characters
        if (strlen(line) > 0) {
            arrput(list, strdup(line));
        }
    }
    fclose(file);
    printf("[INFO] Loaded %td words from %s\n", arrlen(list), filename);
    return list;
}

int main(int argc, char **argv) {
    CrawlerConfig config = parse_arguments(argc, argv);
    char **allowlist = load_wordlist(config.allowlist_file);
    if (!allowlist) {
        fprintf(stderr, "[WARNING] No allowlist given!\n");
    }
    char **blocklist = load_wordlist(config.blocklist_file);
    if (!blocklist) {
        fprintf(stderr, "[WARNING] No blocklist given!\n");
    }
    curl_global_init(CURL_GLOBAL_ALL);
    CURLM *multi_handle = curl_multi_init();
    Link_ht *links = NULL;

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
                    if (config.verbose) {
                        printf("[INFO] Curling: %s\n", links[head].key);
                    }
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
                    if (config.verbose) {
                        printf("[INFO] Parsing %lu bytes from %s\n", ctx->html_body.count, ctx->url);
                    }
                    find_hrefs(&links, &config, ctx->html_body.items, &regex, ctx->depth, allowlist, blocklist);
                } else {
                    if (head == 1) {
                        fprintf(stderr, "[ERROR] Invalid URL %s (HTTP %ld)\n", ctx->url, http_code);
                        return 1;
                    }
                    if (config.verbose) {
                        printf("[INFO] Failed %s (HTTP %ld)\n", ctx->url, http_code);
                    }
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
