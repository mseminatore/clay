/*
 * dap.c — Debug Adapter Protocol handler for Clay
 *
 * Speaks DAP over stdin/stdout. VS Code launches:
 *   clay --dap somefile.clay
 *
 * Uses the existing bytecode VM step_callback infrastructure
 * for breakpoints and stepping.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
/* Declare only what we need from Windows API to avoid windows.h conflicts */
typedef void *HANDLE;
typedef unsigned long DWORD;
#define STD_INPUT_HANDLE ((DWORD)-10)
#define INVALID_HANDLE_VALUE ((HANDLE)(long long)-1)
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD nStdHandle);
__declspec(dllimport) int __stdcall PeekNamedPipe(HANDLE hNamedPipe,
    void *lpBuffer, DWORD nBufferSize, DWORD *lpBytesRead,
    DWORD *lpTotalBytesAvail, DWORD *lpBytesLeftThisMessage);
#else
#include <unistd.h>
#include <sys/select.h>
#endif

#ifdef _MSC_VER
#define strdup _strdup
#endif

#include "cJSON.h"
#include "dap.h"
#include "parser.h"
#include "bc_compiler.h"
#include "clay.h"
#include "vm.h"
#include "hash.h"

/* Forward declarations for bc_compiler conversion helpers */
ClayValue vm_to_clay(clayvm_value_t v);

/* Forward declaration for use in dap_poll_messages */
static int dap_handle_request(cJSON *msg);

/* ------------------------------------------------------------------ */
/*  DAP global state                                                   */
/* ------------------------------------------------------------------ */

static DapState g_dap;

/* DAP protocol uses a saved copy of stdout; program output goes to stderr */
static FILE *g_dap_out = NULL;

/* ------------------------------------------------------------------ */
/*  DAP I/O — Content-Length framed JSON over saved stdout             */
/* ------------------------------------------------------------------ */

static void dap_send_raw(const char *json) {
    FILE *out = g_dap_out ? g_dap_out : stdout;
    int len = (int)strlen(json);
    fprintf(out, "Content-Length: %d\r\n\r\n%s", len, json);
    fflush(out);
}

static void dap_send(cJSON *msg) {
    char *s = cJSON_PrintUnformatted(msg);
    dap_send_raw(s);
    free(s);
    cJSON_Delete(msg);
}

/* Read one DAP message from stdin. Returns parsed JSON or NULL on EOF. */
static cJSON *dap_read(void) {
    /* Read Content-Length header line by line */
    char header[256];
    int content_length = -1;

    for (;;) {
        if (!fgets(header, sizeof(header), stdin)) return NULL;
        /* Strip trailing \r\n or \n */
        size_t hlen = strlen(header);
        while (hlen > 0 && (header[hlen-1] == '\r' || header[hlen-1] == '\n'))
            header[--hlen] = '\0';
        /* Empty line = end of headers */
        if (hlen == 0) break;
        if (strncmp(header, "Content-Length:", 15) == 0) {
            content_length = atoi(header + 15);
        }
    }

    if (content_length <= 0) return NULL;

    char *body = (char *)malloc((size_t)content_length + 1);
    size_t nread = fread(body, 1, (size_t)content_length, stdin);
    body[nread] = '\0';

    cJSON *msg = cJSON_Parse(body);
    free(body);
    if (!msg) {
        fprintf(stderr, "clay-dap: JSON parse failed (read %d of %d bytes)\n",
            (int)nread, content_length);
        fflush(stderr);
    }
    return msg;
}

/* ------------------------------------------------------------------ */
/*  Response / event helpers                                           */
/* ------------------------------------------------------------------ */

static cJSON *dap_base_response(cJSON *request, int success) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddNumberToObject(resp, "seq", ++g_dap.seq);
    cJSON *seq_item = cJSON_GetObjectItem(request, "seq");
    cJSON_AddNumberToObject(resp, "request_seq", seq_item ? seq_item->valueint : 0);
    cJSON_AddBoolToObject(resp, "success", success);
    cJSON *cmd_item = cJSON_GetObjectItem(request, "command");
    cJSON_AddStringToObject(resp, "command", cmd_item ? cmd_item->valuestring : "unknown");
    return resp;
}

static cJSON *dap_event(const char *event_name) {
    cJSON *ev = cJSON_CreateObject();
    cJSON_AddStringToObject(ev, "type", "event");
    cJSON_AddNumberToObject(ev, "seq", ++g_dap.seq);
    cJSON_AddStringToObject(ev, "event", event_name);
    return ev;
}

static void dap_send_event(const char *event_name) {
    dap_send(dap_event(event_name));
}

static void dap_send_output(const char *category, const char *text) {
    cJSON *ev = dap_event("output");
    cJSON *body = cJSON_AddObjectToObject(ev, "body");
    cJSON_AddStringToObject(body, "category", category);
    cJSON_AddStringToObject(body, "output", text);
    dap_send(ev);
}

/* Hook for clay_output_fn — routes program output through DAP output events */
static void dap_output_hook(const char *text) {
    dap_send_output("stdout", text);
}

static void dap_send_stopped(const char *reason, const char *description) {
    cJSON *ev = dap_event("stopped");
    cJSON *body = cJSON_AddObjectToObject(ev, "body");
    cJSON_AddStringToObject(body, "reason", reason);
    cJSON_AddNumberToObject(body, "threadId", 1);
    if (description) {
        cJSON_AddStringToObject(body, "description", description);
        cJSON_AddStringToObject(body, "text", description);
    }
    dap_send(ev);
}

/* ------------------------------------------------------------------ */
/*  Source line extraction                                             */
/* ------------------------------------------------------------------ */

static char *dap_get_source_line(const char *src, int line_num) {
    if (!src || line_num < 1) return NULL;
    const char *p = src;
    int cur = 1;
    while (cur < line_num && *p) {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != line_num) return NULL;
    const char *end = p;
    while (*end && *end != '\n') end++;
    size_t len = (size_t)(end - p);
    char *result = (char *)malloc(len + 1);
    memcpy(result, p, len);
    result[len] = '\0';
    return result;
}

