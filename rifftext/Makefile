all: unittest.jar

clean: 
	rm unittest unittest.jar

VIRGIL_LOC=~/virgil/

LIBS=$(VIRGIL_LOC)/lib/test/*.v3 $(VIRGIL_LOC)/lib/util/*.v3

unittest.jar: RiffIr.v3 RiffParser.v3 Unittest.v3 $(LIBS)
	v3c-jar -fun-exprs -program-name=unittest RiffIr.v3 RiffParser.v3 Unittest.v3 Operators.v3 $(LIBS)
