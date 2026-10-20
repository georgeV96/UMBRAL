// Offline render tool: writes dry/wet WAV files for the synthetic test sources so the result can be auditioned
// without a DAW.   umbral_render <output-dir> [preset-name-substring]
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "../Tests/Harness.h"
#include "Presets.h"

static void writeWav (const std::string& path, const std::vector<float>& l, const std::vector<float>& r, int fs)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (!f) { std::fprintf (stderr, "cannot write %s\n", path.c_str()); return; }
    const uint32_t n = (uint32_t) l.size(), bytes = n * 2 * 2;
    auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
    auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
    std::fwrite ("RIFF", 1, 4, f); w32 (36 + bytes); std::fwrite ("WAVEfmt ", 1, 8, f);
    w32 (16); w16 (1); w16 (2); w32 ((uint32_t) fs); w32 ((uint32_t) fs * 4); w16 (4); w16 (16);
    std::fwrite ("data", 1, 4, f); w32 (bytes);
    for (uint32_t i = 0; i < n; ++i)
    {
        for (const auto* ch : { &l, &r })
        {
            const float v = std::fmax (-1.0f, std::fmin (1.0f, (*ch)[i]));
            w16 ((uint16_t) (int16_t) std::lrintf (v * 32767.0f));
        }
    }
    std::fclose (f);
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::printf ("usage: umbral_render <output-dir> [preset-substring]\n"); return 1; }
    const std::string dir = argv[1], only = argc > 2 ? argv[2] : "";
    const double fs = 48000.0;
    struct Src { const char* name; sig::Buf l, r; };
    std::vector<Src> srcs;
    srcs.push_back ({ "guitar_notes", sig::concat (sig::pluck (82.4, 3.0, fs), sig::concat (sig::pluck (196, 3.0, fs), sig::pluck (329.6, 3.0, fs))), {} });
    srcs.push_back ({ "arpeggio", sig::arpeggio ({ 110, 164.8, 196, 247, 329.6 }, 0.45, 10.0, fs), {} });
    srcs.push_back ({ "dissonant_chord", sig::chord ({ 130.8, 138.6, 185.0, 196.0, 277.2 }, 8.0, fs, 12.0), {} });
    srcs.push_back ({ "distorted", sig::distorted (sig::chord ({ 82.4, 123.5, 164.8 }, 8.0, fs, 12.0)), {} });
    srcs.push_back ({ "pad", sig::pad ({ 110, 138.6, 165 }, 10.0, fs), sig::pad ({ 110.3, 138.2, 165.4 }, 10.0, fs) });
    srcs.push_back ({ "voice", sig::voice (140, 8.0, fs), {} });
    srcs.push_back ({ "drums", sig::drums (8.0, fs), {} });
    int count = 0;
    for (const auto& s : srcs)
    {
        writeWav (dir + "/" + s.name + "__dry.wav", s.l, s.r.empty() ? s.l : s.r, (int) fs);
        for (const auto& pr : umbral::factoryPresets())
        {
            if (!only.empty() && std::string (pr.name).find (only) == std::string::npos) continue;
            umbral::Params p = pr.params; p.blend = 1.0f;
            const auto r = harness::render (s.l, s.r.empty() ? nullptr : &s.r, p);
            std::string pn = pr.name; for (char& c : pn) if (c == ' ') c = '_';
            writeWav (dir + "/" + s.name + "__" + pn + ".wav", r.L, s.r.empty() ? r.L : r.R, (int) fs);
            ++count;
        }
    }
    std::printf ("wrote %d renders to %s\n", count, dir.c_str());
    return 0;
}
