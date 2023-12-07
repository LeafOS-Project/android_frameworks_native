/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "DebugConfig.h"
#include "input/InputDevice.h"

#include "InputState.h"

#include <cinttypes>
#include "InputDispatcher.h"

namespace android::inputdispatcher {

InputState::InputState(const IdGenerator& idGenerator) : mIdGenerator(idGenerator) {}

InputState::~InputState() {}

bool InputState::isHovering(int32_t deviceId, uint32_t source, int32_t displayId) const {
    for (const MotionMemento& memento : mMotionMementos) {
        if (memento.deviceId == deviceId && memento.source == source &&
            memento.displayId == displayId && memento.hovering) {
            return true;
        }
    }
    return false;
}

bool InputState::trackKey(const KeyEntry& entry, int32_t action, int32_t flags) {
    switch (action) {
        case AKEY_EVENT_ACTION_UP: {
            if (entry.flags & AKEY_EVENT_FLAG_FALLBACK) {
                std::erase_if(mFallbackKeys,
                              [&entry](const auto& item) { return item.second == entry.keyCode; });
            }
            ssize_t index = findKeyMemento(entry);
            if (index >= 0) {
                mKeyMementos.erase(mKeyMementos.begin() + index);
                return true;
            }
            /* FIXME: We can't just drop the key up event because that prevents creating
             * popup windows that are automatically shown when a key is held and then
             * dismissed when the key is released.  The problem is that the popup will
             * not have received the original key down, so the key up will be considered
             * to be inconsistent with its observed state.  We could perhaps handle this
             * by synthesizing a key down but that will cause other problems.
             *
             * So for now, allow inconsistent key up events to be dispatched.
             *
    #if DEBUG_OUTBOUND_EVENT_DETAILS
            ALOGD("Dropping inconsistent key up event: deviceId=%d, source=%08x, "
                    "keyCode=%d, scanCode=%d",
                    entry.deviceId, entry.source, entry.keyCode, entry.scanCode);
    #endif
            return false;
            */
            return true;
        }

        case AKEY_EVENT_ACTION_DOWN: {
            ssize_t index = findKeyMemento(entry);
            if (index >= 0) {
                mKeyMementos.erase(mKeyMementos.begin() + index);
            }
            addKeyMemento(entry, flags);
            return true;
        }

        default:
            return true;
    }
}

bool InputState::trackMotion(const MotionEntry& entry, int32_t action, int32_t flags) {
    int32_t actionMasked = action & AMOTION_EVENT_ACTION_MASK;
    switch (actionMasked) {
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL: {
            ssize_t index = findMotionMemento(entry, /*hovering=*/false);
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
                return true;
            }

            return false;
        }

        case AMOTION_EVENT_ACTION_DOWN: {
            ssize_t index = findMotionMemento(entry, /*hovering=*/false);
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
            }
            addMotionMemento(entry, flags, /*hovering=*/false);
            return true;
        }

        case AMOTION_EVENT_ACTION_POINTER_UP:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
        case AMOTION_EVENT_ACTION_MOVE: {
            if (entry.source & AINPUT_SOURCE_CLASS_NAVIGATION) {
                // Trackballs can send MOVE events with a corresponding DOWN or UP. There's no need
                // to generate cancellation events for these since they're based in relative rather
                // than absolute units.
                return true;
            }

            ssize_t index = findMotionMemento(entry, /*hovering=*/false);

            if (entry.source & AINPUT_SOURCE_CLASS_JOYSTICK) {
                // Joysticks can send MOVE events without a corresponding DOWN or UP. Since all
                // joystick axes are normalized to [-1, 1] we can trust that 0 means it's neutral.
                // Any other value and we need to track the motion so we can send cancellation
                // events for anything generating fallback events (e.g. DPad keys for joystick
                // movements).
                if (index >= 0) {
                    if (entry.pointerCoords[0].isEmpty()) {
                        mMotionMementos.erase(mMotionMementos.begin() + index);
                    } else {
                        MotionMemento& memento = mMotionMementos[index];
                        memento.setPointers(entry);
                    }
                } else if (!entry.pointerCoords[0].isEmpty()) {
                    addMotionMemento(entry, flags, /*hovering=*/false);
                }

                // Joysticks and trackballs can send MOVE events without corresponding DOWN or UP.
                return true;
            }

            if (index >= 0) {
                MotionMemento& memento = mMotionMementos[index];
                if (memento.firstNewPointerIdx < 0) {
                    memento.setPointers(entry);
                    return true;
                }
            }

            return false;
        }

