.PHONY: clean all

all: chords

clean:
	rm -f chords Chords

chords: chords.c
	clang -x objective-c -fobjc-arc chords.c -framework Cocoa -o chords