/* ------------------------------------------------------------------ */
/*  Breakpoint management                                              */
/* ------------------------------------------------------------------ */

/* Evaluate a simple expression: supports global variable lookup and dot-notation
   for slot access (e.g., "obj.slot.nested"). Returns the value and sets *found=1,
   or returns CLAY_NIL with *found=0. */
static ClayValue dap_evaluate_expr(const char *expr, int *found) {
    *found = 0;
    if (!expr || !expr[0]) return CLAY_NIL;

    clayvm_vm_t *dvm = (clayvm_vm_t *)g_dap.vm;
    if (!dvm || !dvm->globals) return CLAY_NIL;

    /* Copy expr so we can tokenize on '.' */
    char buf[256];
    size_t elen = strlen(expr);
    if (elen >= sizeof(buf)) return CLAY_NIL;
    memcpy(buf, expr, elen + 1);

    /* Split on first '.' */
    char *dot = strchr(buf, '.');
    if (dot) *dot = '\0';

    /* Look up root variable in globals */
    HashTable *globals = (HashTable *)dvm->globals;
    ht_value_t val;
    if (!globals->table || !ht_lookup(globals, buf, &val))
        return CLAY_NIL;

    clayvm_value_t *vp = (clayvm_value_t *)val;
    ClayValue cv = vm_to_clay(*vp);
    *found = 1;

    /* Walk dot-separated slot chain */
    char *rest = dot ? dot + 1 : NULL;
    while (rest && *rest) {
        char *next_dot = strchr(rest, '.');
        if (next_dot) *next_dot = '\0';

        if (CLAY_IS_NIL(cv) || CLAY_IS_INT(cv) || cv == CLAY_TRUE || cv == CLAY_FALSE) {
            *found = 0; return CLAY_NIL;
        }
        ClayObject *obj = CLAY_OBJ(cv);
        if (!obj || !obj->slots) { *found = 0; return CLAY_NIL; }

        if (clay_has_slot(obj, rest)) {
            cv = clay_get_slot(obj, rest);
        } else {
            *found = 0; return CLAY_NIL;
        }

        rest = next_dot ? next_dot + 1 : NULL;
    }

    return cv;
}

/* Check if a breakpoint condition is truthy */
static int dap_condition_truthy(const char *condition) {
    if (!condition || !condition[0]) return 1; /* no condition = always stop */
    int found = 0;
    ClayValue cv = dap_evaluate_expr(condition, &found);
    if (!found) return 0; /* unknown variable = don't stop */
    return CLAY_IS_TRUTHY(cv);
}

