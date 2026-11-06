CC = aarch64-linux-gnu-gcc
CFLAGS = -O2 -Wall -Wextra -Werror -std=gnu11
LDFLAGS = -static
OUT ?= .

$(OUT)/w202-hall-events: main.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

.PHONY: all
all: $(OUT)/w202-hall-events
