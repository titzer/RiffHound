VIRGIL_LOC=~/Code/virgil
LIBS=$(VIRGIL_LOC)/lib/util/*.v3
SRCS=GpxLayouts.v3 GpxIr.v3 GpxParser.v3 gpxparser.main.v3
TEST_FILE=~/RiffHound/tabs/BBKing-TheThrillIsGone.gp3

all: gpxparser

gpxparser: $(SRCS)
	v3c-host -program-name=gpxparser $(SRCS) $(LIBS)

run: gpxparser
	./gpxparser $(FILE)

check: gpxparser
	./gpxparser $(TEST_FILE)

clean:
	rm -f gpxparser

.PHONY: run check clean
