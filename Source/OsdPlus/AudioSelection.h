#pragma once
#include <vector>

// -1: no selection interface, 0: explicitly inactive, 1: explicitly active.
// A single unknown input is safe; multiple unknown inputs must never be guessed.
inline int SelectOsdAudioInput(const std::vector<int>& states) {
    int selected = -1;
    for (unsigned i = 0; i < states.size(); ++i) {
        if (states[i] == 1) {
            if (selected >= 0) return -1;
            selected = static_cast<int>(i);
        }
    }
    return selected >= 0 ? selected : states.size() == 1 && states[0] == -1 ? 0 : -1;
}
