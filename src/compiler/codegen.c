/*
 * codegen.c - Generate human-readable C from Clay AST
 *
 * Strategy:
 *   - Each Clay expression becomes a C expression returning ClayValue
 *   - Message sends become clay_send() calls
 *   - Binary ops become clay_send() calls
 *   - Methods become static C functions
 *   - Blocks become static C functions + clay_block_new()
 *   - #line directives map back to .clay source
 */
#include "codegen.h"
#include "range.h"
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define strdup _strdup
#endif

/* --- Helpers --- */

static void emit_indent(CodeGen *cg) {
    for (int i = 0; i < cg->indent; i++) fprintf(cg->out, "    ");
}

static void emit_line(CodeGen *cg, int line) {
    /* Use forward slashes for #line directives (portable, avoids C escape issues) */
    fprintf(cg->out, "#line %d \"", line);
    for (const char *p = cg->source_file; *p; p++) {
        fputc(*p == '\\' ? '/' : *p, cg->out);
    }
    fprintf(cg->out, "\"\n");
}

static char *temp_name(CodeGen *cg) {
    char *buf = (char *)malloc(32);
    snprintf(buf, 32, "_t%d", cg->temp_counter++);
    return buf;
}

static void push_owned_temp(CodeGen *cg, const char *name) {
    if (cg->owned_temp_count >= cg->owned_temp_capacity) {
        cg->owned_temp_capacity = cg->owned_temp_capacity == 0 ? 16 : cg->owned_temp_capacity * 2;
        cg->owned_temps = (char **)realloc(cg->owned_temps, sizeof(char *) * (size_t)cg->owned_temp_capacity);
    }
    cg->owned_temps[cg->owned_temp_count++] = strdup(name);
}

/* Emit clay_release_val() for all owned temps except result_temp.
 * Uses runtime alias check to handle methods that return self. */
static void emit_temp_cleanup(CodeGen *cg, const char *result_temp) {
    for (int i = 0; i < cg->owned_temp_count; i++) {
        if (result_temp && strcmp(cg->owned_temps[i], result_temp) == 0) {
            free(cg->owned_temps[i]);
            continue;
        }
        emit_indent(cg);
        if (result_temp) {
            fprintf(cg->out, "if (%s != %s) clay_release_val(%s);\n",
                    cg->owned_temps[i], result_temp, cg->owned_temps[i]);
        } else {
            fprintf(cg->out, "clay_release_val(%s);\n", cg->owned_temps[i]);
        }
        free(cg->owned_temps[i]);
    }
    cg->owned_temp_count = 0;
}

static char *method_func_name(CodeGen *cg, const char *obj_name, const char *method_name) {
    char *buf = (char *)malloc(128);
    snprintf(buf, 128, "_clay_method_%s_%s_%d", obj_name ? obj_name : "anon",
             method_name, cg->method_counter++);
    return buf;
}

static char *block_func_name(CodeGen *cg) {
    char *buf = (char *)malloc(64);
    snprintf(buf, 64, "_clay_block_%d", cg->method_counter++);
    return buf;
}

/* Escape a string for C output */
static void emit_c_string(CodeGen *cg, const char *s) {
    fputc('"', cg->out);
    for (; *s; s++) {
        switch (*s) {
            case '"':  fprintf(cg->out, "\\\""); break;
            case '\\': fprintf(cg->out, "\\\\"); break;
            case '\n': fprintf(cg->out, "\\n"); break;
            case '\t': fprintf(cg->out, "\\t"); break;
            case '\r': fprintf(cg->out, "\\r"); break;
            case '\0': fprintf(cg->out, "\\0"); break;
            default:
                if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7f) {
                    fprintf(cg->out, "\\x%02x", (unsigned char)*s);
                } else {
                    fputc(*s, cg->out);
                }
        }
    }
    fputc('"', cg->out);
}

/* Check if an operator string is safe for C identifier use */
static const char *op_func_suffix(const char *op) {
    if (strcmp(op, "+") == 0) return "add";
    if (strcmp(op, "-") == 0) return "sub";
    if (strcmp(op, "*") == 0) return "mul";
    if (strcmp(op, "/") == 0) return "div";
    if (strcmp(op, "%") == 0) return "mod";
    if (strcmp(op, "==") == 0) return "eq";
    if (strcmp(op, "!=") == 0) return "neq";
    if (strcmp(op, "<") == 0) return "lt";
    if (strcmp(op, ">") == 0) return "gt";
    if (strcmp(op, "<=") == 0) return "le";
    if (strcmp(op, ">=") == 0) return "ge";
    return "op";
}

/* Sanitize Clay identifiers that clash with C keywords */
static const char *c_safe_name(const char *name) {
    static const char *c_keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default",
        "do", "double", "else", "enum", "extern", "float", "for", "goto",
        "if", "int", "long", "register", "return", "short", "signed",
        "sizeof", "static", "struct", "switch", "typedef", "union",
        "unsigned", "void", "volatile", "while",
        /* C99 */
        "inline", "restrict", "_Bool", "_Complex", "_Imaginary",
        /* Common MSVC/GCC extensions */
        "bool", "true", "false", NULL
    };
    for (int i = 0; c_keywords[i]; i++) {
        if (strcmp(name, c_keywords[i]) == 0) {
            /* Return a prefixed name — caller must free if needed.
             * We use a static buffer since codegen runs single-threaded. */
            static char buf[128];
            snprintf(buf, sizeof(buf), "_cv_%s", name);
            return buf;
        }
    }
    return name;
}

/* --- Forward declarations for collecting methods/blocks --- */

/* Capture list for blocks (closure-captured variables) */
typedef struct {
    char **names;
    int count;
    int capacity;
} CaptureList;

static void capture_init(CaptureList *cl) {
    cl->names = NULL;
    cl->count = 0;
    cl->capacity = 0;
}

static int capture_has(CaptureList *cl, const char *name) {
    for (int i = 0; i < cl->count; i++) {
        if (strcmp(cl->names[i], name) == 0) return 1;
    }
    return 0;
}

static void capture_add(CaptureList *cl, const char *name) {
    if (capture_has(cl, name)) return;
    if (cl->count >= cl->capacity) {
        cl->capacity = cl->capacity == 0 ? 4 : cl->capacity * 2;
        cl->names = (char **)realloc(cl->names, sizeof(char *) * (size_t)cl->capacity);
    }
    cl->names[cl->count++] = strdup(name);
}

static void capture_free(CaptureList *cl) {
    for (int i = 0; i < cl->count; i++) free(cl->names[i]);
    free(cl->names);
    cl->names = NULL;
    cl->count = 0;
    cl->capacity = 0;
}

typedef struct MethodDef {
    char *func_name;
    char *obj_context; /* which object this method belongs to */
    AstNode *node;     /* NODE_METHOD_DEF or NODE_BLOCK */
    CaptureList captures; /* captured variables for blocks */
    const char **clone_slots; /* slot names from enclosing clone (Io-style assign) */
    int clone_slot_count;
    struct MethodDef *next;
} MethodDef;

static MethodDef *collected_methods = NULL;

static void collect_method(const char *func_name, const char *obj_ctx, AstNode *node) {
    MethodDef *m = (MethodDef *)malloc(sizeof(MethodDef));
    m->func_name = strdup(func_name);
    m->obj_context = obj_ctx ? strdup(obj_ctx) : NULL;
    m->node = node;
    capture_init(&m->captures);
    m->clone_slots = NULL;
    m->clone_slot_count = 0;
    m->next = collected_methods;
    collected_methods = m;
}

static void collect_method_with_captures(const char *func_name, const char *obj_ctx,
                                         AstNode *node, CaptureList *caps) {
    MethodDef *m = (MethodDef *)malloc(sizeof(MethodDef));
    m->func_name = strdup(func_name);
    m->obj_context = obj_ctx ? strdup(obj_ctx) : NULL;
    m->node = node;
    m->captures = *caps; /* take ownership */
    caps->names = NULL; caps->count = 0; caps->capacity = 0;
    m->clone_slots = NULL;
    m->clone_slot_count = 0;
    m->next = collected_methods;
    collected_methods = m;
}

/* --- Pre-assigned block/method names keyed by AST node pointer --- */

typedef struct NameEntry {
    AstNode *node;
    char *name;
    struct NameEntry *next;
} NameEntry;

static NameEntry *name_table = NULL;
static int name_counter = 0;

static void prewalk_assign_names(AstNode *node, const char *parent_name);

static const char *lookup_name(AstNode *node) {
    for (NameEntry *e = name_table; e; e = e->next) {
        if (e->node == node) return e->name;
    }
    return NULL;
}

static void register_name(AstNode *node, const char *name) {
    NameEntry *e = (NameEntry *)malloc(sizeof(NameEntry));
    e->node = node;
    e->name = strdup(name);
    e->next = name_table;
    name_table = e;
}

