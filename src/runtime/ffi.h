/*
 * ffi.h - Foreign Function Interface for Clay
 *
 * Provides loading of shared libraries (.dll/.so/.dylib) and calling
 * C functions that follow the ClayMethodFunc signature:
 *   ClayValue fn(ClayObject *self, int argc, ClayValue *argv)
 *
 * Usage from Clay:
 *   lib = FFI load("./mylib.dll")
 *   result = lib call("my_function", arg1, arg2)
 */
#ifndef FFI_H
#define FFI_H

#include "clay.h"

/* Initialize FFI prototype and register methods */
void clay_ffi_init(ClayObject *proto);

#endif
