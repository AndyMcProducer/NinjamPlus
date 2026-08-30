#pragma once

#include <JuceHeader.h>
#include <functional>
#include "PluginProcessor.h"

class NinjamVst3AudioProcessorEditor;  // forward declaration for LAF classes
class ChatTtsEngine;

#if JUCE_WINDOWS
struct WinVideoReader;  // Windows Media Foundation frame reader (defined in PluginEditor.cpp)
#endif

class GifPickerPanel;

class EditorBackgroundComponent : public juce::Component
{
public:
    EditorBackgroundComponent()
    {
        setOpaque(true);
        setInterceptsMouseClicks(false, false);
    }

    void setBackgroundImage(juce::Image newImage)
    {
        image = std::move(newImage);
        repaint();
    }

    void setFallbackColour(juce::Colour newColour)
    {
        fallbackColour = newColour;
        if (!image.isValid())
            repaint();
    }

    void paint(juce::Graphics& g) override
    {
        if (image.isValid())
            g.drawImageWithin(image, 0, 0, getWidth(), getHeight(), juce::RectanglePlacement::fillDestination);
        else
            g.fillAll(fallbackColour);
    }

private:
    juce::Image image;
    juce::Colour fallbackColour { juce::Colour(0xff222222) };
};

class IntervalDisplayComponent : public juce::Component
{
public:
    IntervalDisplayComponent(NinjamVst3AudioProcessor& p) : processor(p) {}

    void paint(juce::Graphics& g) override
    {
        int bpi = processor.getBPI();
        if (bpi <= 0)
            bpi = 4;

        const float progress = juce::jlimit(0.0f, 1.0f, processor.getIntervalProgress());
        const float totalBeats = progress * (float)bpi;
        const int currentBeat = (int)totalBeats;

        auto bounds = getLocalBounds().toFloat();
        auto chordArea = bounds.removeFromTop(18.0f);
        bounds.removeFromTop(3.0f);
        const float blockWidth = bounds.getWidth() / (float)bpi;
        const float blockHeight = bounds.getHeight();

        const juce::Colour onColor = juce::Colour(0xFFFFFDD0);
        const juce::Colour offColor = juce::Colours::black.withAlpha(0.3f);
        const juce::Colour chordColor = juce::Colour(0xff56d8c9);
        const auto chordTimeline = processor.getMasterChordTimeline();
        const float chordBlockWidth = chordArea.getWidth() / (float)bpi;

        for (int i = 0; i < bpi; ++i)
        {
            auto cell = juce::Rectangle<float>(chordArea.getX() + (float)i * chordBlockWidth,
                                               chordArea.getY(),
                                               chordBlockWidth,
                                               chordArea.getHeight()).reduced(2.0f, 1.0f);
            const juce::String chord = i < (int)chordTimeline.size() ? chordTimeline[(size_t)i] : juce::String();
            const bool hasChord = chord.isNotEmpty() && chord != "--" && chord != "Off";
            const bool isActiveChord = i == currentBeat && currentBeat < bpi;

            if (hasChord)
            {
                g.setColour(chordColor.withAlpha(isActiveChord ? 0.88f : 0.62f));
                g.fillRoundedRectangle(cell, 2.0f);
                if (cell.getWidth() >= 18.0f && cell.getHeight() >= 9.0f)
                {
                    g.setFont(juce::Font(juce::jlimit(7.0f, 11.0f, cell.getHeight() * 0.82f), juce::Font::bold));
                    g.setColour(juce::Colours::black.withAlpha(0.86f));
                    g.drawFittedText(chord, cell.toNearestInt(), juce::Justification::centred, 1);
                }
            }
            else
            {
                g.setColour(juce::Colours::black.withAlpha(isActiveChord ? 0.28f : 0.16f));
                g.drawRoundedRectangle(cell, 2.0f, 1.0f);
            }
        }

        for (int i = 0; i < bpi; ++i)
        {
            auto blockArea = juce::Rectangle<float>(bounds.getX() + (float)i * blockWidth,
                                                    bounds.getY(),
                                                    blockWidth,
                                                    blockHeight).reduced(2.0f);
            const bool isPast = i < currentBeat;
            const bool isActive = i == currentBeat && currentBeat < bpi;
            const bool isBarStart = (i % 4) == 0;
            float activePulse = 0.0f;

            if (isPast)
            {
                g.setColour(onColor);
                g.fillRect(blockArea);
            }
            else if (isActive)
            {
                const float subBeat = totalBeats - (float)currentBeat;
                activePulse = std::sin(subBeat * juce::MathConstants<float>::pi);
                const float alpha = 0.6f + 0.4f * activePulse;
                g.setColour(onColor.withAlpha(0.18f + 0.22f * activePulse));
                g.fillRect(blockArea.expanded(1.4f));
                g.setColour(onColor.withAlpha(alpha));
                g.fillRect(blockArea);
            }
            else
            {
                g.setColour(offColor);
                g.drawRect(blockArea, 1.0f);
            }

            const auto blockTextArea = blockArea.toNearestInt();
            if (isActive && blockTextArea.getWidth() >= 5 && blockTextArea.getHeight() >= 7)
            {
                auto beatArea = blockTextArea;
                if (isBarStart && beatArea.getHeight() >= 13)
                    beatArea.removeFromBottom(juce::jmax(5, juce::roundToInt((float)beatArea.getHeight() * 0.34f)));

                const juce::String beatText(currentBeat + 1);
                const float fontSize = juce::jlimit(7.0f, 15.0f, juce::jmin(blockArea.getWidth() * 0.9f, beatArea.getHeight() * 0.96f));
                g.setFont(juce::Font(fontSize, juce::Font::bold));
                g.setColour(juce::Colours::black.withAlpha(0.72f + 0.28f * activePulse));
                g.drawFittedText(beatText, beatArea, juce::Justification::centred, 1);
            }

            if (isBarStart && blockTextArea.getWidth() >= 5 && blockTextArea.getHeight() >= 9)
            {
                auto barArea = blockTextArea;
                barArea = barArea.removeFromBottom(juce::jmax(5, juce::roundToInt((float)blockTextArea.getHeight() * 0.34f)));
                const bool lit = isPast || isActive;
                const float tabAlpha = isActive ? (0.32f + 0.48f * activePulse) : (lit ? 0.34f : 0.58f);
                const float barAlpha = isActive ? (0.34f + 0.56f * activePulse) : (lit ? 0.7f : 0.72f);
                auto tabArea = barArea.toFloat().reduced(1.0f, 0.5f);
                g.setColour(lit ? juce::Colours::white.withAlpha(tabAlpha) : juce::Colour(0xff263244).withAlpha(tabAlpha));
                g.fillRoundedRectangle(tabArea, 2.0f);
                g.setFont(juce::Font(juce::jlimit(6.0f, 9.0f, barArea.getHeight() * 0.88f), juce::Font::bold));
                g.setColour(lit ? juce::Colours::black.withAlpha(juce::jmax(barAlpha, 0.78f)) : onColor.withAlpha(juce::jmax(barAlpha, 0.78f)));
                g.drawFittedText(juce::String((i / 4) + 1), barArea, juce::Justification::centred, 1);
            }
        }
    }

private:
    NinjamVst3AudioProcessor& processor;
};

class OutlinedLabelLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawLabel(juce::Graphics& g, juce::Label& label) override
    {
        if (!label.isBeingEdited())
        {
            auto alpha = label.isEnabled() ? 1.0f : 0.5f;
            auto font  = getLabelFont(label);
            g.setFont(font);

            auto textArea = getLabelBorderSize(label).subtractedFrom(label.getLocalBounds());
            juce::String text = label.getText();
            if (text.isEmpty()) return;

            if (label.findParentComponentOfClass<juce::AlertWindow>() != nullptr)
            {
                LookAndFeel_V4::drawLabel(g, label);
                return;
            }

            auto just = label.getJustificationType();

            // black outline: draw at radius-1 and radius-2 for heavier weight
            g.setColour(juce::Colours::black.withAlpha(alpha * 0.80f));
            for (int r = 1; r <= 2; ++r)
                for (int dx = -r; dx <= r; ++dx)
                    for (int dy = -r; dy <= r; ++dy)
                        if (dx != 0 || dy != 0)
                            g.drawFittedText(text,
                                             textArea.translated(dx, dy),
                                             just, 1, 1.0f);

            // main text on top
            g.setColour(label.findColour(juce::Label::textColourId).withMultipliedAlpha(alpha));
            g.drawFittedText(text, textArea, just, 1, 1.0f);
        }
        else
        {
            LookAndFeel_V4::drawLabel(g, label);
        }
    }

    void drawToggleButton(juce::Graphics& g, juce::ToggleButton& button,
                          bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        // Draw the tick box using the default implementation first (painted separately)
        auto tickWidth = juce::jmin(20.0f, (float)button.getHeight() * 0.8f);
        drawTickBox(g, button, 4.0f, ((float)button.getHeight() - tickWidth) * 0.5f,
                    tickWidth, tickWidth, button.getToggleState(),
                    button.isEnabled(), shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

        // Draw label text with black outline
        auto alpha  = button.isEnabled() ? 1.0f : 0.5f;
        auto textX  = (int)(tickWidth + 8.0f);
        auto textArea = button.getLocalBounds().withTrimmedLeft(textX);
        if (button.getButtonText().equalsIgnoreCase("Spread Outputs"))
            textArea = textArea.translated(2, 0); // nudge label right; keep tick box position
        juce::String text = button.getButtonText();
        g.setFont(13.0f);

        g.setColour(juce::Colours::black.withAlpha(alpha * 0.80f));
        for (int r = 1; r <= 2; ++r)
            for (int dx = -r; dx <= r; ++dx)
                for (int dy = -r; dy <= r; ++dy)
                    if (dx != 0 || dy != 0)
                        g.drawFittedText(text, textArea.translated(dx, dy),
                                         juce::Justification::centredLeft, 1, 1.0f);

        g.setColour(button.findColour(juce::ToggleButton::textColourId).withMultipliedAlpha(alpha));
        g.drawFittedText(text, textArea, juce::Justification::centredLeft, 1, 1.0f);
    }
};

class FaderLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          const juce::Slider::SliderStyle, juce::Slider&) override;
    void drawLinearSliderBackground(juce::Graphics&, int x, int y, int width, int height,
                                    float sliderPos, float minSliderPos, float maxSliderPos,
                                    const juce::Slider::SliderStyle, juce::Slider&) override;
};

class SamplePadsButtonLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;
    void drawButtonText(juce::Graphics&, juce::TextButton&,
                        bool, bool) override {}
};

class NonlinearFaderSlider : public juce::Slider
{
public:
    NonlinearFaderSlider() = default;

    double valueToProportionOfLength(double value) override
    {
        auto range = getRange();
        double minV = range.getStart();
        double maxV = range.getEnd();
        if (maxV <= minV)
            return 0.0;

        double norm = (value - minV) / (maxV - minV);
        norm = juce::jlimit(0.0, 1.0, norm);

        double midNorm = (1.0 - minV) / (maxV - minV);
        double p0 = 0.8;

        if (norm <= midNorm)
        {
            if (midNorm <= 0.0)
                return 0.0;
            double p = (norm / midNorm) * p0;
            return p;
        }
        else
        {
            double xProp = (norm - midNorm) / (1.0 - midNorm);
            double p = p0 + xProp * (1.0 - p0);
            return p;
        }
    }

