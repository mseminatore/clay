/*
 * bricks.c - Clay package manager (Bricks)
 *
 * Parses brick.json manifests and implements bricks subcommands:
 *   clay bricks init
 *   clay bricks add <url>
 *   clay bricks install
 *   clay bricks update
 *   clay bricks list
 *   clay bricks remove <name>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#ifdef _MSC_VER
#define strdup _strdup
#endif
#include "bricks.h"

/* --- Input validation ---
 *
 * Brick names, URLs and refs come from brick.json files — including the
 * manifests of transitively-installed bricks, i.e. third-party content — and
 * are interpolated into shell commands (git clone / checkout, rm -rf) and
 * filesystem paths. Restrict them to a conservative character set so they can
 * never break out of the quoted arguments or escape the bricks/ directory.
 */

/* One or more path segments of [A-Za-z0-9._-]; '/' separates segments when
 * allow_slash is set. Rejects empty segments, "." / ".." segments, and a
 * leading '-' (which git would read as an option). */
static int brick_safe_path(const char *s, int allow_slash) {
    if (!s || !*s || *s == '-' || *s == '/') return 0;
    const char *seg = s;
    for (const char *c = s; ; c++) {
        if (*c == '/' || *c == '\0') {
            size_t len = (size_t)(c - seg);
            if (len == 0) return 0;
            if (len == 1 && seg[0] == '.') return 0;
            if (len == 2 && seg[0] == '.' && seg[1] == '.') return 0;
            if (*c == '\0') return 1;
            if (!allow_slash) return 0;
            seg = c + 1;
            continue;
        }
        char ch = *c;
        int ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                 (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) return 0;
    }
}

/* A dependency URL: [https://|http://]host/owner/repo[/subpath][@ref] where
 * every part is a safe path (host may also contain ':' for a port). */
static int brick_safe_url(const char *url) {
    if (!url || !*url) return 0;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;

    char tmp[BRICK_MAX_PATH];
    size_t n = strlen(p);
    if (n >= sizeof(tmp)) return 0;
    memcpy(tmp, p, n + 1);

    char *at = strchr(tmp, '@');
    if (at) {
        *at = '\0';
        if (!brick_safe_path(at + 1, 1)) return 0;   /* ref: tag, branch or SHA */
    }
    /* host[:port] */
    char *slash = strchr(tmp, '/');
    if (!slash) return 0;
    *slash = '\0';
    char *colon = strchr(tmp, ':');
    if (colon) {
        *colon = '\0';
        for (const char *c = colon + 1; *c; c++)
            if (*c < '0' || *c > '9') return 0;
        if (!colon[1]) return 0;
    }
    if (!brick_safe_path(tmp, 0)) return 0;
    /* owner/repo[/subpath] */
    return brick_safe_path(slash + 1, 1);
}

/* --- Minimal JSON-subset parser for brick.json --- */

/* Skip whitespace */
static const char *skip_ws(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return s;
}

/* Parse a JSON string (no escape handling needed for our simple format).
 * Returns pointer past closing quote, or NULL on error. */
static const char *parse_string(const char *s, char *out, size_t out_sz) {
    if (*s != '"') return NULL;
    s++;
    size_t i = 0;
    while (*s && *s != '"') {
        if (*s == '\\' && *(s + 1)) {
            s++; /* skip escaped char */
        }
        if (i < out_sz - 1) out[i++] = *s;
        s++;
    }
    out[i] = '\0';
    if (*s == '"') s++;
    return s;
}

/* Parse brick.json content into a BrickManifest */
static int parse_manifest(const char *json, BrickManifest *m) {
    memset(m, 0, sizeof(*m));
    const char *p = skip_ws(json);
    if (*p != '{') return -1;
    p = skip_ws(p + 1);

    while (*p && *p != '}') {
        /* Parse key */
        char key[128] = "";
        p = parse_string(p, key, sizeof(key));
        if (!p) return -1;
        p = skip_ws(p);
        if (*p != ':') return -1;
        p = skip_ws(p + 1);

        if (strcmp(key, "name") == 0) {
            p = parse_string(p, m->name, sizeof(m->name));
        } else if (strcmp(key, "version") == 0) {
            p = parse_string(p, m->version, sizeof(m->version));
        } else if (strcmp(key, "description") == 0) {
            p = parse_string(p, m->description, sizeof(m->description));
        } else if (strcmp(key, "main") == 0) {
            p = parse_string(p, m->main_file, sizeof(m->main_file));
        } else if (strcmp(key, "deps") == 0) {
            /* Parse deps object */
            if (*p != '{') return -1;
            p = skip_ws(p + 1);
            while (*p && *p != '}') {
                if (m->dep_count >= BRICK_MAX_DEPS) return -1;
                BrickDep *dep = &m->deps[m->dep_count];
                p = parse_string(p, dep->name, sizeof(dep->name));
                if (!p) return -1;
                p = skip_ws(p);
                if (*p != ':') return -1;
                p = skip_ws(p + 1);
                p = parse_string(p, dep->url, sizeof(dep->url));
                if (!p) return -1;
                if (!brick_safe_path(dep->name, 1) || !brick_safe_url(dep->url)) {
                    fprintf(stderr, "error: brick.json: rejecting unsafe dependency "
                            "\"%s\": \"%s\" (names and URLs may only contain "
                            "letters, digits, '.', '_', '-', '/' and an optional @ref)\n",
                            dep->name, dep->url);
                    return -1;
                }
                m->dep_count++;
                p = skip_ws(p);
                if (*p == ',') p = skip_ws(p + 1);
            }
            if (*p == '}') p++;
        } else {
            /* Skip unknown value */
            if (*p == '"') {
                char discard[1024];
                p = parse_string(p, discard, sizeof(discard));
            } else {
                /* Skip non-string value */
                while (*p && *p != ',' && *p != '}') p++;
            }
        }
        if (!p) return -1;
        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
    }
    return 0;
}

