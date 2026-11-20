/*
 * block.c - Block/closure implementation
 */
#include "clay.h"

ClayObject *clay_block_new(ClayMethodFunc func, ClayValue *env, int env_size) {
    ClayObject *obj = clay_object_new();
    obj->type = CLAY_TYPE_BLOCK;
    obj->parent = clay_Block;
    if (clay_Block) clay_retain(clay_Block);
    obj->as.block.func = func;
    obj->as.block.env_are_cells = 0;
    obj->as.block.outer_self = NULL;

    if (env && env_size > 0) {
        obj->as.block.env = (ClayValue *)malloc(sizeof(ClayValue) * (size_t)env_size);
        obj->as.block.env_size = env_size;
        for (int i = 0; i < env_size; i++) {
            obj->as.block.env[i] = env[i];
            if (CLAY_IS_PTR(env[i]) && !CLAY_IS_NIL(env[i]) &&
                env[i] != CLAY_TRUE && env[i] != CLAY_FALSE) {
                clay_retain(CLAY_OBJ(env[i]));
            }
        }
    } else {
        obj->as.block.env = NULL;
        obj->as.block.env_size = 0;
    }

    return obj;
}

/* Cell-aware variant: env entries are ClayCell pointers, not ClayObject pointers */
ClayObject *clay_block_new_cells(ClayMethodFunc func, ClayValue *env, int env_size) {
    ClayObject *obj = clay_object_new();
    obj->type = CLAY_TYPE_BLOCK;
    obj->parent = clay_Block;
    if (clay_Block) clay_retain(clay_Block);
    obj->as.block.func = func;
    obj->as.block.env_are_cells = 1;
    obj->as.block.outer_self = NULL;

    if (env && env_size > 0) {
        obj->as.block.env = (ClayValue *)malloc(sizeof(ClayValue) * (size_t)env_size);
        obj->as.block.env_size = env_size;
        for (int i = 0; i < env_size; i++) {
            obj->as.block.env[i] = env[i];
            clay_cell_retain((ClayCell *)(uintptr_t)env[i]);
        }
    } else {
        obj->as.block.env = NULL;
        obj->as.block.env_size = 0;
    }

    return obj;
}

/* Block "value" messages - call the block with arguments */
static ClayValue block_value(ClayObject *self, int argc, ClayValue *argv) {
    if (self->type != CLAY_TYPE_BLOCK || !self->as.block.func) {
        clay_raise(CLAY_VAL(clay_error_new("value sent to non-block", "TypeError")));
    }
    return self->as.block.func(self, argc, argv);
}

static ClayValue block_whileTrue(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    ClayObject *body = CLAY_OBJ(argv[0]);
    ClayValue result = CLAY_NIL;
    int uw = clay_uw_mark();
    clay_uw_var(&result);       /* released if the block raises or returns */
    while (1) {
        ClayValue cond = self->as.block.func(self, 0, NULL);
        if (!CLAY_IS_TRUTHY(cond)) { clay_release_val(cond); break; }
        clay_release_val(cond);
        ClayTryFrame _tf;
        if (clay_try_push(&_tf) == 0) {
            ClayValue new_result = body->as.block.func(body, 0, NULL);
            clay_release_val(result);
            result = new_result;
            clay_try_pop();
        } else {
            /* Exception caught — check for break/continue signals */
            ClayValue err = clay_error_stack.current_error;
            int sig = clay_loop_signal(err);
            if (sig == CLAY_SIGNAL_BREAK) { clay_uw_reset(uw); return result; }
            if (sig == CLAY_SIGNAL_CONTINUE) continue;
            /* Re-raise unknown errors */
            clay_raise(err);
        }
    }
    clay_uw_reset(uw);
    return result;
}

static ClayValue block_whileFalse(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    ClayObject *body = CLAY_OBJ(argv[0]);
    ClayValue result = CLAY_NIL;
    int uw = clay_uw_mark();
    clay_uw_var(&result);       /* released if the block raises or returns */
    while (1) {
        ClayValue cond = self->as.block.func(self, 0, NULL);
        if (CLAY_IS_TRUTHY(cond)) { clay_release_val(cond); break; }
        clay_release_val(cond);
        ClayTryFrame _tf;
        int tp = clay_try_push(&_tf);
        if (tp == 0) {
            ClayValue new_result = body->as.block.func(body, 0, NULL);
            clay_release_val(result);
            result = new_result;
            clay_try_pop();
        } else {
            ClayValue err = clay_error_stack.current_error;
            int sig = clay_loop_signal(err);
            if (sig == CLAY_SIGNAL_BREAK) { clay_uw_reset(uw); return result; }
            if (sig == CLAY_SIGNAL_CONTINUE) continue;
            clay_raise(err);
        }
    }
    clay_uw_reset(uw);
    return result;
}

void clay_block_init_methods(ClayObject *proto) {
    clay_set_method(proto, "value", block_value);
    clay_set_method(proto, "whileTrue", block_whileTrue);
    clay_set_method(proto, "whileFalse", block_whileFalse);
}

/* Cells (clay_cell_new, clay_cell_retain, clay_cell_release) are in memory.c,
 * with the cycle collector. */
