# Host build of the buck CCM modulator test (no HAL, no target toolchain).
CC     ?= cc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter
SRC    := ../../Core/Src/buck_ccm.c test_buck_ccm.c
OUT    := build/test_buck_ccm

.PHONY: test clean
test: $(OUT)
	./$(OUT)

$(OUT): $(SRC) ../../Core/Inc/buck_ccm.h
	mkdir -p build
	$(CC) $(CFLAGS) -I../../Core/Inc $(SRC) -lm -o $@

clean:
	rm -rf build