static void free_name_table(void) {
    NameEntry *e = name_table;
    while (e) {
        NameEntry *next = e->next;
        free(e->name);
        free(e);
        e = next;
    }
    name_table = NULL;
    name_counter = 0;
}

/* Recursively walk AST and assign function names to all blocks and methods */
static void prewalk_assign_names(AstNode *node, const char *parent_name) {
    if (!node) return;
    switch (node->type) {
        case NODE_BLOCK: {
            char buf[64];
            snprintf(buf, sizeof(buf), "_clay_block_%d", name_counter++);
            register_name(node, buf);
            /* Walk body for nested blocks */
            for (int i = 0; i < node->as.block.body.count; i++)
                prewalk_assign_names(node->as.block.body.items[i], buf);
            break;
        }
        case NODE_METHOD_DEF: {
            char buf[128];
            snprintf(buf, sizeof(buf), "_clay_method_%s_%s_%d",
                     parent_name ? parent_name : "anon",
                     node->as.method_def.name, name_counter++);
            register_name(node, buf);
            for (int i = 0; i < node->as.method_def.body.count; i++)
                prewalk_assign_names(node->as.method_def.body.items[i], buf);
            break;
        }
        case NODE_CLONE:
            prewalk_assign_names(node->as.clone.proto, parent_name);
            for (int i = 0; i < node->as.clone.members.count; i++)
                prewalk_assign_names(node->as.clone.members.items[i], parent_name);
            break;
        case NODE_ASSIGN:
            prewalk_assign_names(node->as.assign.value, node->as.assign.name);
            break;
        case NODE_SEND:
            prewalk_assign_names(node->as.send.receiver, parent_name);
            for (int i = 0; i < node->as.send.args.count; i++)
                prewalk_assign_names(node->as.send.args.items[i], parent_name);
            break;
        case NODE_BINARY:
            prewalk_assign_names(node->as.binary.left, parent_name);
            prewalk_assign_names(node->as.binary.right, parent_name);
            break;
        case NODE_ARRAY_LIT:
            for (int i = 0; i < node->as.array_lit.elements.count; i++)
                prewalk_assign_names(node->as.array_lit.elements.items[i], parent_name);
            break;
        case NODE_PROGRAM:
            for (int i = 0; i < node->as.program.stmts.count; i++)
                prewalk_assign_names(node->as.program.stmts.items[i], NULL);
            break;
        case NODE_SLOT_DEF:
            prewalk_assign_names(node->as.slot_def.value, parent_name);
            break;
        case NODE_TRY_CATCH:
            prewalk_assign_names(node->as.try_catch.try_block, parent_name);
            if (node->as.try_catch.catch_block)
                prewalk_assign_names(node->as.try_catch.catch_block, parent_name);
            break;
        case NODE_RAISE:
            prewalk_assign_names(node->as.raise.expr, parent_name);
            break;
        case NODE_SLOT_ASSIGN:
            prewalk_assign_names(node->as.slot_assign.receiver, parent_name);
            prewalk_assign_names(node->as.slot_assign.value, parent_name);
            break;
        case NODE_RETURN:
            if (node->as.return_stmt.expr)
                prewalk_assign_names(node->as.return_stmt.expr, parent_name);
            break;
        case NODE_DICT_LIT:
            for (int i = 0; i < node->as.dict_lit.values.count; i++)
                prewalk_assign_names(node->as.dict_lit.values.items[i], parent_name);
            break;
        case NODE_DESTRUCTURE:
            prewalk_assign_names(node->as.destructure.value, parent_name);
            break;
        default:
            break;
    }
}

/* --- Expression emission (returns a temp var name holding the ClayValue) --- */

static char *emit_expr(CodeGen *cg, AstNode *node);

static char *emit_int_lit(CodeGen *cg, AstNode *node) {
    char *t = temp_name(cg);
    emit_indent(cg);
    fprintf(cg->out, "ClayValue %s = clay_int(%ld);\n", t, node->as.int_lit.value);
    return t;
}

static char *emit_float_lit(CodeGen *cg, AstNode *node) {
    char *t = temp_name(cg);
    emit_indent(cg);
    /* Tagged float — no heap allocation, not an owned temp.
     * Use %.9g to preserve precision; always append .0 if no decimal point. */
    char buf[32];
    snprintf(buf, sizeof(buf), "%.9g", node->as.float_lit.value);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
        strcat(buf, ".0");
    }
    fprintf(cg->out, "ClayValue %s = clay_float_tag(%sf);\n", t, buf);
    return t;
}

static char *emit_string_lit(CodeGen *cg, AstNode *node) {
    char *t = temp_name(cg);
    emit_indent(cg);
    fprintf(cg->out, "ClayValue %s = CLAY_VAL(clay_string_new(", t);
    emit_c_string(cg, node->as.string_lit.value);
    fprintf(cg->out, "));\n");
    push_owned_temp(cg, t);
    return t;
}

static void push_local(CodeGen *cg, const char *name) {
    if (cg->local_count >= cg->local_capacity) {
        cg->local_capacity = cg->local_capacity == 0 ? 8 : cg->local_capacity * 2;
        cg->local_names = (char **)realloc(cg->local_names, sizeof(char *) * (size_t)cg->local_capacity);
    }
    cg->local_names[cg->local_count++] = strdup(name);
}

static int is_local(CodeGen *cg, const char *name) {
    for (int i = 0; i < cg->local_count; i++) {
        if (strcmp(cg->local_names[i], name) == 0) return 1;
    }
    return 0;
}

/* Check if name is a method/block-specific local (not inherited from outer scope) */
static int is_method_local(CodeGen *cg, const char *name) {
    for (int i = cg->method_local_base; i < cg->local_count; i++) {
        if (strcmp(cg->local_names[i], name) == 0) return 1;
    }
    return 0;
}

static void clear_locals(CodeGen *cg) {
    for (int i = 0; i < cg->local_count; i++) free(cg->local_names[i]);
    cg->local_count = 0;
}

static void push_boxed(CodeGen *cg, const char *name) {
    if (cg->boxed_count >= cg->boxed_capacity) {
        cg->boxed_capacity = cg->boxed_capacity == 0 ? 8 : cg->boxed_capacity * 2;
        cg->boxed_names = (char **)realloc(cg->boxed_names, sizeof(char *) * (size_t)cg->boxed_capacity);
    }
    cg->boxed_names[cg->boxed_count++] = strdup(name);
}

static int is_boxed(CodeGen *cg, const char *name) {
    for (int i = 0; i < cg->boxed_count; i++) {
        if (strcmp(cg->boxed_names[i], name) == 0) return 1;
    }
    return 0;
}

static void clear_boxed(CodeGen *cg) {
    for (int i = 0; i < cg->boxed_count; i++) free(cg->boxed_names[i]);
    cg->boxed_count = 0;
}

static int is_builtin_global(const char *name) {
    if (clay_plugin_is_global(name)) return 1;
    return strcmp(name, "Object") == 0 ||
           strcmp(name, "Number") == 0 ||
           strcmp(name, "String") == 0 ||
           strcmp(name, "Array") == 0 ||
           strcmp(name, "Block") == 0 ||
           strcmp(name, "Nil") == 0 ||
           strcmp(name, "Boolean") == 0 ||
           strcmp(name, "Error") == 0 ||
           strcmp(name, "Dict") == 0 ||
           strcmp(name, "Math") == 0 ||
           strcmp(name, "Console") == 0 ||
           strcmp(name, "File") == 0 ||
           strcmp(name, "System") == 0 ||
           strcmp(name, "FFI") == 0 ||
           strcmp(name, "Date") == 0 ||
           strcmp(name, "Http") == 0 ||
           strcmp(name, "Regex") == 0 ||
           strcmp(name, "Fiber") == 0 ||
           strcmp(name, "Db") == 0 ||
           strcmp(name, "Graphics") == 0 ||
           strcmp(name, "Range") == 0 ||
           strcmp(name, "true") == 0 ||
           strcmp(name, "false") == 0 ||
           strcmp(name, "nil") == 0;
}