    double proportionOfLengthToValue(double proportion) override
    {
        auto range = getRange();
        double minV = range.getStart();
        double maxV = range.getEnd();
        if (maxV <= minV)
            return minV;

        double p = juce::jlimit(0.0, 1.0, (double)proportion);
        double midNorm = (1.0 - minV) / (maxV - minV);
        double p0 = 0.8;

        double norm;
        if (p <= p0)
        {
            if (p0 <= 0.0)
                norm = 0.0;
            else
                norm = (p / p0) * midNorm;
        }
        else
        {
            double xProp = (p - p0) / (1.0 - p0);
            norm = midNorm + xProp * (1.0 - midNorm);
        }

        return minV + norm * (maxV - minV);
    }

    void resized() override
    {
        juce::Slider::resized();
        updateDragSensitivityForCurrentBounds();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        leftInteractionActive = e.mods.isLeftButtonDown();
        if (leftInteractionActive)
        {
            updateDragSensitivityForCurrentBounds();
            juce::Slider::mouseDown(e);
        }
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (leftInteractionActive)
            juce::Slider::mouseDrag(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (leftInteractionActive)
            juce::Slider::mouseUp(e);
        leftInteractionActive = false;
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.mods.isLeftButtonDown())
            juce::Slider::mouseDoubleClick(e);
    }

private:
    void updateDragSensitivityForCurrentBounds()
    {
        const bool horizontal = getSliderStyle() == juce::Slider::LinearHorizontal;
        const int dragPixels = horizontal ? getWidth() : getHeight();
        setMouseDragSensitivity(juce::jmax(1, dragPixels));
    }

    bool leftInteractionActive = false;
};

class LeftClickOnlySlider : public juce::Slider
{
public:
    using juce::Slider::Slider;

    void mouseDown(const juce::MouseEvent& e) override
    {
        leftInteractionActive = e.mods.isLeftButtonDown();
        if (leftInteractionActive)
            juce::Slider::mouseDown(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (leftInteractionActive)
            juce::Slider::mouseDrag(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (leftInteractionActive)
            juce::Slider::mouseUp(e);
        leftInteractionActive = false;
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.mods.isLeftButtonDown())
            juce::Slider::mouseDoubleClick(e);
    }

private:
    bool leftInteractionActive = false;
};

class LeftClickOnlyTextButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    void mouseDown(const juce::MouseEvent& e) override
    {
        leftInteractionActive = e.mods.isLeftButtonDown();
        if (leftInteractionActive)
            juce::TextButton::mouseDown(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (leftInteractionActive)
            juce::TextButton::mouseDrag(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (leftInteractionActive)
            juce::TextButton::mouseUp(e);
        leftInteractionActive = false;
    }

private:
    bool leftInteractionActive = false;
};

class MouseWheelLabel : public juce::Label
{
public:
    using juce::Label::Label;

    std::function<void(int)> onWheelStep;

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (onWheelStep)
        {
            const int steps = (int)juce::roundToInt(std::abs(wheel.deltaY) * 10.0f);
            const int dir = wheel.deltaY > 0.0f ? 1 : -1;
            onWheelStep(dir * juce::jmax(1, steps));
        }
        else
        {
            juce::Label::mouseWheelMove(e, wheel);
        }
    }
};

class TranslateMenuTextButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;

    std::function<void()> onPopupMenuRequest;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
        {
            leftInteractionActive = false;
            return;
        }

        leftInteractionActive = e.mods.isLeftButtonDown();
        if (leftInteractionActive)
            juce::TextButton::mouseDown(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (leftInteractionActive)
            juce::TextButton::mouseDrag(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
        {
            leftInteractionActive = false;
            if (onPopupMenuRequest)
                onPopupMenuRequest();
            return;
        }

        if (leftInteractionActive)
            juce::TextButton::mouseUp(e);
        leftInteractionActive = false;
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.mods.isLeftButtonDown())
            juce::TextButton::mouseDoubleClick(e);
    }

private:
    bool leftInteractionActive = false;
};

class ClickableChatTextEditor : public juce::TextEditor
{
public:
    using juce::TextEditor::TextEditor;

    void setLinkRanges(const juce::Array<juce::Range<int>>& ranges, const juce::StringArray& urls);
    void clearLinkRanges();

    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    int findLinkIndexAt(juce::Point<int> position) const;

    juce::Array<juce::Range<int>> linkRanges;
    juce::StringArray linkUrls;
    int pressedLinkIndex = -1;
};

class RichChatDisplayComponent : public juce::Component,
                                 private juce::ScrollBar::Listener,
                                 private juce::Timer
{
public:
    RichChatDisplayComponent();
    ~RichChatDisplayComponent() override;

    void setMultiLine(bool) {}
    void setReadOnly(bool) {}
    void setFont(const juce::Font& newFont);
    void setBackgroundColour(juce::Colour newColour);
    void setChatText(const juce::StringArray& lines,
                     const juce::StringArray& senders,
                     const NinjamVst3AudioProcessor& processor);
    void setCommandLinkCallback(std::function<void(const juce::String&)> callback);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

private:
    struct Entry
    {
        juce::String line;
        juce::String sender;
        juce::String colourKey;
        juce::String mediaUrl;
        juce::String mediaKind;
        juce::Image mediaPreview;
        std::vector<juce::Image> mediaFrames;
        std::vector<int> mediaFrameDurationsMs;
        int mediaTotalDurationMs = 0;
        bool previewLoading = false;
    };

    struct PaintedLink
    {
        juce::Rectangle<int> bounds;
        juce::String url;
        juce::String command;
    };

    void scrollBarMoved(juce::ScrollBar* scrollBarThatHasMoved, double newRangeStart) override;
    void timerCallback() override;
    void clampScroll();
    void loadPreviewIfNeeded(int entryIndex);
    int getTextWidthForLayout() const;
    int getTextLineHeight() const;
    int getEntryTextHeight(const Entry& entry, int textRightEdge) const;
    int layoutEntryText(const Entry& entry,
                        int y,
                        int textRightEdge,
                        juce::Graphics* graphics,
                        std::vector<PaintedLink>* links = nullptr) const;
    int getMediaTileHeight(const Entry& entry, int textWidth) const;
    juce::Rectangle<int> getMediaTileBounds(const Entry& entry, int y, int textWidth) const;
    int estimateContentHeight() const;
    void updateAnimationTimer();
    int getLinkIndexAt(juce::Point<int> position) const;

    std::vector<Entry> entries;
    std::vector<PaintedLink> paintedLinks;
    juce::ScrollBar scrollBar { true };
    juce::Font chatFont { 14.0f };
    juce::Colour backgroundColour { 0xff101417 };
    std::shared_ptr<std::atomic<bool>> aliveFlag;
    std::function<void(const juce::String&)> commandLinkCallback;
    int scrollY = 0;
    int contentHeight = 0;
    int hoveredLinkIndex = -1;
};

class LeftClickOnlyToggleButton : public juce::ToggleButton
{
public:
    using juce::ToggleButton::ToggleButton;

    void mouseDown(const juce::MouseEvent& e) override
    {
        leftInteractionActive = e.mods.isLeftButtonDown();
        if (leftInteractionActive)
            juce::ToggleButton::mouseDown(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (leftInteractionActive)
            juce::ToggleButton::mouseDrag(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (leftInteractionActive)
            juce::ToggleButton::mouseUp(e);
        leftInteractionActive = false;
    }

private:
    bool leftInteractionActive = false;
};

class MuteSoloBtnLookAndFeel : public juce::LookAndFeel_V4
{
public:
    bool isMute = true; // true = red (mute), false = yellow-orange (solo)

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool shouldDrawButtonAsHighlighted, bool) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
        bool isOn = button.getToggleState();
        const float r = 4.0f;

        juce::Colour bg, rim;
        if (isMute)
        {
            bg  = isOn ? juce::Colour::fromRGB(130, 20, 20) : juce::Colour::fromRGB(35, 8, 8);
            rim = isOn ? juce::Colour::fromRGB(255, 80, 80)
                       : juce::Colour::fromRGB(255, 80, 80).withAlpha(0.25f);
        }
        else
        {
            bg  = isOn ? juce::Colour::fromRGB(155, 100, 5) : juce::Colour::fromRGB(42, 25, 3);
            rim = isOn ? juce::Colour::fromRGB(255, 210, 60)
                       : juce::Colour::fromRGB(255, 210, 60).withAlpha(0.25f);
        }

        g.setColour(bg);
        g.fillRoundedRectangle(bounds, r);
        g.setColour(rim);
        g.drawRoundedRectangle(bounds, r, 1.5f);

        if (shouldDrawButtonAsHighlighted)
        {
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.fillRoundedRectangle(bounds, r);
        }
    }

    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool) override
    {
        bool isOn = button.getToggleState();
        auto bounds = button.getLocalBounds();
        float fontSize = juce::jmin(14.0f, (float)bounds.getHeight() * 0.65f);
        g.setFont(juce::Font(fontSize, juce::Font::bold));

        juce::Colour tc = isOn ? juce::Colours::white : juce::Colours::white.withAlpha(0.30f);

        g.setColour(juce::Colours::black.withAlpha(0.75f));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx != 0 || dy != 0)
                    g.drawFittedText(button.getButtonText(), bounds.translated(dx, dy),
                                     juce::Justification::centred, 1);
        g.setColour(tc);
        g.drawFittedText(button.getButtonText(), bounds, juce::Justification::centred, 1);
    }
};

class UserChannelStrip : public juce::Component, public juce::Timer
{
public:
    UserChannelStrip(NinjamVst3AudioProcessor& p, int userIdx);
    ~UserChannelStrip() override;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;

