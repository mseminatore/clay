CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -pedantic -g \
         -D_POSIX_C_SOURCE=200809L \
         -Isrc/runtime -Isrc/compiler -Isrc/vm/generated \
         -Ideps/hash -Ideps/testy
# VM generated files use GNU computed gotos — drop -pedantic for them
VM_CFLAGS = -std=c99 -Wall -Wextra -g \
            -D_POSIX_C_SOURCE=200809L \
            -Isrc/runtime -Isrc/compiler -Isrc/vm/generated \
            -Ideps/hash -Ideps/testy
AR = ar

# Detect macOS for platform-specific linker flags
UNAME_S := $(shell uname -s)

# Source files
RUNTIME_SRCS = src/runtime/object.c src/runtime/memory.c src/runtime/dispatch.c \
               src/runtime/tagged.c src/runtime/string.c src/runtime/number.c \
               src/runtime/block.c src/runtime/array.c src/runtime/error.c \
               src/runtime/clay_dict.c src/runtime/math.c src/runtime/file.c \
               src/runtime/bootstrap.c src/runtime/intern.c
RUNTIME_OBJS = $(RUNTIME_SRCS:.c=.o)

VM_SRCS = src/vm/generated/vm.c src/vm/generated/native.c \
          src/vm/generated/assembler.c src/vm/generated/disasm.c \
          src/vm/generated/strings.c src/vm/generated/closures.c \
          src/vm/generated/bytecode.c src/vm/generated/verify.c \
          src/vm/generated/optimizer.c
VM_OBJS = $(VM_SRCS:.c=.o)

COMPILER_SRCS = src/compiler/main.c src/compiler/lexer.c src/compiler/parser.c \
                src/compiler/ast.c src/compiler/codegen.c \
                src/compiler/bc_compiler.c src/compiler/bricks.c
COMPILER_OBJS = $(COMPILER_SRCS:.c=.o)

HASH_SRC = deps/hash/hash.c
HASH_OBJ = deps/hash/hash.o

TEST_SRCS = test/test_runtime.c test/test_lexer.c test/test_parser.c deps/testy/test_main.c
TEST_OBJS = $(TEST_SRCS:.c=.o)

# Stack size flags for test_runner (BcCompiler + VM structs are large)
ifeq ($(UNAME_S),Darwin)
    STACK_FLAGS = -Wl,-stack_size,0x400000
else
    STACK_FLAGS = -Wl,-z,stacksize=4194304
endif

# Targets
.PHONY: all clean test test-clay

all: libclay.a libclayvm.a clay

# Runtime static library
libclay.a: $(RUNTIME_OBJS) $(HASH_OBJ)
	$(AR) rcs $@ $^

$(HASH_OBJ): $(HASH_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

# VM static library
libclayvm.a: $(VM_OBJS)
	$(AR) rcs $@ $^

# Compiler binary
clay: $(COMPILER_OBJS) libclay.a libclayvm.a
	$(CC) $(CFLAGS) -o $@ $(COMPILER_OBJS) -L. -lclayvm -lclay -lm

CLAY_TESTS = test/programs/test_numbers.clay test/programs/test_strings.clay \
             test/programs/test_arrays.clay test/programs/test_blocks.clay \
             test/programs/test_objects.clay test/programs/test_control.clay

# Test binary
test: test_runner
	./test_runner

# Clay integration tests (requires clay binary)
test-clay: clay
	@for f in $(CLAY_TESTS); do echo "=== $$f ==="; ./clay $$f || exit 1; done
	@echo "All Clay tests passed."

test_runner: $(TEST_OBJS) libclay.a libclayvm.a
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) -L. -lclayvm -lclay -lm $(STACK_FLAGS)

# VM generated files — compile without -pedantic
src/vm/generated/%.o: src/vm/generated/%.c
	$(CC) $(VM_CFLAGS) -c -o $@ $<

# Generic rule
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(RUNTIME_OBJS) $(COMPILER_OBJS) $(VM_OBJS) $(HASH_OBJ) $(TEST_OBJS)
	rm -f libclay.a libclayvm.a clay test_runner
	rm -f *.exe
