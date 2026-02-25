.PHONY: clean all

all: spectrograph

clean:
	rm -f spectrograph Spectrograph

spectrograph: spectrograph.c
	clang -x objective-c -fobjc-arc spectrograph.c -framework Cocoa -o spectrograph
