/*
Copyright 2022 The DDSP-VST Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "JuceHeader.h"

#include "ui/DDSPLookAndFeel.h"
#include "ui/ModelRangeVisualizerComponent.h"
#include "util/InputUtils.h"

constexpr int kNumTrails = 24;
constexpr float kTrailRadiusMin = 0.0f;
constexpr float kTrailRadiusMax = 20.0f;
constexpr float kNumOctaves = 10.6667f;

using namespace ddsp;

DraggableRangeBox::DraggableRangeBox (ModelRangeVisualizerComponent& o) : owner (o) {}

void DraggableRangeBox::paint (juce::Graphics& g)
{
    const auto localBounds = getLocalBounds().reduced (2).toFloat();
    float fillAlphaMultiplier = 0.13f;
    if (isBallInModelRange.load())
        fillAlphaMultiplier *= 2.0f;
    g.setColour (juce::Colour (DDSPColourPalette::kMagenta).withMultipliedAlpha (fillAlphaMultiplier));
    g.fillRoundedRectangle (localBounds, kRoundedRectangleCornerSize);
}

void DraggableRangeBox::resized() {}

void DraggableRangeBox::mouseDrag (const juce::MouseEvent& e)
{
    lastMousePosition = e.getPosition();
    lastMousePosition = owner.getLocalPoint (this, lastMousePosition);

    const float parentWidth = owner.getWidth();
    const float parentHeight = owner.getHeight();

    const auto rangeBoxOriginal = getModelRangeRect (0.0f, 0.0f).reduced (2);

    const float boxWidth = rangeBoxOriginal.getWidth();
    const float boxHeight = rangeBoxOriginal.getHeight();
    const float boxCenterX = rangeBoxOriginal.getCentreX();
    const float boxCenterY = rangeBoxOriginal.getCentreY();

    // Clamp box to stay within bounds.
    lastMousePosition.setX (juce::jlimit (boxWidth / 2, parentWidth - boxWidth / 2, (float) lastMousePosition.getX()));
    lastMousePosition.setY (
        juce::jlimit (boxHeight / 2, parentHeight - boxHeight / 2, (float) lastMousePosition.getY()));

    // Calculate how many pixels the mouse pos is offset from the original model range.
    const float offsetX = lastMousePosition.getX() - boxCenterX;
    // y direction is flipped i.e. mouse moving up is a decrement in pixels
    const float offsetY = -(lastMousePosition.getY() - boxCenterY);

    // Convert pixel offsets to normalized pitch and loudness offsets.
    owner.modelPitchOffset = juce::jmap (offsetX, 0.0f, parentWidth, 0.0f, 1.0f);
    owner.modelLoudnessOffset = juce::jmap (offsetY, 0.0f, parentHeight, 0.0f, 1.0f);

    updateRangeBox();
}

void DraggableRangeBox::mouseDoubleClick (const juce::MouseEvent& e)
{
    // Reset offsets on double click.
    owner.modelPitchOffset = 0.0f;
    owner.modelLoudnessOffset = 0.0f;

    updateRangeBox();
}

void DraggableRangeBox::updateRangeBox()
{
    auto rangeBoxWithOffset = getModelRangeRect (owner.modelPitchOffset, owner.modelLoudnessOffset);

    setBounds (rangeBoxWithOffset.toNearestInt());

    owner.modelRangeChanged();

    repaint();
}

juce::Rectangle<float> DraggableRangeBox::getModelRangeRect (float pitchOffset, float loudnessOffset)
{
    const float width = owner.getWidth();
    const float height = owner.getHeight();

    // Pitch range calculations.
    const float xStart = (owner.modelPitchRangeMinNorm + pitchOffset) * width;
    const float xEnd = (owner.modelPitchRangeMaxNorm + pitchOffset) * width;

    // Loudness range calculations.
    const float yStart = height - (owner.modelLoudnessRangeMaxNorm + loudnessOffset) * height;
    const float yEnd = height - (owner.modelLoudnessRangeMinNorm + loudnessOffset) * height;

    juce::Rectangle<float> modelPitchRangeRect (xStart, yStart, xEnd - xStart, yEnd - yStart);

    return modelPitchRangeRect;
}

//==============================================================================
ToleranceEdgeHandle::ToleranceEdgeHandle (ModelRangeVisualizerComponent& o, bool isLow) : owner (o), isLowEdge (isLow)
{
}

void ToleranceEdgeHandle::mouseDrag (const juce::MouseEvent& e)
{
    owner.dragToleranceEdge (isLowEdge, owner.getLocalPoint (this, e.getPosition()));
}

void ToleranceEdgeHandle::mouseDoubleClick (const juce::MouseEvent&) { owner.resetToleranceEdge (isLowEdge); }

//==============================================================================
ModelRangeVisualizerComponent::ModelRangeVisualizerComponent (DDSPAudioProcessor& processor)
    : audioProcessor (processor),
      rangeBox (*this),
      pitchOffsetAttach (*processor.getValueTree().getParameter ("InputPitch"),
                         [this] (float pitchOffset)
                         {
                             const juce::ScopedValueSetter<bool> svs (ignoreCallbacks, true);
                             modelPitchOffset = pitchOffset;
                             rangeBox.updateRangeBox();
                         }),
      loudnessOffsetAttach (*processor.getValueTree().getParameter ("InputGain"),
                            [this] (float loudnessOffset)
                            {
                                const juce::ScopedValueSetter<bool> svs (ignoreCallbacks, true);
                                modelLoudnessOffset = loudnessOffset;
                                rangeBox.updateRangeBox();
                            }),
      outerPitchMarginLowAttach (*processor.getValueTree().getParameter ("OuterPitchMarginLow"),
                                 [this] (float margin)
                                 {
                                     const juce::ScopedValueSetter<bool> svs (ignoreCallbacks, true);
                                     outerPitchMarginLowSemitones = margin;
                                     updateToleranceBox();
                                 }),
      outerPitchMarginHighAttach (*processor.getValueTree().getParameter ("OuterPitchMarginHigh"),
                                  [this] (float margin)
                                  {
                                      const juce::ScopedValueSetter<bool> svs (ignoreCallbacks, true);
                                      outerPitchMarginHighSemitones = margin;
                                      updateToleranceBox();
                                  }),
      toleranceLowHandle (*this, true),
      toleranceHighHandle (*this, false)
{
    pitchHistoryBuffer.resize (kNumTrails, 0.0f);

    modelPitchRangeMinNorm = 0.25f;
    modelPitchRangeMaxNorm = 0.75f;
    modelPitchOffset = 0.0f;

    int intervalMs = 30;
    double intervalHz = 1000.0 / static_cast<double> (intervalMs);

    int numSmoothPitch = 4;
    double smoothingRampTimePitch = numSmoothPitch / intervalHz;

    pitchSmoother.reset (intervalHz, smoothingRampTimePitch);

    // Initialize loudness history to VU min.
    loudnessHistoryBuffer.resize (kNumTrails, 0.0f);

    modelLoudnessRangeMinNorm = 0.25f;
    modelLoudnessRangeMaxNorm = 0.75f;
    modelLoudnessOffset = 0.0f;

    int numSmoothLoudness = 8;

    double smoothingRampTimeLoudness = numSmoothLoudness / intervalHz;

    loudnessSmoother.reset (intervalHz, smoothingRampTimeLoudness);

    juce::MouseCursor draggingHand (juce::MouseCursor::DraggingHandCursor);
    rangeBox.setMouseCursor (draggingHand);
    addAndMakeVisible (&rangeBox);

    juce::MouseCursor resizeCursor (juce::MouseCursor::LeftRightResizeCursor);
    toleranceLowHandle.setMouseCursor (resizeCursor);
    toleranceHighHandle.setMouseCursor (resizeCursor);
    addAndMakeVisible (toleranceLowHandle);
    addAndMakeVisible (toleranceHighHandle);

    // LookAndFeel_V4's default tick/tickDisabled colours are tuned for its own dark theme and
    // render near-invisibly (and identically for on/off) against this plugin's light theme, so
    // both must be set explicitly -- textColourId alone isn't enough.
    muteOutsideRangeButton.setColour (juce::ToggleButton::textColourId,
                                      juce::Colour (DDSPColourPalette::kLabelTextColour));
    muteOutsideRangeButton.setColour (juce::ToggleButton::tickColourId, juce::Colour (DDSPColourPalette::kMagenta));
    muteOutsideRangeButton.setColour (juce::ToggleButton::tickDisabledColourId,
                                      juce::Colour (DDSPColourPalette::kDarkerGrey));
    addAndMakeVisible (muteOutsideRangeButton);
    muteOutsideRangeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        audioProcessor.getValueTree(), "MuteOutsideRange", muteOutsideRangeButton);

    // TODO: Temporary fix for the child components being clipped at the component edges.
    // Need to figure out drawing with margin.
    setPaintingIsUnclipped (true);

    setModelMetadata (audioProcessor.getPredictControlsModelMetadata());

    // Send initial update(s).
    pitchOffsetAttach.sendInitialUpdate();
    loudnessOffsetAttach.sendInitialUpdate();
    outerPitchMarginLowAttach.sendInitialUpdate();
    outerPitchMarginHighAttach.sendInitialUpdate();

    startTimer (intervalMs);
}

ModelRangeVisualizerComponent::~ModelRangeVisualizerComponent() {}

void ModelRangeVisualizerComponent::drawBackground (juce::Graphics& g, const int width, const int height)
{
    g.setColour (juce::Colour (DDSPColourPalette::kDarkGrey));

    juce::String noteLabelText = "C";
    int octave = -2;
    const int noteLabelWidth = 24;
    const int noteLabelHeight = 12;

    const int yStart = kRoundedRectangleCornerSize / 2;
    const int yEnd = height - (kRoundedRectangleCornerSize / 2) - noteLabelHeight * 1.25f;
    for (int i = 0; i < kNumOctaves; ++i)
    {
        const float freq_C = kPitchRangeMin_Hz * std::pow (2.0f, (i));
        int xPos = (ddsp::normalizedPitch (freq_C)) * width + 1;
        g.drawLine (xPos, yStart, xPos, yEnd, 0.5f);
        g.setFont (juce::Font ("Google Sans", 12.00f, juce::Font::plain).withExtraKerningFactor (kKerningFactor));
        g.drawText (noteLabelText + juce::String (octave++),
                    xPos - noteLabelWidth / 2,
                    yEnd + (noteLabelHeight * 0.25f),
                    noteLabelWidth,
                    noteLabelHeight,
                    juce::Justification::centred);
    }
}

void ModelRangeVisualizerComponent::paint (juce::Graphics& g)
{
    const float width = getWidth();
    const float height = getHeight();

    drawBackground (g, width, height);

    // Outer tolerance box: drawn behind the inner box as a soft fill only (no outline), so it
    // reads as a halo around the crisp inner reference box rather than competing with it. Must
    // use the same (modelPitchOffset, modelLoudnessOffset) the inner box's actual draggable
    // bounds and this box's own drag handles use (updateToleranceBox()), not (0, 0) -- otherwise
    // this fill stays anchored to the untranslated position while the inner box and the handles
    // track the user's drag, making it look like a second, disconnected box.
    auto outerRangeBoxWithOffset = getOuterRangeRect (modelPitchOffset, modelLoudnessOffset);
    g.setColour (juce::Colour (DDSPColourPalette::kMagenta).withMultipliedAlpha (0.08f));
    g.fillRoundedRectangle (outerRangeBoxWithOffset.reduced (2), kRoundedRectangleCornerSize);

    auto rangeBoxOriginal = rangeBox.getModelRangeRect (0.0f, 0.0f);

    g.setColour (juce::Colour (DDSPColourPalette::kMagenta));
    g.drawRoundedRectangle (rangeBoxOriginal.reduced (2), kRoundedRectangleCornerSize, 1.0f);

    const float alphaGradient = 0.5f / static_cast<float> (kNumTrails);
    float alpha = 0.0f;

    for (int i = 0; i < kNumTrails; ++i)
    {
        g.setColour (juce::Colour (DDSPColourPalette::kMagenta).withMultipliedAlpha (alpha));

        const float ellipseRadius = juce::jmap (
            static_cast<float> (i), 0.0f, static_cast<float> (kNumTrails), kTrailRadiusMin, kTrailRadiusMax);
        // Adding this to the xPitch and yLoudness ensures that the ball shrinks towards
        // the center and not the top left.
        const float shrinkingEllipseCenter = (kTrailRadiusMax - ellipseRadius) / 2.0f;

        const float pitch = *std::next (pitchHistoryBuffer.begin(), i);
        const float loudness = *std::next (loudnessHistoryBuffer.begin(), i);

        const float xPitch = juce::jmap (pitch, 0.0f, 1.0f, 0.0f, width);
        const float yLoudness = juce::jmap (loudness, 0.0f, 1.0f, height, 0.0f);

        if (loudness >= 0.05f)
            g.fillEllipse (xPitch + shrinkingEllipseCenter - (kTrailRadiusMax / 2.0f),
                           yLoudness + shrinkingEllipseCenter - (kTrailRadiusMax / 2.0f),
                           ellipseRadius,
                           ellipseRadius);

        alpha += alphaGradient;
    }
}

void ModelRangeVisualizerComponent::resized()
{
    rangeBox.updateRangeBox();
    updateToleranceBox();

    constexpr int buttonWidth = 150;
    constexpr int buttonHeight = 24;
    muteOutsideRangeButton.setBounds (getWidth() - buttonWidth - kPadding, kPadding, buttonWidth, buttonHeight);
}

bool ModelRangeVisualizerComponent::hitTest (int x, int y)
{
    const juce::Point<int> p (x, y);

    // Component::getComponentAt() gates all child hit-testing on this parent hitTest, so every
    // clickable child region (not just rangeBox) must be included here or it becomes unclickable.
    return rangeBox.getBounds().contains (p) || muteOutsideRangeButton.getBounds().contains (p)
           || toleranceLowHandle.getBounds().contains (p) || toleranceHighHandle.getBounds().contains (p);
}

void ModelRangeVisualizerComponent::timerCallback()
{
    pitchSmoother.setTargetValue (audioProcessor.getPitch());
    const float pitch = pitchSmoother.getNextValue();
    pitchHistoryBuffer.pop_front();
    pitchHistoryBuffer.push_back (pitch);

    loudnessSmoother.setTargetValue (audioProcessor.getRMS());
    const float loudness = loudnessSmoother.getNextValue();
    loudnessHistoryBuffer.pop_front();
    loudnessHistoryBuffer.push_back (loudness);

    auto rangeBoxWithOffset = rangeBox.getModelRangeRect (modelPitchOffset, modelLoudnessOffset);
    const float xPitch = juce::jmap (pitch, 0.0f, 1.0f, 0.0f, static_cast<float> (getWidth()));
    const float yLoudness = juce::jmap (loudness, 0.0f, 1.0f, static_cast<float> (getHeight()), 0.0f);

    if (rangeBoxWithOffset.contains (xPitch, yLoudness))
        rangeBox.isBallInModelRange.store (true);
    else
        rangeBox.isBallInModelRange.store (false);

    repaint();
}

void ModelRangeVisualizerComponent::setModelMetadata (const ddsp::PredictControlsModel::Metadata& metadata)
{
    modelPitchRangeMinNorm = ddsp::normalizedPitch (metadata.minPitch_Hz);
    modelPitchRangeMaxNorm = ddsp::normalizedPitch (metadata.maxPitch_Hz);
    modelLoudnessRangeMinNorm = ddsp::normalizedLoudness (metadata.minPower_dB);
    modelLoudnessRangeMaxNorm = ddsp::normalizedLoudness (metadata.maxPower_dB);

    rangeBox.updateRangeBox();
    updateToleranceBox();
}

juce::Rectangle<float> ModelRangeVisualizerComponent::getOuterRangeRect (float pitchOffset, float loudnessOffset) const
{
    const float width = getWidth();
    const float height = getHeight();

    // Pitch-only tolerance: the outer box extends the inner box's pitch edges by the two
    // independent margins (converted from semitones to normalized units), but uses the same
    // loudness bounds as the inner box.
    const float marginLowNorm = outerPitchMarginLowSemitones / ddsp::kMidiPitchRange;
    const float marginHighNorm = outerPitchMarginHighSemitones / ddsp::kMidiPitchRange;

    const float xStart = (modelPitchRangeMinNorm + pitchOffset - marginLowNorm) * width;
    const float xEnd = (modelPitchRangeMaxNorm + pitchOffset + marginHighNorm) * width;

    const float yStart = height - (modelLoudnessRangeMaxNorm + loudnessOffset) * height;
    const float yEnd = height - (modelLoudnessRangeMinNorm + loudnessOffset) * height;

    return { xStart, yStart, xEnd - xStart, yEnd - yStart };
}

void ModelRangeVisualizerComponent::updateToleranceBox()
{
    const auto outerRect = getOuterRangeRect (modelPitchOffset, modelLoudnessOffset);

    constexpr int handleWidth = 10;
    toleranceLowHandle.setBounds (
        juce::Rectangle<float> (
            outerRect.getX() - handleWidth / 2.0f, outerRect.getY(), (float) handleWidth, outerRect.getHeight())
            .toNearestInt());
    toleranceHighHandle.setBounds (
        juce::Rectangle<float> (
            outerRect.getRight() - handleWidth / 2.0f, outerRect.getY(), (float) handleWidth, outerRect.getHeight())
            .toNearestInt());

    repaint();
}

void ModelRangeVisualizerComponent::dragToleranceEdge (bool isLowEdge, juce::Point<int> positionInThis)
{
    const float width = getWidth();
    if (width <= 0.0f)
        return;

    const float draggedNorm = positionInThis.getX() / width;

    if (isLowEdge)
    {
        const float innerEdgeNorm = modelPitchRangeMinNorm + modelPitchOffset;
        const float marginNorm = innerEdgeNorm - draggedNorm;
        outerPitchMarginLowSemitones = juce::jlimit (0.0f, 24.0f, marginNorm * ddsp::kMidiPitchRange);
    }
    else
    {
        const float innerEdgeNorm = modelPitchRangeMaxNorm + modelPitchOffset;
        const float marginNorm = draggedNorm - innerEdgeNorm;
        outerPitchMarginHighSemitones = juce::jlimit (0.0f, 24.0f, marginNorm * ddsp::kMidiPitchRange);
    }

    updateToleranceBox();

    if (isLowEdge)
        outerPitchMarginLowAttach.setValueAsCompleteGesture (outerPitchMarginLowSemitones);
    else
        outerPitchMarginHighAttach.setValueAsCompleteGesture (outerPitchMarginHighSemitones);
}

void ModelRangeVisualizerComponent::resetToleranceEdge (bool isLowEdge)
{
    if (isLowEdge)
    {
        outerPitchMarginLowSemitones = 0.0f;
        outerPitchMarginLowAttach.setValueAsCompleteGesture (0.0f);
    }
    else
    {
        outerPitchMarginHighSemitones = 0.0f;
        outerPitchMarginHighAttach.setValueAsCompleteGesture (0.0f);
    }

    updateToleranceBox();
}

void ModelRangeVisualizerComponent::modelRangeChanged()
{
    if (ignoreCallbacks)
        return;

    pitchOffsetAttach.setValueAsCompleteGesture (modelPitchOffset);
    loudnessOffsetAttach.setValueAsCompleteGesture (modelLoudnessOffset);
}

void ModelRangeVisualizerComponent::changeListenerCallback (juce::ChangeBroadcaster* b)
{
    setModelMetadata (audioProcessor.getPredictControlsModelMetadata());
}
