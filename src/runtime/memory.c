/*
 * memory.c - Reference counting memory management, and the cycle collector
 */
#include "clay.h"
#include <stdlib.h>
#include <string.h>

/* ---- Cycle collector state (the collector itself is at the end) ----
 * gc_color and gc_flags live in every object and cell. Colors are those of
 * Bacon & Rajan, "Concurrent Cycle Collection in Reference Counted Systems"
 * (2001), synchronous version: purple = a possible root of a garbage cycle
 * (its count went down, not to zero), gray = being trial-deleted, white =
 * garbage, black = in use (the resting state). */
enum { GC_BLACK = 0, GC_GRAY, GC_WHITE, GC_PURPLE };
#define GC_BUFFERED 1   /* in gc_roots */
#define GC_DEAD     2   /* freed while in gc_roots: an empty shell, freed by the next collection */
#define GC_GARBAGE  4   /* being freed by the collector */

static void gc_possible_root(uintptr_t node, uint8_t *color, uint8_t *flags);
static int  gc_unbuffer_recent(uintptr_t node);

void (*clay_ic_invalidate_hook)(void *methods_ptr) = NULL;

/* Memory diagnostics counters */
volatile long clay_alloc_count = 0;
volatile long clay_dealloc_count = 0;

/* Boolean singletons - statically allocated, never freed */
ClayObject clay_true_obj  = { NULL, NULL, NULL, CLAY_REFCOUNT_IMMORTAL, CLAY_TYPE_OBJECT, 0, 0, {{0}} };
ClayObject clay_false_obj = { NULL, NULL, NULL, CLAY_REFCOUNT_IMMORTAL, CLAY_TYPE_OBJECT, 0, 0, {{0}} };

/* Prototypes, true and false live for the whole run: retain/release leave
 * them alone (their count is never reached by real references) */
static int clay_is_singleton(ClayObject *obj) {
    return obj->refcount >= CLAY_REFCOUNT_IMMORTAL;
}

ClayObject *clay_retain(ClayObject *obj) {
    if (obj && !clay_is_singleton(obj)) {
        CLAY_ATOMIC_INC(&obj->refcount);
    }
    return obj;
}

/* Give an object's memory back (to the pool for plain objects) */
static void free_object_memory(ClayObject *obj) {
    if ((obj->type == CLAY_TYPE_OBJECT || obj->type == CLAY_TYPE_VEC3) && obj->slots) {
        ht_clear(obj->slots);
        obj->type = CLAY_TYPE_OBJECT;
        obj->gc_color = GC_BLACK;
        obj->gc_flags = 0;
        clay_object_pool_return(obj);
    } else {
        if (obj->slots) ht_free(obj->slots);
        free(obj);
    }
}

