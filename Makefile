CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -pedantic -g \
         -D_POSIX_C_SOURCE=200809L \
         -Isrc/runtime -Isrc/compiler -Isrc/vm/generated \
         -Ideps/hash -Ideps/testy -Ideps/crossline -Ideps/cjson -Ideps/sqlite
# VM generated files use GNU computed gotos — drop -pedantic for them
VM_CFLAGS = -std=c99 -Wall -Wextra -g \
            -D_POSIX_C_SOURCE=200809L \
            -Isrc/runtime -Isrc/compiler -Isrc/vm/generated \
            -Ideps/hash -Ideps/testy -Ideps/crossline -Ideps/cjson -Ideps/sqlite
AR = ar

# Detect platform
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# On macOS, _POSIX_C_SOURCE hides BSD/Darwin extensions (u_short, malloc_zone_t, etc.)
ifeq ($(UNAME_S),Darwin)
    CFLAGS += -D_DARWIN_C_SOURCE
    VM_CFLAGS += -D_DARWIN_C_SOURCE
endif

# Source files
RUNTIME_SRCS = src/runtime/object.c src/runtime/memory.c src/runtime/dispatch.c \
               src/runtime/tagged.c src/runtime/string.c src/runtime/number.c \
               src/runtime/block.c src/runtime/array.c src/runtime/error.c \
               src/runtime/clay_dict.c src/runtime/range.c src/runtime/math.c src/runtime/file.c \
               src/runtime/ffi.c src/runtime/date.c src/runtime/http.c \
               src/runtime/regex.c src/runtime/db.c \
               src/runtime/bootstrap.c src/runtime/intern.c
RUNTIME_OBJS = $(RUNTIME_SRCS:.c=.o)

VM_SRCS = src/vm/generated/vm.c src/vm/generated/native.c \
          src/vm/generated/assembler.c src/vm/generated/disasm.c \
          src/vm/generated/strings.c \
          src/vm/generated/bytecode.c src/vm/generated/verify.c \
          src/vm/generated/optimizer.c
VM_OBJS = $(VM_SRCS:.c=.o)

COMPILER_SRCS = src/compiler/main.c src/compiler/lexer.c src/compiler/parser.c \
                src/compiler/ast.c src/compiler/codegen.c \
                src/compiler/bc_compiler.c src/compiler/bricks.c \
                src/compiler/dap.c
COMPILER_OBJS = $(COMPILER_SRCS:.c=.o)

# Fiber context switching (assembly, Unix only)
FIBER_SRCS = src/runtime/fiber_ctx.c
FIBER_OBJS = $(FIBER_SRCS:.c=.o)
ifeq ($(UNAME_M),x86_64)
    FIBER_ASM = src/runtime/fiber_amd64.S
else ifeq ($(UNAME_M),aarch64)
    FIBER_ASM = src/runtime/fiber_arm64.S
else ifeq ($(UNAME_M),arm64)
    FIBER_ASM = src/runtime/fiber_arm64.S
endif
FIBER_ASM_OBJS = $(FIBER_ASM:.S=.o)

HASH_SRC = deps/hash/hash.c
HASH_OBJ = deps/hash/hash.o

CROSSLINE_SRC = deps/crossline/crossline.c
CROSSLINE_OBJ = deps/crossline/crossline.o

CJSON_SRC = deps/cjson/cJSON.c
CJSON_OBJ = deps/cjson/cJSON.o

SQLITE_SRC = deps/sqlite/sqlite3.c
SQLITE_OBJ = deps/sqlite/sqlite3.o

TEST_SRCS = test/test_runtime.c test/test_lexer.c test/test_parser.c deps/testy/test_main.c
TEST_OBJS = $(TEST_SRCS:.c=.o)

# Stack size flags for test_runner (BcCompiler + VM structs are large)
ifeq ($(UNAME_S),Darwin)
    STACK_FLAGS = -Wl,-stack_size,0x400000
else
    STACK_FLAGS = -Wl,-z,stacksize=4194304
endif

# Linker flags
LDFLAGS = -lm -ldl

# Optional OpenSSL for HTTPS
OPENSSL_FLAGS :=
OPENSSL_LIBS :=
ifneq ($(shell pkg-config --exists openssl 2>/dev/null && echo yes),)
    OPENSSL_FLAGS = -DCLAY_HAS_SSL $(shell pkg-config --cflags openssl)
    OPENSSL_LIBS = $(shell pkg-config --libs openssl)
endif

CLAY_TESTS = test/programs/test_numbers.clay test/programs/test_strings.clay \
             test/programs/test_arrays.clay test/programs/test_blocks.clay \
             test/programs/test_objects.clay test/programs/test_control.clay \
             test/programs/test_errors.clay

# Targets
.PHONY: all clean test test-clay

all: libclay.a libclayvm.a clay

# Runtime static library
libclay.a: $(RUNTIME_OBJS) $(HASH_OBJ) $(SQLITE_OBJ) $(FIBER_OBJS) $(FIBER_ASM_OBJS)
	$(AR) rcs $@ $^

$(HASH_OBJ): $(HASH_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

$(CROSSLINE_OBJ): $(CROSSLINE_SRC)
	$(CC) $(CFLAGS) -Wno-pedantic -c -o $@ $<

$(CJSON_OBJ): $(CJSON_SRC)
	$(CC) $(CFLAGS) -Wno-pedantic -c -o $@ $<

$(SQLITE_OBJ): $(SQLITE_SRC)
	$(CC) $(CFLAGS) -Wno-pedantic -Wno-unused-parameter -c -o $@ $<

# Fiber assembly
%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

# VM static library
libclayvm.a: $(VM_OBJS)
	$(AR) rcs $@ $^

# Compiler binary
clay: $(COMPILER_OBJS) libclay.a libclayvm.a $(CROSSLINE_OBJ) $(CJSON_OBJ)
	$(CC) $(CFLAGS) -o $@ $(COMPILER_OBJS) $(CROSSLINE_OBJ) $(CJSON_OBJ) \
		-L. -lclayvm -lclay $(LDFLAGS) $(OPENSSL_LIBS)

# Test binary
test: test_runner
	./test_runner

# Clay integration tests (requires clay binary)
test-clay: clay
	@for f in $(CLAY_TESTS); do echo "=== $$f ==="; ./clay $$f || exit 1; done
	@echo "All Clay tests passed."

test_runner: $(TEST_OBJS) libclay.a libclayvm.a $(CJSON_OBJ)
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) $(CJSON_OBJ) -L. -lclayvm -lclay $(LDFLAGS) $(STACK_FLAGS)

# VM generated files — compile without -pedantic
src/vm/generated/%.o: src/vm/generated/%.c
	$(CC) $(VM_CFLAGS) -c -o $@ $<

# Runtime files with OpenSSL flags
src/runtime/http.o: src/runtime/http.c
	$(CC) $(CFLAGS) $(OPENSSL_FLAGS) -c -o $@ $<

# Generic rule
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(RUNTIME_OBJS) $(COMPILER_OBJS) $(VM_OBJS) $(HASH_OBJ) $(TEST_OBJS)
	rm -f $(CROSSLINE_OBJ) $(CJSON_OBJ) $(SQLITE_OBJ)
	rm -f $(FIBER_OBJS) $(FIBER_ASM_OBJS)
	rm -f libclay.a libclayvm.a clay test_runner
	rm -f *.exe