int brick_parse(const char *path, BrickManifest *out) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    int rc = parse_manifest(buf, out);
    free(buf);
    return rc == 0 ? 0 : -2;  /* -1: file missing, -2: invalid manifest */
}

int brick_write(const char *path, const BrickManifest *m) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "{\n");
    fprintf(f, "  \"name\": \"%s\",\n", m->name);
    fprintf(f, "  \"version\": \"%s\",\n", m->version);
    fprintf(f, "  \"description\": \"%s\",\n", m->description);
    fprintf(f, "  \"main\": \"%s\",\n", m->main_file);
    fprintf(f, "  \"deps\": {");
    for (int i = 0; i < m->dep_count; i++) {
        fprintf(f, "%s\n    \"%s\": \"%s\"",
                i > 0 ? "," : "", m->deps[i].name, m->deps[i].url);
    }
    if (m->dep_count > 0) fprintf(f, "\n  ");
    fprintf(f, "}\n}\n");
    fclose(f);
    return 0;
}

/* --- Lockfile support --- */

#define BRICK_LOCK_VERSION 1
#define BRICK_LOCK_FILE "brick.lock"

/* Forward declarations for functions used by lockfile code */
static void split_url_parts(const char *url,
                             char *clone_url, size_t url_sz,
                             char *subpath, size_t sub_sz,
                             char *ref, size_t ref_sz);
static void url_to_repo_name(const char *url, char *name, size_t sz);
static void dep_repo_dir(const char *url, const char *name, char *out, size_t sz);

typedef struct {
    char name[BRICK_MAX_NAME];
    char url[BRICK_MAX_PATH];
    char version[64];
    char resolved[128]; /* git commit SHA or empty for local */
} BrickLockEntry;

typedef struct {
    int lockfile_version;
    BrickLockEntry entries[BRICK_MAX_DEPS];
    int entry_count;
} BrickLock;

/* Parse a brick.lock file. Returns 0 on success, -1 on error. */
static int parse_lock(const char *json, BrickLock *lock) {
    memset(lock, 0, sizeof(*lock));
    lock->lockfile_version = 0;
    const char *p = skip_ws(json);
    if (*p != '{') return -1;
    p = skip_ws(p + 1);

    while (*p && *p != '}') {
        char key[128] = "";
        p = parse_string(p, key, sizeof(key));
        if (!p) return -1;
        p = skip_ws(p);
        if (*p != ':') return -1;
        p = skip_ws(p + 1);

        if (strcmp(key, "lockfile_version") == 0) {
            /* Parse integer */
            lock->lockfile_version = (int)strtol(p, (char **)&p, 10);
        } else if (strcmp(key, "locked") == 0) {
            if (*p != '{') return -1;
            p = skip_ws(p + 1);
            while (*p && *p != '}') {
                if (lock->entry_count >= BRICK_MAX_DEPS) return -1;
                BrickLockEntry *e = &lock->entries[lock->entry_count];
                p = parse_string(p, e->name, sizeof(e->name));
                if (!p) return -1;
                p = skip_ws(p);
                if (*p != ':') return -1;
                p = skip_ws(p + 1);
                /* Parse entry object */
                if (*p != '{') return -1;
                p = skip_ws(p + 1);
                while (*p && *p != '}') {
                    char ekey[64] = "";
                    p = parse_string(p, ekey, sizeof(ekey));
                    if (!p) return -1;
                    p = skip_ws(p);
                    if (*p != ':') return -1;
                    p = skip_ws(p + 1);
                    if (strcmp(ekey, "url") == 0) {
                        p = parse_string(p, e->url, sizeof(e->url));
                    } else if (strcmp(ekey, "version") == 0) {
                        p = parse_string(p, e->version, sizeof(e->version));
                    } else if (strcmp(ekey, "resolved") == 0) {
                        p = parse_string(p, e->resolved, sizeof(e->resolved));
                    } else {
                        /* Skip unknown value */
                        if (*p == '"') {
                            char discard[1024];
                            p = parse_string(p, discard, sizeof(discard));
                        } else {
                            while (*p && *p != ',' && *p != '}') p++;
                        }
                    }
                    if (!p) return -1;
                    p = skip_ws(p);
                    if (*p == ',') p = skip_ws(p + 1);
                }
                if (*p == '}') p++;
                lock->entry_count++;
                p = skip_ws(p);
                if (*p == ',') p = skip_ws(p + 1);
            }
            if (*p == '}') p++;
        } else {
            /* Skip unknown value */
            if (*p == '"') {
                char discard[1024];
                p = parse_string(p, discard, sizeof(discard));
            } else {
                while (*p && *p != ',' && *p != '}') p++;
            }
        }
        if (!p) return -1;
        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
    }
    return 0;
}