    void updateInfo(const NinjamVst3AudioProcessor::UserInfo& info);
    void setOrientation(bool isHorizontal); // True = Mixer layout (Strip is vertical), False = List layout (Strip is horizontal)
    void setClipEnabled(bool enabled);
    int getPreferredHeight() const; // For dynamic height in list layout when expanded
    int getPreferredWidth()  const; // For dynamic width in mixer layout when expanded
    int getUserIndex() const;
    juce::Slider& getVolumeSlider();
    juce::Slider& getPanSlider();
    juce::Button& getMuteButton();
    juce::Button& getSoloButton();
    juce::Slider& getChannelSlider(int channel);

private:
    class PanSliderLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                              float sliderPos, float minSliderPos, float maxSliderPos,
                              const juce::Slider::SliderStyle style, juce::Slider& slider) override
        {
            if (style != juce::Slider::LinearHorizontal)
            {
                juce::LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
                return;
            }

            juce::Rectangle<int> bounds(x, y, width, height);
            int trackHeight = 6;
            juce::Rectangle<int> track(bounds.getX() + 4,
                                       bounds.getCentreY() - trackHeight / 2,
                                       bounds.getWidth() - 8,
                                       trackHeight);

            juce::Colour base = slider.findColour(juce::Slider::backgroundColourId, true);
            if (base == juce::Colour())
                base = juce::Colours::darkgrey.darker();

            g.setColour(base);
            g.fillRect(track);

            g.setColour(juce::Colours::black.withAlpha(0.7f));
            g.drawRect(track);

            double v = slider.getValue();
            double norm = juce::jlimit(-1.0, 1.0, v);

            int centreX = track.getCentreX();

            if (std::abs(norm) > 0.001)
            {
                bool toRight = norm > 0.0;
                float amount = (float)std::abs(norm);
                const int leftEdge = track.getX();
                const int rightEdge = track.getRight();
                const int halfWidth = juce::jmax(1, track.getWidth() / 2);
                const int activeWidth = juce::jmax(1, (int)std::round(halfWidth * amount));

                juce::Rectangle<int> active(track);
                if (toRight)
                {
                    active.setLeft(centreX);
                    active.setRight(juce::jmin(rightEdge, centreX + activeWidth));
                }
                else
                {
                    active.setRight(centreX);
                    active.setLeft(juce::jmax(leftEdge, centreX - activeWidth));
                }

                juce::Colour endColour = toRight ? juce::Colours::red : juce::Colours::white;
                juce::ColourGradient grad(juce::Colours::black, (float)centreX, (float)track.getCentreY(),
                                          endColour, toRight ? (float)active.getRight() : (float)active.getX(), (float)track.getCentreY(), false);

                g.setGradientFill(grad);
                g.setOpacity(1.0f);
                g.fillRect(active);
            }

            int thumbWidth = 6;
            int thumbHeight = trackHeight + 6;
            int thumbX = (int)sliderPos - thumbWidth / 2;
            juce::Rectangle<int> thumb(thumbX, track.getCentreY() - thumbHeight / 2, thumbWidth, thumbHeight);

            g.setColour(juce::Colours::white);
            g.fillRect(thumb);
            g.setColour(juce::Colours::black);
            g.drawRect(thumb);
        }
    };

    NinjamVst3AudioProcessor& processor;
    int userIndex;
    NinjamVst3AudioProcessor::UserInfo userInfo;
    
    juce::Label nameLabel;
    NonlinearFaderSlider volumeSlider;
    LeftClickOnlySlider panSlider;
    PanSliderLookAndFeel panLookAndFeel;
    LeftClickOnlyToggleButton clipButton{"No-Clip"};
    LeftClickOnlyTextButton muteButton{"M"};
    LeftClickOnlyTextButton soloButton{"S"};
    MuteSoloBtnLookAndFeel muteBtnLAF;
    MuteSoloBtnLookAndFeel soloBtnLAF;
    juce::ComboBox outputSelector;
    FaderLookAndFeel faderLookAndFeel;
    juce::Label chordLabel{ "RemoteChord", "--" };
    juce::Label dbLabel;
    bool showOutputSelector = true;
    int cachedTotalOutputs = -1;
    
    float currentPeakL = 0.0f;
    float currentPeakR = 0.0f;
    double clipStartMs = 0.0;
    bool clipPulsing = false;
    bool isHorizontalLayout = false; // Default List view (strip is horizontal)

    // +6 dB clip protection: auto-reduce to -10 dB when source peaks at +6 dB,
    // flash red until source drops back below 0 dB, then restore user's volume.
    bool clipProtectionActive = false;
    float clipProtectionDesiredVolume = 1.0f;
    double clipProtectionEnteredMs = 0.0;

    // Multi-channel remote support
    static constexpr int kMaxRemoteCh = 8;
    LeftClickOnlyTextButton expandButton{ ">" };
    bool isExpanded = false;
    int numRemoteChannels = 1;
    bool isMultiChanPeer = false;
    bool chordToggleArmed = false;
    float perChannelGain[kMaxRemoteCh];
    float channelPeaks[kMaxRemoteCh];
    LeftClickOnlySlider channelSliders[kMaxRemoteCh];
    juce::Label  channelNameLabels[kMaxRemoteCh]; // shows remote channel names

    void applyVolumesToProcessor();
    void refreshOutputSelectorItems();
    void toggleExpanded();
    void volumeChanged();
    void panChanged();
    void outputChanged();
    void muteChanged();
    void soloChanged();
    void clipChanged();
};

class AboutPopup : public juce::Component
{
public:
    AboutPopup(const juce::String& versionString)
        : versionText(versionString)
    {
        setOpaque(true);
        setSize(460, 230);
    }
    
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1b1f23));
        g.setColour(juce::Colour(0xff4a545e));
        g.drawRect(getLocalBounds(), 1);

        auto area = getLocalBounds().reduced(20, 18);

        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(24.0f, juce::Font::bold));
        g.drawText("NINJAMplus", area.removeFromTop(30), juce::Justification::centredTop);

        area.removeFromTop(6);

        g.setColour(juce::Colours::lightgrey);
        g.setFont(juce::Font(16.0f));
        g.drawText("Release " + versionText, area.removeFromTop(24), juce::Justification::centredTop);

        area.removeFromTop(10);

        g.setFont(juce::Font(14.0f));
        g.drawFittedText("Low-latency collaborative jamming with interval sync, video sync, chord detection, and sample pads.",
                         area.removeFromTop(58),
                         juce::Justification::centredTop,
                         3);

        area.removeFromTop(8);

        g.setFont(juce::Font(12.0f));
        g.setColour(juce::Colours::lightgrey.withAlpha(0.82f));
        g.drawFittedText("Sample pad time-stretch uses Signalsmith Stretch and Signalsmith Linear (MIT).",
                         area,
                         juce::Justification::centredTop,
                         2);
    }
    
private:
    juce::String versionText;
};

class AboutWindow : public juce::DialogWindow
{
public:
    AboutWindow(const juce::String& versionString)
        : juce::DialogWindow("About NINJAMplus", juce::Colours::darkgrey, true)
    {
        auto popup = new AboutPopup(versionString);
        setContentOwned(popup, true);
        setResizable(false, false);
        setSize(460, 230);
        centreAroundComponent(nullptr, getWidth(), getHeight());
    }

    void closeButtonPressed() override
    {
        setVisible(false);
        exitModalState(0);
    }
};

class HelpManualContent : public juce::Component
{
public:
    HelpManualContent()
    {
        setOpaque(true);
        setSize(620, 720);

        addAndMakeVisible(viewport);
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false);
        viewport.setColour(juce::ScrollBar::backgroundColourId, juce::Colour(0xff1a1d22));
        viewport.setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff4a545e));

        buildContent();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1b1f23));
    }

    void resized() override
    {
        viewport.setBounds(getLocalBounds());
        content.setBounds(0, 0, viewport.getWidth() - 12, contentHeight);
    }

