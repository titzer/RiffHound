VIRGIL   = $(HOME)/virgil
V3C      = $(VIRGIL)/bin/v3c-x86-64-darwin
TERM_LIB = $(VIRGIL)/lib/term

LIBS  = $(TERM_LIB)/Vt100.v3
SRCS  = RawStdin.v3 Animator.v3 ChordDemo.v3

.PHONY: all run clean

all: ChordDemo

ChordDemo: $(SRCS) $(LIBS)
	$(V3C) -program-name=ChordDemo -output=. $(LIBS) $(SRCS)

run: ChordDemo
	./ChordDemo

clean:
	rm -f ChordDemo