static void clay_dealloc(ClayObject *obj) {
    if (!obj || clay_is_singleton(obj)) return;
    CLAY_ATOMIC_INC(&clay_dealloc_count);

    /* Free fiber context before slots are released (ctx is stored in _ctx slot) */
    if (obj->slots) {
        ht_value_t ctx_val = ht_find(obj->slots, (ht_key_t)"_ctx");
        if (ctx_val) {
            ClayValue cv = (ClayValue)(uintptr_t)ctx_val;
            if (CLAY_IS_INT(cv)) {
                clay_fiber_context_free((intptr_t)clay_to_int(cv));
            }
        }
    }

    /* Close a socket nobody closed (its descriptor is in the _sock slot, -1 once closed) */
    if (obj->slots) {
        ht_value_t sock_val = ht_find(obj->slots, (ht_key_t)"_sock");
        if (sock_val) {
            ClayValue cv = (ClayValue)(uintptr_t)sock_val;
            if (CLAY_IS_INT(cv)) clay_socket_free((intptr_t)clay_to_int(cv));
        }
    }

    /* Release parent */
    if (obj->parent) {
        clay_release(obj->parent);
        obj->parent = NULL;
    }

    /* Free slot keys and release slot values */
    if (obj->slots) {
        size_t pos = 0;
        ht_key_t key;
        ht_value_t val;
        while (ht_next(obj->slots, &pos, &key, &val)) {
            free((void *)key);
            ClayValue cv = (ClayValue)(uintptr_t)val;
            if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && cv != CLAY_TRUE && cv != CLAY_FALSE) {
                clay_release(CLAY_OBJ(cv));
            }
        }
    }

    /* Free method keys and method structs */
    if (obj->methods) {
        clay_method_cache_invalidate(obj->methods);
        if (clay_ic_invalidate_hook) clay_ic_invalidate_hook(obj->methods);
        size_t pos = 0;
        ht_key_t key;
        ht_value_t val;
        while (ht_next(obj->methods, &pos, &key, &val)) {
            free((void *)key);
            free((void *)val);
        }
        ht_free(obj->methods);
        obj->methods = NULL;
    }

    /* Free type-specific data */
    switch (obj->type) {
        case CLAY_TYPE_STRING:
            free(obj->as.string.data);
            break;
        case CLAY_TYPE_ARRAY:
            if (obj->as.array.items) {
                for (size_t i = 0; i < obj->as.array.size; i++) {
                    ClayValue cv = obj->as.array.items[i];
                    if (CLAY_IS_PTR(cv) && !CLAY_IS_NIL(cv) && cv != CLAY_TRUE && cv != CLAY_FALSE) {
                        clay_release(CLAY_OBJ(cv));
                    }
                }
                free(obj->as.array.items);
            }
            break;
        case CLAY_TYPE_BLOCK:
            if (obj->as.block.env) {
                for (int i = 0; i < obj->as.block.env_size; i++) {
                    ClayValue cv = obj->as.block.env[i];
                    if (CLAY_IS_INT(cv) || CLAY_IS_TAGGED_FLOAT(cv) || CLAY_IS_NIL(cv) || cv == CLAY_TRUE || cv == CLAY_FALSE) {
                        continue; /* tagged int, nil, or bool — nothing to free */
                    }
                    if (obj->as.block.env_are_cells) {
                        clay_cell_release((ClayCell *)(uintptr_t)cv);
                    } else {
                        clay_release(CLAY_OBJ(cv));
                    }
                }
                free(obj->as.block.env);
            }
            if (obj->as.block.outer_self) {
                ClayObject *outer = obj->as.block.outer_self;
                obj->as.block.outer_self = NULL;
                clay_release(outer);
            }
            break;
        case CLAY_TYPE_DICT:
            if (obj->as.dict.map) clay_dict_free_contents(obj);
            break;
        case CLAY_TYPE_PLUGIN:
            if (obj->as.plugin.data && obj->as.plugin.free_fn)
                obj->as.plugin.free_fn(obj->as.plugin.data);
            break;
        default:
            break;
    }

    /* still in the collector's candidates: leave the (empty) shell for it to free */
    if ((obj->gc_flags & GC_BUFFERED) && !gc_unbuffer_recent((uintptr_t)obj)) {
        obj->gc_flags |= GC_DEAD;
        return;
    }
    free_object_memory(obj);
}

void clay_release(ClayObject *obj) {
    if (!obj || clay_is_singleton(obj)) return;
    if (CLAY_ATOMIC_DEC(&obj->refcount) <= 0) {
        clay_dealloc(obj);
    } else if (obj->gc_color != GC_PURPLE &&
               obj->type != CLAY_TYPE_STRING && obj->type != CLAY_TYPE_FLOAT) {
        /* still referenced: maybe only by a cycle it is in */
        gc_possible_root((uintptr_t)obj, &obj->gc_color, &obj->gc_flags);
    }
}

/* ---- Cells (heap-allocated boxes for by-reference capture) ---- */

ClayCell *clay_cell_new(ClayValue initial) {
    ClayCell *cell = (ClayCell *)malloc(sizeof(ClayCell));
    if (!cell) { fprintf(stderr, "clay: out of memory\n"); exit(1); }
    cell->refcount = 1;
    cell->gc_color = GC_BLACK;
    cell->gc_flags = 0;
    cell->value = initial;
    clay_retain_val(initial);
    return cell;
}

void clay_cell_retain(ClayCell *cell) {
    if (cell) CLAY_ATOMIC_INC(&cell->refcount);
}

