/*
    This file is part of Helio Workstation.

    Helio is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Helio is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Helio. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Common.h"
#include "MidiRoll.h"
#include "MidiRollHeader.h"
#include "MidiRollExpandMark.h"
#include "MidiEvent.h"
#include "MidiEventComponent.h"
#include "MidiLayer.h"
#include "MidiEventComponentLasso.h"
#include "ProjectTreeItem.h"
#include "TriggersTrackMap.h"

#include "LightShadowDownwards.h"
#include "LightShadowUpwards.h"

#include "WipeSpaceHelper.h"
#include "InsertSpaceHelper.h"
#include "TimelineWarningMarker.h"

#include "InternalClipboard.h"
#include "SerializationKeys.h"

#include "LongTapController.h"
#include "SmoothPanController.h"
#include "SmoothZoomController.h"
#include "MultiTouchController.h"
#include "Origami.h"
#include "MainLayout.h"

#include "Transport.h"
#include "IconComponent.h"

#include "MainWindow.h"
#include "PlayerThread.h"

#include "ProjectTimeline.h"
#include "AnnotationsLayer.h"
#include "TimeSignaturesLayer.h"
#include "MidiRollToolbox.h"
#include "MidiRollListener.h"
#include "VersionControlTreeItem.h"

#include "AnnotationDialog.h"
#include "TimeSignatureDialog.h"

#include "App.h"
#include "Workspace.h"
#include "AudioCore.h"
#include "AudioMonitor.h"

#include <limits.h>

#if HELIO_DESKTOP
//(defined (JUCE_MAC) || defined (JUCE_LINUX))
#   define MIDIROLL_FOLLOWS_INDICATOR 1
#else
//  on Windows and mobiles this sucks, so just turn it off.
#   define MIDIROLL_FOLLOWS_INDICATOR 0
#endif

// force compile template
#include "AnnotationsMap/AnnotationsTrackMap.cpp"
template class AnnotationsTrackMap<AnnotationLargeComponent>;

// force compile template
#include "TimeSignaturesMap/TimeSignaturesTrackMap.cpp"
template class TimeSignaturesTrackMap<TimeSignatureLargeComponent>;


MidiRoll::MidiRoll(ProjectTreeItem &parentProject,
                   Viewport &viewportRef,
                   WeakReference<AudioMonitor> AudioMonitor) :
    clippingDetector(std::move(AudioMonitor)),
    project(parentProject),
    viewport(viewportRef),
    viewportAnchor(0, 0),
    clickAnchor(0, 0),
    zoomAnchor(0, 0),
    primaryActiveLayer(nullptr),
    barWidth(0),
    firstBar(INT_MAX),
    lastBar(INT_MIN),
    trackFirstBeat(0.f),
    trackLastBeat(DEFAULT_NUM_BARS * NUM_BEATS_IN_BAR),
    header(nullptr),
    indicator(nullptr),
    spaceDragMode(false),
    altDrawMode(false),
    draggedDistance(0),
    timeEnteredDragMode(0),
    transportLastCorrectPosition(0.0),
    transportIndicatorOffset(0.0),
    shouldFollowIndicator(false)
{
    this->setOpaque(true);
    this->setBufferedToImage(false);

    this->setSize(this->viewport.getWidth(), this->viewport.getHeight());

    this->setWantsKeyboardFocus(true);
    this->setFocusContainer(true);
    this->setMouseClickGrabsKeyboardFocus(true);

    this->topShadow = new LightShadowDownwards();
    this->bottomShadow = new LightShadowUpwards();

    this->header = new MidiRollHeader(this->project.getTransport(), *this, this->viewport);
    this->annotationsTrack = new AnnotationsLargeMap(this->project, *this);
    this->timeSignaturesTrack = new TimeSignaturesLargeMap(this->project, *this);
    
    this->indicator = new TransportIndicator(*this, this->project.getTransport(), this);

    this->lassoComponent = new MidiEventComponentLasso();
    this->lassoComponent->setWantsKeyboardFocus(false);
    this->lassoComponent->setFocusContainer(false);

    this->addAndMakeVisible(this->topShadow);
    this->addAndMakeVisible(this->bottomShadow);
    this->addAndMakeVisible(this->header);
    this->addAndMakeVisible(this->annotationsTrack);
    this->addAndMakeVisible(this->timeSignaturesTrack);
    this->addAndMakeVisible(this->indicator);

    this->addAndMakeVisible(this->lassoComponent);

    this->longTapController = new LongTapController(*this);
    this->addMouseListener(this->longTapController, true); // on this and * children

    this->multiTouchController = new MultiTouchController(*this);
    this->addMouseListener(this->multiTouchController, false); // on this only

    this->smoothPanController = new SmoothPanController(*this);
    this->smoothZoomController = new SmoothZoomController(*this);
    
    this->project.addListener(this);
    this->project.getEditMode().addChangeListener(this);
    this->project.getTransport().addTransportListener(this);
    
    if (this->clippingDetector != nullptr)
    {
        this->clippingDetector->addClippingListener(this);
    }
}

MidiRoll::~MidiRoll()
{
    if (this->clippingDetector != nullptr)
    {
        this->clippingDetector->removeClippingListener(this);
    }
    
    this->removeAllRollListeners();

    this->project.getTransport().removeTransportListener(this);
    this->project.getEditMode().removeChangeListener(this);
    this->project.removeListener(this);

    this->removeMouseListener(this->multiTouchController);
    this->removeMouseListener(this->longTapController);

    this->eventComponents.clear();
}


Viewport &MidiRoll::getViewport() const noexcept
{
    return this->viewport;
}

Transport &MidiRoll::getTransport() const noexcept
{
    return this->project.getTransport();
}

ProjectTreeItem &MidiRoll::getProject() const noexcept
{
    return this->project;
}

MidiLayer *MidiRoll::getPrimaryActiveMidiLayer() const noexcept
{
    return this->primaryActiveLayer;
}

int MidiRoll::getNumActiveLayers() const noexcept
{
    return this->activeLayers.size();
}

MidiLayer *MidiRoll::getActiveMidiLayer(int index) const noexcept
{
    return this->activeLayers[index];
}

//===----------------------------------------------------------------------===//
// Timeline events
//===----------------------------------------------------------------------===//

float MidiRoll::getPositionForNewTimelineEvent() const
{
	const double indicatorOffset = this->findIndicatorOffsetFromViewCentre();
	const bool indicatorIsWithinScreen = fabs(indicatorOffset) < (this->viewport.getViewWidth() / 2);
	float targetBeat = 0.f;

	// If playhead is visible, put new event on it's position, otherwise just align to the screen center
	if (indicatorIsWithinScreen)
	{
		const int viewCentre = this->viewport.getViewPositionX() + (this->viewport.getViewWidth() / 2);
		const int indicatorPosition = viewCentre + int(indicatorOffset);
		return this->getRoundBeatByXPosition(indicatorPosition);
	}

	const int viewCentre = this->viewport.getViewPositionX() + (this->viewport.getViewWidth() / 2);
	return this->getRoundBeatByXPosition(viewCentre);
}

void MidiRoll::insertAnnotationWithinScreen(const String &annotation)
{
    if (AnnotationsLayer *annotationsLayer = dynamic_cast<AnnotationsLayer *>(this->project.getTimeline()->getAnnotations()))
    {
		annotationsLayer->checkpoint();
		const float targetBeat = this->getPositionForNewTimelineEvent();
		AnnotationEvent event(annotationsLayer, targetBeat, annotation, Colours::transparentWhite);
        annotationsLayer->insert(event, true);
    }
}

void MidiRoll::insertTimeSignatureWithinScreen(int numerator, int denominator)
{
	jassert(denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16 || denominator == 32);
	if (TimeSignaturesLayer *tsLayer = dynamic_cast<TimeSignaturesLayer *>(this->project.getTimeline()->getTimeSignatures()))
	{
		tsLayer->checkpoint();
		const float targetBeat = this->getPositionForNewTimelineEvent();
		TimeSignatureEvent event(tsLayer, targetBeat, numerator, denominator);
		tsLayer->insert(event, true);
	}
}

//===----------------------------------------------------------------------===//
// Custom maps
//===----------------------------------------------------------------------===//

void MidiRoll::addOwnedMap(Component *newTrackMap)
{
    this->trackMaps.add(newTrackMap);
    this->addAndMakeVisible(newTrackMap);

    // fade-in if not the first child
    if (this->trackMaps.size() > 1)
    {
        newTrackMap->setVisible(false);
        this->fader.fadeIn(newTrackMap, 200);
    }

    newTrackMap->toFront(false);
    this->resized();
}

void MidiRoll::removeOwnedMap(Component *existingTrackMap)
{
    if (this->trackMaps.contains(existingTrackMap))
    {
        this->fader.fadeOut(existingTrackMap, 150);
        this->removeChildComponent(existingTrackMap);
        this->trackMaps.removeObject(existingTrackMap);
        this->resized();
    }
}

//===----------------------------------------------------------------------===//
// Modes
//===----------------------------------------------------------------------===//

MidiRollEditMode &MidiRoll::getEditMode()
{
    return this->project.getEditMode();
}

bool MidiRoll::isInSelectionMode() const
{
    return (this->project.getEditMode().isMode(MidiRollEditMode::selectionMode));
}

bool MidiRoll::isInDragMode() const
{
    return (this->project.getEditMode().isMode(MidiRollEditMode::dragMode));
}


//===----------------------------------------------------------------------===//
// MidiRoll listeners management
//===----------------------------------------------------------------------===//

void MidiRoll::addRollListener(MidiRollListener *listener)
{
    jassert(MessageManager::getInstance()->currentThreadHasLockedMessageManager());
    this->listeners.add(listener);
}

void MidiRoll::removeRollListener(MidiRollListener *listener)
{
    jassert(MessageManager::getInstance()->currentThreadHasLockedMessageManager());
    this->listeners.remove(listener);
}

void MidiRoll::removeAllRollListeners()
{
    jassert(MessageManager::getInstance()->currentThreadHasLockedMessageManager());
    this->listeners.clear();
}

void MidiRoll::broadcastRollMoved()
{
    this->listeners.call(&MidiRollListener::onMidiRollMoved, this);
}

void MidiRoll::broadcastRollResized()
{
    this->listeners.call(&MidiRollListener::onMidiRollResized, this);
}


//===----------------------------------------------------------------------===//
// MultiTouchListener
//===----------------------------------------------------------------------===//

void MidiRoll::multiTouchZoomEvent(const Point<float> &origin, const Point<float> &zoom)
{
    //Logger::writeToLog("MidiRoll::multiTouchZoomEvent");
    this->smoothPanController->cancelPan();
    this->smoothZoomController->zoomRelative(origin, zoom);
}

void MidiRoll::multiTouchPanEvent(const Point<float> &offset)
{
    //Logger::writeToLog("MidiRoll::multiTouchPanEvent");
    //this->smoothZoomController->cancelZoom();
    this->smoothPanController->panByOffset(this->viewport.getViewPosition() + offset.toInt());
}

void MidiRoll::multiTouchCancelZoom()
{
    this->clickAnchor = Desktop::getInstance().getMainMouseSource().getScreenPosition();
    this->smoothZoomController->cancelZoom();
}

void MidiRoll::multiTouchCancelPan()
{
    this->clickAnchor = Desktop::getInstance().getMainMouseSource().getScreenPosition();
    this->smoothPanController->cancelPan();
}

Point<float> MidiRoll::getMultiTouchOrigin(const Point<float> &from)
{
    return (from - this->viewport.getViewPosition().toFloat());
}


//===----------------------------------------------------------------------===//
// SmoothPanListener
//===----------------------------------------------------------------------===//

void MidiRoll::panByOffset(const int offsetX, const int offsetY)
{
    this->stopFollowingIndicator();

    const bool needsToStretchRight = (offsetX >= (this->getWidth() - this->viewport.getViewWidth()));
    const bool needsToStretchLeft = (offsetX <= 0);

    int numBarsToExpand = 1;
    if (this->barWidth <= 1)
    {
        numBarsToExpand = 64;
    }
    else if (this->barWidth <= 16)
    {
        numBarsToExpand = 16;
    }
    else if (this->barWidth <= 64)
    {
        numBarsToExpand = 4;
    }
    
    if (needsToStretchRight)
    {
        this->setLastBar(this->lastBar + numBarsToExpand);
        this->viewport.setViewPosition(offsetX, offsetY); // sic! after setLastBar
        this->grabKeyboardFocus();

        const float barCloseToTheRight = float(this->lastBar - numBarsToExpand);
        this->header->addAndMakeVisible(new MidiRollExpandMark(*this, barCloseToTheRight, numBarsToExpand));
    }
    else if (needsToStretchLeft)
    {
        const float deltaW = float(this->barWidth * numBarsToExpand);
        this->clickAnchor.addXY(deltaW / SMOOTH_PAN_SPEED_MULTIPLIER, 0); // an ugly hack
        this->setFirstBar(this->firstBar - numBarsToExpand);
        this->viewport.setViewPosition(offsetX + int(deltaW), offsetY); // sic! after setFirstBar
        this->grabKeyboardFocus();

        const float barCloseToTheLeft = float(this->firstBar);
        this->header->addAndMakeVisible(new MidiRollExpandMark(*this, barCloseToTheLeft, numBarsToExpand));
    }
    else
    {
        this->viewport.setViewPosition(offsetX, offsetY);
    }

    this->updateChildrenPositions();
}

void MidiRoll::panProportionally(const float absX, const float absY)
{
    this->stopFollowingIndicator();
    this->viewport.setViewPositionProportionately(absX, absY);
    this->updateChildrenPositions();
}

Point<int> MidiRoll::getPanOffset() const
{
    return this->viewport.getViewPosition();
}


//===----------------------------------------------------------------------===//
// SmoothZoomListener
//===----------------------------------------------------------------------===//

void MidiRoll::startSmoothZoom(const Point<float> &origin, const Point<float> &factor)
{
    this->smoothZoomController->zoomRelative(origin, factor);
}

void MidiRoll::zoomInImpulse()
{
    const Point<float> origin = this->getViewport().getLocalBounds().getCentre().toFloat();
    const Point<float> factor(0.15f, 0.05f);
    this->startSmoothZoom(origin, factor);
}

void MidiRoll::zoomOutImpulse()
{
    const Point<float> origin = this->getViewport().getLocalBounds().getCentre().toFloat();
    const Point<float> factor(-0.15f, -0.05f);
    this->startSmoothZoom(origin, factor);
}

void MidiRoll::zoomAbsolute(const Point<float> &zoom)
{
//    this->stopFollowingIndicator();

    const float &newWidth = (this->getNumBars() * MAX_BAR_WIDTH) * zoom.getX();
    const float &barsOnNewScreen = float(newWidth / MAX_BAR_WIDTH);
    const float &viewWidth = float(this->viewport.getViewWidth());
    const float &newBarWidth = floorf(viewWidth / barsOnNewScreen + .5f);
    this->setBarWidth(newBarWidth);

    this->transportIndicatorOffset = this->findIndicatorOffsetFromViewCentre();
}

void MidiRoll::zoomRelative(const Point<float> &origin, const Point<float> &factor)
{
    //this->stopFollowingIndicator();

    const Point<float> oldViewPosition = this->viewport.getViewPosition().toFloat();
    const Point<float> absoluteOrigin = oldViewPosition + origin;
    const float oldWidth = float(this->getWidth());

    float newBarWidth = this->getBarWidth() + (factor.getX() * this->getBarWidth());
    const float estimatedNewWidth = newBarWidth * this->getNumBars();

    if (estimatedNewWidth < float(this->viewport.getViewWidth()))
    { newBarWidth = (float(this->viewport.getWidth() + 1) / float(this->getNumBars())); } // a hack
    //{ newBarWidth = (float(this->viewport.getViewWidth()) / float(this->getNumBars())); }

    this->setBarWidth(newBarWidth);

    const float newWidth = float(this->getWidth());
    const float mouseOffsetX = float(absoluteOrigin.getX() - oldViewPosition.getX());
    const float newViewPositionX = float((absoluteOrigin.getX() * newWidth) / oldWidth) - mouseOffsetX;
    this->viewport.setViewPosition(int(newViewPositionX + 0.5f), int(oldViewPosition.getY()));

    this->transportIndicatorOffset = this->findIndicatorOffsetFromViewCentre();

    this->resetDraggingAnchors();
    this->updateChildrenPositions();
    //this->grabKeyboardFocus();
}

float MidiRoll::getZoomFactorX() const
{
    const float &numBars = float(this->getNumBars());
    const float &viewWidth = float(this->viewport.getViewWidth());
    const float &barWidth = float(this->getBarWidth());
    const float &barsOnScreen = (viewWidth / barWidth);
    return (barsOnScreen / numBars);
}

float MidiRoll::getZoomFactorY() const
{
    return 1.f;
}


//===----------------------------------------------------------------------===//
// Accessors
//===----------------------------------------------------------------------===//

int MidiRoll::getXPositionByTransportPosition(double absPosition, double canvasWidth) const
{
    const double rollLengthInBeats = (this->getLastBeat() - this->getFirstBeat());
    const double projectLengthInBeats = (this->trackLastBeat - this->trackFirstBeat);
    const double firstBeatOffset = (this->trackFirstBeat - this->getFirstBeat());

    const double trackWidth = canvasWidth * (projectLengthInBeats / rollLengthInBeats);
    const double trackStart = canvasWidth * (firstBeatOffset / rollLengthInBeats);

    return int(trackStart + absPosition * trackWidth);
}

double MidiRoll::getTransportPositionByXPosition(int xPosition, double canvasWidth) const
{
    const double rollLengthInBeats = (this->getLastBeat() - this->getFirstBeat());
    const double projectLengthInBeats = (this->trackLastBeat - this->trackFirstBeat);
    const double firstBeatOffset = (this->trackFirstBeat - this->getFirstBeat());

    const double trackWidth = canvasWidth * (projectLengthInBeats / rollLengthInBeats);
    const double trackStart = canvasWidth * (firstBeatOffset / rollLengthInBeats);

    return double((xPosition - trackStart) / trackWidth);
}

double MidiRoll::getTransportPositionByBeat(float targetBeat) const
{
    const double projectLengthInBeats = (this->trackLastBeat - this->trackFirstBeat);
    return double((targetBeat - this->trackFirstBeat) / projectLengthInBeats);
}

float MidiRoll::getBeatByTransportPosition(double absSeekPosition) const
{
    const double projectLengthInBeats = (this->trackLastBeat - this->trackFirstBeat);
    return float(projectLengthInBeats * absSeekPosition) + this->trackFirstBeat;
}

float MidiRoll::getBarByXPosition(int xPosition) const
{
    const int zeroCanvasOffset = int(this->getFirstBar() * this->getBarWidth());
    const float bar = float(xPosition + zeroCanvasOffset) / float(this->getBarWidth());
    return bar;
}

int MidiRoll::getXPositionByBar(float targetBar) const
{
    return int((targetBar - this->getFirstBar()) * float(this->getBarWidth()));
}

int MidiRoll::getXPositionByBeat(float targetBeat) const
{
    return this->getXPositionByBar(targetBeat / float(NUM_BEATS_IN_BAR));
}

float MidiRoll::getFloorBeatByXPosition(int x) const
{
	Array<float> allSnaps;
	allSnaps.addArray(this->visibleBars);
	allSnaps.addArray(this->visibleBeats);
	allSnaps.addArray(this->visibleSnaps);

	float d = FLT_MAX;
	float targetX = float(x);
	for (float snapX : allSnaps)
	{
		const float dist = fabs(x - snapX);
		if (dist < d && snapX < x)
		{
			d = dist;
			targetX = snapX;
		}
	}

    const float lastAlignedBeat = float(this->lastBar * NUM_BEATS_IN_BAR);
    const float firstAlignedBeat = float(this->firstBar * NUM_BEATS_IN_BAR);
	float beatNumber = (targetX / this->barWidth) * NUM_BEATS_IN_BAR + firstAlignedBeat;
	return jmin(jmax(beatNumber, firstAlignedBeat), lastAlignedBeat);
}

float MidiRoll::getRoundBeatByXPosition(int x) const
{
	Array<float> allSnaps;
	allSnaps.addArray(this->visibleBars);
	allSnaps.addArray(this->visibleBeats);
	allSnaps.addArray(this->visibleSnaps);

	float d = FLT_MAX;
	float targetX = float(x);
	for (float snapX : allSnaps)
	{
		const float dist = fabs(x - snapX);
		if (dist < d)
		{
			d = dist;
			targetX = snapX;
		}
	}

    const float lastAlignedBeat = float(this->lastBar * NUM_BEATS_IN_BAR);
    const float firstAlignedBeat = float(this->firstBar * NUM_BEATS_IN_BAR);
	float beatNumber = (targetX / this->barWidth) * NUM_BEATS_IN_BAR + firstAlignedBeat;
	return jmin(jmax(beatNumber, firstAlignedBeat), lastAlignedBeat);
}

void MidiRoll::setFirstBar(int bar)
{
    if (this->firstBar == bar)
    {
        return;
    }

    this->firstBar = bar;
    this->updateBounds();
}

void MidiRoll::setLastBar(int bar)
{
    if (this->lastBar == bar)
    {
        return;
    }

    this->lastBar = bar;
    this->updateBounds();
}

void MidiRoll::setBarRange(int first, int last)
{
    if (this->lastBar == last && this->firstBar == first)
    {
        return;
    }

    this->lastBar = last;
    this->firstBar = first;
    this->updateBounds();
}

void MidiRoll::setBarWidth(const float newBarWidth)
{
    if (newBarWidth > 1440 || newBarWidth <= 0) { return; }
    this->barWidth = newBarWidth;
    this->updateBounds();
}

#define MIN_BAR_WIDTH 12
#define MIN_BEAT_WIDTH 8

void MidiRoll::computeVisibleBeatLines()
{
    this->visibleBars.clearQuick();
    this->visibleBeats.clearQuick();
    this->visibleSnaps.clearQuick();

    const auto tsLayer = this->project.getTimeline()->getTimeSignatures();
    
    const float zeroCanvasOffset = this->firstBar * this->barWidth;
    
    const float viewPosX = float(this->viewport.getViewPositionX());
    const float paintStartX = viewPosX + zeroCanvasOffset;
    const float paintEndX = float(viewPosX + this->viewport.getViewWidth()) + zeroCanvasOffset;
    
    const int paintStartBar = int(paintStartX / this->barWidth) - 1;
    const int paintEndBar = int(paintEndX / this->barWidth) + 1;
    
	// Get number of snaps depending on bar width, 
	// 2 for 64, 4 for 128, 8 for 256, etc:
	const float nearestPowTwo = ceilf(log(this->barWidth) / log(2.f));
	const float numSnaps = powf(2, jlimit(1.f, 6.f, nearestPowTwo - 5.f)); // like -4.f for twice as dense grid
	const float snapWidth = this->barWidth / numSnaps;

    int numerator = TIME_SIGNATURE_DEFAULT_NUMERATOR;
    int denominator = TIME_SIGNATURE_DEFAULT_DENOMINATOR;
    float i = float(paintStartBar);

    int nextSignatureIdx = 0;

    // Find a time signature to start from (or use default values):
    // find a first time signature after a paint start and take a previous one, if any
    for (; nextSignatureIdx < tsLayer->size(); ++nextSignatureIdx)
    {
        const auto signature =
            static_cast<TimeSignatureEvent *>(tsLayer->getUnchecked(nextSignatureIdx));
        
        if (signature->getBeat() >= (paintStartBar * NUM_BEATS_IN_BAR))
        {
            break;
        }

        numerator = signature->getNumerator();
        denominator = signature->getDenominator();
        i = signature->getBeat() / NUM_BEATS_IN_BAR;
    }
    
    float barWidthSum = 0.f;
    while (i <= paintEndBar)
    {
        // Expecting the bar to be full (if time signature does not change in between)
        float beatStep = 1.f / float(denominator);
        float barStep = beatStep * float(numerator);
        const float barStartX = this->barWidth * i - zeroCanvasOffset;
        const float stepWidth = this->barWidth * barStep;
        barWidthSum += stepWidth;

        if (barWidthSum > MIN_BAR_WIDTH &&
            barStartX >= viewPosX)
        {
            visibleBars.add(barStartX);
            barWidthSum = 0;
        }
        
        // Check if we have more time signatures to come
        TimeSignatureEvent *nextSignature = nullptr;
        if (nextSignatureIdx < tsLayer->size())
        {
            nextSignature = static_cast<TimeSignatureEvent *>(tsLayer->getUnchecked(nextSignatureIdx));
        }
        
        // Now for the beat lines
        bool lastFrame = false;
        for (float j = 0.f; j < barStep && !lastFrame; j += beatStep)
        {
            const float beatStartX = barStartX + this->barWidth * j;
            float nextBeatStartX = barStartX + this->barWidth * (j + beatStep);
            
            // Check for time signature change at this point
            if (nextSignature != nullptr)
            {
                const float tsBar = nextSignature->getBeat() / NUM_BEATS_IN_BAR;
                if (tsBar <= (i + j + beatStep))
                {
                    numerator = nextSignature->getNumerator();
                    denominator = nextSignature->getDenominator();
                    barStep = (tsBar - i); // i.e. incomplete bar
                    nextBeatStartX = barStartX + this->barWidth * barStep;
                    nextSignatureIdx++;
                    barWidthSum = 0;
                    lastFrame = true;
                }
            }

            // Get snap lines and beat lines
            for (float k = beatStartX + snapWidth;
                 k < (nextBeatStartX - 1);
                 k += snapWidth)
            {
				if (k >= viewPosX)
				{
					visibleSnaps.add(k);
				}
            }
            
            if (beatStartX >= viewPosX &&
				j >= beatStep && // Don't draw the first one as it is a barline
                (nextBeatStartX - beatStartX) > MIN_BEAT_WIDTH)
            {
                visibleBeats.add(beatStartX);
            }
        }
        
        i += barStep;
    }
}


//===----------------------------------------------------------------------===//
// Alternative keydown modes (space for drag, etc.)
//===----------------------------------------------------------------------===//

void MidiRoll::setSpaceDraggingMode(bool dragMode)
{
    if (this->spaceDragMode == dragMode)
    {
        return;
    }

    this->spaceDragMode = dragMode;
    this->resetDraggingAnchors();
    this->draggedDistance = 0;
    this->timeEnteredDragMode = Time::getCurrentTime();

    if (dragMode)
    {
        this->project.getEditMode().setMode(MidiRollEditMode::dragMode, true);
    }
    else
    {
        this->project.getEditMode().unsetLastMode();
    }
}

bool MidiRoll::isUsingSpaceDraggingMode() const
{
    return this->spaceDragMode;
}

void MidiRoll::setAltDrawingMode(bool drawMode)
{
    if (this->altDrawMode == drawMode)
    {
        return;
    }

    this->altDrawMode = drawMode;

    if (drawMode)
    {
        this->project.getEditMode().setMode(MidiRollEditMode::drawMode, true);
    }
    else
    {
        this->project.getEditMode().unsetLastMode();
    }
}

bool MidiRoll::isUsingAltDrawingMode() const
{
    return this->altDrawMode;
}

bool MidiRoll::isUsingAnyAltMode() const
{
    return this->isUsingAltDrawingMode() || this->isUsingSpaceDraggingMode();
}


//===----------------------------------------------------------------------===//
// LassoSource
//===----------------------------------------------------------------------===//

MidiEventSelection &MidiRoll::getLassoSelection()
{
    return this->selection;
}

void MidiRoll::selectEventsInRange(float startBeat, float endBeat, bool shouldClearAllOthers)
{
    if (shouldClearAllOthers)
    {
        this->selection.deselectAll();
    }

    for (int i = 0; i < this->eventComponents.size(); ++i)
    {
        MidiEventComponent *ec = this->eventComponents.getUnchecked(i);
        if (ec->isActive() &&
            ec->getBeat() >= startBeat &&
            ec->getBeat() < endBeat)
        {
            this->selection.addToSelection(ec);
            //this->selection.addToSelectionBasedOnModifiers(ec, Desktop::getInstance().getMainMouseSource().getCurrentModifiers());
        }
    }

}

void MidiRoll::selectEvent(MidiEventComponent *event, bool shouldClearAllOthers)
{
    if (shouldClearAllOthers)
    {
        this->selection.deselectAll();
    }

    if (event)
    {
        this->selection.addToSelection(event);
    }
}

void MidiRoll::deselectEvent(MidiEventComponent *event)
{
    this->selection.deselect(event);
}

void MidiRoll::deselectAll()
{
    this->selection.deselectAll();
}

void MidiRoll::selectAll()
{
    for (int i = 0; i < this->eventComponents.size(); ++i)
    {
        MidiEventComponent *child = this->eventComponents.getUnchecked(i);

        if (child->belongsToLayerSet(this->activeLayers))
        {
            this->selection.addToSelection(child);
        }
    }
}

MidiEventComponentLasso *MidiRoll::getLasso() const
{
    return this->lassoComponent;
}

//===----------------------------------------------------------------------===//
// ProjectListener
//===----------------------------------------------------------------------===//

void MidiRoll::onEventChanged(const MidiEvent &oldEvent, const MidiEvent &newEvent)
{
    // Time signatures have changed, need to repaint
    if (dynamic_cast<const TimeSignatureEvent *>(&oldEvent))
    {
        this->updateChildrenBounds();
		this->repaint();
    }
}

void MidiRoll::onEventAdded(const MidiEvent &event)
{
    if (dynamic_cast<const TimeSignatureEvent *>(&event))
    {
        this->updateChildrenBounds();
		this->repaint();
	}
}

void MidiRoll::onEventRemoved(const MidiEvent &event)
{
    if (dynamic_cast<const TimeSignatureEvent *>(&event))
    {
        this->updateChildrenBounds();
		this->repaint();
	}
}

void MidiRoll::onProjectBeatRangeChanged(float firstBeat, float lastBeat)
{
    //Logger::writeToLog("MidiRoll::onProjectBeatRangeChanged " + String(firstBeat) + " " + String(lastBeat));
    this->trackFirstBeat = firstBeat;
    this->trackLastBeat = lastBeat;

    const int trackFirstBar = int(floorf(firstBeat / float(NUM_BEATS_IN_BAR)));
    const int trackLastBar = int(ceilf(lastBeat / float(NUM_BEATS_IN_BAR)));
    const int rollFirstBar = jmin(this->firstBar, trackFirstBar);
    const int rollLastBar = jmax(this->lastBar, trackLastBar);

    this->setBarRange(rollFirstBar, rollLastBar);
}

//===----------------------------------------------------------------------===//
// Component
//===----------------------------------------------------------------------===//

void MidiRoll::focusGained(FocusChangeType cause)
{
    // juce hack
    if (Origami *parentOrigami = this->findParentComponentOfClass<Origami>())
    {
        parentOrigami->focusOfChildComponentChanged(cause);
    }

    this->header->setActive(true);
}

void MidiRoll::focusLost(FocusChangeType cause)
{
    this->header->setActive(false);
}

bool MidiRoll::keyPressed(const KeyPress &key)
{
    //Logger::writeToLog("MidiRoll::keyPressed " + key.getTextDescription());

    if (key == KeyPress::createFromDescription("command + a"))
    {
        this->selectAll();
        return true;
    }
//    else if (key == KeyPress::createFromDescription("/"))
//    {
//        Logger::writeToLog(String(MidiRollToolbox::findStartBeat(this->getLassoSelection())));
//        Logger::writeToLog(String(MidiRollToolbox::findEndBeat(this->getLassoSelection())));
//    }
    else if (key == KeyPress::createFromDescription("command + z") ||
             key == KeyPress::createFromDescription("ctrl + z"))
    {
        if (this->primaryActiveLayer)
        {
            MIDI_ROLL_BULK_REPAINT_START
            this->primaryActiveLayer->undo();
            MIDI_ROLL_BULK_REPAINT_END
            return true;
        }
    }
    else if (key == KeyPress::createFromDescription("command + y") ||
             key == KeyPress::createFromDescription("ctrl + y") ||
             key == KeyPress::createFromDescription("command + shift + z") ||
             key == KeyPress::createFromDescription("ctrl + shift + z"))
    {
        if (this->primaryActiveLayer)
        {
            MIDI_ROLL_BULK_REPAINT_START
            this->primaryActiveLayer->redo();
            MIDI_ROLL_BULK_REPAINT_END
            return true;
        }
    }
    else if ((key == KeyPress::createFromDescription("command + c")) ||
             (key == KeyPress::createFromDescription("command + insert")) ||
             (key == KeyPress::createFromDescription("ctrl + c")) ||
             (key == KeyPress::createFromDescription("ctrl + insert")))
    {

        InternalClipboard::copy(*this, false);
        return true;
    }
    else if ((key == KeyPress::createFromDescription("command + shift + c")) ||
             (key == KeyPress::createFromDescription("ctrl + shift + c")))
    {
        InternalClipboard::copy(*this, true);
        return true;
    }
    else if ((key == KeyPress::createFromDescription("command + shift + x")) ||
             (key == KeyPress::createFromDescription("ctrl + shift + x")))
    {
        if (this->selection.getNumSelected() > 0)
        {
            InternalClipboard::copy(*this, false);
            this->primaryActiveLayer->checkpoint();
            const float leftBeat = MidiRollToolbox::findStartBeat(this->selection);
            const float rightBeat = MidiRollToolbox::findEndBeat(this->selection);
            MidiRollToolbox::wipeSpace(this->project.getLayersList(), leftBeat, rightBeat, true, false);
            MidiRollToolbox::shiftEventsToTheRight(this->project.getLayersList(), leftBeat, -(rightBeat - leftBeat), false);
            return true;
        }
    }
    else if (key == KeyPress::createFromDescription("cursor left"))
    {
        MidiRollToolbox::shiftBeatRelative(this->getLassoSelection(), -0.25f);
        return true;
    }
    else if (key == KeyPress::createFromDescription("cursor right"))
    {
        MidiRollToolbox::shiftBeatRelative(this->getLassoSelection(), 0.25f);
        return true;
    }
    else if (key == KeyPress::createFromDescription("shift + cursor left"))
    {
        MidiRollToolbox::shiftBeatRelative(this->getLassoSelection(), -0.25f * NUM_BEATS_IN_BAR);
        return true;
    }
    else if (key == KeyPress::createFromDescription("shift + cursor right"))
    {
        MidiRollToolbox::shiftBeatRelative(this->getLassoSelection(), 0.25f * NUM_BEATS_IN_BAR);
        return true;
    }
    else if (key.isKeyCode(KeyPress::spaceKey))
    {
        return true;
    }
    else if (key == KeyPress::createFromDescription("escape"))
    {
        if (this->project.getTransport().isPlaying())
        {
            this->project.getTransport().stopPlayback();
        }
        else
        {
            this->resetAllClippingIndicators();
            this->resetAllOversaturationIndicators();
        }

        App::Workspace().getAudioCore().mute();
        App::Workspace().getAudioCore().unmute();

        return true;
    }
    else if (key == KeyPress::createFromDescription("1"))
    {
        this->project.getEditMode().setMode(MidiRollEditMode::defaultMode);
        return true;
    }
    else if (key == KeyPress::createFromDescription("2"))
    {
        this->project.getEditMode().setMode(MidiRollEditMode::drawMode);
        return true;
    }
    else if (key == KeyPress::createFromDescription("3"))
    {
        this->project.getEditMode().setMode(MidiRollEditMode::selectionMode);
        return true;
    }
//    else if (key == KeyPress::createFromDescription("4"))
//    {
//        this->project.getEditMode().setMode(MidiRollEditMode::zoomMode);
//        return true;
//    }
    else if (key == KeyPress::createFromDescription("4"))
    {
        this->project.getEditMode().setMode(MidiRollEditMode::dragMode);
        return true;
    }
    else if (key == KeyPress::createFromDescription("5"))
    {
        this->project.getEditMode().setMode(MidiRollEditMode::wipeSpaceMode);
        return true;
    }
    else if (key == KeyPress::createFromDescription("6"))
    {
        this->project.getEditMode().setMode(MidiRollEditMode::insertSpaceMode);
        return true;
    }
    else if (key == KeyPress::createFromDescription("z"))
    {
        this->zoomInImpulse();
        return true;
    }
    else if (key == KeyPress::createFromDescription("shift + z"))
    {
        this->zoomOutImpulse();
        return true;
    }
    else if (key == KeyPress::createFromDescription("command + s") ||
             key == KeyPress::createFromDescription("ctrl + s"))
    {
        this->project.getDocument()->forceSave();
        return true;
    }
    else if (key == KeyPress::createFromDescription("shift + Tab"))
    {
        if (VersionControlTreeItem *vcsTreeItem = this->project.findChildOfType<VersionControlTreeItem>())
        {
            vcsTreeItem->toggleQuickStash();
            return true;
        }
    }
    else if (key == KeyPress::returnKey)
    {
        if (this->project.getTransport().isPlaying())
        {
            //this->project.getTransport().stopPlayback();
            this->startFollowingIndicator();
        }
        else
        {
            this->project.getTransport().startPlayback();
            this->startFollowingIndicator();
        }

        return true;
    }

    return false;
}

bool MidiRoll::keyStateChanged(bool isKeyDown)
{
    if (isKeyDown && KeyPress::isKeyCurrentlyDown(KeyPress::spaceKey))
    {
        this->header->setSoundProbeMode(true);

        // без этой проверки при отжатии space - режим сменится на предпредыдущий, а не должен
        // а с ней - не включается плэйбек на space
        //const bool alreadyHasDragMode = this->project.getEditMode().isMode(MidiRollEditMode::dragMode);
        //if (! alreadyHasDragMode)
        {
            this->setSpaceDraggingMode(true);
            return true;
        }
    }
    else if (!isKeyDown && !KeyPress::isKeyCurrentlyDown(KeyPress::spaceKey))
    {
        this->header->setSoundProbeMode(false);

        if (this->isUsingSpaceDraggingMode())
        {
            const Time lastMouseDownTime = Desktop::getInstance().getMainMouseSource().getLastMouseDownTime();
            const bool noClicksWasDone = (lastMouseDownTime < this->timeEnteredDragMode);
            const bool noDraggingWasDone = (this->draggedDistance < 5);
            const bool notTooMuchTimeSpent = (Time::getCurrentTime() - this->timeEnteredDragMode).inMilliseconds() < 500;

            if (noDraggingWasDone && noClicksWasDone && notTooMuchTimeSpent)
            {
                //Logger::writeToLog("toggling playback");

                if (this->project.getTransport().isPlaying())
                {
                    this->project.getTransport().stopPlayback();
                }
                else
                {
                    this->project.getTransport().startPlayback();
                }
            }

            this->setSpaceDraggingMode(false);
            return true;
        }
    }

    return false;
}

void MidiRoll::modifierKeysChanged(const ModifierKeys &modifiers)
{
    const bool altDrawingMode = modifiers.isCommandDown(); // || modifiers.isAltDown();

    if (altDrawingMode)
    {
        //Logger::writeToLog("setAltDrawingMode(true)");
        this->setAltDrawingMode(true);
    }
    else if (! altDrawingMode)
    {
        if (this->isUsingAltDrawingMode())
        {
            //Logger::writeToLog("setAltDrawingMode(false)");
            this->setAltDrawingMode(false);
        }
    }
}


void MidiRoll::mouseMove(const MouseEvent &e)
{
    this->updateWipeSpaceHelperIfNeeded(e);
    this->updateInsertSpaceHelperIfNeeded(e);
}

void MidiRoll::mouseDown(const MouseEvent &e)
{
    if (this->multiTouchController->hasMultitouch() || (e.source.getIndex() > 0))
    {
        this->lassoComponent->endLasso();
        return;
    }

    //Logger::writeToLog("MidiRoll::mouseDown");

    if (this->isLassoEvent(e))
    {
        this->lassoComponent->beginLasso(e, this);
    }
    else if (this->isViewportDragEvent(e))
    {
        this->setMouseCursor(MouseCursor::DraggingHandCursor);
        this->resetDraggingAnchors();
    }
    else if (this->isViewportZoomEvent(e))
    {
        this->startZooming();
    }
    else if (this->isWipeSpaceEvent(e))
    {
        this->startWipingSpace(e);
    }
    else if (this->isInsertSpaceEvent(e))
    {
        this->startInsertingSpace(e);
    }
}

void MidiRoll::mouseDrag(const MouseEvent &e)
{
    if (this->multiTouchController->hasMultitouch() || (e.source.getIndex() > 0))
    {
        return;
    }

    //Logger::writeToLog("MidiRoll::mouseDrag");

    if (this->lassoComponent->isDragging())
    {
        this->lassoComponent->dragLasso(e); // if any. will do the check itself
    }
    else if (this->isViewportDragEvent(e))
    {
        this->continueDragging(e);
    }
    else if (this->isViewportZoomEvent(e))
    {
        this->continueZooming(e);
    }
    else if (this->isWipeSpaceEvent(e))
    {
        this->continueWipingSpace(e);
    }
    else if (this->isInsertSpaceEvent(e))
    {
        this->continueInsertingSpace(e);
    }
}

void MidiRoll::mouseUp(const MouseEvent &e)
{
    if (const bool hasMultitouch = (e.source.getIndex() > 0))
    {
        return;
    }

    this->grabKeyboardFocus();
    this->setMouseCursor(this->project.getEditMode().getCursor());

    if (this->isViewportZoomEvent(e))
    {
        this->endZooming();
    }

    //Logger::writeToLog("MidiRoll::mouseUp");

    if (this->lassoComponent->isDragging())
    {
        this->lassoComponent->endLasso();
    }

    this->endWipingSpaceIfNeeded();
    this->endInsertingSpaceIfNeeded();

#if HELIO_DESKTOP
#   define MIN_PAN_DISTANCE 10
#elif HELIO_MOBILE
#   define MIN_PAN_DISTANCE 20
#endif

    if (e.mods.isLeftButtonDown() && (e.getDistanceFromDragStart() < MIN_PAN_DISTANCE))
    {
        this->deselectAll();
    }
}

void MidiRoll::mouseWheelMove(const MouseEvent &event, const MouseWheelDetails &wheel)
{
    const float &inititalSpeed = this->smoothZoomController->getInitialZoomSpeed();
    const float forwardWheel = wheel.deltaY * (wheel.isReversed ? -inititalSpeed : inititalSpeed);
    const float beatWidth = (this->barWidth / NUM_BEATS_IN_BAR);
    const Point<float> mouseOffset = (event.position - this->viewport.getViewPosition().toFloat());
    if (event.mods.isAnyModifierKeyDown())
    {
        this->startSmoothZoom(mouseOffset, Point<float>(0.f, forwardWheel));
    }
    else
    {
        this->startSmoothZoom(mouseOffset, Point<float>(forwardWheel, 0.f));
    }
}


void MidiRoll::moved()
{
    //this->sendChangeMessage();
}

void MidiRoll::resized()
{
    this->updateChildrenBounds();
    //this->sendChangeMessage();
}

void MidiRoll::paint(Graphics &g)
{
    const Colour barLine = findColour(MidiRoll::barLineColourId);
    const Colour barLineBevel = findColour(MidiRoll::barLineBevelColourId);
    const Colour beatLine = findColour(MidiRoll::beatLineColourId);
    const Colour snapLine = findColour(MidiRoll::snapLineColourId);
    
    this->computeVisibleBeatLines();

    const float paintStartY = float(this->viewport.getViewPositionY());
    const float paintEndY = paintStartY + this->viewport.getViewHeight();

    g.setColour(barLine);
    for (const auto f : this->visibleBars)
    {
        g.drawVerticalLine(int(f), paintStartY, paintEndY);
    }

    g.setColour(barLineBevel);
    for (const auto f : this->visibleBars)
    {
        g.drawVerticalLine(int(f + 1), paintStartY, paintEndY);
    }

    g.setColour(beatLine);
    for (const auto f : this->visibleBeats)
    {
        g.drawVerticalLine(int(f), paintStartY, paintEndY);
    }
    
    g.setColour(snapLine);
    for (const auto f : this->visibleSnaps)
    {
        g.drawVerticalLine(int(f), paintStartY, paintEndY);
    }
}

//===----------------------------------------------------------------------===//
// TransportIndicator::MovementListener
//===----------------------------------------------------------------------===//

void MidiRoll::onTransportIndicatorMoved(int indicatorX)
{
    if (this->shouldFollowIndicator &&
        !this->smoothZoomController->isZooming())
    {
        if (fabs(this->transportIndicatorOffset) > 1.0)
        {
            const double newIndicatorDelta = fabs(this->transportIndicatorOffset - (this->transportIndicatorOffset * 0.975));
            this->transportIndicatorOffset += (jmax(1.0, newIndicatorDelta) * ((this->transportIndicatorOffset < 0.0) ? 1.0 : -1.0));
        }
        else
        {
            this->transportIndicatorOffset = 0.0;
        }
        
        this->viewport.setViewPosition(indicatorX - int(this->transportIndicatorOffset) - (this->viewport.getViewWidth() / 2),
                                       this->viewport.getViewPositionY());
        
        this->updateChildrenPositions();
    }
}

//===----------------------------------------------------------------------===//
// VolumeCallback::ClippingListener
//===----------------------------------------------------------------------===//

void MidiRoll::onClippingWarning()
{
    if (! this->project.getTransport().isPlaying())
    {
        return;
    }
    
    float clippingBeat = 0.f;
    
    {
        ScopedReadLock lock(this->transportLastCorrectPositionLock);
        clippingBeat = this->getBeatByTransportPosition(this->transportLastCorrectPosition);
    }
    
    if (this->clippingIndicators.size() > 0)
    {
        const float lastMarkerEndBeat = this->clippingIndicators.getLast()->getEndBeat();
        const bool justNeedToUpdateLastMarker = ((clippingBeat - lastMarkerEndBeat) < CLIPPING_MARKER_MAX_GAP_IN_BEATS);
        
        if (justNeedToUpdateLastMarker)
        {
            this->clippingIndicators.getLast()->setEndBeat(clippingBeat);
            return;
        }
    }
    
    auto newMarker = new TimelineWarningMarker(TimelineWarningMarker::Red, *this, clippingBeat);
    this->clippingIndicators.add(newMarker);
    this->addAndMakeVisible(newMarker);
}

void MidiRoll::resetAllClippingIndicators()
{
    this->clippingIndicators.clear();
}

void MidiRoll::onOversaturationWarning()
{
    if (! this->project.getTransport().isPlaying())
    {
        return;
    }
    
    float warningBeat = 0.f;
    
    {
        ScopedReadLock lock(this->transportLastCorrectPositionLock);
        warningBeat = this->getBeatByTransportPosition(this->transportLastCorrectPosition);
    }
    
    if (this->oversaturationIndicators.size() > 0)
    {
        const float lastMarkerEndBeat = this->oversaturationIndicators.getLast()->getEndBeat();
        const bool justNeedToUpdateLastMarker = ((warningBeat - lastMarkerEndBeat) < CLIPPING_MARKER_MAX_GAP_IN_BEATS);
        
        if (justNeedToUpdateLastMarker)
        {
            this->oversaturationIndicators.getLast()->setEndBeat(warningBeat);
            return;
        }
    }
    
    auto newMarker = new TimelineWarningMarker(TimelineWarningMarker::Yellow, *this, warningBeat);
    this->oversaturationIndicators.add(newMarker);
    this->addAndMakeVisible(newMarker);
}

void MidiRoll::resetAllOversaturationIndicators()
{
    this->oversaturationIndicators.clear();
}

//===----------------------------------------------------------------------===//
// TransportListener
//===----------------------------------------------------------------------===//

void MidiRoll::onSeek(const double newPosition,
                      const double currentTimeMs,
                      const double totalTimeMs)
{
    {
        ScopedWriteLock lock(this->transportLastCorrectPositionLock);
        this->transportLastCorrectPosition = newPosition;
    }

#if MIDIROLL_FOLLOWS_INDICATOR
//    if (this->shouldFollowIndicator)
//    {
//        Logger::writeToLog("MidiRoll shouldFollowIndicator");
//        this->triggerAsyncUpdate();
//    }
#endif
}

void MidiRoll::onTempoChanged(const double newTempo)
{
}

void MidiRoll::onTotalTimeChanged(const double timeMs)
{
}

void MidiRoll::onPlay()
{
    this->resetAllClippingIndicators();
    this->resetAllOversaturationIndicators();
    
#if MIDIROLL_FOLLOWS_INDICATOR
//    const bool indicatorIsWithinScreen = fabs(this->findIndicatorOffsetFromViewCentre()) < (this->viewport.getViewWidth() / 2);
//    if (indicatorIsWithinScreen)
//    {
//        this->startFollowingIndicator();
//    }
#endif
}

void MidiRoll::onStop()
{
#if MIDIROLL_FOLLOWS_INDICATOR
    // todo sync screen back to indicator?
    this->stopFollowingIndicator();
    //this->scrollToSeekPosition();
#endif
}

void MidiRoll::startFollowingIndicator()
{
#if MIDIROLL_FOLLOWS_INDICATOR
    this->transportIndicatorOffset = this->findIndicatorOffsetFromViewCentre();
    this->shouldFollowIndicator = true;
    this->triggerAsyncUpdate();
#endif
}

void MidiRoll::stopFollowingIndicator()
{
#if MIDIROLL_FOLLOWS_INDICATOR
    this->stopTimer();
    // this introduces the case when I change a note during playback, and the note component position is not updated
    //this->cancelPendingUpdate();
    this->shouldFollowIndicator = false;
#endif
}

void MidiRoll::scrollToSeekPosition()
{
#if MIDIROLL_FOLLOWS_INDICATOR
    this->startFollowingIndicator();
    this->startTimer(7);
#else
    int indicatorX = 0;

    {
        ScopedReadLock lock(this->transportLastCorrectPositionLock);
        indicatorX = this->getXPositionByTransportPosition(this->transportLastCorrectPosition, float(this->getWidth()));
    }

    this->viewport.setViewPosition(indicatorX - (this->viewport.getViewWidth() / 3), this->viewport.getViewPositionY());
    this->updateChildrenBounds();
#endif
}


//===----------------------------------------------------------------------===//
// AsyncUpdater
//===----------------------------------------------------------------------===//

void MidiRoll::handleAsyncUpdate()
{
    // batch repaint & resize stuff
    if (this->batchRepaintList.size() > 0)
    {
        MIDI_ROLL_BULK_REPAINT_START

        for (int i = 0; i < this->batchRepaintList.size(); ++i)
        {
            if (MidiEventComponent *mc = this->batchRepaintList.getUnchecked(i))
            {
                const Rectangle<float> nb(this->getEventBounds(mc));
                mc->updateBounds(nb);
                mc->repaint();
            }
        }

        MIDI_ROLL_BULK_REPAINT_END

        this->batchRepaintList.clear();
    }

#if MIDIROLL_FOLLOWS_INDICATOR
    if (this->shouldFollowIndicator &&
        !this->smoothZoomController->isZooming())
    {
        int indicatorX = 0;

        {
            ScopedReadLock lock(this->transportLastCorrectPositionLock);
            indicatorX = this->getXPositionByTransportPosition(this->transportLastCorrectPosition, float(this->getWidth()));
        }

        if (fabs(this->transportIndicatorOffset) > 1.0)
        {
            const double newIndicatorDelta = fabs(this->transportIndicatorOffset - (this->transportIndicatorOffset * 0.975));
            this->transportIndicatorOffset += (jmax(1.0, newIndicatorDelta) * ((this->transportIndicatorOffset < 0.0) ? 1.0 : -1.0));
        }
        else
        {
            this->transportIndicatorOffset = 0.0;
        }

        this->viewport.setViewPosition(indicatorX - int(this->transportIndicatorOffset) - (this->viewport.getViewWidth() / 2),
                                       this->viewport.getViewPositionY());

        this->updateChildrenPositions();
    }
#endif
}

void MidiRoll::handleCommandMessage(int commandId)
{
	if (commandId == CommandIDs::AddAnnotation)
	{
		const float targetBeat = this->getPositionForNewTimelineEvent();
		if (AnnotationsLayer *annotationsLayer =
			dynamic_cast<AnnotationsLayer *>(this->project.getTimeline()->getAnnotations()))
		{
			Component *dialog =
				AnnotationDialog::createAddingDialog(*this, annotationsLayer, targetBeat);
			App::Layout().showModalNonOwnedDialog(dialog);
		}
	}
	else if (commandId == CommandIDs::AddTimeSignature)
	{
		const float targetBeat = this->getPositionForNewTimelineEvent();
		if (TimeSignaturesLayer *signaturesLayer =
			dynamic_cast<TimeSignaturesLayer *>(this->project.getTimeline()->getTimeSignatures()))
		{
			Component *dialog =
				TimeSignatureDialog::createAddingDialog(*this, signaturesLayer, targetBeat);
			App::Layout().showModalNonOwnedDialog(dialog);
		}
	}
}

double MidiRoll::findIndicatorOffsetFromViewCentre() const
{
    int indicatorX = 0;

    {
        ScopedReadLock lock(this->transportLastCorrectPositionLock);
        indicatorX = this->getXPositionByTransportPosition(this->transportLastCorrectPosition, float(this->getWidth()));
    }

    const int &viewportCentreX = (this->viewport.getViewPositionX() + this->viewport.getViewWidth() / 2);
    return indicatorX - viewportCentreX;
}

void MidiRoll::triggerBatchRepaintFor(MidiEventComponent *target)
{
    this->batchRepaintList.add(target);
    this->triggerAsyncUpdate();
}


//===----------------------------------------------------------------------===//
// Timer
//===----------------------------------------------------------------------===//

void MidiRoll::hiResTimerCallback()
{
    if (fabs(this->transportIndicatorOffset) < 0.01)
    {
        MessageManagerLock lock(Thread::getCurrentThread());

        if (lock.lockWasGained())
        {
            this->stopFollowingIndicator();
        }
    }

    this->triggerAsyncUpdate();
}


//===----------------------------------------------------------------------===//
// Events check
//===----------------------------------------------------------------------===//

bool MidiRoll::isViewportZoomEvent(const MouseEvent &e) const
{
    if (this->project.getEditMode().forbidsViewportZooming())
    {
        return false;
    }

    if (this->project.getEditMode().forcesViewportZooming())
    {
        return true;
    }

    // may add custom logic here
    return false;
}

bool MidiRoll::isViewportDragEvent(const MouseEvent &e) const
{
    if (this->project.getEditMode().forbidsViewportDragging())
    {
        return false;
    }

    if (this->project.getEditMode().forcesViewportDragging())
    {
        return true;
    }

    if (e.source.isTouch())
    {
        return (e.mods.isLeftButtonDown());
    }
    
    return (e.mods.isRightButtonDown() || e.mods.isMiddleButtonDown());
}

bool MidiRoll::isAddEvent(const MouseEvent &e) const
{
    if (this->project.getEditMode().forbidsAddingEvents())
    {
        return false;
    }

    if (this->project.getEditMode().forcesAddingEvents())
    {
        return true;
    }

    return false; // (e.mods.isMiddleButtonDown());
}

bool MidiRoll::isLassoEvent(const MouseEvent &e) const
{
    if (this->project.getEditMode().forbidsSelectionMode())
    {
        return false;
    }

    if (this->project.getEditMode().forcesSelectionMode())
    {
        return true;
    }

    if (e.source.isTouch())
    {
        return false;
    }
    
    return e.mods.isLeftButtonDown();
}

bool MidiRoll::isWipeSpaceEvent(const MouseEvent &e) const
{
    if (this->project.getEditMode().forbidsSpaceWipe())
    {
        return false;
    }

    if (this->project.getEditMode().forcesSpaceWipe())
    {
        return true;
    }

    // may add custom logic for default mode
    return false;
}

bool MidiRoll::isInsertSpaceEvent(const MouseEvent &e) const
{
    if (this->project.getEditMode().forbidsSpaceInsert())
    {
        return false;
    }

    if (this->project.getEditMode().forcesSpaceInsert())
    {
        return true;
    }

    // may add custom logic for default mode
    return false;
}


void MidiRoll::resetDraggingAnchors()
{
    this->viewportAnchor = this->viewport.getViewPosition();
    this->clickAnchor = Desktop::getInstance().getMainMouseSource().getScreenPosition();
}

void MidiRoll::continueDragging(const MouseEvent &e)
{
    this->draggedDistance = e.getDistanceFromDragStart();
    this->smoothZoomController->cancelZoom();
    this->smoothPanController->panByOffset(this->getMouseOffset(e.source.getScreenPosition()).toInt());
}


//===----------------------------------------------------------------------===//
// Wipe space tool
//===----------------------------------------------------------------------===//

void MidiRoll::initWipeSpaceHelper(int xPosition)
{
    if (this->wipeSpaceHelper == nullptr)
    {
        this->wipeSpaceHelper = new WipeSpaceHelper(*this);
        this->addAndMakeVisible(this->wipeSpaceHelper);
    }

    const float startBeat = this->getRoundBeatByXPosition(xPosition);
    this->wipeSpaceHelper->setStartBeat(startBeat);
    this->wipeSpaceHelper->setEndBeat(startBeat);
}

void MidiRoll::updateWipeSpaceHelperIfNeeded(const MouseEvent &e)
{
    if (this->wipeSpaceHelper != nullptr)
    {
        const float endBeat = this->getRoundBeatByXPosition(e.x);
        this->wipeSpaceHelper->setEndBeat(endBeat);
        this->wipeSpaceHelper->snapWidth();
    }
}

void MidiRoll::removeWipeSpaceHelper()
{
    this->wipeSpaceHelper = nullptr;
}

void MidiRoll::startWipingSpace(const MouseEvent &e)
{
    this->initWipeSpaceHelper(e.x);
}

void MidiRoll::continueWipingSpace(const MouseEvent &e)
{
    if (this->wipeSpaceHelper != nullptr)
    {
        const float endBeat = this->getRoundBeatByXPosition(e.x);
        this->wipeSpaceHelper->setEndBeat(endBeat);
    }
}

void MidiRoll::endWipingSpaceIfNeeded()
{
    if (this->wipeSpaceHelper != nullptr)
    {
        const float leftBeat = this->wipeSpaceHelper->getLeftMostBeat();
        const float rightBeat = this->wipeSpaceHelper->getRightMostBeat();

        if ((rightBeat - leftBeat) > 0.01f)
        {
            const bool isAnyModifierKeyDown = Desktop::getInstance().getMainMouseSource().getCurrentModifiers().isAnyModifierKeyDown();

            MidiRollToolbox::wipeSpace(isAnyModifierKeyDown ? this->project.getSelectedLayersList() : this->project.getLayersList(), leftBeat, rightBeat);

            if (! isAnyModifierKeyDown)
            {
                if (this->wipeSpaceHelper->isInverted())
                {
                    MidiRollToolbox::shiftEventsToTheLeft(this->project.getLayersList(), leftBeat, (rightBeat - leftBeat), false);
                }
                else
                {
                    MidiRollToolbox::shiftEventsToTheRight(this->project.getLayersList(), leftBeat, -(rightBeat - leftBeat), false);
                }
            }
        }

        this->wipeSpaceHelper->snapWidth();
    }
}


//===----------------------------------------------------------------------===//
// Insert space tool
//===----------------------------------------------------------------------===//

void MidiRoll::initInsertSpaceHelper(int xPosition)
{
    if (this->insertSpaceHelper == nullptr)
    {
        this->insertSpaceHelper = new InsertSpaceHelper(*this);
        this->addAndMakeVisible(this->insertSpaceHelper);
    }

    const float startBeat = this->getRoundBeatByXPosition(xPosition);
    this->insertSpaceHelper->setStartBeat(startBeat);
    this->insertSpaceHelper->setEndBeat(startBeat);
}

void MidiRoll::updateInsertSpaceHelperIfNeeded(const MouseEvent &e)
{
    if (this->insertSpaceHelper != nullptr)
    {
        const float endBeat = this->getRoundBeatByXPosition(e.x);
        this->insertSpaceHelper->setEndBeat(endBeat);
        this->insertSpaceHelper->snapWidth();
    }
}

void MidiRoll::removeInsertSpaceHelper()
{
    this->insertSpaceHelper = nullptr;
}

void MidiRoll::startInsertingSpace(const MouseEvent &e)
{
    this->initInsertSpaceHelper(e.x);
    this->insertSpaceHelper->resetDragDelta();
    this->insertSpaceHelper->setNeedsCheckpoint(true);
}

void MidiRoll::continueInsertingSpace(const MouseEvent &e)
{
    if (this->insertSpaceHelper != nullptr)
    {
        this->insertSpaceHelper->setEndBeat(this->getRoundBeatByXPosition(e.x));

        const float leftBeat = this->insertSpaceHelper->getLeftMostBeat();
        const float rightBeat = this->insertSpaceHelper->getRightMostBeat();
        const float changeDelta = this->insertSpaceHelper->getDragDelta();

        if (fabs(changeDelta) > 0.01f)
        {
            const bool isInverted = (e.getOffsetFromDragStart().getX() < 0);
            const bool shouldCheckpoint = this->insertSpaceHelper->shouldCheckpoint();

            if (isInverted)
            {
                MidiRollToolbox::shiftEventsToTheLeft(this->project.getLayersList(), rightBeat, changeDelta, shouldCheckpoint);
            }
            else
            {
                MidiRollToolbox::shiftEventsToTheRight(this->project.getLayersList(), leftBeat, changeDelta, shouldCheckpoint);
            }

            this->insertSpaceHelper->resetDragDelta();
            this->insertSpaceHelper->setNeedsCheckpoint(false);
        }
    }
}

void MidiRoll::endInsertingSpaceIfNeeded()
{
    if (this->insertSpaceHelper != nullptr)
    {
        this->insertSpaceHelper->snapWidth();
    }
}


//===----------------------------------------------------------------------===//
// Zooming
//===----------------------------------------------------------------------===//

void MidiRoll::startZooming()
{
    this->smoothPanController->cancelPan();
    this->clickAnchor = Desktop::getInstance().getMainMouseSource().getScreenPosition();

    this->zoomAnchor.setXY(0, 0);

    this->zoomMarker = new IconComponent(Icons::zoomIn);
    this->zoomMarker->setAlwaysOnTop(true);

    const Point<int> vScreenPosition(this->viewport.getScreenPosition());
    const Point<int> sMouseDownPosition(this->clickAnchor.toInt());
    const Point<int> vMouseDownPosition(sMouseDownPosition - vScreenPosition);
    this->zoomMarker->setSize(24, 24);
    this->zoomMarker->setCentrePosition(vMouseDownPosition.getX(), vMouseDownPosition.getY());
    this->viewport.addAndMakeVisible(this->zoomMarker);

    Desktop::getInstance().getMainMouseSource().enableUnboundedMouseMovement(true, false);
}

void MidiRoll::continueZooming(const MouseEvent &e)
{
    Desktop::getInstance().getMainMouseSource().setScreenPosition(e.getMouseDownScreenPosition().toFloat());

    const Point<float> clickOffset = e.position - this->viewport.getViewPosition().toFloat();
    const float dragOffsetX = float(e.getPosition().getX() - e.getMouseDownPosition().getX());
    const float dragOffsetY = -float(e.getPosition().getY() - e.getMouseDownPosition().getY());
    const Point<float> zoomOffset = (Point<float>(dragOffsetX, dragOffsetY) - this->zoomAnchor).toFloat() * 0.005f;

    this->smoothZoomController->zoomRelative(clickOffset, zoomOffset);
    //    this->zoomAnchor.setXY(dragOffsetX, dragOffsetY);

    //    const Point<int> markerCentre(this->zoomMarker->getBounds().getCentre());
    //    this->zoomMarker->setSize(jmax<int>(32, abs(dragOffsetX * 2)), jmax<int>(32, abs(dragOffsetY * 2)));
    //    this->zoomMarker->setCentrePosition(markerCentre.getX(), markerCentre.getY());
}

void MidiRoll::endZooming()
{
    Desktop::getInstance().getMainMouseSource().enableUnboundedMouseMovement(false, false);

    this->zoomAnchor.setXY(0, 0);
    this->zoomMarker = nullptr;
}


Point<float> MidiRoll::getMouseOffset(Point<float> mouseScreenPosition) const
{
    const int w = this->getWidth() - this->viewport.getWidth();

    const Point<float> distanceFromDragStart = mouseScreenPosition - this->clickAnchor;
    float x = this->viewportAnchor.getX() + distanceFromDragStart.getX() * -(SMOOTH_PAN_SPEED_MULTIPLIER);

    x = (x < 0) ? 0 : x;
    x = (x > w) ? w : x;

    const int h = this->getHeight() - this->viewport.getHeight();
    float y = this->viewportAnchor.getY() + distanceFromDragStart.getY() * -(SMOOTH_PAN_SPEED_MULTIPLIER);

    y = (y < 0) ? 0 : y;
    y = (y > h) ? h : y;

    return Point<float>(x, y);
}

void MidiRoll::updateBounds()
{
    const int &newWidth = int(this->getNumBars() * this->barWidth);
    if (this->getWidth() == newWidth)
	{ return; }

    this->setSize(newWidth, this->getHeight());
}


static const int shadowSize = 15;

void MidiRoll::updateChildrenBounds()
{
    const int &viewHeight = this->viewport.getViewHeight();
    const int &viewWidth = this->viewport.getViewWidth();
    const int &viewX = this->viewport.getViewPositionX();
    const int &viewY = this->viewport.getViewPositionY();

    this->topShadow->setBounds(viewX, viewY + MIDIROLL_HEADER_HEIGHT, viewWidth, shadowSize);
    this->bottomShadow->setBounds(viewX, viewY + viewHeight - shadowSize, viewWidth, shadowSize);

    this->header->setBounds(0, viewY, this->getWidth(), MIDIROLL_HEADER_HEIGHT);
    this->annotationsTrack->setBounds(0, viewY + MIDIROLL_HEADER_HEIGHT, this->getWidth(), MIDIROLL_HEADER_HEIGHT);
    this->timeSignaturesTrack->setBounds(0, viewY, this->getWidth(), MIDIROLL_HEADER_HEIGHT);
    this->annotationsTrack->toFront(false);
    this->timeSignaturesTrack->toFront(false);

    if (this->wipeSpaceHelper)
    {
        this->wipeSpaceHelper->rebound();
    }

    if (this->insertSpaceHelper)
    {
        this->insertSpaceHelper->rebound();
    }

    for (int i = 0; i < this->trackMaps.size(); ++i)
    {
        Component *const trackMap = this->trackMaps.getUnchecked(i);
        trackMap->setBounds(0, viewY + viewHeight - trackMap->getHeight(),
                            this->getWidth(), trackMap->getHeight());
    }

    this->broadcastRollResized();
}

void MidiRoll::updateChildrenPositions()
{
    const int &viewWidth = this->viewport.getViewWidth();
    const int &viewHeight = this->viewport.getViewHeight();
    const int &viewX = this->viewport.getViewPositionX();
    const int &viewY = this->viewport.getViewPositionY();

    this->topShadow->setTopLeftPosition(viewX, viewY + MIDIROLL_HEADER_HEIGHT);
    this->bottomShadow->setTopLeftPosition(viewX, viewY + viewHeight - shadowSize);

    this->header->setTopLeftPosition(0, viewY);
    this->annotationsTrack->setTopLeftPosition(0, viewY + MIDIROLL_HEADER_HEIGHT);
    this->timeSignaturesTrack->setTopLeftPosition(0, viewY);
    this->annotationsTrack->toFront(false);
    this->timeSignaturesTrack->toFront(false);

    if (this->wipeSpaceHelper)
    {
        this->wipeSpaceHelper->rebound();
    }

    if (this->insertSpaceHelper)
    {
        this->insertSpaceHelper->rebound();
    }

    for (int i = 0; i < this->trackMaps.size(); ++i)
    {
        Component *const trackMap = this->trackMaps.getUnchecked(i);
        trackMap->setTopLeftPosition(0, viewY + viewHeight - trackMap->getHeight());
    }

    this->broadcastRollMoved();
}


//===----------------------------------------------------------------------===//
// ChangeListener
//===----------------------------------------------------------------------===//

void MidiRoll::changeListenerCallback(ChangeBroadcaster *source)
{
    if (this->lassoComponent->isDragging())
    {
        this->lassoComponent->endLasso();
    }

    if (this->isUsingSpaceDraggingMode() &&
        ! (this->project.getEditMode().isMode(MidiRollEditMode::dragMode)))
    {
        this->setSpaceDraggingMode(false);
    }

    if (this->isUsingAltDrawingMode() &&
        ! (this->project.getEditMode().isMode(MidiRollEditMode::drawMode)))
    {
        this->setAltDrawingMode(false);
    }

    if (this->project.getEditMode().forcesSpaceInsert())
    {
        this->initInsertSpaceHelper(0);
        this->insertSpaceHelper->fadeIn();
    }
    else
    {
        this->removeInsertSpaceHelper();
    }

    if (this->project.getEditMode().forcesSpaceWipe())
    {
        this->initWipeSpaceHelper(0);
        this->wipeSpaceHelper->fadeIn();
    }
    else
    {
        this->removeWipeSpaceHelper();
    }

    const MouseCursor cursor(this->project.getEditMode().getCursor());
    this->setMouseCursor(cursor);

    const bool interactsWithChildren = this->project.getEditMode().shouldInteractWithChildren();

    for (int i = 0; i < this->eventComponents.size(); ++i)
    {
        MidiEventComponent *child = this->eventComponents.getUnchecked(i);
        child->setInterceptsMouseClicks(interactsWithChildren, interactsWithChildren);
        child->setMouseCursor(interactsWithChildren ? MouseCursor::NormalCursor : cursor);
    }
}

