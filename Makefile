CC=clang
CFLAGS=-O1 -Wall -Wextra -Werror -ggdb
# LDFLAGS=-fsanitize=address

BUILDDIR=build
BIN=server

SRCS=$(wildcard *.c)
OBJS=$(patsubst %.c,${BUILDDIR}/%.o,${SRCS})

.PHONY: all clean

all: ${OBJS}
	${CC} ${CFLAGS} ${OBJS} -o ${BIN} ${LDFLAGS}

${BUILDDIR}/%.o: %.c
	mkdir -p ${dir $@}
	${CC} -o $@ $< -c ${CLFAGS}

clean:
	rm -rf ${BIN} ${BUILDDIR}
