/*
 * vec3.c - Vec3 3D vector type with inline float x/y/z
 *
 * Vec3 stores its components directly in the ClayObject union (as.vec3.x/y/z)
 * rather than in hash table slots. This enables fast-path arithmetic in the
 * bytecode compiler — Vec3 ops run 50-100x faster than slot-based dispatch.
 */
#include "clay.h"
#include <math.h>
#include <stdio.h>

ClayObject *clay_vec3_new(float x, float y, float z) {
    ClayObject *obj = clay_object_new();
    obj->parent = clay_Vec3;
    clay_retain(clay_Vec3);
    obj->type = CLAY_TYPE_VEC3;
    obj->as.vec3.x = x;
    obj->as.vec3.y = y;
    obj->as.vec3.z = z;
    return obj;
}

/* Helper to extract float from tagged float or tagged int */
static float val_to_float(ClayValue v) {
    if (CLAY_IS_TAGGED_FLOAT(v)) return clay_float_untag(v);
    if (CLAY_IS_INT(v)) return (float)clay_to_int(v);
    return 0.0f;
}

/* --- Vec3 methods --- */

static ClayValue vec3_init(ClayObject *self, int argc, ClayValue *argv) {
    if (argc >= 3) {
        self->as.vec3.x = val_to_float(argv[0]);
        self->as.vec3.y = val_to_float(argv[1]);
        self->as.vec3.z = val_to_float(argv[2]);
    }
    return CLAY_NIL; /* object_new releases init_result, so don't return self */
}

static ClayValue vec3_x(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_float_tag(self->as.vec3.x);
}

static ClayValue vec3_y(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_float_tag(self->as.vec3.y);
}

static ClayValue vec3_z(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return clay_float_tag(self->as.vec3.z);
}

static ClayValue vec3_add(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_NIL;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_VEC3) return CLAY_NIL;
    return CLAY_VAL(clay_vec3_new(
        self->as.vec3.x + other->as.vec3.x,
        self->as.vec3.y + other->as.vec3.y,
        self->as.vec3.z + other->as.vec3.z));
}

static ClayValue vec3_sub(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_NIL;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_VEC3) return CLAY_NIL;
    return CLAY_VAL(clay_vec3_new(
        self->as.vec3.x - other->as.vec3.x,
        self->as.vec3.y - other->as.vec3.y,
        self->as.vec3.z - other->as.vec3.z));
}

static ClayValue vec3_mul_scalar(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    float s = val_to_float(argv[0]);
    return CLAY_VAL(clay_vec3_new(
        self->as.vec3.x * s, self->as.vec3.y * s, self->as.vec3.z * s));
}

static ClayValue vec3_div_scalar(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1) return CLAY_NIL;
    float s = val_to_float(argv[0]);
    if (s == 0.0f) {
        clay_raise(CLAY_VAL(clay_error_new("Vec3 division by zero", "DivisionByZero")));
        return CLAY_NIL;
    }
    float inv = 1.0f / s;
    return CLAY_VAL(clay_vec3_new(
        self->as.vec3.x * inv, self->as.vec3.y * inv, self->as.vec3.z * inv));
}

static ClayValue vec3_dot(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_NIL;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_VEC3) return CLAY_NIL;
    float d = self->as.vec3.x * other->as.vec3.x +
              self->as.vec3.y * other->as.vec3.y +
              self->as.vec3.z * other->as.vec3.z;
    return clay_float_tag(d);
}

static ClayValue vec3_cross(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_NIL;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_VEC3) return CLAY_NIL;
    float cx = self->as.vec3.y * other->as.vec3.z - self->as.vec3.z * other->as.vec3.y;
    float cy = self->as.vec3.z * other->as.vec3.x - self->as.vec3.x * other->as.vec3.z;
    float cz = self->as.vec3.x * other->as.vec3.y - self->as.vec3.y * other->as.vec3.x;
    return CLAY_VAL(clay_vec3_new(cx, cy, cz));
}

static ClayValue vec3_lengthSquared(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    float x = self->as.vec3.x, y = self->as.vec3.y, z = self->as.vec3.z;
    return clay_float_tag(x*x + y*y + z*z);
}

static ClayValue vec3_length(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    float x = self->as.vec3.x, y = self->as.vec3.y, z = self->as.vec3.z;
    return clay_float_tag(sqrtf(x*x + y*y + z*z));
}

