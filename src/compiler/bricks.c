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

/* Extract brick name from URL: "github.com/user/my-lib@v1.0" → "my-lib" */
static void url_to_name(const char *url, char *name, size_t sz) {
    /* Strip @tag if present */
    char tmp[BRICK_MAX_PATH];
    strncpy(tmp, url, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    char *at = strchr(tmp, '@');
    if (at) *at = '\0';

    /* Find last / segment */
    const char *last = strrchr(tmp, '/');
    if (last) last++;
    else last = tmp;

    strncpy(name, last, sz - 1);
    name[sz - 1] = '\0';
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

/* Install a single dependency. Returns 0 on success. */
static int install_dep(const char *url, const char *name) {
    char clone_url[BRICK_MAX_PATH], ref[256];
    split_url_ref(url, clone_url, sizeof(clone_url), ref, sizeof(ref));

    char dest[BRICK_MAX_PATH];
    snprintf(dest, sizeof(dest), "bricks/%s", name);

    /* Check if already exists */
    struct stat st;
    if (stat(dest, &st) == 0) {
        printf("  %s: already installed\n", name);
        return 0;
    }

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "git clone --quiet \"%s\" \"%s\"", clone_url, dest);
    printf("  Installing %s from %s...\n", name, clone_url);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  error: failed to clone %s\n", clone_url);
        return -1;
    }

    /* Checkout ref if specified */
    if (ref[0]) {
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" checkout --quiet \"%s\"", dest, ref);
        rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "  warning: could not checkout '%s' for %s\n", ref, name);
        }
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

    /* Remove from array */
    for (int i = found; i < m.dep_count - 1; i++) {
        m.deps[i] = m.deps[i + 1];
    }
    m.dep_count--;
    brick_write("brick.json", &m);
    printf("Removed '%s' from brick.json\n", name);

    /* Remove directory */
    char cmd[BRICK_MAX_PATH];
#ifdef _WIN32
    snprintf(cmd, sizeof(cmd), "rmdir /s /q \"bricks\\%s\" 2>nul", name);
#else
    snprintf(cmd, sizeof(cmd), "rm -rf \"bricks/%s\"", name);
#endif
    system(cmd);
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
            printf("    %s → %s\n", m.deps[i].name, m.deps[i].url);
        }
    }
    return 0;
}

/* Update a single installed dependency by pulling latest or re-cloning */
static int update_dep(const char *url, const char *name) {
    char dest[BRICK_MAX_PATH];
    snprintf(dest, sizeof(dest), "bricks/%s", name);

    struct stat st;
    if (stat(dest, &st) != 0) {
        printf("  %s: not installed, installing...\n", name);
        return install_dep(url, name);
    }

    char clone_url[BRICK_MAX_PATH], ref[256];
    split_url_ref(url, clone_url, sizeof(clone_url), ref, sizeof(ref));

    if (ref[0]) {
        /* Pinned ref: fetch and checkout the specific ref */
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" fetch --quiet origin", dest);
        printf("  Updating %s (ref: %s)...\n", name, ref);
        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "  warning: fetch failed for %s\n", name);
            return -1;
        }
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" checkout --quiet \"%s\"", dest, ref);
        system(cmd);
    } else {
        /* No pinned ref: pull latest from default branch */
        char cmd[2048];
        printf("  Updating %s...\n", name);
        snprintf(cmd, sizeof(cmd), "git -C \"%s\" pull --quiet", dest);
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