static int bricks_read_lock(BrickLock *lock) {
    FILE *f = fopen(BRICK_LOCK_FILE, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    int rc = parse_lock(buf, lock);
    free(buf);
    return rc;
}

static int bricks_has_lock(void) {
    struct stat st;
    return stat(BRICK_LOCK_FILE, &st) == 0;
}

/* Run `git -C <dir> <args>` and capture the first line of its output.
 * Returns 0 on success, -1 if git failed or printed nothing. */
static int git_output(const char *dir, const char *args, char *out, size_t out_sz) {
    char cmd[2048];
    char tmp_file[BRICK_MAX_PATH];
    const char *null_device;
    out[0] = '\0';

#ifdef _WIN32
    null_device = "nul";
#else
    null_device = "/dev/null";
#endif

    snprintf(tmp_file, sizeof(tmp_file), "bricks/.brick_git_tmp");
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" %s > \"%s\" 2>%s",
             dir, args, tmp_file, null_device);

    int rc = system(cmd);
    FILE *f = fopen(tmp_file, "r");
    if (f) {
        if (rc == 0 && fgets(out, (int)out_sz, f)) {
            size_t len = strlen(out);
            while (len > 0 && (out[len-1] == '\n' || out[len-1] == '\r'))
                out[--len] = '\0';
        }
        fclose(f);
        remove(tmp_file);
    }
    return (rc == 0 && out[0] != '\0') ? 0 : -1;
}

/* Get the resolved git commit SHA for a dependency directory.
 * Returns 0 on success, -1 if not a git repo or error. */
static int get_dep_resolved(const char *dep_dir, char *out, size_t out_sz) {
    return git_output(dep_dir, "rev-parse HEAD", out, out_sz);
}

/* Write brick.lock from the current manifest and installed deps. */
static void bricks_write_lock(const BrickManifest *m) {
    FILE *f = fopen(BRICK_LOCK_FILE, "w");
    if (!f) {
        fprintf(stderr, "warning: could not write %s\n", BRICK_LOCK_FILE);
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"lockfile_version\": %d,\n", BRICK_LOCK_VERSION);
    fprintf(f, "  \"locked\": {");

    for (int i = 0; i < m->dep_count; i++) {
        char clone_url[BRICK_MAX_PATH], subpath[BRICK_MAX_PATH], ref[256];
        split_url_parts(m->deps[i].url, clone_url, sizeof(clone_url),
                        subpath, sizeof(subpath), ref, sizeof(ref));

        /* Determine the git directory for this dep */
        char git_dir[BRICK_MAX_PATH];
        dep_repo_dir(m->deps[i].url, m->deps[i].name, git_dir, sizeof(git_dir));

        /* Try to resolve commit SHA */
        char resolved[128] = "";
        get_dep_resolved(git_dir, resolved, sizeof(resolved));

        fprintf(f, "%s\n    \"%s\": {\n", i > 0 ? "," : "", m->deps[i].name);
        fprintf(f, "      \"url\": \"%s\",\n", m->deps[i].url);
        fprintf(f, "      \"version\": \"%s\",\n", ref[0] ? ref : "latest");
        fprintf(f, "      \"resolved\": \"%s\"\n", resolved);
        fprintf(f, "    }");
    }

    if (m->dep_count > 0) fprintf(f, "\n  ");
    fprintf(f, "}\n}\n");
    fclose(f);
    printf("Wrote %s\n", BRICK_LOCK_FILE);
}

/* Check if the lock matches the current manifest.
 * Returns 1 if lock is up-to-date, 0 if not. */
