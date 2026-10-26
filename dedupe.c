#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <regex.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------- utilities -------------------------------- */
static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("dedup: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static void *xcalloc(size_t n, size_t size) {
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (!p) {
        fputs("dedup: out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("dedup: out of memory\n", stderr);
        exit(1);
    }
    return q;
}

static char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need)
        cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}

static void buf_add(Buf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_adds(Buf *b, const char *s) {
    buf_add(b, s, strlen(s));
}

static void buf_addc(Buf *b, char c) {
    buf_add(b, &c, 1);
}

static char *buf_steal(Buf *b) {
    if (!b->data) {
        b->data = xmalloc(1);
        b->data[0] = '\0';
        b->cap = 1;
    }
    char *data = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return data;
}

typedef struct {
    char **items;
    size_t n;
    size_t cap;
} StrVec;

static void strvec_push(StrVec *v, char *s) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xrealloc(v->items, v->cap * sizeof(*v->items));
    }
    v->items[v->n++] = s;
}

/* -------------------------------- filters --------------------------------- */
static const char *const STATIC_EXTS[] = {
    "css", "js", "mjs", "map", "json", "xml", "txt",
    "png", "jpg", "jpeg", "gif", "webp", "svg", "ico", "bmp", "tif", "tiff",
    "avif", "woff", "woff2", "ttf", "otf", "eot",
    "mp3", "mp4", "wav", "ogg", "oga", "webm", "avi", "mov", "mkv", "flv",
    "m4a", "m4v", "pdf", "zip", "rar", "7z", "tar", "gz", "bz2", "xz",
    "doc", "docx", "xls", "xlsx", "ppt", "pptx", "rtf", "csv",
};
#define N_STATIC_EXTS (sizeof(STATIC_EXTS) / sizeof(STATIC_EXTS[0]))

static const char *const NOISE_QUERY[] = {
    "replytocom", "cpage", "calendar",
};

static const char *const TRACKING_KEYS[] = {
    "fbclid", "gclid", "gclsrc", "msclkid", "yclid", "dclid",
    "srsltid", "_ga", "_gl", "_hsenc", "_hsmi", "igshid", "twclid",
};

static const char *const TRACKING_PREFIXES[] = {
    "utm_", "mc_", "mtm_", "pk_",
};

static const char *const WP_POST_QUERY[] = {
    "p", "page_id", "attachment_id", "preview_id", "preview_nonce",
};

/*
 * Blog/CMS noise, not just WordPress. This covers WordPress core, feeds,
 * taxonomies, date/numeric archives, and common blog-like content roots.
 */
static const char *const NOISE_PATH_PATTERN =
    "(/(wp-admin|wp-content|wp-includes|wp-json|wp-login[.]php|"
    "wp-cron[.]php|wp-signup[.]php|xmlrpc[.]php|readme[.]html|license[.]txt)"
    "([/?.]|$)"
    "|/(feed|rss|atom)(/|[.]xml|$)"
    "|/(category|tag|author|archives?|taxonomy|topics?|labels?)(/|$)"
    "|/page/[0-9]+/?$"
    "|/comment-page-[0-9]+/?"
    "|/[0-9]{4}(/|$)"
    "|/[0-9]{4}[/-][0-9]{2}([/-][0-9]{2})?([/_-]|$)"
    "|/[0-9]{4}[-_][0-9]{1,2}[-_][0-9]{1,2}([-_/]|$)"
    "|/[0-9]{3,}/?$"
    "|/[0-9]{3,}-[^/]+/?$"
    "|^/(blogs?|weblogs?|news|newsroom|press|articles?|posts?|stories|story|"
    "journals?|notes?|entries|entry|magazines?|publications?|announcements?|"
    "insights?|updates?|events?)(/|$)"
    "|/(attachment|revision)/)";

static regex_t noise_re;

