// notesplit_test.cpp - assert self-check of TuneMacro::noteSplitKnobFromSaved. g++ -std=c++20 -I ../../Source notesplit_test.cpp
#include "DSP/TuneMacro.h"
#include <cassert>
#include <cstdio>
int main(){ using namespace TuneMacro;
 assert(noteSplitKnobFromSaved(0.7275f)==0.5f); assert(noteSplitKnobFromSaved(0.73f)==0.5f);
 assert(noteSplitKnobFromSaved(0.75f)==0.75f); assert(noteSplitKnobFromSaved(0.0f)==0.0f);
 assert(noteSplitKnobFromSaved(1.0f)==1.0f); assert(noteSplitKnobFromSaved(0.2f)==0.0f); assert(noteSplitKnobFromSaved(0.9f)==0.75f);
 puts("notesplit ok"); }