static int bricks_lock_matches(const BrickManifest *m, const BrickLock *lock) {
    if (lock->lockfile_version != BRICK_LOCK_VERSION) return 0;
    if (lock->entry_count != m->dep_count) return 0;

    for (int i = 0; i < m->dep_count; i++) {
        int found = 0;
        for (int j = 0; j < lock->entry_count; j++) {
            if (strcmp(m->deps[i].name, lock->entries[j].name) == 0 &&
                strcmp(m->deps[i].url, lock->entries[j].url) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

/* --- Subcommands --- */

static void make_dir(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

/* Split URL into clone URL and ref: "github.com/user/lib@v1.0" → "https://github.com/user/lib", "v1.0" */
static void split_url_ref(const char *url, char *clone_url, size_t url_sz,
                           char *ref, size_t ref_sz) {
    strncpy(clone_url, url, url_sz - 1);
    clone_url[url_sz - 1] = '\0';
    ref[0] = '\0';

    char *at = strchr(clone_url, '@');
    if (at) {
        strncpy(ref, at + 1, ref_sz - 1);
        ref[ref_sz - 1] = '\0';
        *at = '\0';
    }

    /* Prepend https:// if not already a full URL */
    if (strncmp(clone_url, "http://", 7) != 0 &&
        strncmp(clone_url, "https://", 8) != 0 &&
        strncmp(clone_url, "git@", 4) != 0) {
        char tmp[BRICK_MAX_PATH];
        snprintf(tmp, sizeof(tmp), "https://%s", clone_url);
        strncpy(clone_url, tmp, url_sz - 1);
    }
}

/*
 * Split a monorepo URL into repo URL, subpath, and ref.
 * "github.com/user/repo/sub/path@v1.0" →
 *   clone_url = "https://github.com/user/repo"
 *   subpath   = "sub/path"
 *   ref       = "v1.0"
 *
 * URL format: host/owner/repo[/subpath][@ref]
 * The first 3 slash-separated segments (after stripping protocol) are the repo.
 */
static void split_url_parts(const char *url,
                             char *clone_url, size_t url_sz,
                             char *subpath, size_t sub_sz,
                             char *ref, size_t ref_sz) {
    char tmp[BRICK_MAX_PATH];
    strncpy(tmp, url, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    /* Strip @ref */
    ref[0] = '\0';
    char *at = strchr(tmp, '@');
    if (at) {
        strncpy(ref, at + 1, ref_sz - 1);
        ref[ref_sz - 1] = '\0';
        *at = '\0';
    }

    /* Strip protocol prefix for segment counting */
    const char *path_start = tmp;
    const char *prefix = "";
    if (strncmp(tmp, "https://", 8) == 0) { path_start = tmp + 8; prefix = "https://"; }
    else if (strncmp(tmp, "http://", 7) == 0) { path_start = tmp + 7; prefix = "http://"; }
    else { prefix = "https://"; }

    /* Count 3 segments: host/owner/repo */
    int slashes = 0;
    const char *p = path_start;
    const char *repo_end = NULL;
    while (*p) {
        if (*p == '/') {
            slashes++;
            if (slashes == 3) { repo_end = p; break; }
        }
        p++;
    }

    subpath[0] = '\0';
    if (repo_end) {
        /* Have subpath after 3rd slash */
        size_t repo_len = (size_t)(repo_end - path_start);
        snprintf(clone_url, url_sz, "%s%.*s", prefix, (int)repo_len, path_start);
        if (*(repo_end + 1)) {
            strncpy(subpath, repo_end + 1, sub_sz - 1);
            subpath[sub_sz - 1] = '\0';
        }
    } else {
        /* No subpath — simple repo URL */
        snprintf(clone_url, url_sz, "%s%s", prefix, path_start);
    }
}

/* Extract repo name from URL path (3rd segment): "github.com/user/my-repo/..." → "my-repo" */
static void url_to_repo_name(const char *url, char *name, size_t sz) {
    char tmp[BRICK_MAX_PATH];
    strncpy(tmp, url, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *at = strchr(tmp, '@');
    if (at) *at = '\0';

    /* Strip protocol */
    const char *p = tmp;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;

    /* Find 3rd segment (repo name) = between 2nd and 3rd slash */
    int slashes = 0;
    const char *seg_start = NULL;
    const char *seg_end = NULL;
    for (const char *c = p; *c; c++) {
        if (*c == '/') {
            slashes++;
            if (slashes == 2) seg_start = c + 1;
            if (slashes == 3) { seg_end = c; break; }
        }
    }
    if (seg_start && !seg_end) seg_end = p + strlen(p);
    if (seg_start && seg_end > seg_start) {
        size_t len = (size_t)(seg_end - seg_start);
        if (len >= sz) len = sz - 1;
        memcpy(name, seg_start, len);
        name[len] = '\0';
    } else {
        /* Fallback: last segment */
        const char *last = strrchr(tmp, '/');
        strncpy(name, last ? last + 1 : tmp, sz - 1);
        name[sz - 1] = '\0';
    }
}

/* Extract brick name from URL.
 * Simple: "github.com/user/my-lib@v1.0" → "my-lib"
 * Monorepo: "github.com/user/stdlib/match@v1.0" → "stdlib/match" */
static void url_to_name(const char *url, char *name, size_t sz) {
    char clone_url[BRICK_MAX_PATH], subpath[BRICK_MAX_PATH], ref[256];
    split_url_parts(url, clone_url, sizeof(clone_url),
                    subpath, sizeof(subpath), ref, sizeof(ref));

    if (subpath[0]) {
        /* Monorepo: name = repo-name/subpath */
        char repo[BRICK_MAX_NAME];
        url_to_repo_name(url, repo, sizeof(repo));
        snprintf(name, sz, "%s/%s", repo, subpath);
    } else {
        /* Simple: last segment of URL */
        char tmp[BRICK_MAX_PATH];
        strncpy(tmp, url, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char *at = strchr(tmp, '@');
        if (at) *at = '\0';
        const char *last = strrchr(tmp, '/');
        if (last) last++;
        else last = tmp;
        strncpy(name, last, sz - 1);
        name[sz - 1] = '\0';
    }
}

/* The directory holding a dependency's git checkout. A dependency's name is
 * its import path, so the name decides where it lives, not the repository:
 * a single brick "json" is bricks/json, and a monorepo sub-brick "std/json"
 * lives in the checkout bricks/std, whatever the repository is called. A
 * monorepo name without a slash falls back to the repository's name. */
static void dep_repo_dir(const char *url, const char *name, char *out, size_t sz) {
    char clone_url[BRICK_MAX_PATH], subpath[BRICK_MAX_PATH], ref[256];
    split_url_parts(url, clone_url, sizeof(clone_url),
                    subpath, sizeof(subpath), ref, sizeof(ref));
    const char *slash = strchr(name, '/');
    if (!subpath[0]) {
        snprintf(out, sz, "bricks/%s", name);
    } else if (slash) {
        snprintf(out, sz, "bricks/%.*s", (int)(slash - name), name);
    } else {
        char repo_name[BRICK_MAX_NAME];
        url_to_repo_name(url, repo_name, sizeof(repo_name));
        snprintf(out, sz, "bricks/%s", repo_name);
    }
}

/* Compare two git URLs ignoring the protocol, a trailing ".git" and '/' */
static int same_repo_url(const char *a, const char *b) {
    const char *ua[2] = { a, b };
    char norm[2][BRICK_MAX_PATH];
    for (int k = 0; k < 2; k++) {
        const char *p = ua[k];
        if (strncmp(p, "https://", 8) == 0) p += 8;
        else if (strncmp(p, "http://", 7) == 0) p += 7;
        strncpy(norm[k], p, BRICK_MAX_PATH - 1);
        norm[k][BRICK_MAX_PATH - 1] = '\0';
        size_t n = strlen(norm[k]);
        while (n > 0 && norm[k][n-1] == '/') norm[k][--n] = '\0';
        if (n > 4 && strcmp(norm[k] + n - 4, ".git") == 0) norm[k][n-4] = '\0';
    }
    return strcmp(norm[0], norm[1]) == 0;
}

/* An existing checkout at `dir` must come from `clone_url`: two different
 * repositories installed under the same name would otherwise share it.
 * Returns 0 if it matches (or its origin can't be read), -1 on a clash. */
static int check_repo_origin(const char *dir, const char *clone_url, const char *name) {
    char origin[BRICK_MAX_PATH];
    if (git_output(dir, "config --get remote.origin.url", origin, sizeof(origin)) != 0)
        return 0;
    if (same_repo_url(origin, clone_url)) return 0;
    fprintf(stderr, "  error: %s is already installed from %s, not %s\n"
                    "         (give one of them another name: clay bricks add <url> <name>)\n",
            dir, origin, clone_url);
    (void)name;
    return -1;
}

/* Lock file consulted by install_dep() to pin each dependency to the commit
 * recorded in brick.lock (set by cmd_install when a matching lock exists). */
static const BrickLock *g_install_lock = NULL;

/* Names already visited during this install — guards against dependency cycles */
#define BRICK_MAX_VISITED (BRICK_MAX_DEPS * 4)
static char g_visited[BRICK_MAX_VISITED][BRICK_MAX_NAME];
static int  g_visited_count = 0;

static int brick_mark_visited(const char *name) {
    for (int i = 0; i < g_visited_count; i++)
        if (strcmp(g_visited[i], name) == 0) return 0;   /* already seen */
    if (g_visited_count < BRICK_MAX_VISITED) {
        strncpy(g_visited[g_visited_count], name, BRICK_MAX_NAME - 1);
        g_visited[g_visited_count][BRICK_MAX_NAME - 1] = '\0';
        g_visited_count++;
    }
    return 1;
}

/* The locked commit SHA for (name, url), or NULL if not locked */
static const char *brick_locked_sha(const char *name, const char *url) {
    if (!g_install_lock) return NULL;
    for (int i = 0; i < g_install_lock->entry_count; i++) {
        const BrickLockEntry *e = &g_install_lock->entries[i];
        if (strcmp(e->name, name) != 0 || strcmp(e->url, url) != 0) continue;
        size_t n = strlen(e->resolved);
        if (n < 7 || n > 64) return NULL;
        for (size_t k = 0; k < n; k++) {
            char c = e->resolved[k];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return NULL;
        }
        return e->resolved;
    }
    return NULL;
}

static int install_dep(const char *url, const char *name);

/* Install the dependencies listed in <dir>/brick.json, if any */
static int install_subdeps(const char *dir, const char *name) {
    char sub_manifest[BRICK_MAX_PATH];
    snprintf(sub_manifest, sizeof(sub_manifest), "%s/brick.json", dir);
    BrickManifest sub;
    int prc = brick_parse(sub_manifest, &sub);
    if (prc == -1) return 0;               /* no manifest: nothing to do */
    if (prc != 0) {
        fprintf(stderr, "  error: invalid brick.json in %s\n", name);
        return -1;
    }
    int failed = 0;
    if (sub.dep_count > 0) {
        printf("  Installing dependencies of %s...\n", name);
        for (int i = 0; i < sub.dep_count; i++)
            if (install_dep(sub.deps[i].url, sub.deps[i].name) != 0) failed++;
    }
    return failed ? -1 : 0;
}

/* Check out `ref` (or the locked commit, which takes precedence) in `dir` */
static void brick_checkout(const char *dir, const char *name, const char *url,
                           const char *ref) {
    const char *target = brick_locked_sha(name, url);
    if (!target && ref[0]) target = ref;
    if (!target) return;
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "git -C \"%s\" checkout --quiet \"%s\"", dir, target);
    if (system(cmd) != 0)
        fprintf(stderr, "  warning: could not checkout '%s' for %s\n", target, name);
}

/* Install a single dependency (and, recursively, its dependencies).
 * Returns 0 on success. */
static int install_dep(const char *url, const char *name) {
    if (!brick_mark_visited(name)) return 0;   /* cycle or diamond: done already */

    char clone_url[BRICK_MAX_PATH], subpath[BRICK_MAX_PATH], ref[256];
    split_url_parts(url, clone_url, sizeof(clone_url),
                    subpath, sizeof(subpath), ref, sizeof(ref));

    if (subpath[0]) {
        /* Monorepo: one checkout (named by the dependency, see dep_repo_dir)
         * shared by its sub-bricks, each at its subpath */
        char repo_dest[BRICK_MAX_PATH];
        dep_repo_dir(url, name, repo_dest, sizeof(repo_dest));

        struct stat st;
        if (stat(repo_dest, &st) != 0) {
            /* Clone the repo (first sub-brick from this repo) */
            char cmd[2048];
            snprintf(cmd, sizeof(cmd), "git clone --quiet \"%s\" \"%s\"",
                     clone_url, repo_dest);
            printf("  Cloning %s into %s...\n", clone_url, repo_dest);
            if (system(cmd) != 0) {
                fprintf(stderr, "  error: failed to clone %s\n", clone_url);
                return -1;
            }
            brick_checkout(repo_dest, name, url, ref);
        } else if (check_repo_origin(repo_dest, clone_url, name) != 0) {
            return -1;
        }

        /* Verify the sub-brick exists */
        char brick_dir[BRICK_MAX_PATH * 2];
        snprintf(brick_dir, sizeof(brick_dir), "%s/%s", repo_dest, subpath);
        struct stat st2;
        if (stat(brick_dir, &st2) != 0) {
            fprintf(stderr, "  error: sub-brick '%s' not found in %s\n",
                    subpath, clone_url);
            return -1;
        }

        /* The import path is the directory under bricks/ */
        const char *slash = strchr(name, '/');
        if (slash && strcmp(slash + 1, subpath) != 0)
            fprintf(stderr, "  warning: '%s' is installed at %s; import it as \"%s\"\n",
                    name, brick_dir, brick_dir + strlen("bricks/"));

        printf("  Installed %s\n", name);
        return install_subdeps(brick_dir, name);
    }

    /* Simple single-brick repo */
    char dest[BRICK_MAX_PATH];
    snprintf(dest, sizeof(dest), "bricks/%s", name);

    struct stat st;
    if (stat(dest, &st) == 0) {
        if (check_repo_origin(dest, clone_url, name) != 0) return -1;
        printf("  %s: already installed\n", name);
        return 0;
    }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "git clone --quiet \"%s\" \"%s\"", clone_url, dest);
    printf("  Installing %s from %s...\n", name, clone_url);
    if (system(cmd) != 0) {
        fprintf(stderr, "  error: failed to clone %s\n", clone_url);
        return -1;
    }
    brick_checkout(dest, name, url, ref);
    return install_subdeps(dest, name);
}

/* Directory a dependency is installed into (bricks/<name> or <checkout>/<sub>) */
static void brick_install_dir(const char *url, const char *name, char *out, size_t sz) {
    char clone_url[BRICK_MAX_PATH], subpath[BRICK_MAX_PATH], ref[256];
    split_url_parts(url, clone_url, sizeof(clone_url),
                    subpath, sizeof(subpath), ref, sizeof(ref));
    char repo_dir[BRICK_MAX_PATH];
    dep_repo_dir(url, name, repo_dir, sizeof(repo_dir));
    if (subpath[0]) snprintf(out, sz, "%.*s/%s", (int)(sz / 2), repo_dir, subpath);
    else snprintf(out, sz, "%s", repo_dir);
}

static int cmd_init(void) {
    struct stat st;
    if (stat("brick.json", &st) == 0) {
        fprintf(stderr, "brick.json already exists\n");
        return 1;
    }

    /* Derive name from current directory */
    char cwd[BRICK_MAX_PATH];
#ifdef _WIN32
    _getcwd(cwd, sizeof(cwd));
#else
    if (!getcwd(cwd, sizeof(cwd))) {
        strcpy(cwd, "my-project");
    }
#endif
    const char *dir = strrchr(cwd, '/');
    if (!dir) dir = strrchr(cwd, '\\');
    const char *name = dir ? dir + 1 : cwd;

    BrickManifest m;
    memset(&m, 0, sizeof(m));
    strncpy(m.name, name, sizeof(m.name) - 1);
    strcpy(m.version, "0.1.0");
    strcpy(m.main_file, "src/main.clay");

    brick_write("brick.json", &m);
    printf("Created brick.json for '%s'\n", m.name);
    return 0;
}

static int cmd_install(void) {
    BrickManifest m;
    int prc = brick_parse("brick.json", &m);
    if (prc != 0) {
        if (prc == -1) fprintf(stderr, "error: no brick.json found (run 'clay bricks init' first)\n");
        else fprintf(stderr, "error: brick.json is invalid\n");
        return 1;
    }
    if (m.dep_count == 0) {
        printf("No dependencies to install.\n");
        return 0;
    }

    /* With a matching lockfile, install exactly the locked commits. If every
     * dependency is also already present, there is nothing to do. */
    static BrickLock lock;
    int locked = bricks_has_lock() && bricks_read_lock(&lock) == 0 &&
                 bricks_lock_matches(&m, &lock);
    if (locked) {
        int all_present = 1;
        for (int i = 0; i < m.dep_count && all_present; i++) {
            char dir[BRICK_MAX_PATH];
            struct stat st;
            brick_install_dir(m.deps[i].url, m.deps[i].name, dir, sizeof(dir));
            if (stat(dir, &st) != 0) all_present = 0;
        }
        if (all_present) {
            printf("Dependencies already locked and up to date.\n");
            return 0;
        }
        g_install_lock = &lock;
    }

    make_dir("bricks");
    printf("Installing %d dependencies%s...\n", m.dep_count,
           locked ? " (from brick.lock)" : "");
    int failed = 0;
    g_visited_count = 0;
    for (int i = 0; i < m.dep_count; i++) {
        if (install_dep(m.deps[i].url, m.deps[i].name) != 0) {
            failed++;
        }
    }
    g_install_lock = NULL;
    if (failed) {
        fprintf(stderr, "%d dependencies failed to install.\n", failed);
        return 1;
    }
    bricks_write_lock(&m);
    printf("Done.\n");
    return 0;
}

/* Add a dependency. `as` (optional) names it, which is also its import path
 * and install directory: "stdlib" for a repository, or the part before the
 * sub-brick for a monorepo URL ("stdlib" + ".../markdown" → "stdlib/markdown"). */
static int cmd_add(const char *url, const char *as) {
    BrickManifest m;
    int prc = brick_parse("brick.json", &m);
    if (prc != 0) {
        if (prc == -1) fprintf(stderr, "error: no brick.json found (run 'clay bricks init' first)\n");
        else fprintf(stderr, "error: brick.json is invalid\n");
        return 1;
    }

    if (!brick_safe_url(url)) {
        fprintf(stderr, "error: invalid brick URL '%s'\n", url);
        return 1;
    }

    /* Determine brick name */
    char name[BRICK_MAX_NAME];
    url_to_name(url, name, sizeof(name));
    if (as) {
        char clone_url[BRICK_MAX_PATH], subpath[BRICK_MAX_PATH], ref[256];
        split_url_parts(url, clone_url, sizeof(clone_url),
                        subpath, sizeof(subpath), ref, sizeof(ref));
        if (!brick_safe_path(as, 0)) {
            fprintf(stderr, "error: invalid brick name '%s'\n", as);
            return 1;
        }
        if (subpath[0]) snprintf(name, sizeof(name), "%s/%s", as, subpath);
        else snprintf(name, sizeof(name), "%s", as);
    }
    if (!brick_safe_path(name, 1)) {
        fprintf(stderr, "error: invalid brick name '%s' derived from URL\n", name);
        return 1;
    }

    /* Check if already in deps */
    for (int i = 0; i < m.dep_count; i++) {
        if (strcmp(m.deps[i].name, name) == 0) {
            fprintf(stderr, "'%s' is already a dependency\n", name);
            return 1;
        }
    }
    if (m.dep_count >= BRICK_MAX_DEPS) {
        fprintf(stderr, "error: too many dependencies\n");
        return 1;
    }

    /* Install it */
    make_dir("bricks");
    g_visited_count = 0;
    if (install_dep(url, name) != 0) return 1;

    /* Add to manifest */
    BrickDep *dep = &m.deps[m.dep_count++];
    strncpy(dep->name, name, sizeof(dep->name) - 1);
    strncpy(dep->url, url, sizeof(dep->url) - 1);
    brick_write("brick.json", &m);
    printf("Added '%s' to brick.json\n", name);
    bricks_write_lock(&m);
    return 0;
}

static int cmd_remove(const char *name) {
    BrickManifest m;
    int prc = brick_parse("brick.json", &m);
    if (prc != 0) {
        if (prc == -1) fprintf(stderr, "error: no brick.json found\n");
        else fprintf(stderr, "error: brick.json is invalid\n");
        return 1;
    }

    int found = -1;
    for (int i = 0; i < m.dep_count; i++) {
        if (strcmp(m.deps[i].name, name) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        fprintf(stderr, "'%s' is not a dependency\n", name);
        return 1;
    }

    /* The checkout this dependency lives in, worked out before it's removed */
    char clone_url[BRICK_MAX_PATH], subpath[BRICK_MAX_PATH], ref[256];
    split_url_parts(m.deps[found].url, clone_url, sizeof(clone_url),
                    subpath, sizeof(subpath), ref, sizeof(ref));
    char repo_dir[BRICK_MAX_PATH];
    dep_repo_dir(m.deps[found].url, m.deps[found].name, repo_dir, sizeof(repo_dir));

    /* Remove from array */
    for (int i = found; i < m.dep_count - 1; i++) {
        m.deps[i] = m.deps[i + 1];
    }
    m.dep_count--;
    brick_write("brick.json", &m);
    printf("Removed '%s' from brick.json\n", name);

    /* Delete the checkout unless another dependency still lives in it */
    int others_use_repo = 0;
    for (int i = 0; i < m.dep_count; i++) {
        char other_dir[BRICK_MAX_PATH];
        dep_repo_dir(m.deps[i].url, m.deps[i].name, other_dir, sizeof(other_dir));
        if (strcmp(other_dir, repo_dir) == 0) {
            others_use_repo = 1;
            break;
        }
    }
    if (!others_use_repo) {
        char cmd[BRICK_MAX_PATH * 2];
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd), "rmdir /s /q \"bricks\\%s\" 2>nul",
                 repo_dir + strlen("bricks/"));
#else
        snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", repo_dir);
#endif
        system(cmd);
    }
    bricks_write_lock(&m);
    return 0;
}

static int cmd_list(void) {
    BrickManifest m;
    int prc = brick_parse("brick.json", &m);
    if (prc != 0) {
        if (prc == -1) fprintf(stderr, "error: no brick.json found\n");
        else fprintf(stderr, "error: brick.json is invalid\n");
        return 1;
    }
    printf("%s v%s\n", m.name, m.version);
    if (m.dep_count == 0) {
        printf("  (no dependencies)\n");
    } else {
        printf("  Dependencies:\n");
        for (int i = 0; i < m.dep_count; i++) {
            printf("    %s -> %s\n", m.deps[i].name, m.deps[i].url);
        }
    }
    return 0;
}

/* Update a single installed dependency by pulling latest or re-cloning */
static int update_dep(const char *url, const char *name) {
    char clone_url[BRICK_MAX_PATH], subpath[BRICK_MAX_PATH], ref[256];
    split_url_parts(url, clone_url, sizeof(clone_url),
                    subpath, sizeof(subpath), ref, sizeof(ref));

    /* For monorepo, git operations target the repo root */
    char git_dir[BRICK_MAX_PATH];
    dep_repo_dir(url, name, git_dir, sizeof(git_dir));

    struct stat st;
    if (stat(git_dir, &st) != 0) {
        printf("  %s: not installed, installing...\n", name);
        return install_dep(url, name);
    }

    if (ref[0]) {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" fetch --quiet origin", git_dir);
        printf("  Updating %s (ref: %s)...\n", name, ref);
        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "  warning: fetch failed for %s\n", name);
            return -1;
        }
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" checkout --quiet \"%s\"",
                 git_dir, ref);
        system(cmd);
    } else {
        char cmd[2048];
        printf("  Updating %s...\n", name);
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" pull --quiet", git_dir);
        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "  warning: pull failed for %s\n", name);
            return -1;
        }
    }
    return 0;
}