enum {
    R_INVALID = 0,
    R_SCHEME,
    R_WP_QUERY,
    R_STATIC,
    R_WP_PATH,
    R_BLOG_SLUG,
    R_EXCLUDE,
    R_COUNT,
};

static const char *const REASON_NAMES[R_COUNT] = {
    "invalid", "non-http", "wp-query", "static",
    "wp-path", "blog-slug", "exclude",
};

static int in_strset(const char *key, const char *const *set, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(key, set[i]) == 0)
            return 1;
    return 0;
}

static int is_wp_query(const char *key, int keep_p_param) {
    if (keep_p_param && strcmp(key, "p") == 0)
        return 0;
    return in_strset(key, WP_POST_QUERY,
                     sizeof(WP_POST_QUERY) / sizeof(WP_POST_QUERY[0]));
}

static int keep_param(const char *key, int strip_tracking) {
    if (in_strset(key, NOISE_QUERY,
                  sizeof(NOISE_QUERY) / sizeof(NOISE_QUERY[0])))
        return 0;
    if (strip_tracking) {
        if (in_strset(key, TRACKING_KEYS,
                      sizeof(TRACKING_KEYS) / sizeof(TRACKING_KEYS[0])))
            return 0;
        for (size_t i = 0;
             i < sizeof(TRACKING_PREFIXES) / sizeof(TRACKING_PREFIXES[0]);
             i++) {
            size_t n = strlen(TRACKING_PREFIXES[i]);
            if (strncmp(key, TRACKING_PREFIXES[i], n) == 0)
                return 0;
        }
    }
    return 1;
}

static int path_is_static(const char *path, const char *const *exts,
                          size_t n) {
    const char *last = strrchr(path, '/');
    last = last ? last + 1 : path;
    const char *dot = strrchr(last, '.');
    if (!dot)
        return 0;
    const char *ext = dot + 1;
    for (size_t i = 0; i < n; i++)
        if (strcasecmp(ext, exts[i]) == 0)
            return 1;
    return 0;
}

static int looks_like_blog_slug(const char *path, int min_hyphens) {
    const char *last = strrchr(path, '/');
    last = last ? last + 1 : path;
    const char *dot = strrchr(last, '.');
    size_t len = dot ? (size_t)(dot - last) : strlen(last);
    int hyphens = 0;
    for (size_t i = 0; i < len; i++)
        if (last[i] == '-')
            hyphens++;
    return hyphens >= min_hyphens;
}

/* ----------------------------- URL parsing -------------------------------- */
typedef struct {
    char *key;
    char *segment;
} Param;

typedef struct {
    const char *scheme;
    char *netloc;
    char *path;
    Param *params;
    size_t nparams;
    size_t cap;
} Record;

static int hexval(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static char *decode_slice(const char *s, size_t n, int plus_space,
                          int lower_ascii) {
    char *out = xmalloc(n + 1);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '%' && i + 2 < n) {
            int hi = hexval((unsigned char)s[i + 1]);
            int lo = hexval((unsigned char)s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                c = (unsigned char)((hi << 4) | lo);
                i += 2;
            }
        } else if (plus_space && c == '+') {
            c = ' ';
        }
        if (lower_ascii && c < 128)
            c = (unsigned char)tolower(c);
        out[j++] = (char)c;
    }
    out[j] = '\0';
    return out;
}

static char *normalize_path(const char *s, size_t n) {
    /* Match Python: strip one trailing slash, then collapse runs of slashes. */
    if (n > 1 && s[n - 1] == '/')
        n--;
    char *out = xmalloc(n + 2);
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '/' && j > 0 && out[j - 1] == '/')
            continue;
        out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

