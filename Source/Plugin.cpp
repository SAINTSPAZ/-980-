#include <juce_audio_utils/juce_audio_utils.h>
#include <cmath>

//==============================================================================
struct DelayLine
{
    std::vector<float> buf;
    int w = 0;
    void prepare (int n) { buf.assign ((size_t) juce::jmax (n, 8), 0.0f); w = 0; }
    void push (float x) { buf[(size_t) w] = x; if (++w >= (int) buf.size()) w = 0; }
    float read (float d) const
    {
        const int n = (int) buf.size();
        d = juce::jlimit (1.0f, (float) (n - 2), d);
        float p = (float) w - d;
        if (p < 0) p += (float) n;
        const int i0 = (int) p, i1 = (i0 + 1) % n;
        const float f = p - (float) i0;
        return buf[(size_t) i0] * (1.0f - f) + buf[(size_t) i1] * f;
    }
};

struct ChannelState
{
    DelayLine flanger, ghost, delay, water;
    std::vector<float> stutter;
    int stW = 0, stR = 0, crushCnt = 0;
    float held = 0, waterLp = 0, ghostLp = 0;
};

//==============================================================================
class PinkFXProcessor : public juce::AudioProcessor
{
public:
    PinkFXProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "STATE", createLayout()) {}

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using F = juce::AudioParameterFloat;
        using B = juce::AudioParameterBool;
        juce::AudioProcessorValueTreeState::ParameterLayout l;
        for (auto id : { "bitcrush", "flanger", "ghost", "underwater", "reverb", "distortion", "delay", "panMix" })
            l.add (std::make_unique<F> (juce::ParameterID { id, 1 }, id,
                                        juce::NormalisableRange<float> (0.0f, 1.0f), 0.0f));
        l.add (std::make_unique<F> (juce::ParameterID { "panSpeed", 1 }, "panSpeed",
                                    juce::NormalisableRange<float> (0.1f, 12.0f, 0.0f, 0.5f), 1.0f));
        l.add (std::make_unique<B> (juce::ParameterID { "helicopter", 1 }, "helicopter", false));
        l.add (std::make_unique<B> (juce::ParameterID { "stutter", 1 }, "stutter", false));
        return l;
    }

    void prepareToPlay (double sampleRate, int) override
    {
        sr = (float) sampleRate;
        for (auto& c : ch)
        {
            c.flanger.prepare ((int) (sr * 0.02f));
            c.ghost.prepare   ((int) (sr * 0.25f));
            c.delay.prepare   ((int) (sr * 0.6f));
            c.water.prepare   ((int) (sr * 0.03f));
            c.stutter.assign  ((size_t) (sr * 0.125f), 0.0f);
            c.stW = c.stR = c.crushCnt = 0;
            c.held = c.waterLp = c.ghostLp = 0;
        }
        reverb.reset();
    }

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        auto get = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };

        const float crush = get ("bitcrush"), flg = get ("flanger"), gho = get ("ghost"),
                    und = get ("underwater"), rev = get ("reverb"), dist = get ("distortion"),
                    dly = get ("delay"), pSpd = get ("panSpeed"), pMix = get ("panMix");
        const bool heli = get ("helicopter") > 0.5f, stut = get ("stutter") > 0.5f;

        if (stut && ! wasStutter) for (auto& c : ch) c.stR = 0;
        wasStutter = stut;

        const int nCh = juce::jmin (2, b.getNumChannels());
        const int n = b.getNumSamples();
        const float tp = juce::MathConstants<float>::twoPi;

        const float waterCoef = 1.0f - std::exp (-tp * (18000.0f * std::pow (0.04f, und)) / sr);
        const float levels = std::pow (2.0f, 15.0f - crush * 13.0f);
        const int   factor = 1 + (int) (crush * 24.0f);
        const float drive = 1.0f + dist * 40.0f, distMix = juce::jmin (1.0f, dist * 4.0f);
        const float dNorm = 1.0f / std::tanh (drive);

        for (int i = 0; i < n; ++i)
        {
            float out[2] = { 0, 0 };
            for (int c = 0; c < nCh; ++c)
            {
                auto& s = ch[c];
                float x = b.getReadPointer (c)[i];

                // stutter: freeze the last 1/8 s and loop it
                if (! s.stutter.empty())
                {
                    const int L = (int) s.stutter.size();
                    if (stut) { x = s.stutter[(size_t) ((s.stW + s.stR) % L)]; s.stR = (s.stR + 1) % L; }
                    else      { s.stutter[(size_t) s.stW] = x; s.stW = (s.stW + 1) % L; }
                }

                // bitcrush
                if (crush > 0.001f)
                {
                    if (++s.crushCnt >= factor) { s.crushCnt = 0; s.held = std::round (x * levels) / levels; }
                    x = s.held;
                }

                // distortion
                if (dist > 0.001f)
                    x = x * (1.0f - distMix) + std::tanh (drive * x) * dNorm * distMix;

                // underwater: wobbling short delay + falling low-pass
                s.water.push (x);
                if (und > 0.001f)
                {
                    const float wob = 1.0f + und * sr * 0.004f * (1.0f + std::sin (phWater));
                    s.waterLp += waterCoef * (s.water.read (wob) - s.waterLp);
                    x = s.waterLp;
                }

                // flanger
                {
                    const float d = sr * 0.001f * (1.5f + 1.2f * std::sin (phFlanger));
                    const float dd = s.flanger.read (d);
                    s.flanger.push (x + 0.6f * flg * dd);
                    x += flg * dd;
                }

                // ghost: dark, wobbling, regenerating echo
                {
                    const float gd = sr * 0.11f + sr * 0.004f * std::sin (phGhost);
                    const float g = s.ghost.read (gd);
                    s.ghostLp += 0.25f * (g - s.ghostLp);
                    s.ghost.push (x + 0.55f * gho * s.ghostLp);
                    x += gho * 0.8f * s.ghostLp;
                }

                // delay
                {
                    const float d = s.delay.read (0.375f * sr);
                    s.delay.push (x + 0.45f * d);
                    x += dly * d;
                }

                // helicopter chop
                if (heli)
                {
                    const float g = 0.5f + 0.5f * std::sin (phHeli);
                    x *= 0.1f + 0.9f * g * g;
                }
                out[c] = x;
            }

            // auto-pan
            if (nCh == 2 && pMix > 0.001f)
            {
                const float p = 0.5f + 0.5f * std::sin (phPan);
                out[0] *= 1.0f - pMix * p;
                out[1] *= 1.0f - pMix * (1.0f - p);
            }
            for (int c = 0; c < nCh; ++c) b.getWritePointer (c)[i] = out[c];

            phFlanger += tp * 0.25f / sr;  phGhost += tp * 0.7f / sr;  phWater += tp * 0.5f / sr;
            phHeli += tp * 13.0f / sr;     phPan += tp * pSpd / sr;
            for (float* p : { &phFlanger, &phGhost, &phWater, &phHeli, &phPan })
                if (*p > tp) *p -= tp;
        }

        if (rev > 0.001f && nCh == 2)
        {
            juce::Reverb::Parameters rp;
            rp.roomSize = 0.85f; rp.damping = 0.4f; rp.width = 1.0f;
            rp.wetLevel = rev * 0.6f; rp.dryLevel = 1.0f;
            reverb.setParameters (rp);
            reverb.processStereo (b.getWritePointer (0), b.getWritePointer (1), n);
        }
    }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "PinkFX"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 3.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && l.getMainInputChannelSet()  == juce::AudioChannelSet::stereo();
    }
    void getStateInformation (juce::MemoryBlock& d) override
    {
        if (auto xml = apvts.copyState().createXml()) copyXmlToBinary (*xml, d);
    }
    void setStateInformation (const void* d, int sz) override
    {
        if (auto xml = getXmlFromBinary (d, sz))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }

    juce::AudioProcessorValueTreeState apvts;