static int cmd_update(void) {
    BrickManifest m;
    int prc = brick_parse("brick.json", &m);
    if (prc != 0) {
        if (prc == -1) fprintf(stderr, "error: no brick.json found\n");
        else fprintf(stderr, "error: brick.json is invalid\n");
        return 1;
    }
    if (m.dep_count == 0) {
        printf("No dependencies to update.\n");
        return 0;
    }
    printf("Updating %d dependenc%s...\n", m.dep_count,
           m.dep_count == 1 ? "y" : "ies");
    int failures = 0;
    g_visited_count = 0;
    for (int i = 0; i < m.dep_count; i++) {
        if (update_dep(m.deps[i].url, m.deps[i].name) != 0)
            failures++;
    }
    if (failures)
        printf("Done with %d failure(s).\n", failures);
    else {
        bricks_write_lock(&m);
        printf("All dependencies up to date.\n");
    }
    return failures ? 1 : 0;
}

int brick_command(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: clay bricks <command>\n\n");
        fprintf(stderr, "Commands:\n");
        fprintf(stderr, "  init              Create a new brick.json\n");
        fprintf(stderr, "  install           Install dependencies from brick.json\n");
        fprintf(stderr, "  update            Update installed dependencies\n");
        fprintf(stderr, "  add <url> [name]  Add a dependency (name = its import path)\n");
        fprintf(stderr, "  remove <name>     Remove a dependency\n");
        fprintf(stderr, "  list              Show project info and dependencies\n");
        return 1;
    }

    const char *cmd = argv[0];
    if (strcmp(cmd, "init") == 0) {
        return cmd_init();
    } else if (strcmp(cmd, "install") == 0) {
        return cmd_install();
    } else if (strcmp(cmd, "update") == 0) {
        return cmd_update();
    } else if (strcmp(cmd, "add") == 0) {
        if (argc < 2) {
            fprintf(stderr, "Usage: clay bricks add <url> [name]\n");
            return 1;
        }
        return cmd_add(argv[1], argc >= 3 ? argv[2] : NULL);
    } else if (strcmp(cmd, "remove") == 0) {
        if (argc < 2) {
            fprintf(stderr, "Usage: clay bricks remove <name>\n");
            return 1;
        }
        return cmd_remove(argv[1]);
    } else if (strcmp(cmd, "list") == 0) {
        return cmd_list();
    } else {
        fprintf(stderr, "Unknown bricks command: %s\n", cmd);
        return 1;
    }
}