void clay_cell_release(ClayCell *cell) {
    if (!cell) return;
    if (CLAY_ATOMIC_DEC(&cell->refcount) <= 0) {
        ClayValue v = cell->value;
        cell->value = CLAY_NIL;
        clay_release_val(v);
        if ((cell->gc_flags & GC_BUFFERED) && !gc_unbuffer_recent((uintptr_t)cell | 1)) {
            cell->gc_flags |= GC_DEAD;
            return;
        }
        free(cell);
    } else if (cell->gc_color != GC_PURPLE) {
        gc_possible_root((uintptr_t)cell | 1, &cell->gc_color, &cell->gc_flags);
    }
}


/* ======================================================================
 * Cycle collector
 *
 * Reference counting can't free objects that refer to each other: a node
 * whose child points back at it, a block in an object's slot that captured
 * the object, a local holding a block that calls itself. Each keeps the other
 * at a count of one or more after the program has let go of them all.
 *
 * Any object in such a cycle had its count go down without reaching zero at
 * the moment the program let go, so clay_release puts every such object in
 * gc_roots (purple). A collection then, starting from those:
 *   mark gray  takes away every count that one of the objects reachable from
 *              them holds on another (trial deletion);
 *   scan       an object whose count is still above zero is held from outside
 *              (a variable, the C or VM stack, a global, a fiber): it and
 *              everything it reaches are in use, and get their counts back
 *              (black); the rest are garbage (white);
 *   collect    the white objects let go of each other and are freed.
 * It never looks at stacks or globals, so it is safe wherever every holder of
 * a reference holds a count: at the start of a method or block (the safe
 * point in CLAY_STACK_CHECK), not in the middle of runtime code. A reference
 * the collector doesn't know how to follow (a plugin's data, a method made by
 * setMethod, a suspended fiber's stack) just looks like one from outside, so
 * what it holds is kept.
 *
 * A node is an object, or a cell with the low bit set.
 * ====================================================================== */

typedef uintptr_t GcNode;
#define GC_IS_CELL(n) ((n) & 1)
#define GC_CELL(n)    ((ClayCell *)((n) & ~(uintptr_t)1))
#define GC_OBJ(n)     ((ClayObject *)(n))

typedef struct { GcNode *items; size_t count, cap; } GcVec;

volatile int  clay_gc_pending = 0;
volatile long clay_gc_threads_running = 0;

static GcVec  gc_roots;      /* possible roots (purple, buffered) */
static GcVec  gc_stack;      /* work list for mark gray, scan and collect */
static GcVec  gc_black;      /* work list for scan black */
static GcVec  gc_garbage;    /* the white nodes being freed */
static int    gc_running = 0;
static int    gc_stress = 0; /* CLAY_GC_STRESS=1: collect at every safe point */
static size_t gc_work = 0;   /* nodes a collection visited */

#define GC_MIN_THRESHOLD 10000
static size_t gc_threshold = GC_MIN_THRESHOLD;

#ifdef CLAY_THREAD_SAFE
/* Only the thread running the program collects, and only while no worker
 * thread is running (other threads change counts as they go). Cycles a worker
 * let go of last are not found. */
static CLAY_THREAD_LOCAL int gc_home_thread = 0;
#define GC_HERE() (gc_home_thread && clay_gc_threads_running == 0)
#else
#define GC_HERE() 1
#endif

static void gc_push(GcVec *v, GcNode n) {
    if (v->count == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 256;
        GcNode *items = (GcNode *)realloc(v->items, cap * sizeof(GcNode));
        if (!items) { fprintf(stderr, "clay: out of memory\n"); exit(1); }
        v->items = items;
        v->cap = cap;
    }
    v->items[v->count++] = n;
}

static inline int *gc_rc(GcNode n) {
    return GC_IS_CELL(n) ? &GC_CELL(n)->refcount : &GC_OBJ(n)->refcount;
}
static inline uint8_t *gc_color(GcNode n) {
    return GC_IS_CELL(n) ? &GC_CELL(n)->gc_color : &GC_OBJ(n)->gc_color;
}
static inline uint8_t *gc_flags(GcNode n) {
    return GC_IS_CELL(n) ? &GC_CELL(n)->gc_flags : &GC_OBJ(n)->gc_flags;
}

