# Makefile for L3VM.

SHELL=/bin/bash

SRCS=src/engine.c	\
     src/fail.c		\
     src/main.c		\
	 src/memory_mark_n_sweep.c

# clang sanitizers (see http://clang.llvm.org/docs/)
CLANG_SAN_FLAGS=-fsanitize=address -fsanitize=undefined

# Clang warning flags
CLANG_WARNING_FLAGS=-Weverything		\
                    -Wno-format-nonliteral	\
                    -Wno-c++98-compat		\
                    -Wno-gnu-label-as-value

CFLAGS_COMMON=-std=c11 ${CLANG_WARNING_FLAGS}

# Flags for debugging:
CFLAGS_DEBUG=${CFLAGS_COMMON} ${CLANG_SAN_FLAGS} -g

# Flags for maximum performance:
CFLAGS_RELEASE=${CFLAGS_COMMON} -O3 -DNDEBUG -march=native -flto

CFLAGS=${CFLAGS_RELEASE}
debug: CFLAGS=${CFLAGS_DEBUG}
stats: CFLAGS=${CFLAGS_RELEASE} -DGC_STATS
no0blocks: CFLAGS=${CFLAGS_RELEASE} -DNO_0_BLOCKS

all: vm

debug: all
stats: all
no0blocks: all

vm: ${SRCS}
	mkdir -p bin
	clang ${CFLAGS} ${LDFLAGS} ${SRCS} -o bin/vm

test: vm
	@echo
	@echo "Tests:"
	@echo -n "  - queens: "
	@((echo 8 0 | ./bin/vm test/queens.asm > /dev/null) && echo "ok")
	@echo -n "  - bignums: "
	@((echo 150 | ./bin/vm test/bignums.asm > /dev/null) && echo "ok")
	@echo -n "  - maze: "
	@((echo 10 10 | ./bin/vm test/maze.asm > /dev/null) && echo "ok")
	@echo -n "  - unimaze: "
	@((echo 50 40 10 | ./bin/vm test/unimaze.asm > /dev/null) && echo "ok")
	@echo
	@echo "Reminder: check the tests' output even if they passed!"

clean:
	rm -rf bin