private:
    juce::Viewport viewport;
    juce::Component content;
    int contentHeight = 0;

    void buildContent()
    {
        const int margin = 16;
        const int width = 620 - 12 - margin * 2;
        int y = margin;

        auto addTitle = [&](const juce::String& text)
        {
            auto* label = new juce::Label({}, text);
            label->setFont(juce::Font(20.0f, juce::Font::bold));
            label->setColour(juce::Label::textColourId, juce::Colour(0xff49d5ff));
            label->setBounds(margin, y, width, 30);
            content.addAndMakeVisible(label);
            y += 34;
        };

        auto addHeading = [&](const juce::String& text)
        {
            y += 6;
            auto* label = new juce::Label({}, text);
            label->setFont(juce::Font(15.0f, juce::Font::bold));
            label->setColour(juce::Label::textColourId, juce::Colour(0xffe8d030));
            label->setBounds(margin, y, width, 24);
            content.addAndMakeVisible(label);
            y += 26;
        };

        auto addText = [&](const juce::String& text)
        {
            auto* label = new juce::Label({}, text);
            label->setFont(juce::Font(13.0f));
            label->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            label->setJustificationType(juce::Justification::topLeft);
            const int h = juce::Label().getFont().withHeight(13.0f).getStringWidthFloat(text) > 0
                ? juce::jmax(20, (int)(text.length() / 70.0 * 18.0) + 4)
                : 20;
            label->setBounds(margin, y, width, h);
            content.addAndMakeVisible(label);
            y += h + 2;
        };

        auto addBullet = [&](const juce::String& text)
        {
            auto* label = new juce::Label({}, "  - " + text);
            label->setFont(juce::Font(13.0f));
            label->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            label->setJustificationType(juce::Justification::topLeft);
            const int h = juce::jmax(18, (int)(text.length() / 65.0 * 18.0) + 4);
            label->setBounds(margin + 8, y, width - 8, h);
            content.addAndMakeVisible(label);
            y += h + 1;
        };

        auto addSpacer = [&]() { y += 6; };

        // ---- Content ----
        addTitle("NINJAMplus Instruction Manual");

        addText("NINJAMplus is a NINJAM client built as a VST3 plugin and standalone app. "
                "It lets you jam in real time with musicians over the internet using "
                "interval-based audio exchange, with built-in video (VDO), chat, sampler, "
                "auto-tune, and effects.");

        // Connection
        addHeading("Connection");
        addBullet("Server: Enter a NINJAM server address (e.g. server.com or server.com:2049).");
        addBullet("Name: Your display name in the session.");
        addBullet("Password: Server password (use 'anon' for public servers, or check Anonymous).");
        addBullet("Anonymous: Connect as an anonymous guest.");
        addBullet("Connect: Connect to the specified server.");
        addBullet("Disconnect: Leave the current session.");

        // Local Channels
        addHeading("Local Channels (You)");
        addBullet("You: Label for your local channel section.");
        addBullet("Chord: Shows detected chord from your audio input in real time.");
        addBullet("Faders: Adjust the volume of each local channel you transmit.");
        addBullet("Pan: Stereo pan control per channel.");
        addBullet("Mute/Solo: Mute or solo individual channels.");
        addBullet("Output: Select audio output routing for each channel.");
        addBullet("+/-: Add or remove local channels (up to the server limit).");
        addBullet("AT: Auto-Tune toggle for local channel 1. Right-click for scale, key, and quality settings.");
        addBullet("Voice: Toggle voice chat mode on a separate channel.");
        addBullet("Transmit: Enable/disable transmitting your local audio to the server.");
        addBullet("Monitor: Enable/disable local monitoring of your own audio.");

        // Remote Users
        addHeading("Remote Users");
        addBullet("Each connected user has a channel strip with fader, pan, mute, and solo.");
        addBullet("User names and channel labels are shown above each strip.");
        addBullet("Expand: Click to expand a multi-channel user into separate strips.");
        addBullet("Popout: Pop the remote users mixer out into a separate floating window.");
        addBullet("Size +/-: Adjust the remote users window size (Ableton-hosted only).");

        // Master Section
        addHeading("Master Section");
        addBullet("Master Fader: Controls the overall output volume.");
        addBullet("Limiter: Toggle master limiter to prevent clipping. Adjust threshold and release.");
        addBullet("Auto Adjust Volume: Enable automatic level adjustment.");

        // FX
        addHeading("Effects (FX)");
        addBullet("FX button: Open the reverb and delay settings.");
        addBullet("Reverb: Room size, damping, wet/dry mix, early reflections, and tail controls.");
        addBullet("Delay: Time, feedback, ping-pong, wet/dry mix. Can sync to host tempo.");
        addBullet("NJ FX (sampler): Route sampler pads through the main reverb/delay sends.");

        // Metronome
        addHeading("Metronome");
        addBullet("Metronome button: Toggle the metronome on/off.");
        addBullet("Volume slider: Adjust metronome volume.");
        addBullet("Right-click the metronome button to change sound (Classic, Soft Beep, Soft Tick, Wood Tick, or custom).");
        addBullet("Metronome output can be routed to specific output channels.");

        // Chat
        addHeading("Chat");
        addBullet("Chat button: Toggle the chat panel.");
        addBullet("Type messages and press Enter to send to all users in the session.");
        addBullet("Chat supports translation (auto-translate toggle).");
        addBullet("Attachments can be shared via chat.");

        // Video / VDO
        addHeading("Video (VDO)");
        addBullet("Video button: Toggle video background display.");
        addBullet("VDO camera: Send and receive video synced with NINJAM intervals.");
        addBullet("VDO Room: Create or join a VDO camera room for video sync.");
        addBullet("Change VDO Room: Rename the VDO room (Options menu).");

        // Sync
        addHeading("Sync");
        addBullet("Sync button: Toggle transport sync (Sync Host in plugin, Sync Midi in standalone).");
        addBullet("When on, the button pulses to indicate active sync.");
        addBullet("Right-click to select sync source: VST Host, Ableton Link, or MIDI.");
        addBullet("Sync keeps NINJAM intervals aligned with your DAW or external clock.");

        // Sampler
        addHeading("Sample Pads / Looper");
        addBullet("Sample Pads button: Open the 16-pad sampler window.");
        addBullet("Each pad can hold a sample or loop recording.");
        addBullet("Click a pad to trigger it. Drag and drop audio files to load samples.");
        addBullet("Right-click a pad for options: load file, clear, Sync BPI, playback speed, FX routing.");

        addHeading("Pad Controls (per pad)");
        addBullet("Record (R): Arm loop recording. Hold the pad for 2 seconds to schedule BPI record at next interval.");
        addBullet("Match BPI: Align the pad's loop start to the server's BPI interval boundary.");
        addBullet("Sync BPI (right-click menu): Time-stretch the sample to match the server BPM and cue loop playback to start on BPI 1 (the next interval boundary). Pad turns blue while cued, then green when playing.");
        addBullet("Loop: Continuously loop the sample when triggered. Loop pads start at the next loop boundary and show blue (cued) until they start, then turn green (playing).");
        addBullet("Reverse: Play the sample backwards.");
        addBullet("D: Route this pad through the duck effect (requires global Duck enabled).");
        addBullet("Volume: Per-pad volume (0-200%). Mouse wheel or click to type a value.");
        addBullet("Pad name: Right-click to rename the pad.");

        addHeading("Sampler Panel Controls");
        addBullet("MIDI: Select MIDI input device for triggering pads.");
        addBullet("Input: Select audio input source for loop recording (local channels or remote users).");
        addBullet("Bank: Save and load sets of pads as named banks.");
        addBullet("Load: Load the selected bank or pick a folder to load from.");
        addBullet("Save: Save current pads as a named bank.");
        addBullet("Reset: Reset sampler settings without clearing loaded pads.");
        addBullet("Clear: Remove all samples from all pads.");
        addBullet("+/-: Resize the sampler window (Ableton-hosted only).");

        addHeading("Sampler FX");
        addBullet("FX knobs: Control the amount of each FX (reverb, delay, filter, phaser).");
        addBullet("FX selectors: Choose the FX type for each slot.");
        addBullet("Chain FX: Shift+drag or middle-mouse drag from one FX knob to another to chain them.");
        addBullet("Right-click an FX knob for MIDI learn.");
        addBullet("NJ FX: Route pads through the main NINJAM reverb/delay sends.");

        addHeading("Duck Effect");
        addBullet("Duck button: Master enable for the tempo-synced sidechain volume pump.");
        addBullet("Only pads with D enabled are affected by the duck.");
        addBullet("Right-click Duck to select shape (Smooth Pump, Tight Pump, Slow Pump, Hard Gate, Reverse Swell, Notch Pulse).");
        addBullet("Length: 1/8, 1/4, or 1/2 note — sets the duck cycle length, synced to NINJAM BPM.");
        addBullet("Limiter: Prevents sampler output from exceeding -2 dB.");
        addBullet("Monitor: Newly triggered pads play privately (not transmitted) until released.");

        // Options
        addHeading("Options Menu");
        addBullet("Standalone Settings: Audio device settings (standalone only).");
        addBullet("Midi Settings: MIDI input/output configuration.");
        addBullet("Enable Chord Detection: Toggle real-time chord detection.");
        addBullet("Enable Sample Pads / Looper: Show or hide the sampler.");
        addBullet("Mobile Hotspot Keepalive: Send keepalive signals for mobile hotspot connections.");
        addBullet("Tunnel SSH: Route the NINJAM connection through an SSH tunnel for encrypted transport.");
        addBullet("Automatically Reconnect: Reconnect if the connection drops.");
        addBullet("Ableton Link Audio: Enable Ableton Link audio sync.");
        addBullet("Metronome Sound/Output: Configure metronome sound and output routing.");
        addBullet("Transport Sync Source: Choose sync source (Host, Ableton Link, MIDI).");
        addBullet("Window Size: Quick-select preset sizes for the plugin window (Ableton-hosted). Free resizing is also supported — drag the window edge and the GUI relayouts when you release the mouse.");
        addBullet("Window position and size are remembered — the plugin restores on the same screen and size you left it at.");
        addBullet("Chat Popout Size: Set the chat popout window size.");
        addBullet("Remote Users Popout Size: Set the remote users popout window size.");
        addBullet("Check for Updates: Check for the latest NINJAMplus release.");
        addBullet("Help: Open this instruction manual.");

        // Server List
        addHeading("Server List");
        addBullet("Servers button: Browse public NINJAM servers.");
        addBullet("Shows server name, user count, BPM/BPI, and player names.");
        addBullet("Show Players: Toggle whether player names are displayed.");
        addBullet("Refresh: Reload the server list.");
        addBullet("Set Server: Fill the server field with the selected server.");

        // Chat
        addHeading("Chat (details)");
        addBullet("Chat button: Toggle the chat panel on/off.");
        addBullet("Popout: Open chat in a separate floating window.");
        addBullet("Type a message and press Enter or click the send arrow.");
        addBullet("AT button: Toggle auto-translate. Right-click to select target language.");
        addBullet("+ button: Attach GIFs, images, and emoji to your messages.");

        // Video
        addHeading("Video / VDO (details)");
        addBullet("Video Room button: Open the video session popup.");
        addBullet("VDO Video: Send and receive camera video synced to NINJAM intervals.");
        addBullet("NINJAMZap Video: Alternative compressed video transport.");
        addBullet("Video BG: Toggle between video background and static skin texture.");

        // MIDI/OSC
        addHeading("MIDI / OSC Relay");
        addBullet("MIDI/OSC button: Select relay targets for MIDI and OSC messages.");
        addBullet("Midi Settings (Options): Configure MIDI learn, relay, and pad input devices.");
        addBullet("Ableton Link Audio: Share audio between connected Ableton Link peers.");

        // SSH Tunnel
        addHeading("SSH Tunnel");
        addBullet("Tunnel SSH (Options): Route the NINJAM connection through an SSH server.");
        addBullet("All audio, chat, and VDO sync data flows encrypted through the tunnel.");
        addBullet("The NINJAM server sees the SSH server's IP, not your local IP.");
        addBullet("Configure SSH host, port, user, and optional key file.");
        addBullet("Works with system ssh on Windows 10+, macOS, and Linux.");

        addSpacer();
        addText("For more information, visit https://github.com/AndyMcProducer/NinjamPlus");

        contentHeight = y + margin;
        content.setSize(620 - 12, contentHeight);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HelpManualContent)
};

class HelpWindow : public juce::DialogWindow
{
public:
    HelpWindow()
        : juce::DialogWindow("NINJAMplus Help", juce::Colour(0xff1b1f23), true)
    {
        auto* content = new HelpManualContent();
        setContentOwned(content, true);
        setResizable(true, false);
        setSize(640, 740);
        centreAroundComponent(nullptr, getWidth(), getHeight());
    }

    void closeButtonPressed() override
    {
        setVisible(false);
        exitModalState(0);
    }
};

class MasterPeakMeter : public juce::Component
{
public:
    void setPeak(float newPeak)
    {
        peakL = juce::jlimit(0.0f, 2.0f, newPeak);
        peakR = peakL;
        repaint();
    }

    void setPeak(float newPeakL, float newPeakR)
    {
        peakL = juce::jlimit(0.0f, 2.0f, newPeakL);
        peakR = juce::jlimit(0.0f, 2.0f, newPeakR);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        drawVerticalStereoMeter(g, getLocalBounds(), peakL, peakR);
    }

    static constexpr float meterMinDb = -60.0f;
    static constexpr float meterMaxDb = 6.0f;
    static constexpr int numSegments = 22;

    static float peakToDb(float peak)
    {
        return 20.0f * std::log10(juce::jlimit(1.0e-6f, 2.0f, peak));
    }

    static float dbToMeterProportion(float db)
    {
        return juce::jmap(juce::jlimit(meterMinDb, meterMaxDb, db),
                          meterMinDb, meterMaxDb,
                          0.0f, 1.0f);
    }

    static float meterProportionToDb(float proportion)
    {
        return juce::jmap(juce::jlimit(0.0f, 1.0f, proportion),
                          0.0f, 1.0f,
                          meterMinDb, meterMaxDb);
    }

    static juce::Colour colourForDb(float db)
    {
        if (db >= 0.0f)
            return juce::Colour(0xffff1f1f);
        if (db > -6.0f)
            return juce::Colour(0xffffff26);
        return juce::Colour(0xff20ff37);
    }

    static int yForDb(juce::Rectangle<int> bounds, float db)
    {
        const int y = bounds.getBottom() - juce::roundToInt((float)bounds.getHeight() * dbToMeterProportion(db));
        return juce::jlimit(bounds.getY(), bounds.getBottom() - 1, y);
    }

    static int xForDb(juce::Rectangle<int> bounds, float db)
    {
        const int x = bounds.getX() + juce::roundToInt((float)bounds.getWidth() * dbToMeterProportion(db));
        return juce::jlimit(bounds.getX(), bounds.getRight() - 1, x);
    }

    static void drawVerticalMonoMeter(juce::Graphics& g, juce::Rectangle<int> bounds, float peak)
    {
        if (bounds.isEmpty())
            return;

        g.setColour(juce::Colours::black);
        g.fillRect(bounds);

        auto meterBounds = bounds.reduced(1, 1);
        drawBar(g, meterBounds, peak);
        drawDbTicks(g, meterBounds);
    }

    static void drawVerticalStereoMeter(juce::Graphics& g, juce::Rectangle<int> bounds, float leftPeak, float rightPeak)
    {
        if (bounds.isEmpty())
            return;

        g.setColour(juce::Colours::black);
        g.fillRect(bounds);

        auto meterBounds = bounds.reduced(1, 1);
        const int gap = meterBounds.getWidth() >= 8 ? 2 : 1;
        const int barWidth = juce::jmax(1, (meterBounds.getWidth() - gap) / 2);
        auto leftBar = meterBounds.removeFromLeft(barWidth);
        meterBounds.removeFromLeft(juce::jmin(gap, meterBounds.getWidth()));
        auto rightBar = meterBounds;
        if (rightBar.isEmpty())
            rightBar = leftBar;

        const auto fullMeterBounds = leftBar.getUnion(rightBar);
        drawBar(g, leftBar, leftPeak);
        drawBar(g, rightBar, rightPeak);
        drawDbTicks(g, fullMeterBounds);
    }

    static void drawVerticalMultiMeter(juce::Graphics& g, juce::Rectangle<int> bounds, const float* peaks, int numPeaks)
    {
        if (bounds.isEmpty() || peaks == nullptr || numPeaks <= 0)
            return;

        g.setColour(juce::Colours::black);
        g.fillRect(bounds);

        auto meterBounds = bounds.reduced(1, 1);
        const int count = juce::jmax(1, numPeaks);
        const int gap = meterBounds.getWidth() >= count * 3 ? 1 : 0;
        const int barWidth = juce::jmax(1, (meterBounds.getWidth() - gap * (count - 1)) / count);
        int x = meterBounds.getX();
        for (int i = 0; i < count; ++i)
        {
            juce::Rectangle<int> barBounds(x, meterBounds.getY(), barWidth, meterBounds.getHeight());
            drawBar(g, barBounds, peaks[i]);
            x += barWidth + gap;
            if (x >= meterBounds.getRight())
                break;
        }

        drawDbTicks(g, meterBounds);
    }

    static void drawHorizontalMonoMeter(juce::Graphics& g, juce::Rectangle<int> bounds, float peak)
    {
        if (bounds.isEmpty())
            return;

        g.setColour(juce::Colours::black);
        g.fillRect(bounds);

        auto meterBounds = bounds.reduced(1, 1);
        drawHorizontalBar(g, meterBounds, peak);
        drawHorizontalDbTicks(g, meterBounds);
    }