        case AMOTION_EVENT_ACTION_HOVER_EXIT: {
            ssize_t index = findMotionMemento(entry, /*hovering=*/true);
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
                return true;
            }

            return false;
        }

        case AMOTION_EVENT_ACTION_HOVER_ENTER:
        case AMOTION_EVENT_ACTION_HOVER_MOVE: {
            ssize_t index = findMotionMemento(entry, /*hovering=*/true);
            if (index >= 0) {
                mMotionMementos.erase(mMotionMementos.begin() + index);
            }
            addMotionMemento(entry, flags, /*hovering=*/true);
            return true;
        }

        default:
            return true;
    }
}

ssize_t InputState::findKeyMemento(const KeyEntry& entry) const {
    for (size_t i = 0; i < mKeyMementos.size(); i++) {
        const KeyMemento& memento = mKeyMementos[i];
        if (memento.deviceId == entry.deviceId && memento.source == entry.source &&
            memento.displayId == entry.displayId && memento.keyCode == entry.keyCode &&
            memento.scanCode == entry.scanCode) {
            return i;
        }
    }
    return -1;
}

ssize_t InputState::findMotionMemento(const MotionEntry& entry, bool hovering) const {
    for (size_t i = 0; i < mMotionMementos.size(); i++) {
        const MotionMemento& memento = mMotionMementos[i];
        if (memento.deviceId == entry.deviceId && memento.source == entry.source &&
            memento.displayId == entry.displayId && memento.hovering == hovering) {
            return i;
        }
    }
    return -1;
}

void InputState::addKeyMemento(const KeyEntry& entry, int32_t flags) {
    KeyMemento memento;
    memento.deviceId = entry.deviceId;
    memento.source = entry.source;
    memento.displayId = entry.displayId;
    memento.keyCode = entry.keyCode;
    memento.scanCode = entry.scanCode;
    memento.metaState = entry.metaState;
    memento.flags = flags;
    memento.downTime = entry.downTime;
    memento.policyFlags = entry.policyFlags;
    mKeyMementos.push_back(memento);
}

void InputState::addMotionMemento(const MotionEntry& entry, int32_t flags, bool hovering) {
    MotionMemento memento;
    memento.deviceId = entry.deviceId;
    memento.source = entry.source;
    memento.displayId = entry.displayId;
    memento.flags = flags;
    memento.xPrecision = entry.xPrecision;
    memento.yPrecision = entry.yPrecision;
    memento.xCursorPosition = entry.xCursorPosition;
    memento.yCursorPosition = entry.yCursorPosition;
    memento.downTime = entry.downTime;
    memento.setPointers(entry);
    memento.hovering = hovering;
    memento.policyFlags = entry.policyFlags;
    mMotionMementos.push_back(memento);
}

void InputState::MotionMemento::setPointers(const MotionEntry& entry) {
    pointerCount = 0;
    for (uint32_t i = 0; i < entry.pointerCount; i++) {
        if (MotionEvent::getActionMasked(entry.action) == AMOTION_EVENT_ACTION_POINTER_UP) {
            // In POINTER_UP events, the pointer is leaving. Since the action is not stored,
            // this departing pointer should not be recorded.
            const uint8_t actionIndex = MotionEvent::getActionIndex(entry.action);
            if (i == actionIndex) {
                continue;
            }
        }
        pointerProperties[pointerCount].copyFrom(entry.pointerProperties[i]);
        pointerCoords[pointerCount].copyFrom(entry.pointerCoords[i]);
        pointerCount++;
    }
}

