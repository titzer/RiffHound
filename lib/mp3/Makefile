VIRGIL_LOC=~/Code/virgil
LIBS=$(VIRGIL_LOC)/lib/util/*.v3
SRCS=Mp3Layouts.v3 Mp3Ir.v3 Mp3Parser.v3 mp3dumper.main.v3
PLAYALONG=$(HOME)/RiffHound/playalong

all: mp3dumper

mp3dumper: $(SRCS)
	v3c-host -program-name=mp3dumper $(SRCS) $(LIBS)

run: mp3dumper
	./mp3dumper $(FILE)

check: mp3dumper
	./mp3dumper "$(PLAYALONG)/Stairway To Heaven.mp3"
	./mp3dumper --frames --text "$(PLAYALONG)/Stairway To Heaven.mp3"
	./mp3dumper --audio "$(PLAYALONG)/A Hard Road.mp3"
	./mp3dumper --id3v1 "$(PLAYALONG)/A Hard Road.mp3"

clean:
	rm -f mp3dumper

.PHONY: run check clean
