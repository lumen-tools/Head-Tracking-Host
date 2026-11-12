// Head Tracking Host
// Copyright (C) 2026 lumen-tools and contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// macOS: output volume and mute of Core Audio devices, found by the name JUCE's Core Audio device
// lists show. Uses the main volume control if a device has one, otherwise channels 1 and 2.

#include "SystemVolume.h"

#include <CoreAudio/CoreAudio.h>

#include <chrono>
#include <cmath>
#include <vector>

namespace sysvol
{
namespace
{
AudioObjectPropertyAddress outputProperty (AudioObjectPropertySelector selector, UInt32 element)
{
    return { selector, kAudioDevicePropertyScopeOutput, element };
}

AudioObjectID findDevice (const std::string& name)
{
    if (name.empty())
        return kAudioObjectUnknown;
    AudioObjectPropertyAddress list { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &list, 0, nullptr, &size) != noErr)
        return kAudioObjectUnknown;
    std::vector<AudioObjectID> ids (size / sizeof (AudioObjectID));
    if (ids.empty() || AudioObjectGetPropertyData (kAudioObjectSystemObject, &list, 0, nullptr, &size, ids.data()) != noErr)
        return kAudioObjectUnknown;

    for (auto id : ids)
    {
        AudioObjectPropertyAddress nameAddress { kAudioDevicePropertyDeviceNameCFString, kAudioObjectPropertyScopeGlobal,
                                                 kAudioObjectPropertyElementMain };
        CFStringRef cfName = nullptr;
        UInt32 nameSize = sizeof (cfName);
        if (AudioObjectGetPropertyData (id, &nameAddress, 0, nullptr, &nameSize, &cfName) != noErr || cfName == nullptr)
            continue;
        char buffer[512] = {};
        const bool ok = CFStringGetCString (cfName, buffer, sizeof (buffer), kCFStringEncodingUTF8);
        CFRelease (cfName);
        if (ok && name == buffer)
            return id;
    }
    return kAudioObjectUnknown;
}

bool getFloat (AudioObjectID id, AudioObjectPropertySelector selector, UInt32 element, Float32& value)
{
    auto address = outputProperty (selector, element);
    UInt32 size = sizeof (value);
    return AudioObjectHasProperty (id, &address)
           && AudioObjectGetPropertyData (id, &address, 0, nullptr, &size, &value) == noErr;
}

bool getBool (AudioObjectID id, AudioObjectPropertySelector selector, UInt32 element, bool& value)
{
    auto address = outputProperty (selector, element);
    UInt32 v = 0, size = sizeof (v);
    if (! AudioObjectHasProperty (id, &address) || AudioObjectGetPropertyData (id, &address, 0, nullptr, &size, &v) != noErr)
        return false;
    value = v != 0;
    return true;
}

bool isSettable (AudioObjectID id, AudioObjectPropertySelector selector, UInt32 element)
{
    auto address = outputProperty (selector, element);
    Boolean settable = false;
    return AudioObjectHasProperty (id, &address)
           && AudioObjectIsPropertySettable (id, &address, &settable) == noErr && settable;
}

// The elements that carry the volume control: the main one, or the first two channels.
std::vector<UInt32> volumeElements (AudioObjectID id)
{
    auto main = outputProperty (kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyElementMain);
    if (AudioObjectHasProperty (id, &main))
        return { kAudioObjectPropertyElementMain };
    std::vector<UInt32> elements;
    for (UInt32 channel : { 1u, 2u })
    {
        auto address = outputProperty (kAudioDevicePropertyVolumeScalar, channel);
        if (AudioObjectHasProperty (id, &address))
            elements.push_back (channel);
    }
    return elements;
}

struct Device
{
    std::string name;
    AudioObjectID id = kAudioObjectUnknown;
    std::chrono::steady_clock::time_point lastLookup {};

    void setName (const std::string& n)
    {
        if (n != name)
        {
            name = n;
            id = kAudioObjectUnknown;
            lastLookup = {};
        }
    }

    AudioObjectID get()
    {
        if (id == kAudioObjectUnknown && ! name.empty())
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastLookup > std::chrono::seconds (2))
            {
                lastLookup = now;
                id = findDevice (name);
            }
        }
        return id;
    }

    bool read (Volume& v)
    {
        const auto device = get();
        if (device == kAudioObjectUnknown)
            return false;
        const auto elements = volumeElements (device);
        if (elements.empty())
        {
            id = kAudioObjectUnknown; // gone, or no volume control
            return false;
        }
        float scalar = 0.0f, db = 0.0f;
        int n = 0;
        for (auto e : elements)
        {
            Float32 s = 0.0f, d = 0.0f;
            if (getFloat (device, kAudioDevicePropertyVolumeScalar, e, s))
            {
                scalar += s;
                db += getFloat (device, kAudioDevicePropertyVolumeDecibels, e, d) ? d : 0.0f;
                ++n;
            }
        }
        if (n == 0)
            return false;
        bool muted = false;
        if (! getBool (device, kAudioDevicePropertyMute, kAudioObjectPropertyElementMain, muted))
            getBool (device, kAudioDevicePropertyMute, 1, muted);
        v.scalar = scalar / (float) n;
        v.decibels = db / (float) n;
        v.muted = muted;
        v.appliedToSignal = true; // Core Audio devices turn their own audio down (e.g. BlackHole)
        return true;
    }

    bool write (float scalar, bool muted)
    {
        const auto device = get();
        if (device == kAudioObjectUnknown)
            return false;
        bool any = false;
        for (auto e : volumeElements (device))
        {
            if (! isSettable (device, kAudioDevicePropertyVolumeScalar, e))
                continue;
            Float32 current = -1.0f, value = scalar;
            if (getFloat (device, kAudioDevicePropertyVolumeScalar, e, current) && std::abs (current - value) < 0.0005f)
            {
                any = true;
                continue;
            }
            auto address = outputProperty (kAudioDevicePropertyVolumeScalar, e);
            any = AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (value), &value) == noErr || any;
        }
        // Mute: the main control, or else channels 1 and 2.
        std::vector<UInt32> muteElements;
        if (isSettable (device, kAudioDevicePropertyMute, kAudioObjectPropertyElementMain))
            muteElements = { kAudioObjectPropertyElementMain };
        else
            muteElements = { 1u, 2u };
        for (auto e : muteElements)
        {
            bool current = false;
            if (! isSettable (device, kAudioDevicePropertyMute, e)
                || (getBool (device, kAudioDevicePropertyMute, e, current) && current == muted))
                continue;
            UInt32 value = muted ? 1 : 0;
            auto address = outputProperty (kAudioDevicePropertyMute, e);
            AudioObjectSetPropertyData (device, &address, 0, nullptr, sizeof (value), &value);
        }
        return any;
    }
};
} // namespace

struct Control::Impl
{
    Device source, target;
};

Control::Control() : impl (std::make_unique<Impl>()) {}
Control::~Control() = default;

void Control::setDevices (const std::string& source, const std::string& target)
{
    impl->source.setName (source);
    impl->target.setName (target);
}

bool Control::readSource (Volume& v)                  { return impl->source.read (v); }
bool Control::readTarget (Volume& v)                  { return impl->target.read (v); }
bool Control::writeSource (float scalar, bool muted)  { return impl->source.write (scalar, muted); }
bool Control::writeTarget (float scalar, bool muted)  { return impl->target.write (scalar, muted); }
} // namespace sysvol
