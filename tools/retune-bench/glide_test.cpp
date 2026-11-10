// g++ -std=c++17 -I ../../Source glide_test.cpp -o glide_test && ./glide_test
#include "DSP/ShiftGlide.h"
#include <cassert>
#include <cstdio>
int main()
{
    using namespace ShiftGlide;
    const Neighbour none, dry { true, false, 0.0f }, moved { true, true, 3.0f };
    auto at = [&](int hop, Neighbour p, Neighbour n) { return shiftAtHop(1.0f, hop, 40, 0.18f, 0.18f, p, n, 7.75f); };
    for (int h = 0; h < 40; ++h) assert(at(h, none, none) == 1.0f);            // after a gap: already shifted
    assert(at(0, dry, none) < 0.1f && at(8, dry, none) == 1.0f);               // glides in from the dry neighbour
    for (int h = 1; h < 40; ++h) assert(at(h, dry, none) >= at(h - 1, dry, none)); // monotonic
    assert(at(39, none, dry) < 0.1f && at(20, none, dry) == 1.0f);             // and out again
    const float edge = at(0, moved, none);                                     // two moved notes meet half way
    assert(edge > 1.6f && edge < 2.2f && at(4, moved, none) == 1.0f);
    assert(shiftAtHop(1.0f, 0, 2, 0.18f, 0.18f, dry, dry, 7.75f) == 1.0f);     // too short to glide
    std::puts("ShiftGlide ok");
}
