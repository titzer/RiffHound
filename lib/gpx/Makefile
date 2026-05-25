VIRGIL_LOC=~/Code/virgil
LIBS=$(VIRGIL_LOC)/lib/util/*.v3
SRCS=GpxLayouts.v3 GpxIr.v3 GpxParser.v3 gpxparser.main.v3
DUMPER_SRCS=GpxLayouts.v3 GpxIr.v3 GpxParser.v3 gpxdumper.main.v3
TEST_FILE=~/RiffHound/tabs/BBKing-TheThrillIsGone.gp3

all: gpxparser gpxdumper

gpxparser: $(SRCS)
	v3c-host -program-name=gpxparser $(SRCS) $(LIBS)

gpxdumper: $(DUMPER_SRCS)
	v3c-host -program-name=gpxdumper $(DUMPER_SRCS) $(LIBS)

run: gpxparser
	./gpxparser $(FILE)

dump: gpxdumper
	./gpxdumper $(FILE)

check: gpxparser gpxdumper
	./gpxparser $(TEST_FILE)
	./gpxdumper --info --list-tracks --markers $(TEST_FILE)

clean:
	rm -f gpxparser gpxdumper

.PHONY: run dump check clean
