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

#ifndef _UI_INPUTREADER_CURSOR_INPUT_MAPPER_H
#define _UI_INPUTREADER_CURSOR_INPUT_MAPPER_H

#include "CursorButtonAccumulator.h"
#include "CursorScrollAccumulator.h"
#include "InputMapper.h"

#include <PointerControllerInterface.h>
#include <input/VelocityControl.h>

namespace android {

class VelocityControl;
class PointerControllerInterface;

class CursorButtonAccumulator;
class CursorScrollAccumulator;

/* Keeps track of cursor movements. */
class CursorMotionAccumulator {
public:
    CursorMotionAccumulator();
    void reset(InputDeviceContext& deviceContext);

    void process(const RawEvent* rawEvent);
    void finishSync();

    inline int32_t getRelativeX() const { return mRelX; }
    inline int32_t getRelativeY() const { return mRelY; }

private:
    int32_t mRelX;
    int32_t mRelY;

    void clearRelativeAxes();
};

/* Keeps track of cursor position. */

class CursorPositionAccumulator {
public:
    CursorPositionAccumulator();
    void reset(InputDeviceContext& deviceContext);

    void process(const RawEvent* rawEvent);
    void finishSync();

    inline int32_t getX() const { return mX; }
    inline int32_t getY() const { return mY; }

private:
    int32_t mX;
    int32_t mY;

    void clearPosition();
};

class CursorInputMapper : public InputMapper {
public:
    explicit CursorInputMapper(InputDeviceContext& deviceContext);
    virtual ~CursorInputMapper();

    virtual uint32_t getSources() override;
    virtual void populateDeviceInfo(InputDeviceInfo* deviceInfo) override;
    virtual void dump(std::string& dump) override;
    virtual void configure(nsecs_t when, const InputReaderConfiguration* config,
                           uint32_t changes) override;
    virtual void reset(nsecs_t when) override;
    virtual void process(const RawEvent* rawEvent) override;

    virtual int32_t getScanCodeState(uint32_t sourceMask, int32_t scanCode) override;

    virtual std::optional<int32_t> getAssociatedDisplayId() override;

private:
    // Amount that trackball needs to move in order to generate a key event.
    static const int32_t TRACKBALL_MOVEMENT_THRESHOLD = 6;

    // Immutable configuration parameters.
    struct Parameters {
        enum Mode {
            MODE_POINTER,
            MODE_POINTER_RELATIVE,
            MODE_NAVIGATION,
        };

        Mode mode;
        bool hasAssociatedDisplay;
        bool orientationAware;
    } mParameters;

    CursorButtonAccumulator mCursorButtonAccumulator;
    CursorMotionAccumulator mCursorMotionAccumulator;
    CursorPositionAccumulator mCursorPositionAccumulator;
    CursorScrollAccumulator mCursorScrollAccumulator;

    int32_t mSource;
    float mXScale;
    float mYScale;
    float mXPrecision;
    float mYPrecision;

    float mVWheelScale;
    float mHWheelScale;

    // Velocity controls for mouse pointer and wheel movements.
    // The controls for X and Y wheel movements are separate to keep them decoupled.
    VelocityControl mPointerVelocityControl;
    VelocityControl mWheelXVelocityControl;
    VelocityControl mWheelYVelocityControl;

    int32_t mOrientation;

    sp<PointerControllerInterface> mPointerController;

    int32_t mButtonState;
    nsecs_t mDownTime;

    void configureParameters();
    void dumpParameters(std::string& dump);

    void sync(nsecs_t when);
};

} // namespace android

#endif // _UI_INPUTREADER_CURSOR_INPUT_MAPPER_H