static const char *builtin_c_expr(const char *name) {
    if (strcmp(name, "Object") == 0)  return "CLAY_VAL(clay_Object)";
    if (strcmp(name, "Number") == 0)  return "CLAY_VAL(clay_Number)";
    if (strcmp(name, "String") == 0)  return "CLAY_VAL(clay_String)";
    if (strcmp(name, "Array") == 0)   return "CLAY_VAL(clay_Array)";
    if (strcmp(name, "Block") == 0)   return "CLAY_VAL(clay_Block)";
    if (strcmp(name, "Nil") == 0)     return "CLAY_VAL(clay_Nil)";
    if (strcmp(name, "Boolean") == 0) return "CLAY_VAL(clay_Boolean)";
    if (strcmp(name, "Error") == 0)   return "CLAY_VAL(clay_Error)";
    if (strcmp(name, "Dict") == 0)    return "CLAY_VAL(clay_Dict)";
    if (strcmp(name, "Math") == 0)    return "CLAY_VAL(clay_Math)";
    if (strcmp(name, "Console") == 0) return "CLAY_VAL(clay_Console)";
    if (strcmp(name, "File") == 0)    return "CLAY_VAL(clay_File)";
    if (strcmp(name, "System") == 0)  return "CLAY_VAL(clay_System)";
    if (strcmp(name, "FFI") == 0)     return "CLAY_VAL(clay_FFI)";
    if (strcmp(name, "Date") == 0)    return "CLAY_VAL(clay_Date)";
    if (strcmp(name, "Http") == 0)    return "CLAY_VAL(clay_Http)";
    if (strcmp(name, "Regex") == 0)   return "CLAY_VAL(clay_Regex)";
    if (strcmp(name, "Fiber") == 0)   return "CLAY_VAL(clay_Fiber)";
    if (strcmp(name, "Db") == 0)      return "CLAY_VAL(clay_Db)";
    if (strcmp(name, "Graphics") == 0) return "CLAY_VAL(clay_Graphics)";
    if (strcmp(name, "Range") == 0)   return "CLAY_VAL(clay_Range)";
    if (strcmp(name, "true") == 0)return "CLAY_TRUE";
    if (strcmp(name, "false") == 0)   return "CLAY_FALSE";
    if (strcmp(name, "nil") == 0)     return "CLAY_NIL";
    return name;
}

/* --- Free-variable scanner for closure capture --- */

/* Helper: check if name is in a string array */
static int name_in_list(const char *name, char **list, int count) {
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i], name) == 0) return 1;
    }
    return 0;
}

static void scan_free_vars(AstNode *node, char **block_params, int param_count,
                           CaptureList *caps, CodeGen *cg);
static int is_known_slot(CodeGen *cg, const char *name);

static void scan_free_vars_list(NodeList *list, char **block_params, int param_count,
                                CaptureList *caps, CodeGen *cg) {
    for (int i = 0; i < list->count; i++)
        scan_free_vars(list->items[i], block_params, param_count, caps, cg);
}

static void scan_free_vars(AstNode *node, char **block_params, int param_count,
                           CaptureList *caps, CodeGen *cg) {
    if (!node) return;
    switch (node->type) {
        case NODE_IDENT: {
            const char *name = node->as.ident.name;
            /* Skip builtins, block params, and captures already recorded */
            if (is_builtin_global(name)) break;
            if (name_in_list(name, block_params, param_count)) break;
            if (capture_has(caps, name)) break;
            /* If it's a local in the enclosing scope, it needs capture */
            if (is_local(cg, name)) {
                capture_add(caps, name);
            } else if (is_local(cg, "_method_self") && !capture_has(caps, "_method_self")) {
                /* Not a local — in a method context this will be a slot read
                 * from the method's self, so capture _method_self */
                capture_add(caps, "_method_self");
            }
            break;
        }
        case NODE_BLOCK:
            /* Scan INTO nested blocks to find transitive captures */
            {
                int nested_pc = node->as.block.params.count;
                int combined_count = param_count + nested_pc;
                char **combined = NULL;
                if (combined_count > 0) {
                    combined = (char **)malloc(sizeof(char *) * (size_t)combined_count);
                    for (int i = 0; i < param_count; i++)
                        combined[i] = block_params[i];
                    for (int i = 0; i < nested_pc; i++)
                        combined[param_count + i] = node->as.block.params.items[i]->as.ident.name;
                }
                scan_free_vars_list(&node->as.block.body,
                                    combined, combined_count, caps, cg);
                free(combined);
            }
            break;
        case NODE_INT_LIT:
        case NODE_FLOAT_LIT:
        case NODE_STRING_LIT:
            break;
        case NODE_SELF:
            /* When a block inside a method uses 'self', it needs to capture
             * the method's self (not the block's own self parameter).
             * Only capture if _method_self is a local (i.e., we're in a method). */
            if (is_local(cg, "_method_self") && !capture_has(caps, "_method_self")) {
                capture_add(caps, "_method_self");
            }
            break;
        case NODE_SEND:
            scan_free_vars(node->as.send.receiver, block_params, param_count, caps, cg);
            scan_free_vars_list(&node->as.send.args, block_params, param_count, caps, cg);
            break;
        case NODE_BINARY:
            scan_free_vars(node->as.binary.left, block_params, param_count, caps, cg);
            scan_free_vars(node->as.binary.right, block_params, param_count, caps, cg);
            break;
        case NODE_ASSIGN:
            scan_free_vars(node->as.assign.value, block_params, param_count, caps, cg);
            /* Assignment target may also need capture if it's from enclosing scope */
            {
                const char *aname = node->as.assign.name;
                if (!is_builtin_global(aname) &&
                    !name_in_list(aname, block_params, param_count) &&
                    !capture_has(caps, aname) &&
                    is_local(cg, aname)) {
                    capture_add(caps, aname);
                }
                /* Slot write in a block inside a method needs _method_self */
                if (is_known_slot(cg, aname) &&
                    is_local(cg, "_method_self") &&
                    !capture_has(caps, "_method_self")) {
                    capture_add(caps, "_method_self");
                }
            }
            break;
        case NODE_CLONE:
            scan_free_vars(node->as.clone.proto, block_params, param_count, caps, cg);
            scan_free_vars_list(&node->as.clone.members, block_params, param_count, caps, cg);
            break;
        case NODE_METHOD_DEF:
            scan_free_vars_list(&node->as.method_def.body, block_params, param_count, caps, cg);
            break;
        case NODE_SLOT_DEF:
            scan_free_vars(node->as.slot_def.value, block_params, param_count, caps, cg);
            break;
        case NODE_ARRAY_LIT:
            scan_free_vars_list(&node->as.array_lit.elements, block_params, param_count, caps, cg);
            break;
        case NODE_TRY_CATCH:
            scan_free_vars(node->as.try_catch.try_block, block_params, param_count, caps, cg);
            if (node->as.try_catch.catch_block)
                scan_free_vars(node->as.try_catch.catch_block, block_params, param_count, caps, cg);
            break;
        case NODE_RAISE:
            scan_free_vars(node->as.raise.expr, block_params, param_count, caps, cg);
            break;
        case NODE_SLOT_ASSIGN:
            scan_free_vars(node->as.slot_assign.receiver, block_params, param_count, caps, cg);
            scan_free_vars(node->as.slot_assign.value, block_params, param_count, caps, cg);
            break;
        case NODE_RETURN:
            if (node->as.return_stmt.expr)
                scan_free_vars(node->as.return_stmt.expr, block_params, param_count, caps, cg);
            break;
        case NODE_DICT_LIT:
            scan_free_vars_list(&node->as.dict_lit.values, block_params, param_count, caps, cg);
            break;
        default:
            break;
    }
}

static int is_known_slot(CodeGen *cg, const char *name) {
    for (int i = 0; i < cg->known_slot_count; i++) {
        if (strcmp(cg->known_slots[i], name) == 0) return 1;
    }
    return 0;
}

static char *emit_ident(CodeGen *cg, AstNode *node) {
    char *t = temp_name(cg);
    emit_indent(cg);
    if (is_boxed(cg, node->as.ident.name)) {
        fprintf(cg->out, "ClayValue %s = %s_cell->value;\n", t, c_safe_name(node->as.ident.name));
    } else if (is_local(cg, node->as.ident.name)) {
        fprintf(cg->out, "ClayValue %s = %s;\n", t, c_safe_name(node->as.ident.name));
    } else if (is_builtin_global(node->as.ident.name)) {
        fprintf(cg->out, "ClayValue %s = %s;\n", t, builtin_c_expr(node->as.ident.name));
    } else if (cg->in_method && !is_local(cg, node->as.ident.name)) {
        if (is_boxed(cg, "_method_self")) {
            /* Inside a block within a method — use the captured method self */
            fprintf(cg->out, "ClayValue %s = clay_get_slot(CLAY_OBJ(_method_self_cell->value), \"%s\");\n",
                    t, node->as.ident.name);
        } else {
            fprintf(cg->out, "ClayValue %s = clay_get_slot(self, \"%s\");\n",
                    t, node->as.ident.name);
        }
    } else {
        fprintf(cg->out, "ClayValue %s = %s;\n", t, c_safe_name(node->as.ident.name));
    }
    return t;
}

static char *emit_self(CodeGen *cg, AstNode *node) {
    if (!cg->in_method) {
        fprintf(stderr, "codegen: 'self' used outside of method or block (line %d)\n", node->line);
        return NULL;
    }
    char *t = temp_name(cg);
    emit_indent(cg);
    if (is_boxed(cg, "_method_self")) {
        /* Inside a block that captured the enclosing method's self */
        fprintf(cg->out, "ClayValue %s = _method_self_cell->value;\n", t);
    } else {
        fprintf(cg->out, "ClayValue %s = CLAY_VAL(self);\n", t);
    }
    return t;
}