static char *build_netloc(const char *auth, size_t alen, int http, int *ok) {
    *ok = 0;

    const char *userinfo = NULL;
    size_t ulen = 0;
    const char *hp = auth;
    size_t hplen = alen;
    for (size_t i = 0; i < alen; i++) {
        if (auth[i] == '@') {
            userinfo = auth;
            ulen = i;
            hp = auth + i + 1;
            hplen = alen - i - 1;
        }
    }

    const char *host = NULL;
    size_t hostlen = 0;
    const char *portstr = NULL;
    size_t portlen = 0;
    int ipv6 = 0;

    if (hplen > 0 && hp[0] == '[') {
        const char *close = memchr(hp, ']', hplen);
        if (!close)
            return NULL;
        host = hp + 1;
        hostlen = (size_t)(close - (hp + 1));
        const char *rest = close + 1;
        size_t restlen = hplen - (size_t)(rest - hp);
        if (restlen > 0) {
            if (*rest != ':')
                return NULL;
            portstr = rest + 1;
            portlen = restlen - 1;
        }
        ipv6 = 1;
    } else {
        size_t colons = 0;
        const char *last_colon = NULL;
        for (size_t i = 0; i < hplen; i++) {
            if (hp[i] == ':') {
                colons++;
                last_colon = hp + i;
            }
        }
        if (last_colon && colons == 1) {
            host = hp;
            hostlen = (size_t)(last_colon - hp);
            portstr = last_colon + 1;
            portlen = hplen - (size_t)(last_colon - hp + 1);
        } else {
            host = hp;
            hostlen = hplen;
        }
    }

    if (!host || hostlen == 0)
        return NULL;

    long port = -1;
    if (portstr && portlen > 0) {
        long value = 0;
        for (size_t i = 0; i < portlen; i++) {
            unsigned char c = (unsigned char)portstr[i];
            if (!isdigit(c))
                return NULL;
            value = value * 10 + (c - '0');
            if (value > 65535)
                return NULL;
        }
        port = value;
    }

    int default_port = http ? 80 : 443;
    int include_port = port > 0 && port != default_port;

    Buf b = {0};
    if (userinfo) {
        buf_add(&b, userinfo, ulen);
        buf_addc(&b, '@');
    }
    if (ipv6)
        buf_addc(&b, '[');
    for (size_t i = 0; i < hostlen; i++) {
        unsigned char c = (unsigned char)host[i];
        if (c < 128)
            c = (unsigned char)tolower(c);
        buf_addc(&b, (char)c);
    }
    if (ipv6)
        buf_addc(&b, ']');
    if (include_port) {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), ":%ld", port);
        if (n > 0)
            buf_add(&b, tmp, (size_t)n);
    }

    *ok = 1;
    return buf_steal(&b);
}

static void record_free(Record *r) {
    for (size_t i = 0; i < r->nparams; i++) {
        free(r->params[i].key);
        free(r->params[i].segment);
    }
    free(r->params);
    free(r->netloc);
    free(r->path);
    memset(r, 0, sizeof(*r));
}

static void record_add_param(Record *r, char *key, char *segment) {
    if (r->nparams == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 8;
        r->params = xrealloc(r->params, r->cap * sizeof(*r->params));
    }
    r->params[r->nparams].key = key;
    r->params[r->nparams].segment = segment;
    r->nparams++;
}

typedef struct {
    int drop_static;
    const char *const *static_exts;
    size_t nstatic_exts;
    int drop_blog_slugs;
    int min_hyphens;
    int strip_tracking;
    int keep_p_param;
    int prefer_https;
    int all_values;
    int sort;
    int stats;
    regex_t *extra;
    size_t nextra;
} Options;

static int process_query(Record *rec, const char *query, size_t qlen,
                         const Options *opt, int *reason) {
    const char *pos = query;
    const char *end = query + qlen;

    while (pos < end) {
        const char *amp = memchr(pos, '&', (size_t)(end - pos));
        const char *seg_end = amp ? amp : end;
        size_t seglen = (size_t)(seg_end - pos);

        if (seglen > 0) {
            const char *eq = memchr(pos, '=', seglen);
            size_t keylen = eq ? (size_t)(eq - pos) : seglen;
            char *key = decode_slice(pos, keylen, 1, 1);

            if (is_wp_query(key, opt->keep_p_param)) {
                free(key);
                *reason = R_WP_QUERY;
                return -1;
            }
            if (keep_param(key, opt->strip_tracking)) {
                size_t seglen_full = seglen;
                char *segment = xstrndup(pos, seglen_full);
                record_add_param(rec, key, segment);
            } else {
                free(key);
            }
        }

        if (!amp)
            break;
        pos = amp + 1;
    }
    return 0;
}

