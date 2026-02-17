CC = gcc
CFLAGS = -std=c99 -Wall -Wextra -pedantic -g -Isrc/runtime -Ideps/hash -Ideps/testy
AR = ar

# Source files
RUNTIME_SRCS = src/runtime/object.c src/runtime/memory.c src/runtime/dispatch.c \
               src/runtime/tagged.c src/runtime/string.c src/runtime/number.c \
               src/runtime/block.c src/runtime/array.c src/runtime/bootstrap.c
RUNTIME_OBJS = $(RUNTIME_SRCS:.c=.o)

COMPILER_SRCS = src/compiler/main.c src/compiler/lexer.c src/compiler/parser.c \
                src/compiler/ast.c src/compiler/codegen.c
COMPILER_OBJS = $(COMPILER_SRCS:.c=.o)

HASH_SRC = deps/hash/hash.c
HASH_OBJ = deps/hash/hash.o

TEST_SRCS = test/test_runtime.c deps/testy/test_main.c
TEST_OBJS = $(TEST_SRCS:.c=.o)

# Targets
.PHONY: all clean test

all: libclay.a clay

# Runtime static library
libclay.a: $(RUNTIME_OBJS) $(HASH_OBJ)
	$(AR) rcs $@ $^

$(HASH_OBJ): $(HASH_SRC)
	$(CC) $(CFLAGS) -c -o $@ $<

# Compiler binary
clay: $(COMPILER_OBJS) libclay.a
	$(CC) $(CFLAGS) -o $@ $(COMPILER_OBJS) -L. -lclay

# Test binary
test: test_runner
	./test_runner

test_runner: $(TEST_OBJS) libclay.a
	$(CC) $(CFLAGS) -o $@ $(TEST_OBJS) -L. -lclay

# Generic rule
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(RUNTIME_OBJS) $(COMPILER_OBJS) $(HASH_OBJ) $(TEST_OBJS)
	rm -f libclay.a clay test_runner
	rm -f *.exe
