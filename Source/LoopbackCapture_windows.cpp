// Head Tracking Host
// Copyright (C) 2026 lumen-tools and contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Windows: WASAPI loopback recording of a playback device (see LoopbackCapture.h).

#include "LoopbackCapture.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace loopback
{
namespace
{
// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), defined here to avoid initguid.
const PROPERTYKEY friendlyNameKey = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

struct ComScope
{
    explicit ComScope (DWORD mode) : hr (CoInitializeEx (nullptr, mode)) {}
    ~ComScope() { if (SUCCEEDED (hr)) CoUninitialize(); }
    HRESULT hr;
};

std::string narrow (const wchar_t* w)
{
    if (w == nullptr || *w == 0)
        return {};
    const int n = WideCharToMultiByte (CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s ((size_t) (n > 0 ? n - 1 : 0), '\0');
    if (n > 1)
        WideCharToMultiByte (CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Calls f (name, device) for each active playback device until f returns true.
template <typename Function>
void forEachPlaybackDevice (Function&& f)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS (&enumerator)))
        || FAILED (enumerator->EnumAudioEndpoints (eRender, DEVICE_STATE_ACTIVE, &devices)))
        return;
    UINT count = 0;
    devices->GetCount (&count);
    for (UINT i = 0; i < count; ++i)
    {
        ComPtr<IMMDevice> device;
        ComPtr<IPropertyStore> props;
        if (FAILED (devices->Item (i, &device)) || FAILED (device->OpenPropertyStore (STGM_READ, &props)))
            continue;
        PROPVARIANT value;
        PropVariantInit (&value);
        std::string name;
        if (SUCCEEDED (props->GetValue (friendlyNameKey, &value)) && value.vt == VT_LPWSTR)
            name = narrow (value.pwszVal);
        PropVariantClear (&value);
        if (! name.empty() && f (name, device))
            return;
    }
}

ComPtr<IMMDevice> findDevice (const std::string& name)
{
    ComPtr<IMMDevice> found;
    forEachPlaybackDevice ([&] (const std::string& n, ComPtr<IMMDevice>& device)
                           {
                               if (n != name)
                                   return false;
                               found = device;
                               return true;
                           });
    return found;
}

ComPtr<IAudioClient> activateClient (IMMDevice* device)
{
    ComPtr<IAudioClient> client;
    if (device == nullptr
        || FAILED (device->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**> (client.GetAddressOf()))))
        return {};
    return client;
}

struct SampleFormat
{
    int channels = 0, bytesPerSample = 0, blockAlign = 0;
    double sampleRate = 0.0;
    bool isFloat = false;
};

bool parseFormat (const WAVEFORMATEX* w, SampleFormat& f)
{
    if (w == nullptr)
        return false;
    f.channels = w->nChannels;
    f.sampleRate = w->nSamplesPerSec;
    f.bytesPerSample = w->wBitsPerSample / 8;
    f.blockAlign = w->nBlockAlign;
    WORD tag = w->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22)
        tag = (WORD) reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (w)->SubFormat.Data1; // KSDATAFORMAT_SUBTYPE_*
    f.isFloat = tag == WAVE_FORMAT_IEEE_FLOAT;
    if (f.channels <= 0 || f.blockAlign < f.channels * f.bytesPerSample)
        return false;
    if (f.isFloat)
        return f.bytesPerSample == 4 || f.bytesPerSample == 8;
    return tag == WAVE_FORMAT_PCM && (f.bytesPerSample == 2 || f.bytesPerSample == 3 || f.bytesPerSample == 4);
}

float sampleAt (const BYTE* p, const SampleFormat& f)
{
    if (f.isFloat)
    {
        if (f.bytesPerSample == 4) { float v; std::memcpy (&v, p, 4); return v; }
        double v; std::memcpy (&v, p, 8); return (float) v;
    }
    switch (f.bytesPerSample)
    {
        case 2:  { int16_t v; std::memcpy (&v, p, 2); return (float) v / 32768.0f; }
        case 3:  { const int32_t v = (int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24); return (float) v / 2147483648.0f; }
        default: { int32_t v; std::memcpy (&v, p, 4); return (float) v / 2147483648.0f; }
    }
}
} // namespace

std::vector<std::string> playbackDeviceNames()
{
    ComScope com (COINIT_APARTMENTTHREADED);
    std::vector<std::string> names;
    forEachPlaybackDevice ([&] (const std::string& n, ComPtr<IMMDevice>&) { names.push_back (n); return false; });
    return names;
}

bool queryFormat (const std::string& deviceName, Format& format)
{
    ComScope com (COINIT_APARTMENTTHREADED);
    auto client = activateClient (findDevice (deviceName).Get());
    WAVEFORMATEX* mix = nullptr;
    if (client == nullptr || FAILED (client->GetMixFormat (&mix)))
        return false;
    SampleFormat f;
    const bool ok = parseFormat (mix, f);
    CoTaskMemFree (mix);
    if (! ok)
        return false;
    REFERENCE_TIME defaultPeriod = 0, minimumPeriod = 0;
    client->GetDevicePeriod (&defaultPeriod, &minimumPeriod);
    format.channels = f.channels;
    format.sampleRate = f.sampleRate;
    format.periodFrames = defaultPeriod > 0 ? (int) (defaultPeriod * f.sampleRate / 10000000.0 + 0.5) : 480;
    return true;
}