static ClayValue vec3_normalize(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    float x = self->as.vec3.x, y = self->as.vec3.y, z = self->as.vec3.z;
    float len = sqrtf(x*x + y*y + z*z);
    if (len == 0.0f) return CLAY_VAL(clay_vec3_new(0, 0, 0));
    float inv = 1.0f / len;
    return CLAY_VAL(clay_vec3_new(x * inv, y * inv, z * inv));
}

/* Component-wise multiply */
static ClayValue vec3_mul(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_NIL;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_VEC3) return CLAY_NIL;
    return CLAY_VAL(clay_vec3_new(
        self->as.vec3.x * other->as.vec3.x,
        self->as.vec3.y * other->as.vec3.y,
        self->as.vec3.z * other->as.vec3.z));
}

static ClayValue vec3_reflect(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_NIL;
    ClayObject *n = CLAY_OBJ(argv[0]);
    if (n->type != CLAY_TYPE_VEC3) return CLAY_NIL;
    float d = self->as.vec3.x * n->as.vec3.x +
              self->as.vec3.y * n->as.vec3.y +
              self->as.vec3.z * n->as.vec3.z;
    float t = 2.0f * d;
    return CLAY_VAL(clay_vec3_new(
        self->as.vec3.x - n->as.vec3.x * t,
        self->as.vec3.y - n->as.vec3.y * t,
        self->as.vec3.z - n->as.vec3.z * t));
}

static ClayValue vec3_nearZero(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    float s = 0.00001f;
    return CLAY_BOOL(fabsf(self->as.vec3.x) < s &&
                     fabsf(self->as.vec3.y) < s &&
                     fabsf(self->as.vec3.z) < s);
}

static ClayValue vec3_negated(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    return CLAY_VAL(clay_vec3_new(-self->as.vec3.x, -self->as.vec3.y, -self->as.vec3.z));
}

static ClayValue vec3_toString(ClayObject *self, int argc, ClayValue *argv) {
    (void)argc; (void)argv;
    char buf[128];
    snprintf(buf, sizeof(buf), "Vec3(%g, %g, %g)",
             self->as.vec3.x, self->as.vec3.y, self->as.vec3.z);
    return CLAY_VAL(clay_string_new(buf));
}

static ClayValue vec3_eq(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_FALSE;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_VEC3) return CLAY_FALSE;
    return CLAY_BOOL(self->as.vec3.x == other->as.vec3.x &&
                     self->as.vec3.y == other->as.vec3.y &&
                     self->as.vec3.z == other->as.vec3.z);
}

static ClayValue vec3_neq(ClayObject *self, int argc, ClayValue *argv) {
    if (argc < 1 || CLAY_IS_INT(argv[0]) || CLAY_IS_NIL(argv[0])) return CLAY_TRUE;
    ClayObject *other = CLAY_OBJ(argv[0]);
    if (other->type != CLAY_TYPE_VEC3) return CLAY_TRUE;
    return CLAY_BOOL(self->as.vec3.x != other->as.vec3.x ||
                     self->as.vec3.y != other->as.vec3.y ||
                     self->as.vec3.z != other->as.vec3.z);
}

void clay_vec3_init_methods(ClayObject *proto) {
    clay_set_method(proto, "init", vec3_init);
    clay_set_method(proto, "x", vec3_x);
    clay_set_method(proto, "y", vec3_y);
    clay_set_method(proto, "z", vec3_z);
    clay_set_method(proto, "+", vec3_add);
    clay_set_method(proto, "-", vec3_sub);
    clay_set_method(proto, "*", vec3_mul_scalar);
    clay_set_method(proto, "/", vec3_div_scalar);
    clay_set_method(proto, "dot", vec3_dot);
    clay_set_method(proto, "cross", vec3_cross);
    clay_set_method(proto, "lengthSquared", vec3_lengthSquared);
    clay_set_method(proto, "length", vec3_length);
    clay_set_method(proto, "normalize", vec3_normalize);
    clay_set_method(proto, "mul", vec3_mul);
    clay_set_method(proto, "reflect", vec3_reflect);
    clay_set_method(proto, "nearZero", vec3_nearZero);
    clay_set_method(proto, "negated", vec3_negated);
    clay_set_method(proto, "toString", vec3_toString);
    clay_set_method(proto, "==", vec3_eq);
    clay_set_method(proto, "!=", vec3_neq);
}