static int parse_record(const char *raw, const Options *opt, Record *rec,
                        int *reason) {
    memset(rec, 0, sizeof(*rec));

    const char *sep = strstr(raw, "://");
    if (!sep) {
        *reason = R_INVALID;
        return -1;
    }

    size_t slen = (size_t)(sep - raw);
    int http;
    if (slen == 4 && strncasecmp(raw, "http", 4) == 0)
        http = 1;
    else if (slen == 5 && strncasecmp(raw, "https", 5) == 0)
        http = 0;
    else {
        *reason = R_SCHEME;
        return -1;
    }
    rec->scheme = http ? "http" : "https";

    const char *auth = sep + 3;
    const char *p = auth;
    while (*p && *p != '/' && *p != '?' && *p != '#')
        p++;
    size_t alen = (size_t)(p - auth);
    if (alen == 0) {
        *reason = R_INVALID;
        return -1;
    }

    int ok = 0;
    char *netloc = build_netloc(auth, alen, http, &ok);
    if (!ok) {
        free(netloc);
        *reason = R_INVALID;
        return -1;
    }
    rec->netloc = netloc;

    const char *rest = p;
    const char *path_src;
    size_t path_len;
    if (*rest == '/') {
        const char *e = rest;
        while (*e && *e != '?' && *e != '#')
            e++;
        path_src = rest;
        path_len = (size_t)(e - rest);
        rest = e;
    } else {
        path_src = "/";
        path_len = 1;
    }
    rec->path = normalize_path(path_src, path_len);

    if (*rest == '?') {
        const char *q = rest + 1;
        const char *qe = q;
        while (*qe && *qe != '#')
            qe++;
        if (process_query(rec, q, (size_t)(qe - q), opt, reason) != 0) {
            record_free(rec);
            return -1;
        }
    }

    char *match = strchr(rec->path, '%')
                      ? decode_slice(rec->path, strlen(rec->path), 0, 0)
                      : rec->path;
    int match_alloc = match != rec->path;

    if (opt->drop_static &&
        path_is_static(match, opt->static_exts, opt->nstatic_exts)) {
        *reason = R_STATIC;
        goto drop;
    }
    if (regexec(&noise_re, match, 0, NULL, 0) == 0) {
        *reason = R_WP_PATH;
        goto drop;
    }
    if (opt->drop_blog_slugs &&
        looks_like_blog_slug(match, opt->min_hyphens)) {
        *reason = R_BLOG_SLUG;
        goto drop;
    }
    for (size_t i = 0; i < opt->nextra; i++) {
        if (regexec(&opt->extra[i], raw, 0, NULL, 0) == 0) {
            *reason = R_EXCLUDE;
            goto drop;
        }
    }

    if (match_alloc)
        free(match);
    return 0;

drop:
    if (match_alloc)
        free(match);
    record_free(rec);
    return -1;
}

/* ------------------------------ group table -------------------------------- */
typedef struct Group {
    const char *scheme;
    char *netloc;
    char *path;
    Param *params;
    size_t nparams;
    size_t cap;
    uint64_t hash;
    struct Group *next;
} Group;

typedef struct {
    Group **buckets;
    size_t nbuckets;
    Group **items;
    size_t count;
    size_t cap;
} GroupTable;