void InputState::MotionMemento::mergePointerStateTo(MotionMemento& other) const {
    for (uint32_t i = 0; i < pointerCount; i++) {
        if (other.firstNewPointerIdx < 0) {
            other.firstNewPointerIdx = other.pointerCount;
        }
        other.pointerProperties[other.pointerCount].copyFrom(pointerProperties[i]);
        other.pointerCoords[other.pointerCount].copyFrom(pointerCoords[i]);
        other.pointerCount++;
    }
}

std::vector<std::unique_ptr<EventEntry>> InputState::synthesizeCancelationEvents(
        nsecs_t currentTime, const CancelationOptions& options) {
    std::vector<std::unique_ptr<EventEntry>> events;
    for (KeyMemento& memento : mKeyMementos) {
        if (shouldCancelKey(memento, options)) {
            events.push_back(
                    std::make_unique<KeyEntry>(mIdGenerator.nextId(), currentTime, memento.deviceId,
                                               memento.source, memento.displayId,
                                               memento.policyFlags, AKEY_EVENT_ACTION_UP,
                                               memento.flags | AKEY_EVENT_FLAG_CANCELED,
                                               memento.keyCode, memento.scanCode, memento.metaState,
                                               /*repeatCount=*/0, memento.downTime));
        }
    }

    for (const MotionMemento& memento : mMotionMementos) {
        if (shouldCancelMotion(memento, options)) {
            if (options.pointerIds == std::nullopt) {
                const int32_t action = memento.hovering ? AMOTION_EVENT_ACTION_HOVER_EXIT
                                                        : AMOTION_EVENT_ACTION_CANCEL;
                int32_t flags = memento.flags;
                if (action == AMOTION_EVENT_ACTION_CANCEL) {
                    flags |= AMOTION_EVENT_FLAG_CANCELED;
                }
                events.push_back(
                        std::make_unique<MotionEntry>(mIdGenerator.nextId(), currentTime,
                                                      memento.deviceId, memento.source,
                                                      memento.displayId, memento.policyFlags,
                                                      action, /*actionButton=*/0, flags, AMETA_NONE,
                                                      /*buttonState=*/0, MotionClassification::NONE,
                                                      AMOTION_EVENT_EDGE_FLAG_NONE,
                                                      memento.xPrecision, memento.yPrecision,
                                                      memento.xCursorPosition,
                                                      memento.yCursorPosition, memento.downTime,
                                                      memento.pointerCount,
                                                      memento.pointerProperties,
                                                      memento.pointerCoords));
            } else {
                std::vector<std::unique_ptr<MotionEntry>> pointerCancelEvents =
                        synthesizeCancelationEventsForPointers(memento, options.pointerIds.value(),
                                                               currentTime);
                events.insert(events.end(), std::make_move_iterator(pointerCancelEvents.begin()),
                              std::make_move_iterator(pointerCancelEvents.end()));
            }
        }
    }
    return events;
}

std::vector<std::unique_ptr<EventEntry>> InputState::synthesizePointerDownEvents(
        nsecs_t currentTime) {
    std::vector<std::unique_ptr<EventEntry>> events;
    for (MotionMemento& memento : mMotionMementos) {
        if (!(memento.source & AINPUT_SOURCE_CLASS_POINTER)) {
            continue;
        }

        if (memento.firstNewPointerIdx < 0) {
            continue;
        }

        uint32_t pointerCount = 0;
        PointerProperties pointerProperties[MAX_POINTERS];
        PointerCoords pointerCoords[MAX_POINTERS];

        // We will deliver all pointers the target already knows about
        for (uint32_t i = 0; i < static_cast<uint32_t>(memento.firstNewPointerIdx); i++) {
            pointerProperties[i].copyFrom(memento.pointerProperties[i]);
            pointerCoords[i].copyFrom(memento.pointerCoords[i]);
            pointerCount++;
        }

        // We will send explicit events for all pointers the target doesn't know about
        for (uint32_t i = static_cast<uint32_t>(memento.firstNewPointerIdx);
                i < memento.pointerCount; i++) {

            pointerProperties[i].copyFrom(memento.pointerProperties[i]);
            pointerCoords[i].copyFrom(memento.pointerCoords[i]);
            pointerCount++;

            // Down only if the first pointer, pointer down otherwise
            const int32_t action = (pointerCount <= 1)
                    ? AMOTION_EVENT_ACTION_DOWN
                    : AMOTION_EVENT_ACTION_POINTER_DOWN
                            | (i << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);

            events.push_back(
                    std::make_unique<MotionEntry>(mIdGenerator.nextId(), currentTime,
                                                  memento.deviceId, memento.source,
                                                  memento.displayId, memento.policyFlags, action,
                                                  /*actionButton=*/0, memento.flags, AMETA_NONE,
                                                  /*buttonState=*/0, MotionClassification::NONE,
                                                  AMOTION_EVENT_EDGE_FLAG_NONE, memento.xPrecision,
                                                  memento.yPrecision, memento.xCursorPosition,
                                                  memento.yCursorPosition, memento.downTime,
                                                  pointerCount, pointerProperties, pointerCoords));
        }

        memento.firstNewPointerIdx = INVALID_POINTER_INDEX;
    }

    return events;
}