static char *emit_send(CodeGen *cg, AstNode *node) {
    /* Emit receiver */
    char *recv = emit_expr(cg, node->as.send.receiver);
    if (!recv) return NULL;

    /* Emit arguments */
    int argc = node->as.send.args.count;
    char **arg_temps = NULL;
    if (argc > 0) {
        arg_temps = (char **)malloc(sizeof(char *) * (size_t)argc);
        for (int i = 0; i < argc; i++) {
            arg_temps[i] = emit_expr(cg, node->as.send.args.items[i]);
        }
    }

    char *t = temp_name(cg);
    emit_line(cg, node->line);
    emit_indent(cg);
    if (node->as.send.is_super && cg->super_proto) {
        /* super send: start lookup from defining prototype's parent */
        if (argc == 0) {
            fprintf(cg->out, "ClayValue %s = clay_send_super(%s, CLAY_OBJ(%s), \"%s\", 0, NULL);\n",
                    t, recv, cg->super_proto, node->as.send.message);
        } else {
            fprintf(cg->out, "ClayValue %s = clay_send_super(%s, CLAY_OBJ(%s), \"%s\", %d, (ClayValue[]){",
                    t, recv, cg->super_proto, node->as.send.message, argc);
            for (int i = 0; i < argc; i++) {
                if (i > 0) fprintf(cg->out, ", ");
                fprintf(cg->out, "%s", arg_temps[i]);
            }
            fprintf(cg->out, "});\n");
        }
    } else if (argc == 0) {
        fprintf(cg->out, "ClayValue %s = clay_send_val(%s, \"%s\", 0, NULL);\n",
                t, recv, node->as.send.message);
    } else {
        fprintf(cg->out, "ClayValue %s = clay_send_val(%s, \"%s\", %d, (ClayValue[]){",
                t, recv, node->as.send.message, argc);
        for (int i = 0; i < argc; i++) {
            if (i > 0) fprintf(cg->out, ", ");
            fprintf(cg->out, "%s", arg_temps[i]);
        }
        fprintf(cg->out, "});\n");
    }

    free(recv);
    if (arg_temps) {
        for (int i = 0; i < argc; i++) free(arg_temps[i]);
        free(arg_temps);
    }
    push_owned_temp(cg, t);
    return t;
}

static char *emit_binary(CodeGen *cg, AstNode *node) {
    const char *op = node->as.binary.op;

    /* Short-circuit && and || */
    if (strcmp(op, "&&") == 0) {
        char *left = emit_expr(cg, node->as.binary.left);
        if (!left) return NULL;
        char *t = temp_name(cg);
        emit_line(cg, node->line);
        emit_indent(cg);
        fprintf(cg->out, "ClayValue %s;\n", t);
        emit_indent(cg);
        fprintf(cg->out, "if (CLAY_IS_TRUTHY(%s)) {\n", left);
        cg->indent++;
        char *right = emit_expr(cg, node->as.binary.right);
        emit_indent(cg);
        fprintf(cg->out, "%s = CLAY_BOOL(CLAY_IS_TRUTHY(%s));\n", t, right);
        free(right);
        cg->indent--;
        emit_indent(cg);
        fprintf(cg->out, "} else {\n");
        cg->indent++;
        emit_indent(cg);
        fprintf(cg->out, "%s = CLAY_FALSE;\n", t);
        cg->indent--;
        emit_indent(cg);
        fprintf(cg->out, "}\n");
        free(left);
        return t;
    }
    if (strcmp(op, "||") == 0) {
        char *left = emit_expr(cg, node->as.binary.left);
        if (!left) return NULL;
        char *t = temp_name(cg);
        emit_line(cg, node->line);
        emit_indent(cg);
        fprintf(cg->out, "ClayValue %s;\n", t);
        emit_indent(cg);
        fprintf(cg->out, "if (CLAY_IS_TRUTHY(%s)) {\n", left);
        cg->indent++;
        emit_indent(cg);
        fprintf(cg->out, "%s = CLAY_TRUE;\n", t);
        cg->indent--;
        emit_indent(cg);
        fprintf(cg->out, "} else {\n");
        cg->indent++;
        char *right = emit_expr(cg, node->as.binary.right);
        emit_indent(cg);
        fprintf(cg->out, "%s = CLAY_BOOL(CLAY_IS_TRUTHY(%s));\n", t, right);
        free(right);
        cg->indent--;
        emit_indent(cg);
        fprintf(cg->out, "}\n");
        free(left);
        return t;
    }
    /* Null-coalescing: lhs ?? rhs — returns lhs if non-nil, else rhs */
    if (strcmp(op, "??") == 0) {
        char *left = emit_expr(cg, node->as.binary.left);
        if (!left) return NULL;
        char *t = temp_name(cg);
        emit_line(cg, node->line);
        emit_indent(cg);
        fprintf(cg->out, "ClayValue %s;\n", t);
        emit_indent(cg);
        fprintf(cg->out, "if (%s != CLAY_NIL) {\n", left);
        cg->indent++;
        emit_indent(cg);
        fprintf(cg->out, "%s = %s;\n", t, left);
        cg->indent--;
        emit_indent(cg);
        fprintf(cg->out, "} else {\n");
        cg->indent++;
        char *right = emit_expr(cg, node->as.binary.right);
        emit_indent(cg);
        fprintf(cg->out, "%s = %s;\n", t, right);
        free(right);
        cg->indent--;
        emit_indent(cg);
        fprintf(cg->out, "}\n");
        free(left);
        return t;
    }
    /* Range literal: lhs..rhs */
    if (strcmp(op, "..") == 0) {
        char *left = emit_expr(cg, node->as.binary.left);
        if (!left) return NULL;
        char *right = emit_expr(cg, node->as.binary.right);
        if (!right) { free(left); return NULL; }
        char *t = temp_name(cg);
        emit_line(cg, node->line);
        emit_indent(cg);
        fprintf(cg->out, "ClayValue %s = CLAY_VAL(clay_range_new(clay_to_int(%s), clay_to_int(%s), 1));\n",
                t, left, right);
        free(left);
        free(right);
        push_owned_temp(cg, t);
        return t;
    }

    char *left = emit_expr(cg, node->as.binary.left);
    char *right = emit_expr(cg, node->as.binary.right);
    if (!left || !right) return NULL;

    char *t = temp_name(cg);
    emit_line(cg, node->line);
    emit_indent(cg);

    /* For tagged ints, we can optimize common operations */
    fprintf(cg->out, "ClayValue %s;\n", t);
    emit_indent(cg);
    fprintf(cg->out, "if (CLAY_IS_INT(%s) && CLAY_IS_INT(%s)) {\n", left, right);
    cg->indent++;
    emit_indent(cg);

    /* Direct integer operations for common cases */
    if (strcmp(op, "+") == 0) {
        fprintf(cg->out, "%s = clay_int(clay_to_int(%s) + clay_to_int(%s));\n", t, left, right);
    } else if (strcmp(op, "-") == 0) {
        fprintf(cg->out, "%s = clay_int(clay_to_int(%s) - clay_to_int(%s));\n", t, left, right);
    } else if (strcmp(op, "*") == 0) {
        fprintf(cg->out, "%s = clay_int(clay_to_int(%s) * clay_to_int(%s));\n", t, left, right);
    } else if (strcmp(op, "/") == 0) {
        fprintf(cg->out, "if (clay_to_int(%s) == 0) clay_raise(CLAY_VAL(clay_error_new(\"division by zero\", \"DivisionByZero\")));\n", right);
        emit_indent(cg);
        fprintf(cg->out, "%s = clay_int(clay_to_int(%s) / clay_to_int(%s));\n", t, left, right);
    } else if (strcmp(op, "%%") == 0) {
        fprintf(cg->out, "if (clay_to_int(%s) == 0) clay_raise(CLAY_VAL(clay_error_new(\"modulo by zero\", \"DivisionByZero\")));\n", right);
        emit_indent(cg);
        fprintf(cg->out, "%s = clay_int(clay_to_int(%s) %% clay_to_int(%s));\n", t, left, right);
    } else if (strcmp(op, "==") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_int(%s) == clay_to_int(%s));\n", t, left, right);
    } else if (strcmp(op, "!=") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_int(%s) != clay_to_int(%s));\n", t, left, right);
    } else if (strcmp(op, "<") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_int(%s) < clay_to_int(%s));\n", t, left, right);
    } else if (strcmp(op, ">") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_int(%s) > clay_to_int(%s));\n", t, left, right);
    } else if (strcmp(op, "<=") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_int(%s) <= clay_to_int(%s));\n", t, left, right);
    } else if (strcmp(op, ">=") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_int(%s) >= clay_to_int(%s));\n", t, left, right);
    } else {
        fprintf(cg->out, "%s = clay_send_val(%s, \"%s\", 1, (ClayValue[]){%s});\n",
                t, left, op, right);
    }

    cg->indent--;
    emit_indent(cg);
    fprintf(cg->out, "} else if (CLAY_IS_TAGGED_FLOAT(%s) || CLAY_IS_TAGGED_FLOAT(%s)) {\n", left, right);
    cg->indent++;
    emit_indent(cg);

    /* Direct float operations for common cases */
    if (strcmp(op, "+") == 0) {
        fprintf(cg->out, "%s = clay_float_tag(clay_to_float(%s) + clay_to_float(%s));\n", t, left, right);
    } else if (strcmp(op, "-") == 0) {
        fprintf(cg->out, "%s = clay_float_tag(clay_to_float(%s) - clay_to_float(%s));\n", t, left, right);
    } else if (strcmp(op, "*") == 0) {
        fprintf(cg->out, "%s = clay_float_tag(clay_to_float(%s) * clay_to_float(%s));\n", t, left, right);
    } else if (strcmp(op, "/") == 0) {
        fprintf(cg->out, "{ float _div = clay_to_float(%s); if (_div == 0.0f) clay_raise(CLAY_VAL(clay_error_new(\"division by zero\", \"DivisionByZero\")));\n", right);
        emit_indent(cg);
        fprintf(cg->out, "%s = clay_float_tag(clay_to_float(%s) / _div); }\n", t, left);
    } else if (strcmp(op, "==") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_float(%s) == clay_to_float(%s));\n", t, left, right);
    } else if (strcmp(op, "!=") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_float(%s) != clay_to_float(%s));\n", t, left, right);
    } else if (strcmp(op, "<") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_float(%s) < clay_to_float(%s));\n", t, left, right);
    } else if (strcmp(op, ">") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_float(%s) > clay_to_float(%s));\n", t, left, right);
    } else if (strcmp(op, "<=") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_float(%s) <= clay_to_float(%s));\n", t, left, right);
    } else if (strcmp(op, ">=") == 0) {
        fprintf(cg->out, "%s = CLAY_BOOL(clay_to_float(%s) >= clay_to_float(%s));\n", t, left, right);
    } else {
        fprintf(cg->out, "%s = clay_send_val(%s, \"%s\", 1, (ClayValue[]){%s});\n",
                t, left, op, right);
    }

    cg->indent--;
    emit_indent(cg);
    fprintf(cg->out, "} else {\n");
    cg->indent++;
    emit_indent(cg);
    fprintf(cg->out, "%s = clay_send_val(%s, \"%s\", 1, (ClayValue[]){%s});\n",
            t, left, op, right);
    cg->indent--;
    emit_indent(cg);
    fprintf(cg->out, "}\n");

    free(left);
    free(right);
    push_owned_temp(cg, t);
    return t;
}