static uint64_t hash_key(const char *a, const char *b) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)a; *p; p++) {
        h ^= *p;
        h *= UINT64_C(1099511628211);
    }
    h ^= 0xff;
    h *= UINT64_C(1099511628211);
    for (const unsigned char *p = (const unsigned char *)b; *p; p++) {
        h ^= *p;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static void table_init(GroupTable *t) {
    t->nbuckets = 1024;
    t->buckets = xcalloc(t->nbuckets, sizeof(*t->buckets));
    t->items = NULL;
    t->count = 0;
    t->cap = 0;
}

static void table_rehash(GroupTable *t, size_t nbuckets) {
    Group **buckets = xcalloc(nbuckets, sizeof(*buckets));
    for (size_t i = 0; i < t->count; i++) {
        Group *g = t->items[i];
        size_t idx = (size_t)(g->hash & (nbuckets - 1));
        g->next = buckets[idx];
        buckets[idx] = g;
    }
    free(t->buckets);
    t->buckets = buckets;
    t->nbuckets = nbuckets;
}

static Group *table_get_or_add(GroupTable *t, const char *scheme, char *netloc,
                               char *path, int *created) {
    uint64_t hash = hash_key(netloc, path);
    size_t idx = (size_t)(hash & (t->nbuckets - 1));

    for (Group *g = t->buckets[idx]; g; g = g->next) {
        if (g->hash == hash && strcmp(g->netloc, netloc) == 0 &&
            strcmp(g->path, path) == 0) {
            *created = 0;
            return g;
        }
    }

    if (t->count + 1 > t->nbuckets * 3 / 4) {
        table_rehash(t, t->nbuckets * 2);
        idx = (size_t)(hash & (t->nbuckets - 1));
    }

    Group *g = xcalloc(1, sizeof(*g));
    g->scheme = scheme;
    g->netloc = netloc;
    g->path = path;
    g->hash = hash;
    g->next = t->buckets[idx];
    t->buckets[idx] = g;

    if (t->count == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 1024;
        t->items = xrealloc(t->items, t->cap * sizeof(*t->items));
    }
    t->items[t->count++] = g;
    *created = 1;
    return g;
}

static void group_add_param(Group *g, char *key, char *segment,
                            int all_values) {
    for (size_t i = 0; i < g->nparams; i++) {
        Param *p = &g->params[i];
        if (strcmp(p->key, key) == 0) {
            if (!all_values || strcmp(p->segment, segment) == 0) {
                free(key);
                free(segment);
                return;
            }
        }
    }

    if (g->nparams == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 8;
        g->params = xrealloc(g->params, g->cap * sizeof(*g->params));
    }
    g->params[g->nparams].key = key;
    g->params[g->nparams].segment = segment;
    g->nparams++;
}

static void group_to_buf(Buf *b, const Group *g) {
    buf_adds(b, g->scheme);
    buf_adds(b, "://");
    buf_adds(b, g->netloc);
    buf_adds(b, g->path);
    if (g->nparams > 0) {
        buf_addc(b, '?');
        for (size_t i = 0; i < g->nparams; i++) {
            if (i > 0)
                buf_addc(b, '&');
            buf_adds(b, g->params[i].segment);
        }
    }
}

static void table_free(GroupTable *t) {
    for (size_t i = 0; i < t->count; i++) {
        Group *g = t->items[i];
        for (size_t j = 0; j < g->nparams; j++) {
            free(g->params[j].key);
            free(g->params[j].segment);
        }
        free(g->params);
        free(g->netloc);
        free(g->path);
        free(g);
    }
    free(t->items);
    free(t->buckets);
}

static int cmp_line(const void *a, const void *b) {
    const char *const *pa = a;
    const char *const *pb = b;
    return strcmp(*pa, *pb);
}

/* --------------------------------- CLI ------------------------------------- */
static void usage(FILE *out) {
    fprintf(out,
        "Usage: dedupe [OPTIONS] [FILE]\n"
        "\n"
        "Dedupe URLs, drop blog-like/CMS noise, merge query params per URL.\n"
        "Reads stdin when FILE is omitted.\n"
        "\n"
        "Options:\n"
        "  -o, --output FILE              write to FILE (default: stdout)\n"
        "      --keep-static              do not drop static-asset URLs\n"
        "      --include-statics EXT      keep static extensions, e.g.\n"
        "                                 js,json,xml,txt (repeatable)\n"
        "      --drop-blog-slugs          drop slug-like blog posts\n"
        "      --min-hyphens N            hyphens required (default: 4)\n"
        "      --exclude REGEX            extra POSIX ERE drop rule (repeatable)\n"
        "      --exclude-case-sensitive   match --exclude case-sensitively\n"
        "      --keep-p-param             do not treat ?p= as a CMS/blog post\n"
        "      --no-prefer-https          do not upgrade http to https\n"
        "      --keep-tracking            keep utm_*/fbclid/etc. params\n"
        "      --all-values               keep all values for a repeated key\n"
        "      --sort                     sort final output\n"
        "      --stats                    print counts to stderr\n"
        "  -h, --help                     show this help\n");
}

static int parse_int(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    long value = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || value < -2147483647L ||
        value > 2147483647L)
        return -1;
    *out = (int)value;
    return 0;
}

