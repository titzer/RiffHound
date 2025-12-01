#!/bin/bash

VIRGIL_LOC="$HOME/virgil/"
LIBS="$VIRGIL_LOC/lib/test/*.v3 $VIRGIL_LOC/lib/util/*.v3"

v3i $V3C_OPTS -fun-exprs RiffIr.v3 RiffParser.v3 Unittest.v3 Operators.v3 $LIBS
