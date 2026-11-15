# SPDX-License-Identifier: MIT
CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=c11

awdlctl: awdlctl.c
	$(CC) $(CFLAGS) -framework IOKit -framework CoreFoundation -o $@ $<

clean:
	rm -f awdlctl

.PHONY: clean