private:
    ChannelState ch[2];
    juce::Reverb reverb;
    float sr = 44100.0f;
    float phFlanger = 0, phGhost = 0, phWater = 0, phHeli = 0, phPan = 0;
    bool wasStutter = false;
};

//==============================================================================
class PinkFXEditor : public juce::AudioProcessorEditor
{
public:
    explicit PinkFXEditor (PinkFXProcessor& p) : AudioProcessorEditor (&p)
    {
        const char* knobIds[]   = { "bitcrush", "flanger", "ghost", "underwater", "reverb", "distortion", "delay", "panSpeed", "panMix" };
        const char* knobNames[] = { "Bitcrush", "Flanger", "Ghost", "Underwater", "Reverb", "Distortion", "Delay", "Pan Speed", "Pan Mix" };

        for (int i = 0; i < 9; ++i)
        {
            auto* s = sliders.add (new juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow));
            s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            s->setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xffd0308f));
            s->setColour (juce::Slider::thumbColourId, juce::Colour (0xff8a1060));
            s->setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xff6b0f4a));
            s->setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            addAndMakeVisible (s);
            sliderAtt.add (new juce::AudioProcessorValueTreeState::SliderAttachment (p.apvts, knobIds[i], *s));
            names.add (knobNames[i]);
        }
        const char* btnIds[] = { "helicopter", "stutter" };
        const char* btnNames[] = { "Helicopter", "Stutter" };
        for (int i = 0; i < 2; ++i)
        {
            auto* b = buttons.add (new juce::ToggleButton (btnNames[i]));
            b->setColour (juce::ToggleButton::textColourId, juce::Colour (0xff6b0f4a));
            b->setColour (juce::ToggleButton::tickColourId, juce::Colour (0xffd0308f));
            addAndMakeVisible (b);
            btnAtt.add (new juce::AudioProcessorValueTreeState::ButtonAttachment (p.apvts, btnIds[i], *b));
        }
        setSize (360, 470);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xfffbe3f3));
        g.setColour (juce::Colour (0xffa0185f));
        g.fillRoundedRectangle (90.0f, 14.0f, 180.0f, 44.0f, 6.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
        g.drawText ("PinkFX", 90, 14, 180, 44, juce::Justification::centred);

        g.setColour (juce::Colour (0xffc0409a));
        g.setFont (juce::FontOptions (14.0f));
        for (int i = 0; i < 9; ++i)
        {
            auto r = slot (i);
            g.fillRoundedRectangle (r.getX() + 8.0f, r.getY(), r.getWidth() - 16.0f, 20.0f, 4.0f);
            g.setColour (juce::Colours::white);
            g.drawText (names[i], r.withHeight (20), juce::Justification::centred);
            g.setColour (juce::Colour (0xffc0409a));
        }
    }

    void resized() override
    {
        for (int i = 0; i < 9; ++i) sliders[i]->setBounds (slot (i).withTrimmedTop (22));
        buttons[0]->setBounds (50, 430, 120, 26);
        buttons[1]->setBounds (190, 430, 120, 26);
    }

private:
    static juce::Rectangle<int> slot (int i) { return { 10 + (i % 3) * 114, 72 + (i / 3) * 118, 114, 112 }; }

    juce::OwnedArray<juce::Slider> sliders;
    juce::OwnedArray<juce::ToggleButton> buttons;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAtt;
    juce::OwnedArray<juce::AudioProcessorValueTreeState::ButtonAttachment> btnAtt;
    juce::StringArray names;
};

juce::AudioProcessorEditor* PinkFXProcessor::createEditor() { return new PinkFXEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PinkFXProcessor(); }
