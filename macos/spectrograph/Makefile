.PHONY: clean all run

BUNDLE  = Spectrograph.app
BINARY  = $(BUNDLE)/Contents/MacOS/spectrograph
SOURCES = spectrograph.c fft.c

all: $(BUNDLE)

$(BUNDLE): $(SOURCES) fft.h Info.plist
	mkdir -p $(BUNDLE)/Contents/MacOS
	cp Info.plist $(BUNDLE)/Contents/
	clang -x objective-c -fobjc-arc $(SOURCES) \
	    -framework Cocoa \
	    -framework AVFoundation \
	    -framework Accelerate \
	    -o $(BINARY)

run: $(BUNDLE)
	open $(BUNDLE)

clean:
	rm -rf $(BUNDLE)
