.PHONY: clean all run

BUNDLE  = Tones.app
BINARY  = $(BUNDLE)/Contents/MacOS/tones
SOURCES = tones.c

all: $(BINARY)

$(BINARY): $(SOURCES) Info.plist
	mkdir -p $(BUNDLE)/Contents/MacOS
	cp Info.plist $(BUNDLE)/Contents/
	clang -x objective-c -fobjc-arc $(SOURCES) \
	    -framework Cocoa \
	    -framework AVFoundation \
	    -o $(BINARY)

run: $(BINARY)
	open $(BUNDLE)

clean:
	rm -rf $(BUNDLE)
