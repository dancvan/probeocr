CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PKGS     = tesseract lept
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

ifeq ($(OS),Windows_NT)
  EXE     = .exe
  LDLIBS += -lshell32
endif

bin/probeocr$(EXE): src/probeocr.c | bin
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

bin:
	mkdir -p bin

clean:
	rm -rf bin

.PHONY: clean
