.PHONY: clean all

all: metronome

clean:
	rm -f metronome Metronome

metronome: metronome.c
	clang -x objective-c -fobjc-arc metronome.c -framework Cocoa -o metronome