static void gc_possible_root(GcNode node, uint8_t *color, uint8_t *flags) {
    if (*flags & GC_GARBAGE) return;        /* the collector is freeing it */
    if (!GC_HERE()) return;
    *color = GC_PURPLE;
    if (!(*flags & GC_BUFFERED)) {
        *flags |= GC_BUFFERED;
        gc_push(&gc_roots, node);
        if (gc_roots.count >= gc_threshold) clay_gc_pending = 1;
    }
}

/* A candidate freed by reference counting is usually one buffered just
 * before (a temporary whose count went 2 -> 1 -> 0): take it out of the
 * buffer if it is among the last few, so its memory goes back now. */
static int gc_unbuffer_recent(GcNode node) {
    if (gc_running) return 0;
    size_t stop = gc_roots.count > 8 ? gc_roots.count - 8 : 0;
    for (size_t i = gc_roots.count; i > stop; i--) {
        if (gc_roots.items[i - 1] == node) {
            gc_roots.items[i - 1] = gc_roots.items[--gc_roots.count];
            *gc_flags(node) &= (uint8_t)~GC_BUFFERED;
            return 1;
        }
    }
    return 0;
}

/* --- Following references: exactly the counts an object holds (what
 * clay_dealloc releases), less what the collector leaves alone --- */

static inline GcNode gc_value_node(ClayValue v) {
    if (!CLAY_IS_PTR(v) || CLAY_IS_NIL(v) || v == CLAY_TRUE || v == CLAY_FALSE) return 0;
    if (CLAY_OBJ(v)->refcount >= CLAY_REFCOUNT_IMMORTAL) return 0;   /* prototypes */
    return (GcNode)v;
}

static inline GcNode gc_env_node(ClayValue v, int cells) {
    if (!cells) return gc_value_node(v);
    if (!CLAY_IS_PTR(v) || CLAY_IS_NIL(v) || v == CLAY_TRUE || v == CLAY_FALSE) return 0;
    return (GcNode)v | 1;
}

typedef void (*GcVisit)(GcNode child);

static void gc_children(GcNode n, GcVisit visit) {
    GcNode c;
    if (GC_IS_CELL(n)) {
        if ((c = gc_value_node(GC_CELL(n)->value))) visit(c);
        return;
    }
    ClayObject *o = GC_OBJ(n);
    if (o->parent && (c = gc_value_node(CLAY_VAL(o->parent)))) visit(c);
    if (o->slots) {
        size_t pos = 0;
        ht_key_t key;
        ht_value_t val;
        while (ht_next(o->slots, &pos, &key, &val))
            if ((c = gc_value_node((ClayValue)(uintptr_t)val))) visit(c);
    }
    switch (o->type) {
        case CLAY_TYPE_ARRAY:
            for (size_t i = 0; i < o->as.array.size; i++)
                if ((c = gc_value_node(o->as.array.items[i]))) visit(c);
            break;
        case CLAY_TYPE_BLOCK:
            for (int i = 0; i < o->as.block.env_size; i++)
                if ((c = gc_env_node(o->as.block.env[i], o->as.block.env_are_cells))) visit(c);
            if (o->as.block.outer_self && (c = gc_value_node(CLAY_VAL(o->as.block.outer_self))))
                visit(c);
            break;
        case CLAY_TYPE_DICT:
            if (o->as.dict.map) {
                size_t pos = 0;
                const char *key;
                ClayValue val;
                while (clay_dict_next(o, &pos, &key, &val))
                    if ((c = gc_value_node(val))) visit(c);
            }
            break;
        default:
            break;
    }
}

/* --- mark gray: take away the counts the subgraph holds on itself --- */

static void gc_mark_gray_child(GcNode t) {
    (*gc_rc(t))--;
    if (*gc_color(t) != GC_GRAY) {
        *gc_color(t) = GC_GRAY;
        gc_push(&gc_stack, t);
    }
}

static void gc_mark_gray(GcNode s) {
    if (*gc_color(s) == GC_GRAY) return;
    *gc_color(s) = GC_GRAY;
    gc_push(&gc_stack, s);
    while (gc_stack.count > 0) {
        GcNode n = gc_stack.items[--gc_stack.count];
        gc_work++;
        gc_children(n, gc_mark_gray_child);
    }
}

