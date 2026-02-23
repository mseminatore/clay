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
    return rc;
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

/* Install a single dependency. Returns 0 on success. */
static int install_dep(const char *url, const char *name) {
    char clone_url[BRICK_MAX_PATH], subpath[BRICK_MAX_PATH], ref[256];
    split_url_parts(url, clone_url, sizeof(clone_url),
                    subpath, sizeof(subpath), ref, sizeof(ref));

    if (subpath[0]) {
        /* Monorepo: clone into bricks/<repo-name>/, brick at subpath */
        char repo_name[BRICK_MAX_NAME];
        url_to_repo_name(url, repo_name, sizeof(repo_name));

        char repo_dest[BRICK_MAX_PATH];
        snprintf(repo_dest, sizeof(repo_dest), "bricks/%s", repo_name);

        struct stat st;
        if (stat(repo_dest, &st) != 0) {
            /* Clone the repo (first sub-brick from this repo) */
            char cmd[2048];
            snprintf(cmd, sizeof(cmd), "git clone --quiet \"%s\" \"%s\"",
                     clone_url, repo_dest);
            printf("  Cloning %s...\n", repo_name);
            int rc = system(cmd);
            if (rc != 0) {
                fprintf(stderr, "  error: failed to clone %s\n", clone_url);
                return -1;
            }
            if (ref[0]) {
                snprintf(cmd, sizeof(cmd),
                         "git -C \"%s\" checkout --quiet \"%s\"", repo_dest, ref);
                rc = system(cmd);
                if (rc != 0)
                    fprintf(stderr, "  warning: could not checkout '%s'\n", ref);
            }
        }

        /* Verify the sub-brick exists */
        char brick_dir[BRICK_MAX_PATH];
        snprintf(brick_dir, sizeof(brick_dir), "bricks/%s/%s", repo_name, subpath);
        struct stat st2;
        if (stat(brick_dir, &st2) != 0) {
            fprintf(stderr, "  error: sub-brick '%s' not found in %s\n",
                    subpath, repo_name);
            return -1;
        }

        printf("  Installed %s (from %s)\n", name, repo_name);

        /* Recursively install sub-dependencies from the sub-brick's manifest */
        char sub_manifest[BRICK_MAX_PATH];
        snprintf(sub_manifest, sizeof(sub_manifest), "%s/brick.json", brick_dir);
        BrickManifest sub;
        if (brick_parse(sub_manifest, &sub) == 0 && sub.dep_count > 0) {
            printf("  Installing dependencies of %s...\n", name);
            for (int i = 0; i < sub.dep_count; i++) {
                install_dep(sub.deps[i].url, sub.deps[i].name);
            }
        }
    } else {
        /* Simple single-brick repo (existing behavior) */
        char dest[BRICK_MAX_PATH];
        snprintf(dest, sizeof(dest), "bricks/%s", name);

        struct stat st;
        if (stat(dest, &st) == 0) {
            printf("  %s: already installed\n", name);
            return 0;
        }

        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "git clone --quiet \"%s\" \"%s\"",
                 clone_url, dest);
        printf("  Installing %s from %s...\n", name, clone_url);
        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "  error: failed to clone %s\n", clone_url);
            return -1;
        }

        if (ref[0]) {
            snprintf(cmd, sizeof(cmd),
                     "git -C \"%s\" checkout --quiet \"%s\"", dest, ref);
            rc = system(cmd);
            if (rc != 0)
                fprintf(stderr, "  warning: could not checkout '%s' for %s\n",
                        ref, name);
        }

        /* Recursively install sub-dependencies */
        char sub_manifest[BRICK_MAX_PATH];
        snprintf(sub_manifest, sizeof(sub_manifest), "%s/brick.json", dest);
        BrickManifest sub;
        if (brick_parse(sub_manifest, &sub) == 0 && sub.dep_count > 0) {
            printf("  Installing dependencies of %s...\n", name);
            for (int i = 0; i < sub.dep_count; i++) {
                install_dep(sub.deps[i].url, sub.deps[i].name);
            }
        }
    }

    return 0;
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
    if (brick_parse("brick.json", &m) != 0) {
        fprintf(stderr, "error: no brick.json found (run 'clay bricks init' first)\n");
        return 1;
    }
    if (m.dep_count == 0) {
        printf("No dependencies to install.\n");
        return 0;
    }

    make_dir("bricks");
    printf("Installing %d dependencies...\n", m.dep_count);
    int failed = 0;
    for (int i = 0; i < m.dep_count; i++) {
        if (install_dep(m.deps[i].url, m.deps[i].name) != 0) {
            failed++;
        }
    }
    if (failed) {
        fprintf(stderr, "%d dependencies failed to install.\n", failed);
        return 1;
    }
    printf("Done.\n");
    return 0;
}

