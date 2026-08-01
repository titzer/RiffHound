VIRGIL   = $(HOME)/virgil
V3C      = $(VIRGIL)/bin/v3c-x86-64-darwin
TERM_LIB = $(VIRGIL)/lib/term

LIBS  = $(TERM_LIB)/Vt100.v3
BASE  = RawStdin.v3 Animator.v3
SRCS  = $(BASE) ChordDemo.v3
KSRCS = $(BASE) Karaoke.v3

.PHONY: all run runk clean

all: ChordDemo Karaoke

ChordDemo: $(SRCS) $(LIBS)
	$(V3C) -program-name=ChordDemo -output=. $(LIBS) $(SRCS)

Karaoke: $(KSRCS) $(LIBS)
	$(V3C) -program-name=Karaoke -output=. $(LIBS) $(KSRCS)

run: ChordDemo
	./ChordDemo

runk: Karaoke
	./Karaoke

clean:
	rm -f ChordDemo Karaoke
