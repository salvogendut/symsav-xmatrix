#!/bin/bash
# Build xmatrix screensaver for SymbOS using scc

SCC="${SCC:-../scc/bin/cc}"

"$SCC" xmatrix.c \
    -N "Matrix" \
    -o xmatrix.sav \
    -h 512 \
    -lgfx

python3 add_preview.py