static char *emit_block(CodeGen *cg, AstNode *node) {
    const char *fname = lookup_name(node);
    if (!fname) {
        fprintf(stderr, "codegen: block has no pre-assigned name\n");
        return NULL;
    }

    /* Scan for captured variables from enclosing scope */
    char **param_names = NULL;
    int param_count = node->as.block.params.count;
    if (param_count > 0) {
        param_names = (char **)malloc(sizeof(char *) * (size_t)param_count);
        for (int i = 0; i < param_count; i++)
            param_names[i] = node->as.block.params.items[i]->as.ident.name;
    }

    CaptureList caps;
    capture_init(&caps);
    for (int i = 0; i < node->as.block.body.count; i++)
        scan_free_vars(node->as.block.body.items[i], param_names, param_count, &caps, cg);
    free(param_names);

    collect_method_with_captures(fname, NULL, node, &caps);
    /* Propagate known slots from enclosing method to block */
    if (cg->in_method && cg->known_slots) {
        collected_methods->clone_slots = cg->known_slots;
        collected_methods->clone_slot_count = cg->known_slot_count;
    }

    char *t = temp_name(cg);

    /* Find the MethodDef we just added to get its captures */
    MethodDef *md = collected_methods; /* it's the most recent one */
    if (md->captures.count > 0) {
        /* Create cells for each captured variable (reuse existing cells) */
        for (int i = 0; i < md->captures.count; i++) {
            const char *name = md->captures.names[i];
            if (!is_boxed(cg, name)) {
                emit_indent(cg);
                if (strcmp(name, "_method_self") == 0) {
                    /* Capture the method's self (ClayObject* → ClayValue) */
                    fprintf(cg->out, "ClayCell *%s_cell = clay_cell_new(CLAY_VAL(self));\n", name);
                } else {
                    fprintf(cg->out, "ClayCell *%s_cell = clay_cell_new(%s);\n", c_safe_name(name), c_safe_name(name));
                }
                push_boxed(cg, name);
            }
        }
        /* Build env array with cell pointers */
        emit_indent(cg);
        fprintf(cg->out, "ClayValue %s_env[] = {", t);
        for (int i = 0; i < md->captures.count; i++) {
            if (i > 0) fprintf(cg->out, ", ");
            fprintf(cg->out, "(ClayValue)(uintptr_t)%s_cell", c_safe_name(md->captures.names[i]));
        }
        fprintf(cg->out, "};\n");
        emit_indent(cg);
        fprintf(cg->out, "ClayValue %s = CLAY_VAL(clay_block_new_cells(%s, %s_env, %d));\n",
                t, fname, t, md->captures.count);
    } else {
        emit_indent(cg);
        fprintf(cg->out, "ClayValue %s = CLAY_VAL(clay_block_new(%s, NULL, 0));\n", t, fname);
    }
    push_owned_temp(cg, t);
    return t;
}

static char *emit_array_lit(CodeGen *cg, AstNode *node) {
    char *t = temp_name(cg);
    emit_indent(cg);
    fprintf(cg->out, "ClayObject *%s_obj = clay_array_new();\n", t);

    for (int i = 0; i < node->as.array_lit.elements.count; i++) {
        char *elem = emit_expr(cg, node->as.array_lit.elements.items[i]);
        emit_indent(cg);
        fprintf(cg->out, "clay_array_append(%s_obj, %s);\n", t, elem);
        free(elem);
    }

    emit_indent(cg);
    fprintf(cg->out, "ClayValue %s = CLAY_VAL(%s_obj);\n", t, t);
    push_owned_temp(cg, t);
    return t;
}

static char *emit_dict_lit(CodeGen *cg, AstNode *node) {
    char *t = temp_name(cg);
    emit_indent(cg);
    fprintf(cg->out, "ClayObject *%s_obj = clay_dict_new();\n", t);

    for (int i = 0; i < node->as.dict_lit.keys.count; i++) {
        AstNode *key_node = node->as.dict_lit.keys.items[i];
        const char *key_str = key_node->as.string_lit.value;
        char *val = emit_expr(cg, node->as.dict_lit.values.items[i]);
        emit_indent(cg);
        fprintf(cg->out, "clay_dict_set(%s_obj, \"%s\", %s);\n", t, key_str, val);
        free(val);
    }

    emit_indent(cg);
    fprintf(cg->out, "ClayValue %s = CLAY_VAL(%s_obj);\n", t, t);
    push_owned_temp(cg, t);
    return t;
}

static char *emit_try_catch(CodeGen *cg, AstNode *node) {
    char *t = temp_name(cg);
    char *try_tmp = temp_name(cg);
    char *err_tmp = temp_name(cg);

    /* Declare result variable */
    emit_indent(cg);
    fprintf(cg->out, "ClayValue %s;\n", t);

    /* Emit try block as a block object */
    char *try_block = emit_expr(cg, node->as.try_catch.try_block);

    /* Emit catch block if present */
    char *catch_block = NULL;
    if (node->as.try_catch.catch_block) {
        catch_block = emit_expr(cg, node->as.try_catch.catch_block);
    }

    /* Emit finally block if present */
    char *finally_block = NULL;
    if (node->as.try_catch.finally_block) {
        finally_block = emit_expr(cg, node->as.try_catch.finally_block);
    }

    /* Declare try frame and push error frame */
    emit_indent(cg);
    fprintf(cg->out, "ClayTryFrame _try_%s;\n", t);
    emit_indent(cg);
    fprintf(cg->out, "if (clay_try_push(&_try_%s) == 0) {\n", t);
    cg->indent++;

    /* Execute try block */
    emit_indent(cg);
    fprintf(cg->out, "ClayValue %s = clay_send_val(%s, \"value\", 0, NULL);\n", try_tmp, try_block);
    emit_indent(cg);
    fprintf(cg->out, "clay_try_pop();\n");
    emit_indent(cg);
    fprintf(cg->out, "%s = %s;\n", t, try_tmp);

    cg->indent--;
    emit_indent(cg);
    fprintf(cg->out, "} else {\n");
    cg->indent++;

    /* Error was raised — get the error value */
    emit_indent(cg);
    fprintf(cg->out, "ClayValue %s = clay_error_stack.current_error;\n", err_tmp);

    if (catch_block) {
        emit_indent(cg);
        fprintf(cg->out, "%s = clay_send_val(%s, \"value\", 1, (ClayValue[]){ %s });\n",
                t, catch_block, err_tmp);
    } else {
        emit_indent(cg);
        fprintf(cg->out, "%s = CLAY_NIL;\n", t);
    }

    cg->indent--;
    emit_indent(cg);
    fprintf(cg->out, "}\n");

    /* Finally block — always executed */
    if (finally_block) {
        emit_indent(cg);
        fprintf(cg->out, "clay_send_val(%s, \"value\", 0, NULL);\n", finally_block);
        free(finally_block);
    }

    free(try_block);
    if (catch_block) free(catch_block);
    free(try_tmp);
    free(err_tmp);
    push_owned_temp(cg, t);
    return t;
}

