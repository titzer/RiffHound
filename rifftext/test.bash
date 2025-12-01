#!/bin/bash

VIRGIL_LOC="$HOME/virgil/"
LIBS="$VIRGIL_LOC/lib/test/*.v3 $VIRGIL_LOC/lib/util/*.v3"

if [ "$1" = "-fatal" ]; then
    V3C_OPTS="$V3C_OPTS -redef-field=UnitTests.fatal=true"
    shift
fi

v3i $V3C_OPTS -fun-exprs RiffIr.v3 RiffParser.v3 Unittest.v3 Operators.v3 $LIBS
