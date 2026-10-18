#!/usr/bin/env bash
# Build and run the fog oracle: regenerate gen_samplefoglight.inc with emit.py, compile oracle.cpp, run it.
#
# The flags are required:
#   -m32                 the kernels are 32-bit throughout
#   -masm=intel          or GCC substitutes AT&T operands into the intel blocks
#   -fno-pic             ebx is the PIC register in 32-bit PIC; the kernels use it freely
#   -fomit-frame-pointer ebp is a general register here (saved/restored via SavedEBP)
set -eu
cd "$(dirname "$0")"

python3 ../emit.py ../../../Source/SoftDrv/Src/DrawSurf.cpp \
    MMXFogLight8 --sub SampleFogLight --alphafog 0 > gen_samplefoglight.inc

g++ -m32 -O1 -fno-pic -fomit-frame-pointer -masm=intel -std=c++17 \
    -o oracle oracle.cpp 2>&1 | grep -viE 'warning|DT_TEXTREL' || true

./oracle
