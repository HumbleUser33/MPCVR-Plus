#pragma once

// Display the bit depth reported by the playback graph.
// For compressed audio, this does not prove the original PCM precision.
// Preserve missing values instead of assuming 16-bit or 24-bit.
inline unsigned OsdAudioDepth(unsigned /*tag*/, unsigned reported) {
    return reported > 0 && reported <= 64 ? reported : 0;
}