static char *emit_raise(CodeGen *cg, AstNode *node) {
    char *val = emit_expr(cg, node->as.raise.expr);

    /* If the value is a string, wrap it in an Error object */
    char *t = temp_name(cg);
    emit_indent(cg);
    fprintf(cg->out, "ClayValue %s = %s;\n", t, val);
    emit_indent(cg);
    fprintf(cg->out, "if (CLAY_IS_PTR(%s) && !CLAY_IS_NIL(%s) && CLAY_OBJ(%s)->type == CLAY_TYPE_STRING) {\n",
            t, t, t);
    cg->indent++;
    emit_indent(cg);
    fprintf(cg->out, "%s = CLAY_VAL(clay_error_new(clay_string_data(CLAY_OBJ(%s)), \"RuntimeError\"));\n", t, t);
    cg->indent--;
    emit_indent(cg);
    fprintf(cg->out, "}\n");
    emit_indent(cg);
    fprintf(cg->out, "clay_raise(%s);\n", t);

    free(val);
    return t;
}

static char *emit_expr(CodeGen *cg, AstNode *node) {
    if (!node) return NULL;
    switch (node->type) {
        case NODE_INT_LIT:    return emit_int_lit(cg, node);
        case NODE_FLOAT_LIT:  return emit_float_lit(cg, node);
        case NODE_STRING_LIT: return emit_string_lit(cg, node);
        case NODE_IDENT:      return emit_ident(cg, node);
        case NODE_SELF:       return emit_self(cg, node);
        case NODE_SEND:       return emit_send(cg, node);
        case NODE_BINARY:     return emit_binary(cg, node);
        case NODE_BLOCK:      return emit_block(cg, node);
        case NODE_ARRAY_LIT:  return emit_array_lit(cg, node);
        case NODE_DICT_LIT:   return emit_dict_lit(cg, node);
        case NODE_TRY_CATCH:  return emit_try_catch(cg, node);
        case NODE_RAISE:      return emit_raise(cg, node);
        case NODE_CLONE: {
            /* Inline clone expression (e.g. Object clone {} inside methods) */
            char *proto = emit_expr(cg, node->as.clone.proto);
            if (!proto) return NULL;
            char *t = temp_name(cg);
            emit_indent(cg);
            fprintf(cg->out, "ClayValue %s = CLAY_VAL(clay_clone(CLAY_OBJ(%s)));\n", t, proto);
            free(proto);
            /* Emit slot/method members on the new clone */
            for (int i = 0; i < node->as.clone.members.count; i++) {
                AstNode *member = node->as.clone.members.items[i];
                if (member->type == NODE_SLOT_DEF) {
                    char *val = emit_expr(cg, member->as.slot_def.value);
                    if (val) {
                        emit_indent(cg);
                        fprintf(cg->out, "clay_set_slot(CLAY_OBJ(%s), \"%s\", %s);\n",
                                t, member->as.slot_def.name, val);
                        free(val);
                    }
                } else if (member->type == NODE_METHOD_DEF) {
                    const char *fname = lookup_name(member);
                    if (fname) {
                        collect_method(fname, NULL, member);
                        emit_indent(cg);
                        fprintf(cg->out, "clay_set_method(CLAY_OBJ(%s), \"%s\", %s);\n",
                                t, member->as.method_def.name, fname);
                    }
                }
            }
            push_owned_temp(cg, t);
            return t;
        }
        default:
            fprintf(stderr, "codegen: unhandled node type %d\n", node->type);
            return NULL;
    }
}

/* --- Emit statements --- */

static void emit_stmt(CodeGen *cg, AstNode *node) {
    if (!node) return;

    if (node->type == NODE_ASSIGN) {
        emit_line(cg, node->line);
        char *val = emit_expr(cg, node->as.assign.value);
        if (!val) return;
        emit_indent(cg);
        if (is_boxed(cg, node->as.assign.name)) {
            fprintf(cg->out, "%s_cell->value = %s;\n", c_safe_name(node->as.assign.name), val);
        } else {
            fprintf(cg->out, "%s = %s;\n", c_safe_name(node->as.assign.name), val);
        }
        /* Auto-set _name on bare clone: X = Y clone */
        if (node->as.assign.value && node->as.assign.value->type == NODE_SEND &&
            strcmp(node->as.assign.value->as.send.message, "clone") == 0) {
            emit_indent(cg);
            if (is_boxed(cg, node->as.assign.name)) {
                fprintf(cg->out, "clay_set_slot(CLAY_OBJ(%s_cell->value), \"_name\", CLAY_VAL(clay_string_new(\"%s\")));\n",
                        c_safe_name(node->as.assign.name), node->as.assign.name);
            } else {
                fprintf(cg->out, "clay_set_slot(CLAY_OBJ(%s), \"_name\", CLAY_VAL(clay_string_new(\"%s\")));\n",
                        c_safe_name(node->as.assign.name), node->as.assign.name);
            }
        }
        emit_temp_cleanup(cg, val);
        free(val);
        return;
    }

    /* Destructuring: [a, b, c] = expr */
    if (node->type == NODE_DESTRUCTURE) {
        emit_line(cg, node->line);
        char *src = emit_expr(cg, node->as.destructure.value);
        if (!src) return;
        /* Store source in a temp to avoid re-evaluation */
        emit_indent(cg);
        fprintf(cg->out, "ClayValue __destr_src = %s;\n", src);
        for (int i = 0; i < node->as.destructure.targets.count; i++) {
            const char *tname = node->as.destructure.targets.items[i]->as.ident.name;
            emit_indent(cg);
            fprintf(cg->out, "{ ClayValue __idx = CLAY_INT(%d); ", i);
            if (is_boxed(cg, tname)) {
                fprintf(cg->out, "%s_cell->value = clay_send_val(__destr_src, \"at\", 1, &__idx); }\n",
                        c_safe_name(tname));
            } else {
                fprintf(cg->out, "%s = clay_send_val(__destr_src, \"at\", 1, &__idx); }\n",
                        c_safe_name(tname));
            }
        }
        emit_temp_cleanup(cg, NULL);
        free(src);
        return;
    }

    /* Return: emit expression and early return from method/block */
    if (node->type == NODE_RETURN) {
        emit_line(cg, node->line);
        if (node->as.return_stmt.expr) {
            char *val = emit_expr(cg, node->as.return_stmt.expr);
            if (val) {
                emit_indent(cg);
                fprintf(cg->out, "__rv = %s;\n", val);
                emit_temp_cleanup(cg, val);
                free(val);
            }
        }
        emit_indent(cg);
        fprintf(cg->out, "return __rv;\n");
        return;
    }

    /* Break: raise BreakSignal */
    if (node->type == NODE_BREAK) {
        emit_line(cg, node->line);
        emit_indent(cg);
        fprintf(cg->out, "clay_raise(CLAY_VAL(clay_error_new(\"break\", \"BreakSignal\")));\n");
        return;
    }

    /* Continue: raise ContinueSignal */
    if (node->type == NODE_CONTINUE) {
        emit_line(cg, node->line);
        emit_indent(cg);
        fprintf(cg->out, "clay_raise(CLAY_VAL(clay_error_new(\"continue\", \"ContinueSignal\")));\n");
        return;
    }

    /* Slot assignment: receiver slot = value */
    if (node->type == NODE_SLOT_ASSIGN) {
        emit_line(cg, node->line);
        char *recv = emit_expr(cg, node->as.slot_assign.receiver);
        char *val = emit_expr(cg, node->as.slot_assign.value);
        if (recv && val) {
            emit_indent(cg);
            fprintf(cg->out, "clay_set_slot(CLAY_OBJ(%s), \"%s\", %s);\n",
                    recv, node->as.slot_assign.slot_name, val);
        }
        /* clay_set_slot retains the value, so release all temps */
        emit_temp_cleanup(cg, NULL);
        free(recv);
        free(val);
        return;
    }

    /* Expression statement — emit and discard result */
    char *val = emit_expr(cg, node);
    emit_temp_cleanup(cg, NULL);
    free(val);
    fprintf(cg->out, "\n");
}

/* --- Emit clone --- */