    static void drawHorizontalStereoMeter(juce::Graphics& g, juce::Rectangle<int> bounds, float leftPeak, float rightPeak)
    {
        if (bounds.isEmpty())
            return;

        g.setColour(juce::Colours::black);
        g.fillRect(bounds);

        auto meterBounds = bounds.reduced(1, 1);
        const int gap = meterBounds.getHeight() >= 6 ? 1 : 0;
        const int barHeight = juce::jmax(1, (meterBounds.getHeight() - gap) / 2);
        auto topBar = meterBounds.removeFromTop(barHeight);
        meterBounds.removeFromTop(juce::jmin(gap, meterBounds.getHeight()));
        auto bottomBar = meterBounds;
        if (bottomBar.isEmpty())
            bottomBar = topBar;

        const auto fullMeterBounds = topBar.getUnion(bottomBar);
        drawHorizontalBar(g, topBar, leftPeak);
        drawHorizontalBar(g, bottomBar, rightPeak);
        drawHorizontalDbTicks(g, fullMeterBounds);
    }

    static void drawHorizontalMultiMeter(juce::Graphics& g, juce::Rectangle<int> bounds, const float* peaks, int numPeaks)
    {
        if (bounds.isEmpty() || peaks == nullptr || numPeaks <= 0)
            return;

        g.setColour(juce::Colours::black);
        g.fillRect(bounds);

        auto meterBounds = bounds.reduced(1, 1);
        const int count = juce::jmax(1, numPeaks);
        const int gap = meterBounds.getHeight() >= count * 3 ? 1 : 0;
        const int barHeight = juce::jmax(1, (meterBounds.getHeight() - gap * (count - 1)) / count);
        int y = meterBounds.getY();
        for (int i = 0; i < count; ++i)
        {
            juce::Rectangle<int> barBounds(meterBounds.getX(), y, meterBounds.getWidth(), barHeight);
            drawHorizontalBar(g, barBounds, peaks[i]);
            y += barHeight + gap;
            if (y >= meterBounds.getBottom())
                break;
        }

        drawHorizontalDbTicks(g, meterBounds);
    }

private:
    static void drawBar(juce::Graphics& g, juce::Rectangle<int> barBounds, float peak)
    {
        if (barBounds.isEmpty())
            return;

        const float fillProportion = dbToMeterProportion(peakToDb(peak));
        const float slotHeight = (float)barBounds.getHeight() / (float)numSegments;
        const bool overZero = peak > 1.0f;

        // Fire FX: animated flicker glow when peak exceeds 0dB
        if (overZero)
        {
            const double phase = juce::Time::getMillisecondCounterHiRes() * 0.001
                               * (juce::MathConstants<double>::twoPi * 3.5);
            const float flicker = 0.6f + 0.4f * (float)std::sin(phase);
            const float fireAlpha = juce::jmap(flicker, 0.3f, 0.7f);
            auto fireBounds = barBounds.toFloat();
            // Fire rises from the top portion (where 0dB+ sits)
            const float zeroY = (float)yForDb(barBounds, 0.0f);
            juce::Rectangle<float> fireRect(barBounds.getX() - 2.0f, (float)barBounds.getY() - 2.0f,
                                            (float)barBounds.getWidth() + 4.0f,
                                            juce::jmax(4.0f, zeroY - (float)barBounds.getY() + 3.0f));
            juce::ColourGradient fireGrad(
                juce::Colour(0xffff6600).withAlpha(fireAlpha), fireRect.getCentreX(), fireRect.getY(),
                juce::Colour(0xffff2200).withAlpha(fireAlpha * 0.3f), fireRect.getCentreX(), fireRect.getBottom(), false);
            g.setGradientFill(fireGrad);
            g.fillRoundedRectangle(fireRect, 2.0f);

            // Inner bright core
            juce::ColourGradient coreGrad(
                juce::Colour(0xffffff88).withAlpha(fireAlpha * 0.8f), fireRect.getCentreX(), fireRect.getY(),
                juce::Colours::transparentBlack, fireRect.getCentreX(), fireRect.getBottom(), false);
            g.setGradientFill(coreGrad);
            g.fillRoundedRectangle(fireRect.reduced(1.0f, 1.0f), 1.5f);
        }

        for (int i = 0; i < numSegments; ++i)
        {
            const float segmentBottom = (float)barBounds.getBottom() - (float)i * slotHeight;
            const float segmentTop = (float)barBounds.getBottom() - (float)(i + 1) * slotHeight;
            const int y = juce::roundToInt(segmentTop + 1.0f);
            const int h = juce::jmax(1, juce::roundToInt(segmentBottom - segmentTop - 1.0f));
            const auto segment = juce::Rectangle<int>(barBounds.getX(), y, barBounds.getWidth(), h);

            const float segmentProportion = ((float)i + 0.5f) / (float)numSegments;
            const bool redSegment = i >= numSegments - 2;
            const juce::Colour base = redSegment ? juce::Colour(0xffff1f1f)
                                                 : colourForDb(meterProportionToDb(segmentProportion));
            const float litThreshold = redSegment ? ((float)i / (float)numSegments) : segmentProportion;
            const bool lit = litThreshold <= fillProportion;

            if (lit && h > 2)
            {
                g.setColour(base.withAlpha(0.16f));
                g.fillRoundedRectangle(segment.toFloat().expanded(1.0f, 0.5f), 1.6f);
            }

            // Center-lighter gradient: brighter in the horizontal middle, darker at edges
            if (lit)
            {
                const auto segF = segment.toFloat();
                const float cx = segF.getCentreX();
                const float edgeAlpha = 0.55f;
                const float centerAlpha = overZero && redSegment ? 1.0f : 0.96f;
                juce::ColourGradient grad(
                    base.withAlpha(centerAlpha), cx, segF.getY(),
                    base.withAlpha(edgeAlpha), segF.getX(), segF.getY(), false);
                grad.addColour(segF.getWidth() * 0.5f, base.withAlpha(centerAlpha));
                g.setGradientFill(grad);
                g.fillRoundedRectangle(segF, 1.2f);
            }
            else
            {
                g.setColour(base.withMultipliedBrightness(0.36f).withAlpha(0.20f));
                g.fillRoundedRectangle(segment.toFloat(), 1.2f);
            }
        }
    }

    static void drawHorizontalBar(juce::Graphics& g, juce::Rectangle<int> barBounds, float peak)
    {
        if (barBounds.isEmpty())
            return;

        const float fillProportion = dbToMeterProportion(peakToDb(peak));
        const float slotWidth = (float)barBounds.getWidth() / (float)numSegments;
        const bool overZero = peak > 1.0f;

        // Fire FX: animated flicker glow when peak exceeds 0dB
        if (overZero)
        {
            const double phase = juce::Time::getMillisecondCounterHiRes() * 0.001
                               * (juce::MathConstants<double>::twoPi * 3.5);
            const float flicker = 0.6f + 0.4f * (float)std::sin(phase);
            const float fireAlpha = juce::jmap(flicker, 0.3f, 0.7f);
            const float zeroX = (float)xForDb(barBounds, 0.0f);
            juce::Rectangle<float> fireRect(zeroX - 2.0f, (float)barBounds.getY() - 2.0f,
                                            juce::jmax(4.0f, (float)barBounds.getRight() - zeroX + 3.0f),
                                            (float)barBounds.getHeight() + 4.0f);
            juce::ColourGradient fireGrad(
                juce::Colour(0xffff6600).withAlpha(fireAlpha), fireRect.getRight(), fireRect.getCentreY(),
                juce::Colour(0xffff2200).withAlpha(fireAlpha * 0.3f), fireRect.getX(), fireRect.getCentreY(), false);
            g.setGradientFill(fireGrad);
            g.fillRoundedRectangle(fireRect, 2.0f);

            juce::ColourGradient coreGrad(
                juce::Colour(0xffffff88).withAlpha(fireAlpha * 0.8f), fireRect.getRight(), fireRect.getCentreY(),
                juce::Colours::transparentBlack, fireRect.getX(), fireRect.getCentreY(), false);
            g.setGradientFill(coreGrad);
            g.fillRoundedRectangle(fireRect.reduced(1.0f, 1.0f), 1.5f);
        }

        for (int i = 0; i < numSegments; ++i)
        {
            const float segmentLeft = (float)barBounds.getX() + (float)i * slotWidth;
            const float segmentRight = (float)barBounds.getX() + (float)(i + 1) * slotWidth;
            const int x = juce::roundToInt(segmentLeft + 1.0f);
            const int w = juce::jmax(1, juce::roundToInt(segmentRight - segmentLeft - 1.0f));
            const auto segment = juce::Rectangle<int>(x, barBounds.getY(), w, barBounds.getHeight());

            const float segmentProportion = ((float)i + 0.5f) / (float)numSegments;
            const bool redSegment = i >= numSegments - 2;
            const juce::Colour base = redSegment ? juce::Colour(0xffff1f1f)
                                                 : colourForDb(meterProportionToDb(segmentProportion));
            const float litThreshold = redSegment ? ((float)i / (float)numSegments) : segmentProportion;
            const bool lit = litThreshold <= fillProportion;

            if (lit && w > 2)
            {
                g.setColour(base.withAlpha(0.16f));
                g.fillRoundedRectangle(segment.toFloat().expanded(0.5f, 1.0f), 1.4f);
            }

            // Center-lighter gradient: brighter in the vertical middle, darker at top/bottom edges
            if (lit)
            {
                const auto segF = segment.toFloat();
                const float cy = segF.getCentreY();
                const float edgeAlpha = 0.55f;
                const float centerAlpha = overZero && redSegment ? 1.0f : 0.96f;
                juce::ColourGradient grad(
                    base.withAlpha(centerAlpha), segF.getX(), cy,
                    base.withAlpha(edgeAlpha), segF.getX(), segF.getY(), false);
                grad.addColour(segF.getHeight() * 0.5f, base.withAlpha(centerAlpha));
                g.setGradientFill(grad);
                g.fillRoundedRectangle(segF, 1.0f);
            }
            else
            {
                g.setColour(base.withMultipliedBrightness(0.36f).withAlpha(0.20f));
                g.fillRoundedRectangle(segment.toFloat(), 1.0f);
            }
        }
    }

    static void drawDbTicks(juce::Graphics& g, juce::Rectangle<int> meterBounds)
    {
        static constexpr float ticks[] { 6.0f, 0.0f, -3.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f, -60.0f };

        for (float db : ticks)
        {
            const int y = yForDb(meterBounds, db);
            const bool major = db == 6.0f || db == 0.0f || db == -6.0f || db == -12.0f || db == -24.0f || db == -60.0f;
            const int tickLength = major ? juce::jmax(3, meterBounds.getWidth() / 3)
                                         : juce::jmax(2, meterBounds.getWidth() / 5);
            const float fy = (float)y + 0.5f;

            g.setColour((db >= -3.0f ? juce::Colours::red : juce::Colours::white).withAlpha(major ? 0.38f : 0.24f));
            g.drawLine((float)meterBounds.getX(), fy, (float)(meterBounds.getX() + tickLength), fy, 1.0f);
            g.drawLine((float)(meterBounds.getRight() - tickLength), fy, (float)meterBounds.getRight(), fy, 1.0f);
        }
    }