static int str_in_vec(const char *s, const StrVec *v) {
    for (size_t i = 0; i < v->n; i++)
        if (strcmp(s, v->items[i]) == 0)
            return 1;
    return 0;
}

static void parse_include_arg(const char *arg, StrVec *out) {
    char *copy = xstrndup(arg, strlen(arg));
    char *save = NULL;
    for (char *tok = strtok_r(copy, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        char *s = tok;
        while (isspace((unsigned char)*s))
            s++;
        char *e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1]))
            *--e = '\0';
        while (*s == '.')
            s++;
        for (char *p = s; *p; p++)
            if ((unsigned char)*p < 128)
                *p = (char)tolower((unsigned char)*p);
        if (*s)
            strvec_push(out, xstrndup(s, strlen(s)));
    }
    free(copy);
}

enum {
    OPT_KEEP_STATIC = 1000,
    OPT_INCLUDE_STATICS,
    OPT_DROP_BLOG_SLUGS,
    OPT_MIN_HYPHENS,
    OPT_EXCLUDE,
    OPT_EXCLUDE_CASE_SENSITIVE,
    OPT_KEEP_P_PARAM,
    OPT_NO_PREFER_HTTPS,
    OPT_KEEP_TRACKING,
    OPT_ALL_VALUES,
    OPT_SORT,
    OPT_STATS,
};

