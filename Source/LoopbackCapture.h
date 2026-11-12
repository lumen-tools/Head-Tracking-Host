// Head Tracking Host
// Copyright (C) 2026 lumen-tools and contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Windows: records what is played into a playback device (WASAPI loopback), e.g. "Hi-Fi Cable
// Input" - the device that games and apps play into. Its channel layout is the one set in the
// Windows sound settings (7.1 = 8 channels). A silent stream is played into the same device so it
// keeps delivering audio (in the device's own rhythm) even while nothing else plays.
// Kept free of JUCE and system headers.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace loopback
{
struct Format
{
    int channels = 0;
    double sampleRate = 0.0;
    int periodFrames = 0;   // the device's processing period
};

// Format of a playback device's mix. False if the device is not found.
bool queryFormat (const std::string& deviceName, Format& format);

class Capture
{
public:
    // Called on the capture thread: one planar float buffer per channel.
    using Callback = std::function<void (const float* const* channels, int numChannels, int numFrames)>;

    Capture();
    ~Capture();

    // Starts recording the playback device; returns an error text, or an empty string.
    std::string start (const std::string& deviceName, Callback callback);
    void stop();
    bool isRunning() const;     // false after stop() or when the device went away

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace loopback
