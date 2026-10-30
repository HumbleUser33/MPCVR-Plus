#include "../AudioSelection.h"
#include "../AudioDepth.h"
#include <cassert>
#include <iostream>

int main() {
    assert(SelectOsdAudioInput({}) == -1);
    assert(SelectOsdAudioInput({-1}) == 0); // Ordinary decoder without selector.
    assert(SelectOsdAudioInput({0}) == -1); // Explicitly inactive is not a fallback.
    assert(SelectOsdAudioInput({-1, -1}) == -1);
    assert(SelectOsdAudioInput({1, 1}) == -1);
    // Eight connected tracks: switch DTS -> DTS -> E-AC-3 -> AAC, without
    // rebuilding the graph or choosing the first connected input each time.
    for (int active : {0, 1, 2, 7, 0}) {
        std::vector<int> states(8, 0);
        states[active] = 1;
        assert(SelectOsdAudioInput(states) == active);
    }
    assert(SelectOsdAudioInput({-1, 1, -1}) == 1);
    assert(OsdAudioDepth(0x2001, 24) == 24); // DTS declaration is not discarded.
    assert(OsdAudioDepth(0x2001, 0) == 0);   // Do not invent a missing DTS depth.
    assert(OsdAudioDepth(0xff, 16) == 16); // Preserve reported AAC depth.
    assert(OsdAudioDepth(0, 16) == 16);    // Preserve reported depth without a format tag.
    assert(OsdAudioDepth(1, 24) == 24);
    assert(OsdAudioDepth(3, 32) == 32);
    assert(OsdAudioDepth(0x2001, 65535) == 0);
    std::cout << "audio selection regression checks passed\n";
}
