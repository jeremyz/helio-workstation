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

#pragma once

class Note;
class MidiEvent;
class MidiLayer;
class PianoLayerTreeItem;

#include "Diff.h"
#include "DiffLogic.h"

namespace VCS
{
    class PianoLayerDiffLogic : public DiffLogic
    {
    public:

        explicit PianoLayerDiffLogic(TrackedItem &targetItem);

        ~PianoLayerDiffLogic() override;

        //===------------------------------------------------------------------===//
        // DiffLogic
        //

        const String getType() const override;

        void resetStateTo(const TrackedItem &newState) override;

        Diff *createDiff(const TrackedItem &initialState) const override;

        Diff *createMergedItem(const TrackedItem &initialState) const override;

    private:

        XmlElement *mergePath(const XmlElement *state, const XmlElement *changes) const;

        XmlElement *mergeMute(const XmlElement *state, const XmlElement *changes) const;

        XmlElement *mergeColour(const XmlElement *state, const XmlElement *changes) const;

        XmlElement *mergeInstrument(const XmlElement *state, const XmlElement *changes) const;

        XmlElement *mergeNotesAdded(const XmlElement *state, const XmlElement *changes) const;

        XmlElement *mergeNotesRemoved(const XmlElement *state, const XmlElement *changes) const;

        XmlElement *mergeNotesChanged(const XmlElement *state, const XmlElement *changes) const;

    private:

        NewSerializedDelta createPathDiff(const XmlElement *state, const XmlElement *changes) const;

        NewSerializedDelta createMuteDiff(const XmlElement *state, const XmlElement *changes) const;

        NewSerializedDelta createColourDiff(const XmlElement *state, const XmlElement *changes) const;

        NewSerializedDelta createInstrumentDiff(const XmlElement *state, const XmlElement *changes) const;

        Array<NewSerializedDelta> createEventsDiffs(const XmlElement *state, const XmlElement *changes) const;

    private:

        void deserializeChanges(MidiLayer &layer,
                                const XmlElement *state,
                                const XmlElement *changes,
                                OwnedArray<Note> &stateNotes,
                                OwnedArray<Note> &changesNotes) const;

        NewSerializedDelta serializeChanges(Array<const MidiEvent *> changes,
                                            const String &description,
                                            int64 numChanges,
                                            const String &deltaType) const;

        XmlElement *serializeLayer(Array<const MidiEvent *> changes,
                                   const String &tag) const;

        bool checkIfDeltaIsNotesType(const Delta *delta) const;


    };
} // namespace VCS