static int cmd_add(const char *url) {
    BrickManifest m;
    if (brick_parse("brick.json", &m) != 0) {
        fprintf(stderr, "error: no brick.json found (run 'clay bricks init' first)\n");
        return 1;
    }

    /* Determine brick name */
    char name[BRICK_MAX_NAME];
    url_to_name(url, name, sizeof(name));

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
    if (install_dep(url, name) != 0) return 1;

    /* Add to manifest */
    BrickDep *dep = &m.deps[m.dep_count++];
    strncpy(dep->name, name, sizeof(dep->name) - 1);
    strncpy(dep->url, url, sizeof(dep->url) - 1);
    brick_write("brick.json", &m);
    printf("Added '%s' to brick.json\n", name);
    return 0;
}

static int cmd_remove(const char *name) {
    BrickManifest m;
    if (brick_parse("brick.json", &m) != 0) {
        fprintf(stderr, "error: no brick.json found\n");
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

    /* Check if this is a monorepo sub-brick */
    char clone_url[BRICK_MAX_PATH], subpath[BRICK_MAX_PATH], ref[256];
    split_url_parts(m.deps[found].url, clone_url, sizeof(clone_url),
                    subpath, sizeof(subpath), ref, sizeof(ref));

    /* Remove from array */
    for (int i = found; i < m.dep_count - 1; i++) {
        m.deps[i] = m.deps[i + 1];
    }
    m.dep_count--;
    brick_write("brick.json", &m);
    printf("Removed '%s' from brick.json\n", name);

    if (subpath[0]) {
        /* Monorepo: only remove repo dir if no other deps use it */
        char repo_name[BRICK_MAX_NAME];
        url_to_repo_name(m.deps[found < m.dep_count ? found : 0].url,
                         repo_name, sizeof(repo_name));
        /* Re-derive repo name from the removed dep's URL (saved before shift) */
        /* Actually we need the original URL — use clone_url to get repo name */
        {
            const char *p = clone_url;
            if (strncmp(p, "https://", 8) == 0) p += 8;
            else if (strncmp(p, "http://", 7) == 0) p += 7;
            const char *last = strrchr(p, '/');
            if (last) {
                strncpy(repo_name, last + 1, sizeof(repo_name) - 1);
                repo_name[sizeof(repo_name) - 1] = '\0';
            }
        }

        int others_use_repo = 0;
        for (int i = 0; i < m.dep_count; i++) {
            char other_repo[BRICK_MAX_NAME];
            url_to_repo_name(m.deps[i].url, other_repo, sizeof(other_repo));
            if (strcmp(other_repo, repo_name) == 0) {
                others_use_repo = 1;
                break;
            }
        }
        if (!others_use_repo) {
            char cmd[BRICK_MAX_PATH];
#ifdef _WIN32
            snprintf(cmd, sizeof(cmd), "rmdir /s /q \"bricks\\%s\" 2>nul",
                     repo_name);
#else
            snprintf(cmd, sizeof(cmd), "rm -rf \"bricks/%s\"", repo_name);
#endif
            system(cmd);
        }
    } else {
        /* Simple brick: remove its directory */
        char cmd[BRICK_MAX_PATH];
#ifdef _WIN32
        snprintf(cmd, sizeof(cmd), "rmdir /s /q \"bricks\\%s\" 2>nul", name);
#else
        snprintf(cmd, sizeof(cmd), "rm -rf \"bricks/%s\"", name);
#endif
        system(cmd);
    }
    return 0;
}

static int cmd_list(void) {
    BrickManifest m;
    if (brick_parse("brick.json", &m) != 0) {
        fprintf(stderr, "error: no brick.json found\n");
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
    if (subpath[0]) {
        char repo_name[BRICK_MAX_NAME];
        url_to_repo_name(url, repo_name, sizeof(repo_name));
        snprintf(git_dir, sizeof(git_dir), "bricks/%s", repo_name);
    } else {
        snprintf(git_dir, sizeof(git_dir), "bricks/%s", name);
    }

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
    if (brick_parse("brick.json", &m) != 0) {
        fprintf(stderr, "error: no brick.json found\n");
        return 1;
    }
    if (m.dep_count == 0) {
        printf("No dependencies to update.\n");
        return 0;
    }
    printf("Updating %d dependenc%s...\n", m.dep_count,
           m.dep_count == 1 ? "y" : "ies");
    int failures = 0;
    for (int i = 0; i < m.dep_count; i++) {
        if (update_dep(m.deps[i].url, m.deps[i].name) != 0)
            failures++;
    }
    if (failures)
        printf("Done with %d failure(s).\n", failures);
    else
        printf("All dependencies up to date.\n");
    return failures ? 1 : 0;
}

int brick_command(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "Usage: clay bricks <command>\n\n");
        fprintf(stderr, "Commands:\n");
        fprintf(stderr, "  init              Create a new brick.json\n");
        fprintf(stderr, "  install           Install dependencies from brick.json\n");
        fprintf(stderr, "  update            Update installed dependencies\n");
        fprintf(stderr, "  add <url>         Add a dependency\n");
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
            fprintf(stderr, "Usage: clay bricks add <url>\n");
            return 1;
        }
        return cmd_add(argv[1]);
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
