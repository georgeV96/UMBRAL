#pragma once

#include <vector>

#include "DSP/UmbralEngine.h"

namespace umbral
{
struct Preset
{
    const char* name;
    Params params;   // memory, erosion, drift, threshold, depth, blend, outputDb
};

// Ordered from subtle to radical. They are used by the plugin (programs), the tests and the render tool.
inline const std::vector<Preset>& factoryPresets()
{
    static const std::vector<Preset> list = {
        { "Glass Veil",       { 0.35f, 0.15f, 0.15f, 0.30f, 0.30f, 0.50f,  0.0f } },   // barely there: upper partials loosen
        { "Slow Patina",      { 0.60f, 0.30f, 0.25f, 0.35f, 0.45f, 0.65f,  0.0f } },   // a clean note ages into a duller, stranger one
        { "Bell Rust",        { 0.50f, 0.50f, 0.20f, 0.30f, 0.60f, 0.75f,  0.0f } },   // stretched, metallic upper partials
        { "Ghost Strings",    { 0.85f, 0.30f, 0.35f, 0.25f, 0.55f, 0.70f,  0.0f } },   // long memory: arpeggios colour each other
        { "Dark Bloom",       { 0.70f, 0.55f, 0.50f, 0.65f, 0.70f, 0.80f, -1.0f } },   // needs a held, strong note; blooms late
        { "Spectral Decay",   { 0.50f, 0.75f, 0.45f, 0.35f, 0.80f, 1.00f, -1.0f } },   // the harmonic structure dissolves
        { "Frozen Metal",     { 0.60f, 0.60f, 0.00f, 0.20f, 0.70f, 0.85f,  0.0f } },   // DRIFT 0: a fixed inharmonic mould
        { "Whisper Drift",    { 0.40f, 0.10f, 1.00f, 0.30f, 0.50f, 0.60f,  0.0f } },   // slight erosion, constant slow movement
        { "Percussion Rust",  { 0.30f, 0.45f, 0.30f, 0.70f, 0.55f, 0.60f,  0.0f } },   // attacks stay natural, rings get metallic
        { "Abyss Texture",    { 0.90f, 1.00f, 0.80f, 0.15f, 1.00f, 1.00f, -3.0f } },   // everything: abstract spectral texture
    };
    return list;
}
} // namespace umbral
