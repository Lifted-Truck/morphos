#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
// WaveformDisplay — draws a mono waveform with a draggable scrub marker.
//
// A read-only view: the editor hands it a pointer to a SampleSource's immutable
// mono buffer (which outlives any drag), plus the current read-position. Click or
// drag calls onScrub(pos01) so the editor can update the anchor's readPosition.
// ─────────────────────────────────────────────────────────────────────────────
class WaveformDisplay : public juce::Component
{
public:
    std::function<void(float)> onScrub;   // new normalised read-position [0,1]

    void setSource(const float* data, int numSamples) noexcept
    {
        if (data == data_ && numSamples == len_) return;   // unchanged → no repaint
        data_ = data;
        len_  = numSamples;
        repaint();
    }

    void setReadPosition(float pos01) noexcept
    {
        pos01 = juce::jlimit(0.0f, 1.0f, pos01);
        if (pos01 != pos_) { pos_ = pos01; repaint(); }
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xFF15151A));
        g.fillRoundedRectangle(b, 3.0f);

        if (data_ == nullptr || len_ <= 0)
        {
            g.setColour(juce::Colours::grey);
            g.drawText("no sample", getLocalBounds(), juce::Justification::centred);
            return;
        }

        const int   W     = juce::jmax(1, getWidth());
        const float midY  = b.getCentreY();
        const float halfH = b.getHeight() * 0.45f;

        g.setColour(juce::Colour(0xFF3DC9B0));   // anchor teal
        for (int xpix = 0; xpix < W; ++xpix)
        {
            const int s0 = (int) ((juce::int64) xpix       * len_ / W);
            const int s1 = (int) ((juce::int64) (xpix + 1)  * len_ / W);
            float mn = 0.0f, mx = 0.0f;
            for (int s = s0; s < s1 && s < len_; ++s)
            {
                mn = juce::jmin(mn, data_[s]);
                mx = juce::jmax(mx, data_[s]);
            }
            const float y0 = midY - mx * halfH;
            const float y1 = midY - mn * halfH;
            g.drawVerticalLine(xpix, juce::jmin(y0, y1), juce::jmax(y0, y1) + 1.0f);
        }

        // Scrub marker.
        const float markerX = b.getX() + pos_ * b.getWidth();
        g.setColour(juce::Colours::white);
        g.drawVerticalLine((int) markerX, b.getY(), b.getBottom());
    }

    void mouseDown(const juce::MouseEvent& e) override { scrubTo(e); }
    void mouseDrag(const juce::MouseEvent& e) override { scrubTo(e); }

private:
    void scrubTo(const juce::MouseEvent& e)
    {
        const float p = juce::jlimit(0.0f, 1.0f,
                                     (float) e.x / (float) juce::jmax(1, getWidth()));
        setReadPosition(p);
        if (onScrub) onScrub(p);
    }

    const float* data_ = nullptr;
    int          len_  = 0;
    float        pos_  = 0.5f;
};
