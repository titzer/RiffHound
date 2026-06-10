VIRGIL_LOC=~/Code/virgil
LIBS=$(VIRGIL_LOC)/lib/util/*.v3
SRCS=PngLayouts.v3 PngIr.v3 PngParser.v3 pngdumper.main.v3

all: pngdumper

pngdumper: $(SRCS)
	v3c-host -program-name=pngdumper $(SRCS) $(LIBS)

run: pngdumper
	./pngdumper $(FILE)

clean:
	rm -f pngdumper

.PHONY: run clean
