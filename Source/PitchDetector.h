#pragma once

#include <cmath>
#include <vector>

// YIN fundamental-frequency estimator (de Cheveigné & Kawahara 2002) for a monophonic voice.
// detect() takes a frame of windowSize + maxLag samples and returns the frequency in Hz, or 0 if unvoiced.
class PitchDetector
{
public:
    void prepare (double newSampleRate, int newWindowSize, float minHz = 65.0f, float maxHz = 1100.0f)
    {
        sampleRate = newSampleRate;
        windowSize = newWindowSize;
        minLag = juce_max_int (2, (int) (sampleRate / maxHz));
        maxLag = (int) (sampleRate / minHz);
        diff.assign ((size_t) maxLag + 2, 0.0f);
        cmnd.assign ((size_t) maxLag + 2, 1.0f);
    }

    int frameSize() const { return windowSize + maxLag + 1; }

    // clarity: 1 - normalised difference at the chosen lag (1 = perfectly periodic)
    float detect (const float* x, float& clarity)
    {
        clarity = 0.0f;

        for (int tau = 1; tau <= maxLag; ++tau)
        {
            float sum = 0.0f;
            const float* a = x;
            const float* b = x + tau;
            for (int i = 0; i < windowSize; ++i)
            {
                const float d = a[i] - b[i];
                sum += d * d;
            }
            diff[(size_t) tau] = sum;
        }

        float running = 0.0f;
        cmnd[0] = 1.0f;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            running += diff[(size_t) tau];
            cmnd[(size_t) tau] = running > 0.0f ? diff[(size_t) tau] * (float) tau / running : 1.0f;
        }

        int best = -1;
        for (int tau = minLag; tau < maxLag; ++tau)
        {
            if (cmnd[(size_t) tau] < threshold)
            {
                while (tau + 1 < maxLag && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau])
                    ++tau;
                best = tau;
                break;
            }
        }

        if (best < 0)
            return 0.0f;

        // parabolic interpolation around the minimum
        const float y0 = cmnd[(size_t) best - 1], y1 = cmnd[(size_t) best], y2 = cmnd[(size_t) best + 1];
        const float denom = y0 - 2.0f * y1 + y2;
        const float shift = std::abs (denom) > 1.0e-9f ? 0.5f * (y0 - y2) / denom : 0.0f;
        const float lag = (float) best + juce_clamp (shift, -1.0f, 1.0f);

        clarity = 1.0f - y1;
        return (float) sampleRate / lag;
    }

private:
    static int juce_max_int (int a, int b) { return a > b ? a : b; }
    static float juce_clamp (float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    double sampleRate = 24000.0;
    int windowSize = 1024, minLag = 20, maxLag = 370;
    float threshold = 0.12f;
    std::vector<float> diff, cmnd;
};