struct Capture::Impl
{
    std::thread thread;
    std::atomic<bool> running { false }, stopRequested { false };

    void stop()
    {
        stopRequested = true;
        if (thread.joinable())
            thread.join();
        running = false;
    }

    void run (const std::string& name, const Callback& callback, std::promise<std::string>& result)
    {
        ComScope com (COINIT_MULTITHREADED);
        SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

        auto device = findDevice (name);
        auto captureClient = activateClient (device.Get());
        auto renderClient = activateClient (device.Get());
        if (captureClient == nullptr || renderClient == nullptr)
            return result.set_value ("Playback device not found: " + name);

        WAVEFORMATEX* mix = nullptr;
        SampleFormat f;
        if (FAILED (captureClient->GetMixFormat (&mix)) || ! parseFormat (mix, f))
        {
            CoTaskMemFree (mix);
            return result.set_value ("Unsupported audio format of " + name);
        }

        // A silent stream played into the same device: keeps it delivering audio while nothing else
        // plays, and its events (in the device's own rhythm) tell when to collect recorded audio.
        HANDLE event = CreateEvent (nullptr, FALSE, FALSE, nullptr);
        HRESULT hr = renderClient->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
        if (SUCCEEDED (hr))
            hr = renderClient->SetEventHandle (event);
        if (SUCCEEDED (hr))
            hr = captureClient->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                            2000000 /* 200 ms */, 0, mix, nullptr);
        CoTaskMemFree (mix);

        ComPtr<IAudioRenderClient> renderService;
        ComPtr<IAudioCaptureClient> captureService;
        UINT32 renderFrames = 0;
        if (SUCCEEDED (hr)) hr = renderClient->GetService (IID_PPV_ARGS (&renderService));
        if (SUCCEEDED (hr)) hr = captureClient->GetService (IID_PPV_ARGS (&captureService));
        if (SUCCEEDED (hr)) hr = renderClient->GetBufferSize (&renderFrames);
        BYTE* buffer = nullptr;
        if (SUCCEEDED (hr) && SUCCEEDED (renderService->GetBuffer (renderFrames, &buffer)))
            renderService->ReleaseBuffer (renderFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        if (SUCCEEDED (hr)) hr = renderClient->Start();
        if (SUCCEEDED (hr)) hr = captureClient->Start();
        if (FAILED (hr))
        {
            renderClient->Stop();
            CloseHandle (event);
            char text[16];
            snprintf (text, sizeof (text), "%08lx", (unsigned long) hr);
            return result.set_value ("Could not record " + name + " (error 0x" + text + ")");
        }

        running = true;
        result.set_value ({});

        std::vector<std::vector<float>> planar ((size_t) f.channels);
        std::vector<float*> pointers ((size_t) f.channels);
        while (! stopRequested)
        {
            WaitForSingleObject (event, 100);

            UINT32 padding = 0;
            if (FAILED (renderClient->GetCurrentPadding (&padding)))
                break;
            if (renderFrames > padding && SUCCEEDED (renderService->GetBuffer (renderFrames - padding, &buffer)))
                renderService->ReleaseBuffer (renderFrames - padding, AUDCLNT_BUFFERFLAGS_SILENT);

            bool failed = false;
            for (;;)
            {
                UINT32 packet = 0, frames = 0;
                DWORD flags = 0;
                BYTE* data = nullptr;
                if (FAILED (captureService->GetNextPacketSize (&packet)))
                {
                    failed = true;
                    break;
                }
                if (packet == 0 || stopRequested)
                    break;
                if (FAILED (captureService->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
                {
                    failed = true;
                    break;
                }
                if (planar[0].size() < frames)
                    for (size_t ch = 0; ch < planar.size(); ++ch)
                    {
                        planar[ch].resize (frames);
                        pointers[ch] = planar[ch].data();
                    }
                for (int ch = 0; ch < f.channels; ++ch)
                {
                    auto* out = planar[(size_t) ch].data();
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
                        std::fill (out, out + frames, 0.0f);
                    else
                        for (UINT32 i = 0; i < frames; ++i)
                            out[i] = sampleAt (data + (size_t) i * (size_t) f.blockAlign + (size_t) ch * (size_t) f.bytesPerSample, f);
                }
                captureService->ReleaseBuffer (frames);
                if (frames > 0)
                    callback (pointers.data(), f.channels, (int) frames);
            }
            if (failed)
                break; // e.g. the device was removed or its format changed
        }

        captureClient->Stop();
        renderClient->Stop();
        CloseHandle (event);
        running = false;
    }
};

Capture::Capture() : impl (std::make_unique<Impl>()) {}
Capture::~Capture() { stop(); }

std::string Capture::start (const std::string& deviceName, Callback callback)
{
    stop();
    impl->stopRequested = false;
    std::promise<std::string> promise;
    auto future = promise.get_future();
    impl->thread = std::thread ([impl = impl.get(), deviceName, callback = std::move (callback), promise = std::move (promise)]() mutable
                                { impl->run (deviceName, callback, promise); });
    auto error = future.get();
    if (! error.empty())
        stop();
    return error;
}

void Capture::stop()            { impl->stop(); }
bool Capture::isRunning() const { return impl->running; }
} // namespace loopback
