.PHONY: clean all

all: midistats

clean:
	rm -f midistats Midistats

midistats: midistats.c
	clang -x objective-c -fobjc-arc midistats.c -framework Cocoa -o midistats
