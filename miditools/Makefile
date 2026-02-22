UNAME := $(shell uname)
CC     := clang
CFLAGS := -Wall -Wextra -O2

ifeq ($(UNAME), Darwin)
  LDFLAGS := -framework CoreMIDI -framework CoreFoundation
else ifeq ($(UNAME), Linux)
  LDFLAGS := -lasound
endif

SRCS := midi_stream.c midi_platform.c
HDRS := midi_stream.h midi_platform.h midi_decode.h

all: midiecho

midiecho: midiecho.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) midiecho.c $(SRCS) -o $@ $(LDFLAGS)

clean:
	rm -f midiecho
