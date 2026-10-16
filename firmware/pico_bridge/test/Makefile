# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Dave Hammer
# Host tests for the Pico decoder and the routing logic. No ESP-IDF needed.
#   make -C firmware/pico_bridge/test          build and run everything
#   make -C firmware/pico_bridge/test vectors ORACLE=<dir>  regenerate vectors.h from the Python oracle
#                                              (<dir> contains lutron_pico.py, the reference decoder)

CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
ORACLE ?=

decode_test: decode_test.c ../main/pico_decode.c ../main/pico_decode.h vectors.h
	$(CC) $(CFLAGS) -o $@ decode_test.c ../main/pico_decode.c -lm

logic_test: logic_test.c ../main/pico_logic.c ../main/pico_logic.h
	$(CC) $(CFLAGS) -o $@ logic_test.c ../main/pico_logic.c

.PHONY: run vectors clean
run: decode_test logic_test
	./decode_test
	./logic_test

vectors:
	@test -n "$(ORACLE)" || { echo "set ORACLE=<dir containing lutron_pico.py>" >&2; exit 2; }
	python3 -I gen_vectors.py $(ORACLE) > vectors.h

clean:
	rm -f decode_test logic_test

.DEFAULT_GOAL := run