std::vector<std::unique_ptr<MotionEntry>> InputState::synthesizeCancelationEventsForPointers(
        const MotionMemento& memento, std::bitset<MAX_POINTER_ID + 1> pointerIds,
        nsecs_t currentTime) {
    std::vector<std::unique_ptr<MotionEntry>> events;
    std::vector<uint32_t> canceledPointerIndices;
    std::vector<PointerProperties> pointerProperties(MAX_POINTERS);
    std::vector<PointerCoords> pointerCoords(MAX_POINTERS);
    for (uint32_t pointerIdx = 0; pointerIdx < memento.pointerCount; pointerIdx++) {
        uint32_t pointerId = uint32_t(memento.pointerProperties[pointerIdx].id);
        pointerProperties[pointerIdx].copyFrom(memento.pointerProperties[pointerIdx]);
        pointerCoords[pointerIdx].copyFrom(memento.pointerCoords[pointerIdx]);
        if (pointerIds.test(pointerId)) {
            canceledPointerIndices.push_back(pointerIdx);
        }
    }

    if (canceledPointerIndices.size() == memento.pointerCount) {
        const int32_t action =
                memento.hovering ? AMOTION_EVENT_ACTION_HOVER_EXIT : AMOTION_EVENT_ACTION_CANCEL;
        int32_t flags = memento.flags;
        if (action == AMOTION_EVENT_ACTION_CANCEL) {
            flags |= AMOTION_EVENT_FLAG_CANCELED;
        }
        events.push_back(
                std::make_unique<MotionEntry>(mIdGenerator.nextId(), currentTime, memento.deviceId,
                                              memento.source, memento.displayId,
                                              memento.policyFlags, action, /*actionButton=*/0,
                                              flags, AMETA_NONE, /*buttonState=*/0,
                                              MotionClassification::NONE,
                                              AMOTION_EVENT_EDGE_FLAG_NONE, memento.xPrecision,
                                              memento.yPrecision, memento.xCursorPosition,
                                              memento.yCursorPosition, memento.downTime,
                                              memento.pointerCount, memento.pointerProperties,
                                              memento.pointerCoords));
    } else {
        // If we aren't canceling all pointers, we need to generate ACTION_POINTER_UP with
        // FLAG_CANCELED for each of the canceled pointers. For each event, we must remove the
        // previously canceled pointers from PointerProperties and PointerCoords, and update
        // pointerCount appropriately. For convenience, sort the canceled pointer indices so that we
        // can just slide the remaining pointers to the beginning of the array when a pointer is
        // canceled.
        std::sort(canceledPointerIndices.begin(), canceledPointerIndices.end(),
                  std::greater<uint32_t>());

        uint32_t pointerCount = memento.pointerCount;
        for (const uint32_t pointerIdx : canceledPointerIndices) {
            const int32_t action = pointerCount == 1 ? AMOTION_EVENT_ACTION_CANCEL
                                                     : AMOTION_EVENT_ACTION_POINTER_UP |
                            (pointerIdx << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
            events.push_back(
                    std::make_unique<MotionEntry>(mIdGenerator.nextId(), currentTime,
                                                  memento.deviceId, memento.source,
                                                  memento.displayId, memento.policyFlags, action,
                                                  /*actionButton=*/0,
                                                  memento.flags | AMOTION_EVENT_FLAG_CANCELED,
                                                  AMETA_NONE, /*buttonState=*/0,
                                                  MotionClassification::NONE,
                                                  AMOTION_EVENT_EDGE_FLAG_NONE, memento.xPrecision,
                                                  memento.yPrecision, memento.xCursorPosition,
                                                  memento.yCursorPosition, memento.downTime,
                                                  pointerCount, pointerProperties.data(),
                                                  pointerCoords.data()));

            // Cleanup pointer information
            pointerProperties.erase(pointerProperties.begin() + pointerIdx);
            pointerCoords.erase(pointerCoords.begin() + pointerIdx);
            pointerCount--;
        }
    }
    return events;
}

void InputState::clear() {
    mKeyMementos.clear();
    mMotionMementos.clear();
    mFallbackKeys.clear();
}

void InputState::mergePointerStateTo(InputState& other) {
    for (size_t i = 0; i < mMotionMementos.size(); i++) {
        MotionMemento& memento = mMotionMementos[i];
        // Since we support split pointers we need to merge touch events
        // from the same source + device + screen.
        if (memento.source & AINPUT_SOURCE_CLASS_POINTER) {
            bool merged = false;
            for (size_t j = 0; j < other.mMotionMementos.size(); j++) {
                MotionMemento& otherMemento = other.mMotionMementos[j];
                if (memento.deviceId == otherMemento.deviceId &&
                    memento.source == otherMemento.source &&
                    memento.displayId == otherMemento.displayId) {
                    memento.mergePointerStateTo(otherMemento);
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                memento.firstNewPointerIdx = 0;
                other.mMotionMementos.push_back(memento);
            }
        }
    }
}

std::optional<int32_t> InputState::getFallbackKey(int32_t originalKeyCode) {
    auto it = mFallbackKeys.find(originalKeyCode);
    if (it == mFallbackKeys.end()) {
        return {};
    }
    return it->second;
}

void InputState::setFallbackKey(int32_t originalKeyCode, int32_t fallbackKeyCode) {
    mFallbackKeys.insert_or_assign(originalKeyCode, fallbackKeyCode);
}

void InputState::removeFallbackKey(int32_t originalKeyCode) {
    mFallbackKeys.erase(originalKeyCode);
}

bool InputState::shouldCancelKey(const KeyMemento& memento, const CancelationOptions& options) {
    if (options.keyCode && memento.keyCode != options.keyCode.value()) {
        return false;
    }

    if (options.deviceId && memento.deviceId != options.deviceId.value()) {
        return false;
    }

    if (options.displayId && memento.displayId != options.displayId.value()) {
        return false;
    }

    switch (options.mode) {
        case CancelationOptions::Mode::CANCEL_ALL_EVENTS:
        case CancelationOptions::Mode::CANCEL_NON_POINTER_EVENTS:
            return true;
        case CancelationOptions::Mode::CANCEL_FALLBACK_EVENTS:
            return memento.flags & AKEY_EVENT_FLAG_FALLBACK;
        default:
            return false;
    }
}

bool InputState::shouldCancelMotion(const MotionMemento& memento,
                                    const CancelationOptions& options) {
    if (options.deviceId && memento.deviceId != options.deviceId.value()) {
        return false;
    }

    if (options.displayId && memento.displayId != options.displayId.value()) {
        return false;
    }

    switch (options.mode) {
        case CancelationOptions::Mode::CANCEL_ALL_EVENTS:
            return true;
        case CancelationOptions::Mode::CANCEL_POINTER_EVENTS:
            return memento.source & AINPUT_SOURCE_CLASS_POINTER;
        case CancelationOptions::Mode::CANCEL_NON_POINTER_EVENTS:
            return !(memento.source & AINPUT_SOURCE_CLASS_POINTER);
        default:
            return false;
    }
}

} // namespace android::inputdispatcher