static void emit_clone(CodeGen *cg, AstNode *node, const char *var_name) {
    /* Emit the prototype expression */
    char *proto = emit_expr(cg, node->as.clone.proto);
    if (!proto) return;

    emit_line(cg, node->line);
    emit_indent(cg);
    fprintf(cg->out, "/* %s = ... clone({ ... }) */\n", var_name);
    emit_indent(cg);
    fprintf(cg->out, "%s = CLAY_VAL(clay_clone(CLAY_OBJ(%s)));\n", var_name, proto);
    free(proto);

    /* Collect slot names for Io-style assignment in methods */
    int slot_name_count = 0;
    for (int i = 0; i < node->as.clone.members.count; i++) {
        if (node->as.clone.members.items[i]->type == NODE_SLOT_DEF)
            slot_name_count++;
    }
    const char **slot_names = NULL;
    if (slot_name_count > 0) {
        slot_names = (const char **)malloc((size_t)slot_name_count * sizeof(const char *));
        int si = 0;
        for (int i = 0; i < node->as.clone.members.count; i++) {
            if (node->as.clone.members.items[i]->type == NODE_SLOT_DEF)
                slot_names[si++] = node->as.clone.members.items[i]->as.slot_def.name;
        }
    }

    /* Emit slot assignments and method registrations */
    for (int i = 0; i < node->as.clone.members.count; i++) {
        AstNode *member = node->as.clone.members.items[i];

        if (member->type == NODE_SLOT_DEF) {
            char *val = emit_expr(cg, member->as.slot_def.value);
            if (!val) continue;
            emit_indent(cg);
            fprintf(cg->out, "clay_set_slot(CLAY_OBJ(%s), \"%s\", %s);\n",
                    var_name, member->as.slot_def.name, val);
            free(val);
        } else if (member->type == NODE_METHOD_DEF) {
            const char *fname = lookup_name(member);
            if (!fname) {
                fprintf(stderr, "codegen: method has no pre-assigned name\n");
                continue;
            }
            collect_method(fname, var_name, member);
            /* Attach clone slot names to the just-collected method */
            collected_methods->clone_slots = slot_names;
            collected_methods->clone_slot_count = slot_name_count;
            emit_indent(cg);
            fprintf(cg->out, "clay_set_method(CLAY_OBJ(%s), \"%s\", %s);\n",
                    var_name, member->as.method_def.name, fname);
        }
    }

    /* slot_names is shared by all methods; freed when last MethodDef is freed */

    /* Auto-set _name slot to variable name */
    emit_indent(cg);
    fprintf(cg->out, "clay_set_slot(CLAY_OBJ(%s), \"_name\", CLAY_VAL(clay_string_new(\"%s\")));\n",
            var_name, var_name);
    /* Clean up any owned temps from slot value expressions */
    emit_temp_cleanup(cg, NULL);
}

/* --- Emit method/block function bodies --- */

static void emit_method_bodies(CodeGen *cg) {
    /* Process iteratively: emitting a method body may discover new nested blocks */
    while (collected_methods) {
        /* Reverse the current list */
        MethodDef *reversed = NULL;
        MethodDef *cur = collected_methods;
        while (cur) {
            MethodDef *next = cur->next;
            cur->next = reversed;
            reversed = cur;
            cur = next;
        }
        collected_methods = NULL; /* Clear so new blocks go into fresh list */

        cur = reversed;
        while (cur) {
        AstNode *node = cur->node;
        fprintf(cg->out, "\nstatic ClayValue %s(ClayObject *self, int argc, ClayValue *argv) {\n",
                cur->func_name);
        fprintf(cg->out, "    (void)self; (void)argc; (void)argv;\n");

        /* Enter method context */
        int saved_in_method = cg->in_method;
        int saved_local_count = cg->local_count;
        int saved_boxed_count = cg->boxed_count;
        int saved_method_local_base = cg->method_local_base;
        const char *saved_super_proto = cg->super_proto;
        const char **saved_known_slots = cg->known_slots;
        int saved_known_slot_count = cg->known_slot_count;
        cg->method_local_base = cg->local_count;
        cg->in_method = 1;
        cg->super_proto = cur->obj_context;
        cg->known_slots = cur->clone_slots;
        cg->known_slot_count = cur->clone_slot_count;

        if (node->type == NODE_METHOD_DEF) {
            /* Make method's self available for capture by nested blocks */
            push_local(cg, "_method_self");
            /* Register params as locals, handling default values */
            for (int i = 0; i < node->as.method_def.params.count; i++) {
                AstNode *param = node->as.method_def.params.items[i];
                if (param->type == NODE_ASSIGN) {
                    /* Default parameter: name = defaultExpr */
                    const char *pname = param->as.assign.name;
                    char *def_val = emit_expr(cg, param->as.assign.value);
                    fprintf(cg->out, "    ClayValue %s = (argc > %d) ? argv[%d] : %s;\n",
                            c_safe_name(pname), i, i, def_val ? def_val : "CLAY_NIL");
                    push_local(cg, pname);
                    free(def_val);
                } else {
                    fprintf(cg->out, "    ClayValue %s = (argc > %d) ? argv[%d] : CLAY_NIL;\n",
                            c_safe_name(param->as.ident.name), i, i);
                    push_local(cg, param->as.ident.name);
                }
            }
            fprintf(cg->out, "    ClayValue __rv = CLAY_NIL;\n");
            int saved_indent = cg->indent;
            cg->indent = 1;
            for (int i = 0; i < node->as.method_def.body.count; i++) {
                AstNode *stmt = node->as.method_def.body.items[i];
                if (stmt->type == NODE_ASSIGN) {
                    char *val = emit_expr(cg, stmt->as.assign.value);
                    if (val) {
                        emit_indent(cg);
                        if (is_boxed(cg, stmt->as.assign.name)) {
                            fprintf(cg->out, "%s_cell->value = %s;\n", c_safe_name(stmt->as.assign.name), val);
                        } else if (is_method_local(cg, stmt->as.assign.name)) {
                            /* Reassignment to existing method local — release old value */
                            fprintf(cg->out, "clay_release_val(%s);\n", c_safe_name(stmt->as.assign.name));
                            emit_indent(cg);
                            fprintf(cg->out, "%s = %s;\n", c_safe_name(stmt->as.assign.name), val);
                        } else if (is_known_slot(cg, stmt->as.assign.name) &&
                                   !is_local(cg, stmt->as.assign.name)) {
                            /* Known slot from enclosing clone → write to self */
                            fprintf(cg->out, "clay_set_slot(self, \"%s\", %s);\n",
                                    stmt->as.assign.name, val);
                        } else {
                            /* New method-local variable */
                            fprintf(cg->out, "ClayValue %s = %s;\n", c_safe_name(stmt->as.assign.name), val);
                            push_local(cg, stmt->as.assign.name);
                        }
                        fprintf(cg->out, "    __rv = %s;\n", val);
                        emit_temp_cleanup(cg, val);
                        free(val);
                    }
                } else if (stmt->type == NODE_SLOT_ASSIGN ||
                           stmt->type == NODE_RETURN ||
                           stmt->type == NODE_BREAK ||
                           stmt->type == NODE_CONTINUE ||
                           stmt->type == NODE_DESTRUCTURE) {
                    emit_stmt(cg, stmt);
                } else {
                    char *val = emit_expr(cg, stmt);
                    if (val) {
                        emit_indent(cg);
                        fprintf(cg->out, "__rv = %s;\n", val);
                        emit_temp_cleanup(cg, val);
                        free(val);
                    }
                }
            }
            cg->indent = saved_indent;
            fprintf(cg->out, "    return __rv;\n");
        } else if (node->type == NODE_BLOCK) {
            /* Declare captured variables from env as cell pointers */
            int saved_boxed_count = cg->boxed_count;
            for (int i = 0; i < cur->captures.count; i++) {
                fprintf(cg->out, "    ClayCell *%s_cell = (ClayCell *)(uintptr_t)self->as.block.env[%d];\n",
                        c_safe_name(cur->captures.names[i]), i);
                push_local(cg, cur->captures.names[i]);
                push_boxed(cg, cur->captures.names[i]);
            }
            /* Register block params as locals */
            for (int i = 0; i < node->as.block.params.count; i++) {
                AstNode *param = node->as.block.params.items[i];
                fprintf(cg->out, "    ClayValue %s = (argc > %d) ? argv[%d] : CLAY_NIL;\n",
                        c_safe_name(param->as.ident.name), i, i);
                push_local(cg, param->as.ident.name);
            }
            fprintf(cg->out, "    ClayValue __rv = CLAY_NIL;\n");
            int saved_indent = cg->indent;
            cg->indent = 1;
            for (int i = 0; i < node->as.block.body.count; i++) {
                AstNode *stmt = node->as.block.body.items[i];
                if (stmt->type == NODE_ASSIGN) {
                    char *val = emit_expr(cg, stmt->as.assign.value);
                    if (val) {
                        emit_indent(cg);
                        if (is_known_slot(cg, stmt->as.assign.name)) {
                            /* Slot write from block inside method — use _method_self */
                            if (is_boxed(cg, "_method_self")) {
                                fprintf(cg->out, "clay_set_slot(CLAY_OBJ(_method_self_cell->value), \"%s\", %s);\n",
                                        stmt->as.assign.name, val);
                            } else {
                                fprintf(cg->out, "clay_set_slot(self, \"%s\", %s);\n",
                                        stmt->as.assign.name, val);
                            }
                        } else if (is_boxed(cg, stmt->as.assign.name)) {
                            fprintf(cg->out, "%s_cell->value = %s;\n", c_safe_name(stmt->as.assign.name), val);
                        } else if (is_local(cg, stmt->as.assign.name)) {
                            /* Reassignment — release old value */
                            fprintf(cg->out, "clay_release_val(%s);\n", c_safe_name(stmt->as.assign.name));
                            emit_indent(cg);
                            fprintf(cg->out, "%s = %s;\n", c_safe_name(stmt->as.assign.name), val);
                        } else {
                            fprintf(cg->out, "ClayValue %s = %s;\n", c_safe_name(stmt->as.assign.name), val);
                            push_local(cg, stmt->as.assign.name);
                        }
                        fprintf(cg->out, "    __rv = %s;\n", val);
                        emit_temp_cleanup(cg, val);
                        free(val);
                    }
                } else if (stmt->type == NODE_SLOT_ASSIGN ||
                           stmt->type == NODE_RETURN ||
                           stmt->type == NODE_BREAK ||
                           stmt->type == NODE_CONTINUE ||
                           stmt->type == NODE_DESTRUCTURE) {
                    emit_stmt(cg, stmt);
                } else {
                    char *val = emit_expr(cg, stmt);
                    if (val) {
                        emit_indent(cg);
                        fprintf(cg->out, "__rv = %s;\n", val);
                        emit_temp_cleanup(cg, val);
                        free(val);
                    }
                }
            }
            cg->indent = saved_indent;
            fprintf(cg->out, "    return __rv;\n");

            /* Clear boxed state for this block scope */
            while (cg->boxed_count > saved_boxed_count) {
                cg->boxed_count--;
                free(cg->boxed_names[cg->boxed_count]);
            }
        }

        /* Restore context */
        cg->in_method = saved_in_method;
        cg->super_proto = saved_super_proto;
        cg->method_local_base = saved_method_local_base;
        cg->known_slots = saved_known_slots;
        cg->known_slot_count = saved_known_slot_count;
        /* Clear locals added in this scope */
        while (cg->local_count > saved_local_count) {
            cg->local_count--;
            free(cg->local_names[cg->local_count]);
        }
        /* Clear boxed vars from this scope */
        while (cg->boxed_count > saved_boxed_count) {
            cg->boxed_count--;
            free(cg->boxed_names[cg->boxed_count]);
        }

        fprintf(cg->out, "}\n");

        MethodDef *next = cur->next;
        capture_free(&cur->captures);
        free(cur->func_name);
        free(cur->obj_context);
        free(cur);
        cur = next;
    }
    } /* end while (collected_methods) */
}

