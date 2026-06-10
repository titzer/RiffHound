VIRGIL_LOC=~/Code/virgil
LIBS=$(VIRGIL_LOC)/lib/util/*.v3
SRCS=GifLayouts.v3 GifIr.v3 GifParser.v3 gifdumper.main.v3

all: gifdumper

gifdumper: $(SRCS)
	v3c-host -program-name=gifdumper $(SRCS) $(LIBS)

run: gifdumper
	./gifdumper $(FILE)

clean:
	rm -f gifdumper

.PHONY: run clean
