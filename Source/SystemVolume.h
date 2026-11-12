// Head Tracking Host
// Copyright (C) 2026 lumen-tools and contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Reads and sets the volume of playback devices through the operating system (Windows Core Audio
// endpoint volume, macOS Core Audio). Used so that the volume keys - which change the virtual
// device the system plays into - control the headphones. Kept free of JUCE and system headers so
// the platform files can include their own.

#pragma once

#include <memory>
#include <string>

namespace sysvol
{
// Volume of a playback device as the system's volume keys and sliders set it.
struct Volume
{
    float scalar = 1.0f;          // 0..1, the slider position
    float decibels = 0.0f;        // the gain this position stands for
    bool muted = false;
    bool appliedToSignal = false; // the device itself turns its audio down by this setting
};

class Control
{
public:
    Control();
    ~Control();

    // Playback devices by name: the one the volume keys change (the virtual device the system plays
    // into) and the one that follows it (the headphones). Empty names: nothing.
    void setDevices (const std::string& source, const std::string& target);

    bool readSource (Volume& v);
    bool readTarget (Volume& v);
    bool writeSource (float scalar, bool muted);
    bool writeTarget (float scalar, bool muted); // false if the device has no volume control

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace sysvol