int main(int argc, char **argv) {
    Options opt = {
        .drop_static = 1,
        .min_hyphens = 4,
        .strip_tracking = 1,
        .prefer_https = 1,
    };
    const char *infile = NULL;
    const char *outfile = NULL;
    StrVec include_args = {0};
    StrVec exclude_exprs = {0};
    int exclude_case_sensitive = 0;

    static const struct option long_opts[] = {
        {"output", required_argument, NULL, 'o'},
        {"keep-static", no_argument, NULL, OPT_KEEP_STATIC},
        {"include-statics", required_argument, NULL, OPT_INCLUDE_STATICS},
        {"drop-blog-slugs", no_argument, NULL, OPT_DROP_BLOG_SLUGS},
        {"min-hyphens", required_argument, NULL, OPT_MIN_HYPHENS},
        {"exclude", required_argument, NULL, OPT_EXCLUDE},
        {"exclude-case-sensitive", no_argument, NULL,
         OPT_EXCLUDE_CASE_SENSITIVE},
        {"keep-p-param", no_argument, NULL, OPT_KEEP_P_PARAM},
        {"no-prefer-https", no_argument, NULL, OPT_NO_PREFER_HTTPS},
        {"keep-tracking", no_argument, NULL, OPT_KEEP_TRACKING},
        {"all-values", no_argument, NULL, OPT_ALL_VALUES},
        {"sort", no_argument, NULL, OPT_SORT},
        {"stats", no_argument, NULL, OPT_STATS},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "ho:", long_opts, NULL)) != -1) {
        switch (c) {
        case 'o':
            outfile = optarg;
            break;
        case 'h':
            usage(stdout);
            return 0;
        case OPT_KEEP_STATIC:
            opt.drop_static = 0;
            break;
        case OPT_INCLUDE_STATICS:
            strvec_push(&include_args, xstrndup(optarg, strlen(optarg)));
            break;
        case OPT_DROP_BLOG_SLUGS:
            opt.drop_blog_slugs = 1;
            break;
        case OPT_MIN_HYPHENS:
            if (parse_int(optarg, &opt.min_hyphens) != 0) {
                fprintf(stderr, "dedup: invalid --min-hyphens %s\n", optarg);
                return 2;
            }
            break;
        case OPT_EXCLUDE:
            strvec_push(&exclude_exprs, xstrndup(optarg, strlen(optarg)));
            break;
        case OPT_EXCLUDE_CASE_SENSITIVE:
            exclude_case_sensitive = 1;
            break;
        case OPT_KEEP_P_PARAM:
            opt.keep_p_param = 1;
            break;
        case OPT_NO_PREFER_HTTPS:
            opt.prefer_https = 0;
            break;
        case OPT_KEEP_TRACKING:
            opt.strip_tracking = 0;
            break;
        case OPT_ALL_VALUES:
            opt.all_values = 1;
            break;
        case OPT_SORT:
            opt.sort = 1;
            break;
        case OPT_STATS:
            opt.stats = 1;
            break;
        default:
            usage(stderr);
            return 2;
        }
    }

    if (optind < argc)
        infile = argv[optind++];
    if (optind < argc) {
        fputs("dedup: too many input files\n", stderr);
        return 2;
    }

    StrVec include_exts = {0};
    for (size_t i = 0; i < include_args.n; i++)
        parse_include_arg(include_args.items[i], &include_exts);

    const char *drop_exts[N_STATIC_EXTS];
    size_t ndrop_exts = 0;
    for (size_t i = 0; i < N_STATIC_EXTS; i++)
        if (!str_in_vec(STATIC_EXTS[i], &include_exts))
            drop_exts[ndrop_exts++] = STATIC_EXTS[i];
    opt.static_exts = drop_exts;
    opt.nstatic_exts = ndrop_exts;

    int rc = regcomp(&noise_re, NOISE_PATH_PATTERN,
                     REG_EXTENDED | REG_ICASE | REG_NOSUB);
    if (rc != 0) {
        char err[256];
        regerror(rc, &noise_re, err, sizeof(err));
        fprintf(stderr, "dedup: internal filter regex: %s\n", err);
        return 1;
    }

    if (exclude_exprs.n > 0) {
        int flags = REG_EXTENDED | REG_NOSUB;
        if (!exclude_case_sensitive)
            flags |= REG_ICASE;
        opt.extra = xcalloc(exclude_exprs.n, sizeof(*opt.extra));
        for (size_t i = 0; i < exclude_exprs.n; i++) {
            rc = regcomp(&opt.extra[i], exclude_exprs.items[i], flags);
            if (rc != 0) {
                char err[256];
                regerror(rc, &opt.extra[i], err, sizeof(err));
                fprintf(stderr, "dedup: invalid --exclude %s: %s\n",
                        exclude_exprs.items[i], err);
                return 2;
            }
        }
        opt.nextra = exclude_exprs.n;
    }

    FILE *src = stdin;
    if (infile) {
        src = fopen(infile, "r");
        if (!src) {
            fprintf(stderr, "dedup: cannot open %s: %s\n", infile,
                    strerror(errno));
            return 2;
        }
    }

    GroupTable table;
    table_init(&table);
    size_t stats[R_COUNT] = {0};
    size_t input_count = 0;
    size_t survivors = 0;

    char *line = NULL;
    size_t linecap = 0;
    ssize_t got;
    while ((got = getline(&line, &linecap, src)) != -1) {
        (void)got;
        char *start = line;
        char *end = line + strlen(line);
        while (start < end && isspace((unsigned char)*start))
            start++;
        while (end > start && isspace((unsigned char)end[-1]))
            end--;
        if (start == end)
            continue;
        *end = '\0';
        input_count++;

        Record rec;
        int reason = R_INVALID;
        if (parse_record(start, &opt, &rec, &reason) != 0) {
            stats[reason]++;
            continue;
        }
        survivors++;

        int created = 0;
        Group *g = table_get_or_add(&table, rec.scheme, rec.netloc, rec.path,
                                    &created);
        if (!created) {
            free(rec.netloc);
            free(rec.path);
        }
        if (opt.prefer_https && strcmp(g->scheme, "http") == 0 &&
            strcmp(rec.scheme, "https") == 0)
            g->scheme = "https";

        for (size_t i = 0; i < rec.nparams; i++)
            group_add_param(g, rec.params[i].key, rec.params[i].segment,
                            opt.all_values);
        free(rec.params);
    }
    free(line);

    int read_failed = ferror(src);
    if (infile && fclose(src) != 0)
        read_failed = 1;
    if (read_failed) {
        fprintf(stderr, "dedup: read error: %s\n", strerror(errno));
        table_free(&table);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    FILE *dst = stdout;
    if (outfile) {
        dst = fopen(outfile, "w");
        if (!dst) {
            fprintf(stderr, "dedup: cannot open %s: %s\n", outfile,
                    strerror(errno));
            table_free(&table);
            return 2;
        }
    }

    if (opt.sort) {
        char **lines = xmalloc(table.count * sizeof(*lines));
        for (size_t i = 0; i < table.count; i++) {
            Buf b = {0};
            group_to_buf(&b, table.items[i]);
            lines[i] = buf_steal(&b);
        }
        qsort(lines, table.count, sizeof(*lines), cmp_line);
        for (size_t i = 0; i < table.count; i++) {
            fputs(lines[i], dst);
            fputc('\n', dst);
            free(lines[i]);
        }
        free(lines);
    } else {
        for (size_t i = 0; i < table.count; i++) {
            Buf b = {0};
            group_to_buf(&b, table.items[i]);
            fwrite(b.data, 1, b.len, dst);
            fputc('\n', dst);
            free(b.data);
        }
    }

    int write_failed = ferror(dst);
    int saved_errno = errno;
    if (dst != stdout && fclose(dst) != 0)
        write_failed = 1;
    if (write_failed && saved_errno != EPIPE) {
        fprintf(stderr, "dedup: write error: %s\n", strerror(saved_errno));
        table_free(&table);
        return 1;
    }

    if (opt.stats) {
        size_t dropped = 0;
        for (int i = 0; i < R_COUNT; i++)
            dropped += stats[i];
        fprintf(stderr,
                "input: %zu  filtered: %zu  dropped: %zu (",
                input_count, survivors, dropped);
        int first = 1;
        for (int i = 0; i < R_COUNT; i++) {
            if (!stats[i])
                continue;
            if (!first)
                fputs("  ", stderr);
            fprintf(stderr, "%s: %zu", REASON_NAMES[i], stats[i]);
            first = 0;
        }
        if (first)
            fputs("none", stderr);
        fprintf(stderr, ")  after dedupe: %zu\n", table.count);
    }

    table_free(&table);
    regfree(&noise_re);
    for (size_t i = 0; i < opt.nextra; i++)
        regfree(&opt.extra[i]);
    free(opt.extra);
    for (size_t i = 0; i < include_args.n; i++)
        free(include_args.items[i]);
    free(include_args.items);
    for (size_t i = 0; i < include_exts.n; i++)
        free(include_exts.items[i]);
    free(include_exts.items);
    for (size_t i = 0; i < exclude_exprs.n; i++)
        free(exclude_exprs.items[i]);
    free(exclude_exprs.items);

    return 0;
}
