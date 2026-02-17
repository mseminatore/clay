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

/* Block "value" messages - call the block with arguments */
static ClayValue block_value(ClayObject *self, int argc, ClayValue *argv) {
    if (self->type != CLAY_TYPE_BLOCK || !self->as.block.func) {
        fprintf(stderr, "clay: value sent to non-block\n");
        exit(1);
    }
    return self->as.block.func(self, argc, argv);
}

void clay_block_init_methods(ClayObject *proto) {
    clay_set_method(proto, "value", block_value);
}
