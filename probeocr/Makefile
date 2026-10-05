CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PKGS     = tesseract lept
CFLAGS  += $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lm

ifeq ($(OS),Windows_NT)
  EXE     = .exe
  LDLIBS += -lshell32
endif

GUI_SRC  = src/gui/main.c src/gui/ui.c src/gui/tree.c src/gui/canvas.c src/gui/batch.c src/gui/selftest.c
GUI_LIBS = $(shell pkg-config --libs raylib) -lpthread
ifeq ($(OS),Windows_NT)
  GUI_LIBS += -lopengl32 -lgdi32 -lwinmm -mwindows
else ifeq ($(shell uname),Darwin)
  GUI_LIBS += -framework Cocoa -framework IOKit -framework OpenGL -framework CoreVideo
endif

all: bin/probeocr$(EXE) bin/probeocr-gui$(EXE)

cli: bin/probeocr$(EXE)
gui: bin/probeocr-gui$(EXE)

bin/probeocr$(EXE): src/probeocr.c src/core.c src/core.h | bin
	$(CC) $(CFLAGS) -o $@ src/probeocr.c src/core.c $(LDLIBS)

# Clay is compiled on its own without our warning flags (vendored code).
bin/clay_impl.o: src/gui/clay_impl.c src/gui/vendor/clay.h | bin
	$(CC) -O2 -std=c11 -w -c -o $@ $<

bin/probeocr-gui$(EXE): $(GUI_SRC) src/gui/app.h src/core.c src/core.h bin/clay_impl.o | bin
	$(CC) $(CFLAGS) -Wno-missing-field-initializers $(shell pkg-config --cflags raylib) -o $@ $(GUI_SRC) src/core.c bin/clay_impl.o $(LDLIBS) $(GUI_LIBS)

bin:
	mkdir -p bin

clean:
	rm -rf bin

.PHONY: all cli gui clean
