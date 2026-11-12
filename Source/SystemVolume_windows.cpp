// Head Tracking Host
// Copyright (C) 2026 lumen-tools and contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Windows: endpoint volume of playback devices (IAudioEndpointVolume), found by their friendly
// name - the same names JUCE's "Windows Audio" device lists show.

#include "SystemVolume.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>

using Microsoft::WRL::ComPtr;

namespace sysvol
{
namespace
{
// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), defined here to avoid initguid.
const PROPERTYKEY friendlyNameKey = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

std::wstring widen (const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar (CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w ((size_t) n, L'\0');
    MultiByteToWideChar (CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}

ComPtr<IAudioEndpointVolume> findPlaybackDevice (const std::string& name)
{
    if (name.empty())
        return {};
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator))))
        return {};
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &devices)))
        return {};

    UINT count = 0;
    devices->GetCount (&count);
    const auto wanted = widen (name);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> props;
        if (FAILED (devices->Item (i, &device)) || FAILED (device->OpenPropertyStore (STGM_READ, &props)))
            continue;
        PROPVARIANT value;
        PropVariantInit (&value);
        const bool match = SUCCEEDED (props->GetValue (friendlyNameKey, &value)) && value.vt == VT_LPWSTR
                           && wanted == value.pwszVal;
        PropVariantClear (&value);
        if (! match)
            continue;
        ComPtr<IAudioEndpointVolume> volume;
        if (SUCCEEDED (device->Activate (__uuidof (IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                         reinterpret_cast<void**> (volume.GetAddressOf()))))
            return volume;
    }
    return {};
}

// A device found by name; looked up again (at most every 2 s) when it is missing or stops answering.
struct Endpoint
{
    std::string name;
    ComPtr<IAudioEndpointVolume> volume;
    std::chrono::steady_clock::time_point lastLookup {};

    void setName (const std::string& n)
    {
        if (n != name)
        {
            name = n;
            volume.Reset();
            lastLookup = {};
        }
    }

    IAudioEndpointVolume* get()
    {
        if (volume == nullptr && ! name.empty())
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastLookup > std::chrono::seconds (2))
            {
                lastLookup = now;
                volume = findPlaybackDevice (name);
            }
        }
        return volume.Get();
    }

    bool read (Volume& v)
    {
        auto* ep = get();
        float scalar = 0.0f, db = 0.0f;
        BOOL mute = FALSE;
        if (ep == nullptr || FAILED (ep->GetMasterVolumeLevelScalar (&scalar)) || FAILED (ep->GetMasterVolumeLevel (&db))
            || FAILED (ep->GetMute (&mute)))
        {
            volume.Reset();
            return false;
        }
        DWORD hardware = 0;
        ep->QueryHardwareSupport (&hardware);
        v.scalar = scalar;
        v.decibels = db;
        v.muted = mute != FALSE;
        // Without a volume control of its own the device gets its audio turned down by Windows.
        v.appliedToSignal = (hardware & ENDPOINT_HARDWARE_SUPPORT_VOLUME) == 0;
        return true;
    }

    bool write (float scalar, bool muted)
    {
        auto* ep = get();
        if (ep == nullptr)
            return false;
        float current = 0.0f;
        BOOL mute = FALSE;
        bool ok = true;
        if (FAILED (ep->GetMasterVolumeLevelScalar (&current)) || std::abs (current - scalar) > 0.0005f)
            ok = SUCCEEDED (ep->SetMasterVolumeLevelScalar (scalar, nullptr));
        if (FAILED (ep->GetMute (&mute)) || (mute != FALSE) != muted)
            ok = SUCCEEDED (ep->SetMute (muted ? TRUE : FALSE, nullptr)) && ok;
        if (! ok)
            volume.Reset();
        return ok;
    }
};
} // namespace

struct Control::Impl
{
    Impl()  { comInitialised = SUCCEEDED (CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED)); }
    ~Impl()
    {
        source.volume.Reset();
        target.volume.Reset();
        if (comInitialised)
            CoUninitialize();
    }

    bool comInitialised = false;
    Endpoint source, target;
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
