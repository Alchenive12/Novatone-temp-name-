#!/bin/bash
# hybrid eval: $1 = floor (or "off")
B=/home/claude/build-tools/NovaTonePolyBench_artefacts/Release/NovaTonePolyBench
P=/home/claude/polyfix
if [ "$1" != "off" ]; then export NT_PRIOR_FLOOR=$1; fi
cd /home/claude/novatone-temp-name-/tools/polytest
( [ "$1" != "off" ] && export NT_PRIOR=$P/piano.prior; $B file wrld_midi2_flkeys.wav > nt_out.txt 2>/dev/null ); python3 score.py | grep NovaTone | cut -c1-110 | sed 's/^/  piano  /'; python3 $P/octave.py | tail -1 | sed 's/^/  piano  /'
cd /home/claude/gtest; ( [ "$1" != "off" ] && export NT_PRIOR=$P/guitar.prior; $B file titan8.wav > nt.txt 2>/dev/null ); python3 cmp.py | sed 's/^/  guitar /'
( [ "$1" != "off" ] && export NT_PRIOR=$P/vocal.prior; $B file /home/claude/vocaltest/part.wav 2>/dev/null | head -1 | sed 's/^/  vocal  /' )