/* --- scan: held from outside -> black (counts back), else white --- */

static void gc_scan_black_child(GcNode t) {
    (*gc_rc(t))++;
    if (*gc_color(t) != GC_BLACK) {
        *gc_color(t) = GC_BLACK;
        gc_push(&gc_black, t);
    }
}

static void gc_scan_black(GcNode s) {
    *gc_color(s) = GC_BLACK;
    gc_push(&gc_black, s);
    while (gc_black.count > 0) {
        GcNode n = gc_black.items[--gc_black.count];
        gc_children(n, gc_scan_black_child);
    }
}

static void gc_scan_child(GcNode t) { gc_push(&gc_stack, t); }

static void gc_scan(GcNode s) {
    gc_push(&gc_stack, s);
    while (gc_stack.count > 0) {
        GcNode n = gc_stack.items[--gc_stack.count];
        if (*gc_color(n) != GC_GRAY) continue;
        if (*gc_rc(n) > 0) {
            gc_scan_black(n);
        } else {
            *gc_color(n) = GC_WHITE;
            gc_children(n, gc_scan_child);
        }
    }
}

/* --- collect: gather the white nodes, and free them --- */

static void gc_collect_white(GcNode s) {
    gc_push(&gc_stack, s);
    while (gc_stack.count > 0) {
        GcNode n = gc_stack.items[--gc_stack.count];
        if (*gc_color(n) != GC_WHITE || (*gc_flags(n) & GC_GARBAGE)) continue;
        *gc_flags(n) |= GC_GARBAGE;
        gc_push(&gc_garbage, n);
        gc_children(n, gc_scan_child);
    }
}

static void gc_restore_child(GcNode t) { (*gc_rc(t))++; }

/* Drop every reference a garbage node holds (setting it to nil), so freeing
 * it releases nothing twice. Slots holding numbers stay: clay_dealloc reads
 * some (a fiber's _ctx, a socket's _sock) to free what they stand for. */
static void gc_clear(GcNode n) {
    if (GC_IS_CELL(n)) {
        ClayValue v = GC_CELL(n)->value;
        GC_CELL(n)->value = CLAY_NIL;
        clay_release_val(v);
        return;
    }
    ClayObject *o = GC_OBJ(n);
    if (o->parent && o->parent->refcount < CLAY_REFCOUNT_IMMORTAL) {
        ClayObject *p = o->parent;
        o->parent = NULL;
        clay_release(p);
    }
    if (o->slots) {
        size_t mark = gc_stack.count;
        size_t pos = 0;
        ht_key_t key;
        ht_value_t val;
        while (ht_next(o->slots, &pos, &key, &val))
            if (CLAY_IS_OBJ((ClayValue)(uintptr_t)val)) gc_push(&gc_stack, (GcNode)key);
        for (size_t i = mark; i < gc_stack.count; i++) {
            ht_key_t k = (ht_key_t)gc_stack.items[i];
            ClayValue v = (ClayValue)(uintptr_t)ht_find(o->slots, k);
            ht_add(o->slots, k, (ht_value_t)(uintptr_t)CLAY_NIL);   /* keeps the key */
            clay_release_val(v);
        }
        gc_stack.count = mark;
    }
    switch (o->type) {
        case CLAY_TYPE_ARRAY: {
            size_t size = o->as.array.size;
            o->as.array.size = 0;
            for (size_t i = 0; i < size; i++) {
                ClayValue v = o->as.array.items[i];
                o->as.array.items[i] = CLAY_NIL;
                clay_release_val(v);
            }
            break;
        }
        case CLAY_TYPE_BLOCK:
            for (int i = 0; i < o->as.block.env_size; i++) {
                ClayValue v = o->as.block.env[i];
                if (!CLAY_IS_OBJ(v) || v == CLAY_TRUE || v == CLAY_FALSE) continue;  /* e.g. a VM block's chunk id */
                o->as.block.env[i] = CLAY_NIL;
                if (o->as.block.env_are_cells) clay_cell_release((ClayCell *)(uintptr_t)v);
                else clay_release(CLAY_OBJ(v));
            }
            if (o->as.block.outer_self) {
                ClayObject *outer = o->as.block.outer_self;
                o->as.block.outer_self = NULL;
                clay_release(outer);
            }
            break;
        case CLAY_TYPE_DICT:
            if (o->as.dict.map) {
                size_t mark = gc_stack.count;
                size_t pos = 0;
                const char *key;
                ClayValue val;
                while (clay_dict_next(o, &pos, &key, &val))
                    if (CLAY_IS_OBJ(val)) gc_push(&gc_stack, (GcNode)key);
                for (size_t i = mark; i < gc_stack.count; i++) {
                    ht_key_t k = (ht_key_t)gc_stack.items[i];
                    ClayValue v = (ClayValue)(uintptr_t)ht_find(o->as.dict.map, k);
                    ht_add(o->as.dict.map, k, (ht_value_t)(uintptr_t)CLAY_NIL);
                    clay_release_val(v);
                }
                gc_stack.count = mark;
            }
            break;
        default:
            break;
    }
}