    static void drawHorizontalDbTicks(juce::Graphics& g, juce::Rectangle<int> meterBounds)
    {
        static constexpr float ticks[] { 6.0f, 0.0f, -3.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f, -60.0f };

        for (float db : ticks)
        {
            const int x = xForDb(meterBounds, db);
            const bool major = db == 6.0f || db == 0.0f || db == -6.0f || db == -12.0f || db == -24.0f || db == -60.0f;
            const int tickLength = major ? juce::jmax(2, meterBounds.getHeight() / 2)
                                         : juce::jmax(1, meterBounds.getHeight() / 3);
            const float fx = (float)x + 0.5f;

            g.setColour((db >= -3.0f ? juce::Colours::red : juce::Colours::white).withAlpha(major ? 0.38f : 0.24f));
            g.drawLine(fx, (float)meterBounds.getY(), fx, (float)(meterBounds.getY() + tickLength), 1.0f);
            g.drawLine(fx, (float)(meterBounds.getBottom() - tickLength), fx, (float)meterBounds.getBottom(), 1.0f);
        }
    }

    float peakL = 0.0f;
    float peakR = 0.0f;
};

class UserListComponent : public juce::Component
{
public:
    UserListComponent(NinjamVst3AudioProcessor& p);
    ~UserListComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void updateContent();
    void setLayoutMode(bool horizontal); // True = Horizontal Mixer, False = Vertical List
    void setAllClipEnabled(bool enabled);
    std::vector<UserChannelStrip*> getStripPointers() const;

    void setAbletonHostedMode(bool hosted, int currentPreset, std::function<void(int)> onPresetChanged);
    void setBackgroundImage(juce::Image img);
    void setPoppedOut(bool poppedOut);
    bool isPoppedOut() const { return poppedOut; }

private:
    NinjamVst3AudioProcessor& processor;
    juce::Viewport viewport;
    juce::Component contentComponent;
    std::vector<std::unique_ptr<UserChannelStrip>> strips;
    bool isHorizontal = false;
    juce::Image backgroundImage;
    bool poppedOut = false;

    // Ableton popout size preset buttons (+/-)
    bool abletonHosted = false;
    int currentSizePreset = 1;
    std::function<void(int)> onSizePresetChanged;
    std::array<LeftClickOnlyTextButton, 2> sizeButtons { LeftClickOnlyTextButton{ "-" },
                                                          LeftClickOnlyTextButton{ "+" } };
    void updateSizeButtons();
};

class CustomKnobLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void setExplicitEditor(NinjamVst3AudioProcessorEditor* editor) { explicitEditor = editor; }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                          const float rotaryStartAngle, const float rotaryEndAngle, juce::Slider& slider) override;

private:
    NinjamVst3AudioProcessorEditor* explicitEditor = nullptr;
};

class SyncIconLookAndFeel : public juce::LookAndFeel_V4
{
public:
    juce::String bottomText { "HOST" };
    float glowPhase = 0.0f; // 0..2pi, for pulsing when on

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool shouldDrawButtonAsHighlighted, bool /*shouldDrawButtonAsDown*/) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
        bool isOn = button.getToggleState();
        const float r = 4.0f;

        juce::Colour bg  = isOn ? juce::Colour::fromRGB(110, 60, 10)
                                : juce::Colour::fromRGB(35, 18, 4);
        juce::Colour rim = isOn ? juce::Colour::fromRGB(255, 160, 60)
                                : juce::Colour::fromRGB(255, 160, 60).withAlpha(0.25f);
        // Text/icon color: bright when off, black when on
        juce::Colour ic  = isOn ? juce::Colours::black
                                : juce::Colour::fromRGB(255, 185, 90);

        // Pulse brightness when on
        if (isOn)
        {
            float t = (std::sin(glowPhase) + 1.0f) * 0.5f; // 0..1
            uint8 r = (uint8)(110 + (uint8)(145 * t));
            uint8 g = (uint8)(60  + (uint8)(100 * t));
            uint8 b = (uint8)(10  + (uint8)(50 * t));
            bg = juce::Colour::fromRGB(r, g, b);
            rim = juce::Colour::fromRGB(
                (uint8)(160 + (uint8)(95 * t)),
                (uint8)(60  + (uint8)(120 * t)),
                (uint8)(0   + (uint8)(50 * t)));
        }

        g.setColour(bg);
        g.fillRoundedRectangle(bounds, r);
        g.setColour(rim);
        g.drawRoundedRectangle(bounds, r, 1.5f);

        // --- "SYNC" / divider / bottom text stacked icon ---
        g.setColour(ic);

        const float cx = bounds.getCentreX();
        const float cy = bounds.getCentreY();
        const float halfH = bounds.getHeight() * 0.5f;

        // Top: SYNC
        auto syncFont = juce::Font(juce::jmax(7.0f, bounds.getHeight() * 0.26f), juce::Font::bold);
        g.setFont(syncFont);
        auto syncTextBounds = juce::Rectangle<float>(bounds.getX(), bounds.getY(),
                                                     bounds.getWidth(), halfH * 0.7f);
        g.drawText("SYNC", syncTextBounds, juce::Justification::centred);

        // Middle: divider line
        const float lineY = cy;
        const float lineMargin = bounds.getWidth() * 0.18f;
        g.setColour(ic.withAlpha(ic.getAlpha() * 0.6f));
        g.drawLine(bounds.getX() + lineMargin, lineY,
                   bounds.getRight() - lineMargin, lineY,
                   juce::jmax(0.8f, bounds.getHeight() * 0.04f));
        g.setColour(ic);

        // Bottom: HOST or MIDI
        auto hostFont = juce::Font(juce::jmax(7.0f, bounds.getHeight() * 0.26f), juce::Font::bold);
        g.setFont(hostFont);
        auto hostTextBounds = juce::Rectangle<float>(bounds.getX(), cy + halfH * 0.15f,
                                                     bounds.getWidth(), halfH * 0.7f);
        g.drawText(bottomText, hostTextBounds, juce::Justification::centred);

        if (shouldDrawButtonAsHighlighted)
        {
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.fillRoundedRectangle(bounds, r);
        }
    }
};

class MetronomeButtonLookAndFeel : public juce::LookAndFeel_V4
{
public:
    juce::Colour themeColour { juce::Colour::fromRGB(80, 185, 255) };

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool shouldDrawButtonAsHighlighted, bool /*shouldDrawButtonAsDown*/) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
        bool isOn = button.getToggleState();
        const float r = 4.0f;

        juce::Colour bg  = isOn ? themeColour.withMultipliedBrightness(0.55f)
                                : themeColour.withMultipliedBrightness(0.09f);
        juce::Colour rim = isOn ? themeColour
                                : themeColour.withAlpha(0.25f);
        juce::Colour ic  = isOn ? juce::Colours::white
                                : juce::Colours::white.withAlpha(0.30f);

        g.setColour(bg);
        g.fillRoundedRectangle(bounds, r);
        g.setColour(rim);
        g.drawRoundedRectangle(bounds, r, 1.5f);

        // --- metronome icon (scaled to 72% of button, centred) ---
        float cx = bounds.getCentreX();
        float cy = bounds.getCentreY();
        float bw = bounds.getWidth()  * 0.72f;
        float bh = bounds.getHeight() * 0.72f;
        float bx = cx - bw * 0.5f;
        float by = cy - bh * 0.5f;

        float sw   = juce::jmax(1.2f, bw * 0.085f);  // stroke width scales with size

        float baseH     = bh * 0.16f;
        float baseY     = by + bh - baseH;
        float bodyBot   = baseY;           // trapezoid bottom (top of base)
        float bodyTop   = by;
        float topRad    = bw * 0.28f;      // half-width at top
        float botRad    = bw * 0.46f;      // half-width at bottom

        // --- outer body: trapezoid with rounded arch top ---
        juce::Path body;
        // arc at top (rounded cap)
        body.startNewSubPath(cx - topRad, bodyTop + topRad * 0.6f);
        body.quadraticTo(cx - topRad, bodyTop,  cx,              bodyTop);
        body.quadraticTo(cx + topRad, bodyTop,  cx + topRad,     bodyTop + topRad * 0.6f);
        // right slant down to base
        body.lineTo(cx + botRad, bodyBot);
        // straight bottom
        body.lineTo(cx - botRad, bodyBot);
        body.closeSubPath();

        g.setColour(ic);
        g.strokePath(body, juce::PathStrokeType(sw, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // --- solid thick base bar ---
        float baseCorner = juce::jmax(1.0f, baseH * 0.35f);
        g.fillRoundedRectangle(cx - botRad, baseY, botRad * 2.0f, baseH, baseCorner);

        // --- 3 small pill tick marks on left interior ---
        float innerTop  = bodyTop + bh * 0.18f;
        float innerBot  = bodyBot - bh * 0.06f;
        float innerH    = innerBot - innerTop;
        float pillW     = bw * 0.26f;
        float pillH     = juce::jmax(1.5f, bh * 0.075f);
        float pillR     = pillH * 0.5f;
        float pillX     = cx - botRad + (botRad - topRad) * 0.3f + bw * 0.03f;  // left interior
        for (int i = 0; i < 3; ++i)
        {
            float py = innerTop + innerH * (float)i / 2.5f + innerH * 0.05f;
            g.fillRoundedRectangle(pillX, py - pillH * 0.5f, pillW, pillH, pillR);
        }

        // --- pendulum arm: pivots at bottom-centre, swings up to upper-right ---
        float armX0 = cx;
        float armY0 = bodyBot;
        float armX1 = cx + botRad * 0.85f;
        float armY1 = bodyTop + bh * 0.08f;
        g.drawLine(armX0, armY0, armX1, armY1, sw * 1.1f);

        if (shouldDrawButtonAsHighlighted)
        {
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.fillRoundedRectangle(bounds, r);
        }
    }
};

class ATButtonLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool shouldDrawButtonAsHighlighted, bool) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
        bool isOn = button.getToggleState();
        const float r = 4.0f;
        juce::Colour bg  = isOn ? juce::Colour::fromRGB(10,  90, 160) : juce::Colour::fromRGB(5, 22, 42);
        juce::Colour rim = isOn ? juce::Colour::fromRGB(80, 185, 255)
                                : juce::Colour::fromRGB(80, 185, 255).withAlpha(0.25f);
        g.setColour(bg);
        g.fillRoundedRectangle(bounds, r);
        g.setColour(rim);
        g.drawRoundedRectangle(bounds, r, 1.5f);
        if (shouldDrawButtonAsHighlighted)
        {
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.fillRoundedRectangle(bounds, r);
        }
    }
    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool) override
    {
        bool isOn = button.getToggleState();
        auto bounds = button.getLocalBounds();
        float fontSize = juce::jmin(13.0f, (float)bounds.getHeight() * 0.65f);
        g.setFont(juce::Font(fontSize, juce::Font::bold));
        juce::Colour tc = isOn ? juce::Colours::white : juce::Colours::white.withAlpha(0.30f);
        g.setColour(juce::Colours::black.withAlpha(0.75f));
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx != 0 || dy != 0)
                    g.drawFittedText(button.getButtonText(), bounds.translated(dx, dy),
                                     juce::Justification::centred, 1);
        g.setColour(tc);
        g.drawFittedText(button.getButtonText(), bounds, juce::Justification::centred, 1);
    }
};

class FaderIconLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool shouldDrawButtonAsHighlighted, bool /*shouldDrawButtonAsDown*/) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(1.0f);
        bool isOn = button.getToggleState();
        const float r = 4.0f;

        g.setColour(isOn ? juce::Colour::fromRGB(15, 55, 60)
                         : juce::Colour::fromRGB(10, 22, 26));
        g.fillRoundedRectangle(bounds, r);

        g.setColour(isOn ? juce::Colour::fromRGB(30, 180, 200)
                         : juce::Colour::fromRGB(30, 180, 200).withAlpha(0.22f));
        g.drawRoundedRectangle(bounds, r, 1.0f);

        // 5 fader tracks + handles
        juce::Colour iconCol = isOn ? juce::Colour::fromRGB(40, 210, 230)
                                    : juce::Colour::fromRGB(40, 210, 230).withAlpha(0.22f);
        g.setColour(iconCol);

        float ix = bounds.getX() + bounds.getWidth() * 0.09f;
        float iw = bounds.getWidth() * 0.82f;
        float iy = bounds.getY() + bounds.getHeight() * 0.12f;
        float ih = bounds.getHeight() * 0.76f;

        const float pos[5] = { 0.35f, 0.65f, 0.2f, 0.55f, 0.45f };
        float fw = iw / 5.0f;
        for (int i = 0; i < 5; ++i)
        {
            float cx = ix + fw * (i + 0.5f);
            g.drawLine(cx, iy, cx, iy + ih, 1.2f);
            float hy = iy + ih * pos[i];
            float hw = fw * 0.62f;
            float hh = juce::jmax(3.0f, ih * 0.18f);
            g.fillRect(cx - hw * 0.5f, hy - hh * 0.5f, hw, hh);
        }

        if (shouldDrawButtonAsHighlighted)
        {
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            g.fillRoundedRectangle(bounds, r);
        }
    }
};

class ClickableLabel : public juce::Label
{
public:
    using juce::Label::Label;
    std::function<void()> onClick;
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isLeftButtonDown() && onClick)
            onClick();
        juce::Label::mouseDown(e);
    }
};

// Custom popup menu item with a speed slider for auto-tune correction speed
class AutoTuneSpeedMenuItem : public juce::PopupMenu::CustomComponent,
                              public juce::Slider::Listener
{
public:
    explicit AutoTuneSpeedMenuItem(NinjamVst3AudioProcessor& p)
        : processor(p), slider(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight)
    {
        slider.setRange(0.0, 1.0, 0.01);
        slider.setValue((double)processor.getAutoTuneSpeed(), juce::dontSendNotification);
        slider.setTextValueSuffix("");
        slider.addListener(this);
        addAndMakeVisible(label);
        label.setText("Speed", juce::dontSendNotification);
        label.setColour(juce::Label::textColourId, juce::Colours::white);
        label.setFont(juce::Font(13.0f));
        addAndMakeVisible(slider);
        setSize(220, 36);
    }

    void sliderValueChanged(juce::Slider* s) override
    {
        processor.setAutoTuneSpeed((float)s->getValue());
    }

    void getIdealSize(int& idealWidth, int& idealHeight) override
    {
        idealWidth = 220;
        idealHeight = 36;
    }

private:
    void resized() override
    {
        auto area = getLocalBounds().reduced(6);
        label.setBounds(area.removeFromLeft(50));
        area.removeFromLeft(4);
        slider.setBounds(area);
    }

    NinjamVst3AudioProcessor& processor;
    juce::Label label;
    juce::Slider slider;
};