static int dap_has_breakpoint(int line) {
    for (int s = 0; s < g_dap.source_count; s++) {
        for (int i = 0; i < g_dap.sources[s].bp.count; i++) {
            if (g_dap.sources[s].bp.lines[i] == line) {
                /* Check hit count */
                g_dap.sources[s].bp.hit_counts[i]++;
                if (g_dap.sources[s].bp.hit_targets[i] > 0 &&
                    g_dap.sources[s].bp.hit_counts[i] < g_dap.sources[s].bp.hit_targets[i]) {
                    continue; /* haven't reached target count yet */
                }
                /* Check condition */
                if (!dap_condition_truthy(g_dap.sources[s].bp.conditions[i])) {
                    continue;
                }
                return 1;
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Value formatting                                                   */
/* ------------------------------------------------------------------ */

static char *dap_format_clay_value(ClayValue cv) {
    char buf[256];
    if (CLAY_IS_NIL(cv)) {
        return strdup("nil");
    } else if (CLAY_IS_INT(cv)) {
        snprintf(buf, sizeof(buf), "%ld", (long)clay_to_int(cv));
        return strdup(buf);
    } else if (cv == CLAY_TRUE) {
        return strdup("true");
    } else if (cv == CLAY_FALSE) {
        return strdup("false");
    } else {
        ClayObject *obj = CLAY_OBJ(cv);
        if (!obj) return strdup("nil");
        /* Use type-specific formatting to avoid method dispatch issues */
        switch (obj->type) {
            case CLAY_TYPE_STRING:
                return strdup(clay_string_data(obj));
            case CLAY_TYPE_FLOAT: {
                snprintf(buf, sizeof(buf), "%g", obj->as.number_float.value);
                return strdup(buf);
            }
            case CLAY_TYPE_ARRAY:
                snprintf(buf, sizeof(buf), "<Array length=%zu>", obj->as.array.size);
                return strdup(buf);
            case CLAY_TYPE_BLOCK:
                return strdup("<Block>");
            default: {
                /* For generic objects, show type */
                int slot_count = 0;
                if (obj->slots && obj->slots->table)
                    slot_count = (int)ht_size(obj->slots);
                snprintf(buf, sizeof(buf), "<Object slots=%d>", slot_count);
                return strdup(buf);
            }
        }
    }
}

static const char *dap_type_name(ClayValue cv) {
    if (CLAY_IS_NIL(cv)) return "nil";
    if (CLAY_IS_INT(cv)) return "Number";
    if (cv == CLAY_TRUE || cv == CLAY_FALSE) return "Boolean";
    ClayObject *obj = CLAY_OBJ(cv);
    if (!obj) return "nil";
    if (obj->type == CLAY_TYPE_STRING) return "String";
    if (obj->type == CLAY_TYPE_ARRAY) return "Array";
    if (obj->type == CLAY_TYPE_BLOCK) return "Block";
    if (obj->type == CLAY_TYPE_FLOAT) return "Float";
    return "Object";
}

static char *dap_format_vm_value(clayvm_value_t v) {
    ClayValue cv = vm_to_clay(v);
    return dap_format_clay_value(cv);
}

/* ------------------------------------------------------------------ */
/*  Variable reference tracking for expandable objects                 */
/* ------------------------------------------------------------------ */

static void dap_var_refs_reset(void) {
    g_dap.var_refs.count = 3; /* 1=Globals, 2=Locals, 3+ = objects */
}

/* Returns a variablesReference for an expandable ClayValue, or 0 if not expandable */
static int dap_var_ref_for(ClayValue cv) {
    if (CLAY_IS_NIL(cv) || CLAY_IS_INT(cv) || cv == CLAY_TRUE || cv == CLAY_FALSE)
        return 0;
    ClayObject *obj = CLAY_OBJ(cv);
    if (!obj) return 0;
    if (obj->type == CLAY_TYPE_ARRAY && obj->as.array.size > 0) {
        int id = g_dap.var_refs.count;
        if (id < DAP_MAX_VAR_REFS) {
            g_dap.var_refs.values[id] = cv;
            g_dap.var_refs.count++;
            return id;
        }
    }
    if (obj->type == CLAY_TYPE_OBJECT && obj->slots && obj->slots->table && ht_size(obj->slots) > 0) {
        int id = g_dap.var_refs.count;
        if (id < DAP_MAX_VAR_REFS) {
            g_dap.var_refs.values[id] = cv;
            g_dap.var_refs.count++;
            return id;
        }
    }
    return 0;
}

/* Check if stdin has data available (non-blocking) */
static int dap_stdin_has_data(void) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD avail = 0;
    if (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return avail > 0;
    return 0;
#else
    fd_set fds;
    struct timeval tv = {0, 0};
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
#endif
}

/* Poll for DAP messages during execution (called every N instructions).
   Handles "pause" and "disconnect" requests without blocking. */
static void dap_poll_messages(void) {
    while (dap_stdin_has_data()) {
        cJSON *msg = dap_read();
        if (!msg) break;
        cJSON *type_item = cJSON_GetObjectItem(msg, "type");
        if (!type_item || !type_item->valuestring ||
            strcmp(type_item->valuestring, "request") != 0) {
            cJSON_Delete(msg); continue;
        }
        cJSON *cmd_item = cJSON_GetObjectItem(msg, "command");
        if (!cmd_item || !cmd_item->valuestring) { cJSON_Delete(msg); continue; }
        const char *cmd = cmd_item->valuestring;

        if (strcmp(cmd, "pause") == 0) {
            cJSON *resp = dap_base_response(msg, 1);
            dap_send(resp);
            g_dap.pause_requested = 1;
        } else if (strcmp(cmd, "disconnect") == 0) {
            cJSON *resp = dap_base_response(msg, 1);
            dap_send(resp);
            g_dap.terminate = 1;
        } else if (strcmp(cmd, "setBreakpoints") == 0) {
            /* Allow setting breakpoints while running */
            dap_handle_request(msg);
        } else {
            cJSON *resp = dap_base_response(msg, 1);
            dap_send(resp);
        }
        cJSON_Delete(msg);
    }
}

static int g_dap_poll_counter = 0;

/* ------------------------------------------------------------------ */
/*  Step callback — fires at each VM instruction                       */
/* ------------------------------------------------------------------ */

static void dap_step_callback(void *vm_ptr, size_t ip) {
    clayvm_vm_t *vm = (clayvm_vm_t *)vm_ptr;
    BcCompiler *bc = (BcCompiler *)g_dap.bc;
    if (!bc) return;

    /* Poll for DAP messages every 1024 instructions (for pause support) */
    if (++g_dap_poll_counter >= 1024) {
        g_dap_poll_counter = 0;
        dap_poll_messages();
        if (g_dap.terminate) { vm->running = 0; return; }
    }

    /* Determine the correct source line from the active code's line_map.
     * When executing main code, vm->code == bc->code and we use bc->line_map.
     * When inside a block/method chunk, the trampoline has already set
     * clay_source_line from the chunk's line_map before calling us. */
    int line;
    if (vm->code == bc->code) {
        /* Main code — look up from the main compiler's line map */
        if (ip >= (size_t)bc->len) return;
        line = bc->line_map[ip];
    } else {
        /* Inside a chunk — use clay_source_line set by the trampoline */
        line = clay_source_line;
    }
    if (line <= 0) return;

    /* Check if we should stop */
    int should_stop = 0;
    const char *reason = NULL;

    /* Check pause request first (works regardless of step mode) */
    if (g_dap.pause_requested) {
        g_dap.pause_requested = 0;
        should_stop = 1;
        reason = "pause";
    } else if (g_dap.step_mode == 1) {
        /* Step Next — stop when line changes at same or shallower depth */
        if (line != g_dap.step_line && clay_call_trace.depth <= g_dap.step_depth) {
            should_stop = 1;
            reason = "step";
        }
    } else if (g_dap.step_mode == 2) {
        /* Step In — stop on any new line (including inside called methods/blocks) */
        if (line != g_dap.step_line || clay_call_trace.depth > g_dap.step_depth) {
            should_stop = 1;
            reason = "step";
        }
    } else if (g_dap.step_mode == 3) {
        /* Step Out — stop when trace depth decreases */
        if (clay_call_trace.depth < g_dap.step_depth && line != g_dap.step_line) {
            should_stop = 1;
            reason = "step";
        }
    } else if (g_dap.step_mode == 0) {
        /* Running — check breakpoints (skip if still on the line we continued from) */
        if (line != g_dap.last_stopped_line && dap_has_breakpoint(line)) {
            should_stop = 1;
            reason = "breakpoint";
        } else if (line != g_dap.last_stopped_line) {
            /* Line changed — clear the skip guard */
            g_dap.last_stopped_line = 0;
        }
    }

    if (!should_stop) return;

    /* We're stopped — enter DAP message loop */
    g_dap.stopped = 1;
    g_dap.step_mode = 0;
    g_dap.last_stopped_line = line;
    dap_var_refs_reset();
    dap_send_stopped(reason, NULL);

    /* Process DAP requests while stopped */
    while (g_dap.stopped && !g_dap.terminate) {
        cJSON *msg = dap_read();
        if (!msg) { g_dap.terminate = 1; break; }

        cJSON *type_item = cJSON_GetObjectItem(msg, "type");
        if (!type_item || !type_item->valuestring ||
            strcmp(type_item->valuestring, "request") != 0) {
            cJSON_Delete(msg); continue;
        }

        cJSON *cmd_item = cJSON_GetObjectItem(msg, "command");
        if (!cmd_item || !cmd_item->valuestring) { cJSON_Delete(msg); continue; }
        const char *command = cmd_item->valuestring;

        if (strcmp(command, "continue") == 0) {
            cJSON *resp = dap_base_response(msg, 1);
            cJSON *body = cJSON_AddObjectToObject(resp, "body");
            cJSON_AddBoolToObject(body, "allThreadsContinued", 1);
            dap_send(resp);
            g_dap.stopped = 0;
            g_dap.step_mode = 0;
        } else if (strcmp(command, "next") == 0) {
            cJSON *resp = dap_base_response(msg, 1);
            dap_send(resp);
            g_dap.stopped = 0;
            g_dap.step_mode = 1;
            g_dap.step_line = line;
            g_dap.step_depth = clay_call_trace.depth;
        } else if (strcmp(command, "stepIn") == 0) {
            cJSON *resp = dap_base_response(msg, 1);
            dap_send(resp);
            g_dap.stopped = 0;
            g_dap.step_mode = 2;
            g_dap.step_line = line;
            g_dap.step_depth = clay_call_trace.depth;
        } else if (strcmp(command, "stepOut") == 0) {
            cJSON *resp = dap_base_response(msg, 1);
            dap_send(resp);
            g_dap.stopped = 0;
            g_dap.step_mode = 3;
            g_dap.step_line = line;
            g_dap.step_depth = clay_call_trace.depth;
        } else if (strcmp(command, "threads") == 0) {
            cJSON *resp = dap_base_response(msg, 1);
            cJSON *body = cJSON_AddObjectToObject(resp, "body");
            cJSON *threads = cJSON_AddArrayToObject(body, "threads");
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "id", 1);
            cJSON_AddStringToObject(t, "name", "main");
            cJSON_AddItemToArray(threads, t);
            dap_send(resp);
        } else if (strcmp(command, "stackTrace") == 0) {
            cJSON *resp = dap_base_response(msg, 1);
            cJSON *body = cJSON_AddObjectToObject(resp, "body");
            cJSON *frames = cJSON_AddArrayToObject(body, "stackFrames");

            /* Current location is frame 0 */
            cJSON *f0 = cJSON_CreateObject();
            cJSON_AddNumberToObject(f0, "id", 0);
            cJSON_AddStringToObject(f0, "name", "<main>");
            cJSON *src0 = cJSON_AddObjectToObject(f0, "source");
            cJSON_AddStringToObject(src0, "name", g_dap.source_file ? g_dap.source_file : "<unknown>");
            if (g_dap.source_file)
                cJSON_AddStringToObject(src0, "path", g_dap.source_file);
            cJSON_AddNumberToObject(f0, "line", line);
            cJSON_AddNumberToObject(f0, "column", 1);
            cJSON_AddItemToArray(frames, f0);

            /* Walk call trace for remaining frames */
            for (int i = clay_call_trace.depth - 1; i >= 0; i--) {
                ClayTraceFrame *tf = &clay_call_trace.frames[i];
                cJSON *fr = cJSON_CreateObject();
                cJSON_AddNumberToObject(fr, "id", clay_call_trace.depth - i);
                cJSON_AddStringToObject(fr, "name", tf->selector ? tf->selector : "<block>");
                cJSON *src = cJSON_AddObjectToObject(fr, "source");
                cJSON_AddStringToObject(src, "name", tf->file ? tf->file : "<unknown>");
                if (tf->file) cJSON_AddStringToObject(src, "path", tf->file);
                cJSON_AddNumberToObject(fr, "line", tf->line);
                cJSON_AddNumberToObject(fr, "column", 1);
                cJSON_AddItemToArray(frames, fr);
            }

            cJSON_AddNumberToObject(body, "totalFrames", clay_call_trace.depth + 1);
            dap_send(resp);
        } else if (strcmp(command, "scopes") == 0) {
            cJSON *resp = dap_base_response(msg, 1);
            cJSON *body = cJSON_AddObjectToObject(resp, "body");
            cJSON *scopes = cJSON_AddArrayToObject(body, "scopes");

            cJSON *globals_scope = cJSON_CreateObject();
            cJSON_AddStringToObject(globals_scope, "name", "Globals");
            cJSON_AddNumberToObject(globals_scope, "variablesReference", 1);
            cJSON_AddBoolToObject(globals_scope, "expensive", 0);
            cJSON_AddItemToArray(scopes, globals_scope);

            /* Add Locals scope if we have stack data */
            clayvm_vm_t *dvm = (clayvm_vm_t *)g_dap.vm;
            if (dvm && dvm->sp > 0) {
                cJSON *locals_scope = cJSON_CreateObject();
                cJSON_AddStringToObject(locals_scope, "name", "Locals");
                cJSON_AddNumberToObject(locals_scope, "variablesReference", 2);
                cJSON_AddBoolToObject(locals_scope, "expensive", 0);
                cJSON_AddItemToArray(scopes, locals_scope);
            }

            dap_send(resp);
        } else if (strcmp(command, "variables") == 0) {
            cJSON *args = cJSON_GetObjectItem(msg, "arguments");
            int varRef = cJSON_GetObjectItem(args, "variablesReference")->valueint;

            cJSON *resp = dap_base_response(msg, 1);
            cJSON *body = cJSON_AddObjectToObject(resp, "body");
            cJSON *variables = cJSON_AddArrayToObject(body, "variables");

            clayvm_vm_t *dvm = (clayvm_vm_t *)g_dap.vm;

            if (varRef == 1 && dvm && dvm->globals) {
                /* Globals */
                HashTable *globals = (HashTable *)dvm->globals;
                if (globals->size > 0 && globals->table) {
                    size_t ipos = 0;
                    ht_key_t key;
                    ht_value_t val;
                    while (ht_next(globals, &ipos, &key, &val)) {
                        const char *name = (const char *)key;
                        clayvm_value_t *vp = (clayvm_value_t *)val;
                        char *formatted = dap_format_vm_value(*vp);
                        ClayValue cv = vm_to_clay(*vp);

                        cJSON *v = cJSON_CreateObject();
                        cJSON_AddStringToObject(v, "name", name);
                        cJSON_AddStringToObject(v, "value", formatted);
                        cJSON_AddStringToObject(v, "type", dap_type_name(cv));
                        cJSON_AddNumberToObject(v, "variablesReference", dap_var_ref_for(cv));
                        cJSON_AddItemToArray(variables, v);
                        free(formatted);
                    }
                }
            } else if (varRef == 2 && dvm) {
                /* Locals — show top of stack */
                int show = dvm->sp < 16 ? dvm->sp : 16;
                for (int i = 0; i < show; i++) {
                    clayvm_value_t sv = dvm->stack[i];
                    char *formatted = dap_format_vm_value(sv);
                    ClayValue cv = vm_to_clay(sv);
                    char name_buf[32];
                    snprintf(name_buf, sizeof(name_buf), "[%d]", i);

                    cJSON *v = cJSON_CreateObject();
                    cJSON_AddStringToObject(v, "name", name_buf);
                    cJSON_AddStringToObject(v, "value", formatted);
                    cJSON_AddStringToObject(v, "type", dap_type_name(cv));
                    cJSON_AddNumberToObject(v, "variablesReference", dap_var_ref_for(cv));
                    cJSON_AddItemToArray(variables, v);
                    free(formatted);
                }
            } else if (varRef >= 3 && varRef < g_dap.var_refs.count) {
                /* Child variables of an expanded object/array */
                ClayValue parent_cv = g_dap.var_refs.values[varRef];
                if (!CLAY_IS_NIL(parent_cv) && !CLAY_IS_INT(parent_cv)) {
                    ClayObject *parent = CLAY_OBJ(parent_cv);
                    if (parent && parent->type == CLAY_TYPE_ARRAY) {
                        /* Array elements */
                        size_t len = parent->as.array.size;
                        for (size_t i = 0; i < len && i < 100; i++) {
                            ClayValue elem = parent->as.array.items[i];
                            char *formatted = dap_format_clay_value(elem);
                            char idx_buf[16];
                            snprintf(idx_buf, sizeof(idx_buf), "[%zu]", i);

                            cJSON *v = cJSON_CreateObject();
                            cJSON_AddStringToObject(v, "name", idx_buf);
                            cJSON_AddStringToObject(v, "value", formatted);
                            cJSON_AddStringToObject(v, "type", dap_type_name(elem));
                            cJSON_AddNumberToObject(v, "variablesReference", dap_var_ref_for(elem));
                            cJSON_AddItemToArray(variables, v);
                            free(formatted);
                        }
                    } else if (parent && parent->slots && parent->slots->table) {
                        /* Object slots */
                        size_t ipos = 0;
                        ht_key_t key;
                        ht_value_t val;
                        while (ht_next(parent->slots, &ipos, &key, &val)) {
                            ClayValue sv = (ClayValue)(uintptr_t)val;
                            char *formatted = dap_format_clay_value(sv);

                            cJSON *v = cJSON_CreateObject();
                            cJSON_AddStringToObject(v, "name", (const char *)key);
                            cJSON_AddStringToObject(v, "value", formatted);
                            cJSON_AddStringToObject(v, "type", dap_type_name(sv));
                            cJSON_AddNumberToObject(v, "variablesReference", dap_var_ref_for(sv));
                            cJSON_AddItemToArray(variables, v);
                            free(formatted);
                        }
                    }
                }
            }

            dap_send(resp);
        } else if (strcmp(command, "setBreakpoints") == 0) {
            /* Handle setBreakpoints while stopped (e.g., user adds breakpoint during pause) */
            cJSON *args = cJSON_GetObjectItem(msg, "arguments");
            cJSON *source = cJSON_GetObjectItem(args, "source");
            const char *path = NULL;
            cJSON *pathItem = cJSON_GetObjectItem(source, "path");
            if (pathItem) path = pathItem->valuestring;

            /* Find or create source entry */
            int si = -1;
            for (int i = 0; i < g_dap.source_count; i++) {
                if (g_dap.sources[i].file && path && strcmp(g_dap.sources[i].file, path) == 0) {
                    si = i; break;
                }
            }
            if (si < 0 && g_dap.source_count < DAP_MAX_SOURCES) {
                si = g_dap.source_count++;
                g_dap.sources[si].file = path ? strdup(path) : NULL;
            }

            /* Clear and set breakpoints */
            cJSON *breakpoints = cJSON_GetObjectItem(args, "breakpoints");
            cJSON *resp = dap_base_response(msg, 1);
            cJSON *rbody = cJSON_AddObjectToObject(resp, "body");
            cJSON *rbps = cJSON_AddArrayToObject(rbody, "breakpoints");

            if (si >= 0) {
                /* Free old conditions */
                for (int i = 0; i < g_dap.sources[si].bp.count; i++)
                    free(g_dap.sources[si].bp.conditions[i]);
                g_dap.sources[si].bp.count = 0;
                int count = breakpoints ? cJSON_GetArraySize(breakpoints) : 0;
                for (int i = 0; i < count && i < DAP_MAX_BREAKPOINTS; i++) {
                    cJSON *bp = cJSON_GetArrayItem(breakpoints, i);
                    cJSON *line_item = bp ? cJSON_GetObjectItem(bp, "line") : NULL;
                    if (!line_item) continue;
                    int idx = g_dap.sources[si].bp.count;
                    int bpline = line_item->valueint;
                    g_dap.sources[si].bp.lines[idx] = bpline;
                    /* Condition */
                    cJSON *cond = bp ? cJSON_GetObjectItem(bp, "condition") : NULL;
                    g_dap.sources[si].bp.conditions[idx] = (cond && cond->valuestring && cond->valuestring[0])
                        ? strdup(cond->valuestring) : NULL;
                    /* Hit count */
                    cJSON *hitCond = bp ? cJSON_GetObjectItem(bp, "hitCondition") : NULL;
                    g_dap.sources[si].bp.hit_targets[idx] = (hitCond && hitCond->valuestring)
                        ? atoi(hitCond->valuestring) : 0;
                    g_dap.sources[si].bp.hit_counts[idx] = 0;
                    g_dap.sources[si].bp.count++;

                    cJSON *rbp = cJSON_CreateObject();
                    cJSON_AddBoolToObject(rbp, "verified", 1);
                    cJSON_AddNumberToObject(rbp, "line", bpline);
                    cJSON_AddItemToArray(rbps, rbp);
                }
            }
            dap_send(resp);
        } else if (strcmp(command, "evaluate") == 0) {
            /* Evaluate — supports globals and dot-notation (obj.slot.nested) */
            cJSON *args = cJSON_GetObjectItem(msg, "arguments");
            const char *expr = NULL;
            cJSON *expr_item = cJSON_GetObjectItem(args, "expression");
            if (expr_item) expr = expr_item->valuestring;

            cJSON *resp = dap_base_response(msg, 1);
            cJSON *body = cJSON_AddObjectToObject(resp, "body");

            int found = 0;
            ClayValue cv = dap_evaluate_expr(expr, &found);
            if (found) {
                char *formatted = dap_format_clay_value(cv);
                cJSON_AddStringToObject(body, "result", formatted);
                cJSON_AddStringToObject(body, "type", dap_type_name(cv));
                cJSON_AddNumberToObject(body, "variablesReference", dap_var_ref_for(cv));
                free(formatted);
            } else {
                cJSON_AddStringToObject(body, "result", "<not found>");
                cJSON_AddNumberToObject(body, "variablesReference", 0);
            }
            dap_send(resp);
        } else if (strcmp(command, "disconnect") == 0) {
            cJSON *resp = dap_base_response(msg, 1);
            dap_send(resp);
            g_dap.terminate = 1;
            g_dap.stopped = 0;
        } else {
            /* Unknown command while stopped — respond with success */
            cJSON *resp = dap_base_response(msg, 1);
            dap_send(resp);
        }

        cJSON_Delete(msg);
    }

    /* If terminate requested, stop the VM */
    if (g_dap.terminate) {
        clayvm_vm_t *dvm = (clayvm_vm_t *)g_dap.vm;
        if (dvm) dvm->running = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  DAP println hook — redirect output to DAP output events           */
/* ------------------------------------------------------------------ */

/* Saved original stdout for DAP protocol messages */
static FILE *dap_original_stdout = NULL;

/* ------------------------------------------------------------------ */
/*  Handle a single DAP request (during the initial handshake phase)  */
/* ------------------------------------------------------------------ */

static int dap_handle_request(cJSON *msg) {
    cJSON *cmd_item = cJSON_GetObjectItem(msg, "command");
    if (!cmd_item || !cmd_item->valuestring) {
        fprintf(stderr, "clay-dap: request missing 'command' field\n");
        fflush(stderr);
        return 0;
    }
    const char *command = cmd_item->valuestring;

    if (strcmp(command, "initialize") == 0) {
        cJSON *resp = dap_base_response(msg, 1);
        cJSON *body = cJSON_AddObjectToObject(resp, "body");
        cJSON_AddBoolToObject(body, "supportsConfigurationDoneRequest", 1);
        cJSON_AddBoolToObject(body, "supportsSingleThreadExecutionRequests", 1);
        cJSON_AddBoolToObject(body, "supportsStepBack", 0);
        cJSON_AddBoolToObject(body, "supportsSetVariable", 0);
        cJSON_AddBoolToObject(body, "supportsRestartFrame", 0);
        cJSON_AddBoolToObject(body, "supportsGotoTargetsRequest", 0);
        cJSON_AddBoolToObject(body, "supportsExceptionInfoRequest", 0);
        cJSON_AddBoolToObject(body, "supportsTerminateRequest", 1);
        cJSON_AddBoolToObject(body, "supportsConditionalBreakpoints", 1);
        cJSON_AddBoolToObject(body, "supportsHitConditionalBreakpoints", 1);
        cJSON_AddBoolToObject(body, "supportsEvaluateForHovers", 1);
        dap_send(resp);

        /* Send initialized event */
        dap_send_event("initialized");
        return 0;
    }

    if (strcmp(command, "launch") == 0) {
        /* Acknowledge launch — actual compilation/run happens after configurationDone */
        cJSON *resp = dap_base_response(msg, 1);
        dap_send(resp);
        g_dap.launched = 1;
        return 0;
    }

    if (strcmp(command, "setBreakpoints") == 0) {
        cJSON *args = cJSON_GetObjectItem(msg, "arguments");
        cJSON *source = cJSON_GetObjectItem(args, "source");
        const char *path = NULL;
        cJSON *pathItem = cJSON_GetObjectItem(source, "path");
        if (pathItem) path = pathItem->valuestring;

        /* Find or create source entry */
        int si = -1;
        for (int i = 0; i < g_dap.source_count; i++) {
            if (g_dap.sources[i].file && path && strcmp(g_dap.sources[i].file, path) == 0) {
                si = i; break;
            }
        }
        if (si < 0 && g_dap.source_count < DAP_MAX_SOURCES) {
            si = g_dap.source_count++;
            g_dap.sources[si].file = path ? strdup(path) : NULL;
        }

        /* Clear and set breakpoints */
        cJSON *breakpoints = cJSON_GetObjectItem(args, "breakpoints");
        cJSON *resp = dap_base_response(msg, 1);
        cJSON *body = cJSON_AddObjectToObject(resp, "body");
        cJSON *rbps = cJSON_AddArrayToObject(body, "breakpoints");

        if (si >= 0) {
            /* Free old conditions */
            for (int i = 0; i < g_dap.sources[si].bp.count; i++)
                free(g_dap.sources[si].bp.conditions[i]);
            g_dap.sources[si].bp.count = 0;
            int count = breakpoints ? cJSON_GetArraySize(breakpoints) : 0;
            for (int i = 0; i < count && i < DAP_MAX_BREAKPOINTS; i++) {
                cJSON *bp = cJSON_GetArrayItem(breakpoints, i);
                cJSON *line_item = bp ? cJSON_GetObjectItem(bp, "line") : NULL;
                if (!line_item) continue;
                int idx = g_dap.sources[si].bp.count;
                int bpline = line_item->valueint;
                g_dap.sources[si].bp.lines[idx] = bpline;
                /* Condition */
                cJSON *cond = bp ? cJSON_GetObjectItem(bp, "condition") : NULL;
                g_dap.sources[si].bp.conditions[idx] = (cond && cond->valuestring && cond->valuestring[0])
                    ? strdup(cond->valuestring) : NULL;
                /* Hit count */
                cJSON *hitCond = bp ? cJSON_GetObjectItem(bp, "hitCondition") : NULL;
                g_dap.sources[si].bp.hit_targets[idx] = (hitCond && hitCond->valuestring)
                    ? atoi(hitCond->valuestring) : 0;
                g_dap.sources[si].bp.hit_counts[idx] = 0;
                g_dap.sources[si].bp.count++;

                cJSON *rbp = cJSON_CreateObject();
                cJSON_AddBoolToObject(rbp, "verified", 1);
                cJSON_AddNumberToObject(rbp, "line", bpline);
                cJSON_AddItemToArray(rbps, rbp);
            }
        }

        dap_send(resp);
        return 0;
    }

    if (strcmp(command, "configurationDone") == 0) {
        cJSON *resp = dap_base_response(msg, 1);
        dap_send(resp);
        g_dap.configured = 1;
        return 1; /* signal to start execution */
    }

    if (strcmp(command, "threads") == 0) {
        cJSON *resp = dap_base_response(msg, 1);
        cJSON *body = cJSON_AddObjectToObject(resp, "body");
        cJSON *threads = cJSON_AddArrayToObject(body, "threads");
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "id", 1);
        cJSON_AddStringToObject(t, "name", "main");
        cJSON_AddItemToArray(threads, t);
        dap_send(resp);
        return 0;
    }

    if (strcmp(command, "disconnect") == 0) {
        cJSON *resp = dap_base_response(msg, 1);
        dap_send(resp);
        g_dap.terminate = 1;
        return 1;
    }

    if (strcmp(command, "terminate") == 0) {
        cJSON *resp = dap_base_response(msg, 1);
        dap_send(resp);
        g_dap.terminate = 1;
        return 1;
    }

    /* Unknown command — respond OK */
    cJSON *resp = dap_base_response(msg, 1);
    dap_send(resp);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  File reading helper                                                */
/* ------------------------------------------------------------------ */

static char *dap_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)len + 1);
    size_t nread = fread(buf, 1, (size_t)len, f);
    buf[nread] = '\0';
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Main DAP entry point                                               */
/* ------------------------------------------------------------------ */

int dap_run(const char *filename) {
    memset(&g_dap, 0, sizeof(g_dap));

    /* Save stdout for DAP protocol, then redirect stdout to stderr
     * so that program output (println) doesn't corrupt the DAP stream. */
#ifdef _WIN32
    int dap_fd = _dup(_fileno(stdout));
    g_dap_out = _fdopen(dap_fd, "w");
    /* Set DAP output to binary mode for exact protocol framing */
    _setmode(dap_fd, _O_BINARY);
    /* Set stdin to binary mode */
    _setmode(_fileno(stdin), _O_BINARY);
    /* Redirect stdout to stderr so println goes to debug console */
    _dup2(_fileno(stderr), _fileno(stdout));
#else
    int dap_fd = dup(fileno(stdout));
    g_dap_out = fdopen(dap_fd, "w");
    /* Redirect stdout to stderr */
    dup2(fileno(stderr), fileno(stdout));
#endif

    fprintf(stderr, "clay-dap: starting for %s\n", filename);
    fflush(stderr);

    /* Verify the source file exists before entering the handshake */
    {
        FILE *test = fopen(filename, "r");
        if (!test) {
            fprintf(stderr, "clay-dap: cannot open source file: %s\n", filename);
            fflush(stderr);
            return 1;
        }
        fclose(test);
        fprintf(stderr, "clay-dap: source file verified\n");
        fflush(stderr);
    }

    /* Phase 1: DAP handshake — process initialize, launch, setBreakpoints, configurationDone */
    while (!g_dap.configured && !g_dap.terminate) {
        cJSON *msg = dap_read();
        if (!msg) {
            fprintf(stderr, "clay-dap: failed to read message\n");
            return 1;
        }

        const char *type_str = NULL;
        cJSON *type = cJSON_GetObjectItem(msg, "type");
        if (type) type_str = type->valuestring;

        if (type_str && strcmp(type_str, "request") == 0) {
            int done = dap_handle_request(msg);
            cJSON_Delete(msg);
            if (done) break;
        } else {
            cJSON_Delete(msg);
        }
    }

    if (g_dap.terminate) return 0;

    /* Phase 2: Compile and run the Clay file */
    char *source = dap_read_file(filename);
    if (!source) {
        dap_send_output("stderr", "clay: cannot open source file\n");
        dap_send_event("terminated");
        return 1;
    }

    g_dap.source = source;
    g_dap.source_file = filename;

    /* Bootstrap runtime */
    clay_bootstrap();
    clay_source_file = filename;

    /* Set up VM */
    clayvm_vm_t vm;
    clayvm_vm_init(&vm);

    BcCompiler bc;
    bc_compiler_init(&bc, &vm);
    bc.source_file = filename;

    g_dap.bc = &bc;
    g_dap.vm = &vm;

    /* Install step callback for breakpoints and stepping */
    clayvm_vm_set_step_callback(&vm, dap_step_callback);

    /* Route program output through DAP output events */
    clay_output_fn = dap_output_hook;

    /* Parse */
    Parser parser;
    parser_init(&parser, source, filename);
    AstNode *program = parser_parse(&parser);

    if (!program || parser.had_error) {
        dap_send_output("stderr", "clay: compilation failed due to parse errors\n");
        dap_send_event("terminated");
        free(source);
        return 1;
    }

    /* Compile to bytecode */
    if (bc_compile(&bc, program) != 0) {
        dap_send_output("stderr", "clay: bytecode compilation failed\n");
        ast_free(program);
        dap_send_event("terminated");
        free(source);
        return 1;
    }


    /* Run using step mode (instruction-by-instruction with callback) */
    int exit_code = 0;
    vm.sp = 0;

    int caught = clay_try_push();
    if (caught != 0) {
        /* Exception occurred */
        ClayValue err = clay_error_stack.current_error;
        if (!CLAY_IS_NIL(err) && !CLAY_IS_INT(err)) {
            ClayValue msg_val = clay_send_val(err, "message", 0, NULL);
            const char *msg_str = clay_string_data(CLAY_OBJ(msg_val));
            char buf[512];
            snprintf(buf, sizeof(buf), "Error: %s\n", msg_str);
            dap_send_output("stderr", buf);

            /* Send stopped event with exception reason */
            dap_send_stopped("exception", msg_str);

            /* Process requests while stopped on exception */
            g_dap.stopped = 1;
            while (g_dap.stopped && !g_dap.terminate) {
                cJSON *req = dap_read();
                if (!req) { g_dap.terminate = 1; break; }
                cJSON *t_item = cJSON_GetObjectItem(req, "type");
                if (!t_item || !t_item->valuestring || strcmp(t_item->valuestring, "request") != 0) {
                    cJSON_Delete(req); continue;
                }
                cJSON *cmd_item2 = cJSON_GetObjectItem(req, "command");
                if (!cmd_item2 || !cmd_item2->valuestring) { cJSON_Delete(req); continue; }
                const char *cmd = cmd_item2->valuestring;
                if (strcmp(cmd, "continue") == 0 || strcmp(cmd, "disconnect") == 0) {
                        cJSON *resp = dap_base_response(req, 1);
                        dap_send(resp);
                        g_dap.stopped = 0;
                        if (strcmp(cmd, "disconnect") == 0) g_dap.terminate = 1;
                    } else if (strcmp(cmd, "threads") == 0 || strcmp(cmd, "stackTrace") == 0 ||
                               strcmp(cmd, "scopes") == 0 || strcmp(cmd, "variables") == 0) {
                        /* Reuse the stopped-state handlers from step_callback */
                        /* For simplicity, handle threads/stackTrace inline */
                        if (strcmp(cmd, "threads") == 0) {
                            cJSON *resp = dap_base_response(req, 1);
                            cJSON *body = cJSON_AddObjectToObject(resp, "body");
                            cJSON *threads = cJSON_AddArrayToObject(body, "threads");
                            cJSON *th = cJSON_CreateObject();
                            cJSON_AddNumberToObject(th, "id", 1);
                            cJSON_AddStringToObject(th, "name", "main");
                            cJSON_AddItemToArray(threads, th);
                            dap_send(resp);
                        } else if (strcmp(cmd, "stackTrace") == 0) {
                            cJSON *resp = dap_base_response(req, 1);
                            cJSON *body = cJSON_AddObjectToObject(resp, "body");
                            cJSON *frames = cJSON_AddArrayToObject(body, "stackFrames");
                            for (int i = clay_call_trace.depth - 1; i >= 0; i--) {
                                ClayTraceFrame *tf = &clay_call_trace.frames[i];
                                cJSON *fr = cJSON_CreateObject();
                                cJSON_AddNumberToObject(fr, "id", clay_call_trace.depth - i);
                                cJSON_AddStringToObject(fr, "name", tf->selector ? tf->selector : "<block>");
                                cJSON *src = cJSON_AddObjectToObject(fr, "source");
                                cJSON_AddStringToObject(src, "name", tf->file ? tf->file : "<unknown>");
                                if (tf->file) cJSON_AddStringToObject(src, "path", tf->file);
                                cJSON_AddNumberToObject(fr, "line", tf->line);
                                cJSON_AddNumberToObject(fr, "column", 1);
                                cJSON_AddItemToArray(frames, fr);
                            }
                            cJSON_AddNumberToObject(body, "totalFrames", clay_call_trace.depth);
                            dap_send(resp);
                        } else {
                            cJSON *resp = dap_base_response(req, 1);
                            cJSON *body = cJSON_AddObjectToObject(resp, "body");
                            if (strcmp(cmd, "scopes") == 0) {
                                cJSON_AddArrayToObject(body, "scopes");
                            } else {
                                cJSON_AddArrayToObject(body, "variables");
                            }
                            dap_send(resp);
                        }
                    } else {
                        cJSON *resp = dap_base_response(req, 1);
                        dap_send(resp);
                    }
                cJSON_Delete(req);
            }

            clay_release(CLAY_OBJ(msg_val));
        }
        exit_code = 1;
    } else {
        /* Normal execution — step through bytecode */
        vm.code = bc.code;
        vm.code_len = bc.len;
        vm.ip = 0;
        vm.sp = 0;
        vm.running = 1;

        while (vm.running && vm.ip < vm.code_len && !g_dap.terminate) {
            if (clayvm_vm_step(&vm) < 0) break;
        }

        clay_try_pop();
    }

    /* Send terminated event */
    if (!g_dap.terminate) {
        dap_send_event("terminated");

        /* Wait for disconnect */
        while (!g_dap.terminate) {
            cJSON *msg = dap_read();
            if (!msg) break;
            cJSON *t_item = cJSON_GetObjectItem(msg, "type");
            cJSON *c_item = cJSON_GetObjectItem(msg, "command");
            if (t_item && t_item->valuestring && strcmp(t_item->valuestring, "request") == 0 &&
                c_item && c_item->valuestring) {
                cJSON *resp = dap_base_response(msg, 1);
                dap_send(resp);
                if (strcmp(c_item->valuestring, "disconnect") == 0) g_dap.terminate = 1;
            }
            cJSON_Delete(msg);
        }
    }

    /* Cleanup */
    clay_output_fn = NULL;
    ast_free(program);
    free(source);
    clayvm_vm_free(&vm);

    for (int i = 0; i < g_dap.source_count; i++) {
        free(g_dap.sources[i].file);
        for (int j = 0; j < g_dap.sources[i].bp.count; j++)
            free(g_dap.sources[i].bp.conditions[j]);
    }

    if (g_dap_out) { fclose(g_dap_out); g_dap_out = NULL; }

    return exit_code;
}