static void gc_free_garbage(void) {
    /* the white nodes' references to anything were taken away in mark gray
     * and never given back: give them back, so every count is true again */
    for (size_t i = 0; i < gc_garbage.count; i++)
        gc_children(gc_garbage.items[i], gc_restore_child);
    /* hold each one while they let go of each other, then let go */
    for (size_t i = 0; i < gc_garbage.count; i++) {
        GcNode n = gc_garbage.items[i];
        (*gc_rc(n))++;
        *gc_color(n) = GC_BLACK;
    }
    for (size_t i = 0; i < gc_garbage.count; i++) gc_clear(gc_garbage.items[i]);
    for (size_t i = 0; i < gc_garbage.count; i++) {
        GcNode n = gc_garbage.items[i];
        *gc_flags(n) = 0;
        if (GC_IS_CELL(n)) clay_cell_release(GC_CELL(n));
        else clay_release(GC_OBJ(n));
    }
    gc_garbage.count = 0;
}

static void gc_free_shell(GcNode n) {
    if (GC_IS_CELL(n)) free(GC_CELL(n));
    else free_object_memory(GC_OBJ(n));
}

long clay_gc_collect(void) {
    if (gc_running || !GC_HERE()) return 0;
    gc_running = 1;
    clay_gc_pending = gc_stress;
    long freed_before = clay_dealloc_count;
    gc_work = 0;

    /* take the candidates; anything buffered from here on waits for the next run */
    GcVec roots = gc_roots;
    gc_roots.items = NULL;
    gc_roots.count = gc_roots.cap = 0;

    size_t kept = 0;
    for (size_t i = 0; i < roots.count; i++) {
        GcNode n = roots.items[i];
        uint8_t *flags = gc_flags(n);
        if (*flags & GC_DEAD) { gc_free_shell(n); continue; }
        *flags &= (uint8_t)~GC_BUFFERED;
        if (*gc_color(n) == GC_PURPLE) roots.items[kept++] = n;
    }
    for (size_t i = 0; i < kept; i++)
        if (*gc_color(roots.items[i]) == GC_PURPLE) gc_mark_gray(roots.items[i]);
    for (size_t i = 0; i < kept; i++) gc_scan(roots.items[i]);
    for (size_t i = 0; i < kept; i++) gc_collect_white(roots.items[i]);
    free(roots.items);

    gc_free_garbage();

    /* the next run waits until there are about as many candidates as this
     * one had to look at, so the work stays in proportion */
    gc_threshold = gc_work > GC_MIN_THRESHOLD ? gc_work : GC_MIN_THRESHOLD;
    if (gc_roots.count >= gc_threshold) clay_gc_pending = 1;
    gc_running = 0;
    return clay_dealloc_count - freed_before;
}

void clay_gc_safepoint(void) {
    if (GC_HERE()) clay_gc_collect();
}

void clay_gc_init(void) {
#ifdef CLAY_THREAD_SAFE
    gc_home_thread = 1;
#endif
    const char *stress = getenv("CLAY_GC_STRESS");
    if (stress && *stress && strcmp(stress, "0") != 0) {
        gc_stress = 1;
        clay_gc_pending = 1;
    }
}