class NinjamVst3AudioProcessorEditor : public juce::AudioProcessorEditor,
                                       public juce::Timer,
                                       private juce::OSCReceiver,
                                       private juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>,
                                       private juce::MidiInputCallback
{
public:
    NinjamVst3AudioProcessorEditor (NinjamVst3AudioProcessor&);
    ~NinjamVst3AudioProcessorEditor() override;
    
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    void moved() override;
    void timerCallback() override;
    void parentHierarchyChanged() override;
    void mouseDown(const juce::MouseEvent& event) override;
    bool shouldDeferHeavyUiWork() const;
    void stopBackgroundVideoReader();
    void setStandaloneOptionsMenuHandler(std::function<void(juce::Component*)> handler);
    void armSamplePadMidiLearn(int padIndex);
    void forgetSamplePadMidiLearn(int padIndex);
    void armSamplePadOscLearn(int padIndex);
    void forgetSamplePadOscLearn(int padIndex);
    bool hasSamplePadMidiLearn(int padIndex) const;
    bool hasSamplePadOscLearn(int padIndex) const;
    void registerSamplerFxKnobLearnTarget(juce::Component& component, int slotIndex);
    void unregisterSamplerFxKnobLearnTarget(juce::Component& component, int slotIndex);
    void showMidiLearnMenuForComponent(juce::Component& component, juce::Point<int> screenPos);
    juce::LookAndFeel& getSamplerKnobLookAndFeel() { return customKnobLookAndFeel; }
    
    juce::Image backgroundImage;
    juce::Image radioKnobImage;
    juce::Image faderKnobImage;
    juce::Array<juce::File> textureFiles;
#if JUCE_WINDOWS
    std::unique_ptr<WinVideoReader> videoFrameReader;
#endif
    juce::String knobColourPreset { "grey" };
    juce::String faderColourPreset { "grey" };
    juce::Colour knobThemeColour { juce::Colours::grey };
    juce::Colour faderThemeColour { juce::Colour(0xff666666) };
    bool sandSkinOpaqueKnobs = false;
    juce::Colour metronomeThemeColour { juce::Colour::fromRGB(80, 185, 255) };
    juce::Colour windowThemeColour    { juce::Colour(0x00000000) };  // transparent = no override
    juce::Colour buttonThemeColour    { juce::Colour(0x00000000) };  // transparent = no override
    juce::Colour menuBarThemeColour   { juce::Colour(0x00000000) };  // transparent = no override
    juce::String chatWindowColourKey { "default" };
    CustomKnobLookAndFeel customKnobLookAndFeel;
    FaderIconLookAndFeel faderIconLookAndFeel;
    MetronomeButtonLookAndFeel metronomeBtnLAF;
    SyncIconLookAndFeel syncIconLAF;
    ATButtonLookAndFeel atBtnLAF;
    ATButtonLookAndFeel chatBtnLAF;
    SamplePadsButtonLookAndFeel samplePadsBtnLAF;
    OutlinedLabelLookAndFeel outlinedLabelLAF;

    void notifyPersistentSettingsDirty() { markPersistentSettingsDirty(); }

private:
    NinjamVst3AudioProcessor& audioProcessor;
    IntervalDisplayComponent intervalDisplay;
    juce::TooltipWindow tooltipWindow{ this, 600 };
    
    // UI components
    EditorBackgroundComponent backgroundComponent;
    juce::Label statusLabel;
    
    // Login
    juce::Label serverLabel{ "Server", "Server:" };
    juce::TextEditor serverField;
    LeftClickOnlyTextButton serverListButton;
    juce::Label userLabel{ "User", "Name:" };
    juce::TextEditor userField;
    LeftClickOnlyToggleButton anonymousButton{ "Anonymous" };
    juce::Label passLabel{ "Password", "Password:" };
    juce::TextEditor passField;
    LeftClickOnlyTextButton connectButton;
    
    // Controls
    LeftClickOnlyTextButton transmitButton{ "Transmit" };
    LeftClickOnlyTextButton localMonitorButton{ "Monitor Local" };
    LeftClickOnlyTextButton voiceChatButton{ "Voice Chat" };
    juce::ComboBox bitrateSelector;
    LeftClickOnlyTextButton midiRelayTargetSelector{ "" };
    LeftClickOnlyTextButton layoutButton{ "" };
    LeftClickOnlyTextButton opusSyncToggle{ "HD" };
    juce::Label metronomeLabel{ "Metro", "Metronome:" };
    LeftClickOnlySlider metronomeSlider;
    LeftClickOnlyTextButton metronomeMuteButton{ "" };
    LeftClickOnlyTextButton autoLevelButton{ "Auto Level" };
    TranslateMenuTextButton syncButton{ "" };
    LeftClickOnlyTextButton fxButton{ "FX" };
    LeftClickOnlyTextButton optionsButton{ "Options" };
    LeftClickOnlyTextButton aboutButton{ "?" };
    juce::Label tempoLabel;
    juce::ComboBox backgroundSelector{ "Background" };
    LeftClickOnlyToggleButton videoBgToggle{ "Video BG" };
    LeftClickOnlyTextButton videoButton{ "Video Room" };
    LeftClickOnlyTextButton samplePadsButton{ "" };
    LeftClickOnlyTextButton chatButton{ "Chat" };
    
    // Chat
    RichChatDisplayComponent chatDisplay;
    juce::TextEditor chatInput;
    LeftClickOnlyTextButton sendButton{ "Send" };
    LeftClickOnlyTextButton chatEmojiButton{ "" };
    TranslateMenuTextButton atButton{ "AT" };
    LeftClickOnlyTextButton chatPopoutButton{ "Popout" };
    std::unique_ptr<GifPickerPanel> gifPickerPanel;
    
    // Users
    juce::Label usersLabel{ "Users", "Connected Users:" };
    LeftClickOnlyToggleButton spreadOutputsButton{ "Spread Outputs" };
    LeftClickOnlyTextButton usersPopoutButton{ "Popout" };
    juce::Label maxChannelsLabel{ "MaxCh", "Max Ch: --" };
    UserListComponent userList;
    std::unique_ptr<juce::DocumentWindow> remoteUsersWindow;
    bool usersPoppedOut = false;

    FaderLookAndFeel mixerFaderLookAndFeel;
    juce::Label localFaderLabel{ "Local", "You" };
    juce::Label localChordLabel{ "LocalChord", "Chord --" };
    juce::Label localChordStatsLabel{ "LocalChordStats", "CPU 0.00%  MEM -- KB" };
    LeftClickOnlyTextButton addLocalChannelButton{ "+" };
    LeftClickOnlyTextButton removeLocalChannelButton{ "-" };
    TranslateMenuTextButton autoTuneButton{ "AT" };
    std::array<NonlinearFaderSlider, NinjamVst3AudioProcessor::maxLocalChannels> localFaders;
    std::array<MasterPeakMeter, NinjamVst3AudioProcessor::maxLocalChannels> localPeakMeters;
    std::array<juce::ComboBox, NinjamVst3AudioProcessor::maxLocalChannels> localInputModeSelectors;
    std::array<juce::ComboBox, NinjamVst3AudioProcessor::maxLocalChannels> localInputSelectors;
    std::array<LeftClickOnlySlider, NinjamVst3AudioProcessor::maxLocalChannels> localReverbSendKnobs;
    std::array<LeftClickOnlySlider, NinjamVst3AudioProcessor::maxLocalChannels> localDelaySendKnobs;
    std::array<juce::Label, NinjamVst3AudioProcessor::maxLocalChannels> localReverbSendLabels;
    std::array<juce::Label, NinjamVst3AudioProcessor::maxLocalChannels> localDelaySendLabels;
    juce::Label voiceChannelNameLabel{ "VoiceName", "Voice" };
    NonlinearFaderSlider voiceFader;
    MasterPeakMeter voicePeakMeter;
    juce::ComboBox voiceInputSelector;
    juce::Label voiceDbLabel;
    juce::Label masterFaderLabel{ "Master", "Master" };
    NonlinearFaderSlider masterFader;
    MasterPeakMeter masterPeakMeter;
    std::array<juce::Label, NinjamVst3AudioProcessor::maxLocalChannels> localDbLabels;
    std::array<juce::Label, NinjamVst3AudioProcessor::maxLocalChannels> localChannelNameLabels; // editable channel name
    ClickableLabel masterDbLabel;
    bool masterLufsMode = false;
    juce::Label masterLufsPeakLabel;
    LeftClickOnlyTextButton limiterButton{ "Limiter" };
    juce::Label limiterReleaseLabel{ "Release", "Release" };
    class LimiterThresholdLookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                              float sliderPos, float minSliderPos, float maxSliderPos,
                              const juce::Slider::SliderStyle style, juce::Slider& slider) override
        {
            juce::Rectangle<int> bounds(x, y, width, height);
            auto track = bounds.reduced(width / 3, 6);
            g.setColour(juce::Colours::black);
            g.fillRect(track);
            g.setColour(juce::Colours::darkgrey.brighter(0.2f));
            g.drawRect(track);

            int handleHeight = 10;
            int handleWidth = track.getWidth() + 4;
            int clampedY = juce::jlimit(track.getY() + handleHeight / 2,
                                        track.getBottom() - handleHeight / 2,
                                        (int)sliderPos);
            juce::Rectangle<int> handle(track.getCentreX() - handleWidth / 2,
                                        clampedY - handleHeight / 2,
                                        handleWidth,
                                        handleHeight);

            g.setColour(juce::Colours::lightblue);
            g.fillRect(handle);
            g.setColour(juce::Colours::black);
            g.drawRect(handle);
        }
    } limiterThresholdLookAndFeel;

    LeftClickOnlySlider limiterThresholdSlider;
    LeftClickOnlySlider limiterReleaseSlider;
    juce::Label reverbRoomLabel{ "Reverb", "Reverb" };
    LeftClickOnlySlider reverbRoomSlider;
    juce::Label delayTimeLabel{ "Delay", "Delay" };
    LeftClickOnlySlider delayTimeSlider;
    juce::ComboBox delayDivisionSelector;
    LeftClickOnlyToggleButton delayPingPongButton{ "PingPong" };
    
    void connectClicked();
    void sendClicked();
    void transmitToggled();
    void layoutToggled();
    void metronomeChanged();
    void anonymousToggled();
    void atToggled();
    void showTranslateLanguageMenu(juce::Component& anchorComponent);
    void showSyncCompensationMenu(juce::Component& anchorComponent);
    void syncToggled();
    void chatToggled();
    void chatPopoutClicked();
    void usersPopoutClicked();
    void showSamplePadsWindow();
    void videoClicked();

    void serverListClicked();
    void showAutoTuneMenu();
    void showSshTunnelSettingsPopup();
    void showHelpWindow();
    void updateAutoLevelButtonColor();
    void updateChatButtonColor();
    void updateTranslateButtonState();
    void updateTransmitButtonColor();
    void updateMonitorButtonColor();
    void updateLimiterButtonColor();
    void updateVoiceChatButtonColor();
    void updateLayoutButtonColor();
    void updateMetronomeButtonColor();
    void updateSyncButtonColor();
    void updateSyncButtonTooltip();
    void focusDockedChatInputForTyping();
    void updateFxButtonLabel();
    void updateSamplePadsFeatureVisibility();
    void showFxMenu();
    void showOptionsMenu();
    void showSettingsCallout(std::unique_ptr<juce::Component> content, juce::Component& anchorComponent);
    void showReverbSettingsPopup();
    void showDelaySettingsPopup();
    void updateFxControlsVisibility();
    void refreshLocalInputSelectors();
    void refreshMidiRelayTargetSelector();
    void showMidiRelayTargetMenu();
    void oscMessageReceived(const juce::OSCMessage& message) override;
    void applyOscMappings();
    void applyRemoteMidiRelaySelection(int channel, int inputIndex);
    void refreshLocalInputSelector(int channel);
    void refreshVoiceInputSelector();
    void showMidiOptionsPopup();
    void showLinkAudioOptionsPopup();
    void refreshExternalMidiInputDevices();
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;
    void syncLearnMappingsToProcessor();
    void loadLearnMappingsFromProcessor();
    void saveLearnMappingsToDisk();
    void loadLearnMappingsFromDisk();
    void markPersistentSettingsDirty();
    juce::String buildPersistentSettingsFingerprint(bool includeProcessorState) const;
    void setChatWindowColourKey(const juce::String& key, bool markDirty);
    void applyChatWindowColourToDisplays();
    void setChatTtsEnabled(bool enabled, bool markDirty);
    void setChatTtsVoiceId(const juce::String& voiceId, bool markDirty);
    void setChatTtsVolume(float volume, bool markDirty);
    void setChatTtsOutputId(const juce::String& outputId, bool markDirty);
    void enqueueChatTtsForNewLines(const juce::StringArray& history, const juce::StringArray& senders);
    void setAbletonChatWindowSizePreset(int presetIndex);
    void setAbletonSamplerWindowSizePreset(int presetIndex);
    void setAbletonRemoteUsersWindowSizePreset(int presetIndex);
    void rememberSamplePadsWindowBounds(juce::Rectangle<int> bounds, bool saveNow);
    void openChatPopoutWindow(const juce::StringArray& history,
                              const juce::StringArray& senders,
                              const juce::String& draftText);
    void savePersistentSettingsToDisk(bool includeProcessorState = true);
    void loadPersistentSettingsFromDisk();
    void clearLearnMappings();
    bool isSidechainInputActive() const;
    bool isAbletonLiveHost() const;
    void setAbletonWindowSizePreset(int presetIndex);
    void updateHostResizeModeForConnectionStatus(int status);
    void showAboutWindow();
    void beginUpdateCheck(bool userInitiated);
    void completeUpdateCheck(bool userInitiated,
                             bool requestOk,
                             bool updateAvailable,
                             const juce::String& latestVersion,
                             const juce::String& releaseUrl,
                             const juce::String& downloadUrl,
                             const juce::String& errorMessage);
    void showUpdateAvailablePrompt(const juce::String& latestVersion,
                                   const juce::String& releaseUrl,
                                   const juce::String& downloadUrl);
    void loadControlImages(const juce::File& themeDir);
    void updateEditorTimerInterval();
    void applyThemeColours();
    void registerMidiLearnTarget(juce::Component& component, const juce::String& targetId, bool isToggle);
    void syncUserStripMidiTargets();
    void applyMidiMappings();

    struct MidiLearnTarget
    {
        juce::String id;
        juce::Component* component = nullptr;
        bool isToggle = false;
    };

    struct MidiSourceMapping
    {
        bool isController = true;
        int midiChannel = 1;
        int number = 0;
        int lastBinaryState = -1;
    };

    struct OscSourceMapping
    {
        juce::String address;
        int lastBinaryState = -1;
    };

    struct PendingOscEvent
    {
        juce::String address;
        float normalized = 0.0f;
        bool binaryOn = false;
    };

    struct PendingChatTtsLine
    {
        int historyIndex = -1;
        double dueMs = 0.0;
    };
    
    int lastChatRevision = 0;
    int lastChatTtsHistorySize = 0;
    std::vector<PendingChatTtsLine> pendingChatTtsLines;

    std::unique_ptr<juce::DocumentWindow> serverListWindow;
    std::unique_ptr<juce::DocumentWindow> chatWindow;
    std::unique_ptr<juce::DocumentWindow> samplePadsWindow;

    bool autoLevelEnabled = false;
    bool chatPoppedOut = false;
    bool chatPopoutOpenPending = false;
    bool pendingDeferredResizeLayout = false;
    bool applyingDeferredResizeLayout = false;
    bool hostResizeLockedForConnection = false;
    int abletonWindowSizePreset = 1;
    int abletonChatWindowSizePreset = 1;
    int abletonSamplerWindowSizePreset = 1;
    int abletonRemoteUsersWindowSizePreset = 1;
    juce::Rectangle<int> samplePadsWindowBounds { 0, 0, 980, 600 };
    bool samplePadsWindowBoundsValid = false;
    std::unique_ptr<juce::DialogWindow> aboutWindow;
    std::atomic<bool> updateCheckInProgress { false };
    bool updatePromptShowing = false;
    double automaticUpdateCheckDueMs = 0.0;
    double lastResizeEventMs = 0.0;
    double suppressHeavyUiUntilMs = 0.0;
    int lastLaidOutEditorWidth = -1;
    int lastLaidOutEditorHeight = -1;
    int lastSavedEditorWidth = -1;
    int lastSavedEditorHeight = -1;
    int currentEditorTimerIntervalMs = 0;
    int heavyUiTickCounter = 0;
    float voiceChatGlowPhase = 0.0f;
    float syncGlowPhase = 0.0f;
    bool chatTtsEnabled = false;
    juce::String chatTtsVoiceId;
    float chatTtsVolume = 1.0f;
    juce::String chatTtsOutputId;
    juce::StringArray chatTtsVoiceIds;
    juce::StringArray chatTtsVoiceNames;
    juce::StringArray chatTtsOutputIds;
    juce::StringArray chatTtsOutputNames;
    std::unique_ptr<ChatTtsEngine> chatTtsEngine;
    float storedMetronomeVolume = 0.5f;
    std::map<int, float> autoLevelCurrentGains;
    std::map<int, float> autoLevelLastAppliedGains;
    std::map<int, float> autoLevelPeakLevels;
    std::map<int, int> autoLevelChannelActiveTicks;
    std::map<int, int> autoLevelMeasureTicks;
    std::map<int, int> autoLevelOverTargetTicks;
    std::map<int, int> autoLevelUnderTargetTicks;
    std::map<int, juce::String> autoLevelUserNameById;
    std::map<juce::Component*, MidiLearnTarget> midiTargetsByComponent;
    std::map<juce::String, MidiLearnTarget> midiTargetsById;
    std::map<juce::String, MidiSourceMapping> midiSourceByTargetId;
    std::map<juce::String, OscSourceMapping> oscSourceByTargetId;
    juce::String midiLearnArmedTargetId;
    juce::String oscLearnArmedTargetId;
    juce::SpinLock oscEventQueueLock;
    std::vector<PendingOscEvent> pendingOscEvents;
    std::map<int, juce::String> midiRelayTargetByMenuId;
    std::function<void(juce::Component*)> standaloneOptionsMenuHandler;
    NinjamVst3AudioProcessor::SyncMode preferredSyncMode = NinjamVst3AudioProcessor::SyncMode::host;
    std::unique_ptr<juce::MidiInput> midiLearnInputDevice;
    std::unique_ptr<juce::MidiInput> midiRelayInputDevice;
    std::unique_ptr<juce::MidiInput> samplePadsMidiInputDevice;
    juce::String openedMidiLearnInputDeviceId;
    juce::String openedMidiRelayInputDeviceId;
    juce::String openedSamplePadsMidiInputDeviceId;
    juce::String lastLinkAudioLocalInputLabel;
    double lastPersistentSettingsSaveMs = 0.0;
    double lastVideoBackgroundRepaintMs = 0.0;
    double lastTransmitPulseRepaintMs = 0.0;
    double lastVideoButtonPulseRepaintMs = 0.0;
    int lastLocalVoiceLayoutServerMaxChannels = -1;
    int lastLocalVoiceLayoutNumLocalChannels = -1;
    bool lastLocalVoiceLayoutCanUseDedicatedVoice = false;
    double lastClipPulseRepaintMs = 0.0;
    std::array<juce::Rectangle<int>, NinjamVst3AudioProcessor::maxLocalChannels> localChannelPulseBounds;
    juce::Rectangle<int> voiceChannelPulseBounds;
    juce::Rectangle<int> masterChannelPulseBounds;
    std::array<double, NinjamVst3AudioProcessor::maxLocalChannels> localClipStartMs {};
    std::array<bool, NinjamVst3AudioProcessor::maxLocalChannels> localClipPulsing {};
    double voiceClipStartMs = 0.0;
    bool voiceClipPulsing = false;
    double masterClipStartMs = 0.0;
    bool masterClipPulsing = false;
    bool persistentSettingsDirty = false;
    juce::String lastSavedUiSettingsFingerprint;
    int autoLevelWorkTickCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NinjamVst3AudioProcessorEditor)
};
