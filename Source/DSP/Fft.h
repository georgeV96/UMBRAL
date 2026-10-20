#pragma once

#include <cmath>
#include <vector>

namespace umbral
{
// In-place iterative radix-2 complex FFT on split re/im arrays. All tables are built in init() (never on the
// audio thread). forward() is unnormalised; inverse() is unnormalised too (the caller applies 1/N).
class Fft
{
public:
    void init (int sizePow2)
    {
        n = sizePow2;
        rev.assign ((size_t) n, 0);
        int bits = 0;
        while ((1 << bits) < n)
            ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b))
                    r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        cosT.assign ((size_t) n / 2, 0.0f);
        sinT.assign ((size_t) n / 2, 0.0f);
        for (int i = 0; i < n / 2; ++i)
        {
            const double a = -2.0 * 3.14159265358979323846 * (double) i / (double) n;
            cosT[(size_t) i] = (float) std::cos (a);
            sinT[(size_t) i] = (float) std::sin (a);
        }
    }

    int size() const noexcept { return n; }

    void forward (float* re, float* im) const noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int j = rev[(size_t) i];
            if (j > i)
            {
                std::swap (re[i], re[j]);
                std::swap (im[i], im[j]);
            }
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len >> 1, step = n / len;
            for (int base = 0; base < n; base += len)
            {
                for (int k = 0, t = 0; k < half; ++k, t += step)
                {
                    const float wr = cosT[(size_t) t], wi = sinT[(size_t) t];
                    const int a = base + k, b = a + half;
                    const float xr = re[b] * wr - im[b] * wi;
                    const float xi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - xr;
                    im[b] = im[a] - xi;
                    re[a] += xr;
                    im[a] += xi;
                }
            }
        }
    }

    // inverse(x) = swap(forward(swap(x))): conjugation by exchanging the two arrays.
    void inverse (float* re, float* im) const noexcept { forward (im, re); }

private:
    int n = 0;
    std::vector<int> rev;
    std::vector<float> cosT, sinT;
};
} // namespace umbral