/* --- Main emission --- */

void codegen_init(CodeGen *cg, FILE *out, const char *source_file) {
    cg->out = out;
    cg->source_file = source_file;
    cg->indent = 1;
    cg->temp_counter = 0;
    cg->method_counter = 0;
    cg->in_method = 0;
    cg->local_names = NULL;
    cg->local_count = 0;
    cg->local_capacity = 0;
    cg->method_local_base = 0;
    cg->boxed_names = NULL;
    cg->boxed_count = 0;
    cg->boxed_capacity = 0;
    cg->known_slots = NULL;
    cg->known_slot_count = 0;
    cg->owned_temps = NULL;
    cg->owned_temp_count = 0;
    cg->owned_temp_capacity = 0;
}

/* First pass: collect all variable names from assignments and emit as file-scoped statics.
 * This ensures module-level variables are accessible from static method functions
 * that capture them via closures. */
static void collect_vars(AstNode *program, FILE *out) {
    const char *declared[256];
    int decl_count = 0;
    for (int i = 0; i < program->as.program.stmts.count; i++) {
        AstNode *stmt = program->as.program.stmts.items[i];
        if (stmt->type == NODE_ASSIGN) {
            /* Skip if already declared */
            int found = 0;
            for (int j = 0; j < decl_count; j++) {
                if (strcmp(declared[j], stmt->as.assign.name) == 0) { found = 1; break; }
            }
            if (!found && decl_count < 256) {
                declared[decl_count++] = stmt->as.assign.name;
                fprintf(out, "static ClayValue %s = CLAY_NIL;\n", c_safe_name(stmt->as.assign.name));
            }
        } else if (stmt->type == NODE_DESTRUCTURE) {
            for (int j = 0; j < stmt->as.destructure.targets.count; j++) {
                const char *name = stmt->as.destructure.targets.items[j]->as.ident.name;
                int found = 0;
                for (int k = 0; k < decl_count; k++) {
                    if (strcmp(declared[k], name) == 0) { found = 1; break; }
                }
                if (!found && decl_count < 256) {
                    declared[decl_count++] = name;
                    fprintf(out, "static ClayValue %s = CLAY_NIL;\n", c_safe_name(name));
                }
            }
        }
    }
}

void codegen_emit(CodeGen *cg, AstNode *program) {
    collected_methods = NULL;

    /* Pre-walk: assign stable function names to all blocks and methods */
    free_name_table();
    prewalk_assign_names(program, NULL);

    /* Register top-level variable names as locals so blocks can capture them */
    for (int i = 0; i < program->as.program.stmts.count; i++) {
        AstNode *stmt = program->as.program.stmts.items[i];
        if (stmt->type == NODE_ASSIGN && !is_local(cg, stmt->as.assign.name)) {
            push_local(cg, stmt->as.assign.name);
        } else if (stmt->type == NODE_DESTRUCTURE) {
            for (int j = 0; j < stmt->as.destructure.targets.count; j++) {
                const char *name = stmt->as.destructure.targets.items[j]->as.ident.name;
                if (!is_local(cg, name)) push_local(cg, name);
            }
        }
    }

    /* Emit header */
    fprintf(cg->out, "/* Generated by Clay compiler */\n");
    fprintf(cg->out, "#include \"clay.h\"\n");
    fprintf(cg->out, "#include \"range.h\"\n\n");
    /* Fiber stubs — the real implementations live in bc_compiler.c (VM only) */
    fprintf(cg->out, "void clay_fiber_init_methods(ClayObject *proto) { (void)proto; }\n");
    fprintf(cg->out, "void clay_fiber_context_free(intptr_t p) { (void)p; }\n\n");

    /* Buffer main code in a temp file so we can emit method bodies first */
    FILE *main_stream = tmpfile();
    if (!main_stream) {
        fprintf(stderr, "codegen: failed to create temp file\n");
        return;
    }

    FILE *saved_out = cg->out;
    cg->out = main_stream;

    /* Generate main code body */
    for (int i = 0; i < program->as.program.stmts.count; i++) {
        AstNode *stmt = program->as.program.stmts.items[i];

        if (stmt->type == NODE_ASSIGN && stmt->as.assign.value &&
            stmt->as.assign.value->type == NODE_CLONE) {
            emit_clone(cg, stmt->as.assign.value, stmt->as.assign.name);
        } else {
            emit_stmt(cg, stmt);
        }
    }

    fflush(main_stream);

    /* Now switch back to real output and emit everything in order */
    cg->out = saved_out;

    /* Emit forward declarations for ALL pre-assigned function names */
    for (NameEntry *e = name_table; e; e = e->next) {
        fprintf(cg->out, "static ClayValue %s(ClayObject *self, int argc, ClayValue *argv);\n",
                e->name);
    }
    fprintf(cg->out, "\n");

    /* Emit file-scoped top-level variables (before method bodies so they're visible) */
    collect_vars(program, cg->out);
    fprintf(cg->out, "\n");

    /* Clear boxed state from main body emission before emitting method bodies.
     * Main body may have boxed variables (e.g., 'output', 'Cli') that should not
     * leak into unrelated method body scopes. */
    clear_boxed(cg);

    /* Emit method/block bodies */
    emit_method_bodies(cg);

    /* Emit main function */
    fprintf(cg->out, "\nint main(int argc, char **argv) {\n");
    fprintf(cg->out, "    clay_bootstrap();\n");
    fprintf(cg->out, "    clay_set_args(argc - 1, argv + 1);\n\n");

    /* Read back the main code buffer from temp file */
    rewind(main_stream);
    {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), main_stream)) > 0) {
            fwrite(buf, 1, n, cg->out);
        }
        fclose(main_stream);
    }

    fprintf(cg->out, "\n    clay_shutdown();\n");
    fprintf(cg->out, "    return 0;\n");
    fprintf(cg->out, "}\n");

    free_name_table();
}
