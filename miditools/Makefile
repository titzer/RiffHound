UNAME := $(shell uname)
CC     := clang
CFLAGS := -Wall -Wextra -O2

ifeq ($(UNAME), Darwin)
  LDFLAGS := -framework CoreMIDI -framework CoreFoundation
else ifeq ($(UNAME), Linux)
  LDFLAGS := -lasound
endif

COMMON := midi_stream.c midi_platform.c

all: midiecho

midiecho: midiecho.c $(COMMON)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f midiecho
