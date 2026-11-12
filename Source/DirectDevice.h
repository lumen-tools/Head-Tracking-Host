// Head Tracking Host
// Copyright (C) 2026 lumen-tools and contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// "Direct" audio device type with clock drift correction.
//
// The virtual device the system plays into (e.g. VB-Audio Hi-Fi Cable, BlackHole) and the
// headphones are opened as two separate devices of the system's own audio API (WASAPI shared
// mode on Windows, Core Audio on the Mac). No ASIO driver and no aggregate device are needed.
//
// The two devices run on their own clocks, which drift apart slowly - without correction a
// buffer runs over or empty every few minutes and you hear a click. Here the host (plugin,
// headphone correction) runs on the input's clock, and its output reaches the headphones
// through a small buffer. That buffer is read with a very slightly varied speed (adaptive
// resampling, at most +/- 0.2 %, in practice a few hundredths of a percent) so that it always
// stays at its target fill level. Only the output channels (usually two) are resampled.
//
// Uses JUCE only (AbstractFifo, WindowedSincInterpolator, the system device types).

#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#if JUCE_WINDOWS
 #include "LoopbackCapture.h"
#endif

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace hth
{
using namespace juce;

//==============================================================================
// A FIFO between two audio clocks: one thread writes, another reads with adaptive resampling.
class DriftFifo
{
public:
    // Called while no audio thread is running.
    void prepare (int numChannels, double newSampleRate, int writeBlock, int readBlock)
    {
        channels = jmax (1, numChannels);
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
        // Target fill: one block of each side plus 10 ms of reserve for irregular delivery.
        target = jmax (64, writeBlock) + jmax (64, readBlock) + (int) (0.010 * sampleRate);
        const int capacity = jmax (target * 4, (int) (sampleRate * 0.5));
        buffer.setSize (channels, capacity);
        buffer.clear();
        fifo.setTotalSize (capacity);
        maxChunk = jmax (4096, readBlock * 2);
        scratch.setSize (channels, maxChunk * 2 + 16);
        dummy.setSize (1, maxChunk);
        interpolators.clear();
        interpolators.resize ((size_t) channels);
        reset();
    }

    // Called while no audio thread is running.
    void reset()
    {
        fifo.reset();
        for (auto& i : interpolators)
            i.reset();
        primed = false;
        ratio = 1.0;
        integral = 0.0;
        filteredFill = 0.0;
    }

    int getTargetFill() const noexcept     { return target; }
    int getNumChannels() const noexcept    { return channels; }
    int takeDropouts() noexcept            { return dropouts.exchange (0); }

    // Writer thread.
    void write (const float* const* data, int numData, int numSamples)
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite (numSamples, start1, size1, start2, size2);
        for (int ch = 0; ch < channels; ++ch)
        {
            const float* src = ch < numData ? data[ch] : nullptr;
            auto* dest = buffer.getWritePointer (ch);
            if (src != nullptr)
            {
                FloatVectorOperations::copy (dest + start1, src, size1);
                FloatVectorOperations::copy (dest + start2, src + size1, size2);
            }
            else
            {
                FloatVectorOperations::clear (dest + start1, size1);
                FloatVectorOperations::clear (dest + start2, size2);
            }
        }
        fifo.finishedWrite (size1 + size2);
        if (size1 + size2 < numSamples)
            dropouts.fetch_add (1); // full: the reader stopped or fell far behind
    }

    // Reader thread: fills numOut output channels with numSamples samples.
    void read (float* const* out, int numOut, int numSamples)
    {
        for (int done = 0; done < numSamples;)
        {
            const int n = jmin (maxChunk, numSamples - done);
            readChunk (out, numOut, done, n);
            done += n;
        }
    }

private:
    void readChunk (float* const* out, int numOut, int offset, int n)
    {
        auto silence = [&]
        {
            for (int ch = 0; ch < numOut; ++ch)
                if (out[ch] != nullptr)
                    FloatVectorOperations::clear (out[ch] + offset, n);
        };

        int ready = fifo.getNumReady();
        if (! primed)
        {
            // Start (or restart after an underrun) only once the target fill is reached.
            if (ready < target)
            {
                silence();
                return;
            }
            primed = true;
            filteredFill = ready;
            for (auto& i : interpolators)
                i.reset();
        }

        // Far too full (e.g. the output device paused): drop the excess instead of lagging behind.
        if (ready > target * 3)
        {
            fifo.finishedRead (ready - target);
            ready = target;
            filteredFill = target;
            dropouts.fetch_add (1);
        }

        const int need = (int) std::ceil (n * ratio) + 4;
        if (ready < need || need > scratch.getNumSamples())
        {
            primed = false;               // ran empty (the input stopped): silence, then refill
            dropouts.fetch_add (1);
            silence();
            return;
        }

        int start1, size1, start2, size2;
        fifo.prepareToRead (need, start1, size1, start2, size2);
        int used = 0;
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* src = buffer.getReadPointer (ch);
            auto* tmp = scratch.getWritePointer (ch);
            FloatVectorOperations::copy (tmp, src + start1, size1);
            FloatVectorOperations::copy (tmp + size1, src + start2, size2);
            float* dest = ch < numOut && out[ch] != nullptr ? out[ch] + offset : dummy.getWritePointer (0);
            used = interpolators[(size_t) ch].process (ratio, tmp, dest, n);
        }
        for (int ch = channels; ch < numOut; ++ch)
            if (out[ch] != nullptr)
                FloatVectorOperations::clear (out[ch] + offset, n);
        fifo.finishedRead (jlimit (0, size1 + size2, used));

        // Controller: keep the (smoothed) fill level at the target by reading slightly faster or
        // slower. PI control, time constant about 20 s; a speed change of a few hundredths of a
        // percent is far below anything audible.
        const double dt = n / sampleRate;
        filteredFill += (fifo.getNumReady() - filteredFill) * jmin (1.0, dt / smoothingSeconds);
        const double error = (filteredFill - target) / sampleRate;   // seconds of audio too many (+) or too few (-)
        integral = jlimit (-maxDeviation, maxDeviation, integral + ki * error * dt);
        ratio = 1.0 + jlimit (-maxDeviation, maxDeviation, kp * error + integral);
    }

    static constexpr double kp = 0.05, ki = kp * kp / 4.0;   // critically damped
    static constexpr double maxDeviation = 0.002;           // +/- 0.2 % (real drift: ~0.01 %)
    static constexpr double smoothingSeconds = 1.0;

    AbstractFifo fifo { 1 };
    AudioBuffer<float> buffer, scratch, dummy;
    std::vector<WindowedSincInterpolator> interpolators;
    int channels = 1, target = 1024, maxChunk = 4096;
    double sampleRate = 48000.0, ratio = 1.0, integral = 0.0, filteredFill = 0.0;
    bool primed = false;
    std::atomic<int> dropouts { 0 };
};

//==============================================================================
// One output device (headphones) plus one input device (the virtual device), joined with
// drift correction. Without an input it simply runs on the output device.
class DirectDevice : public AudioIODevice
{
public:
    DirectDevice (const String& typeName, std::unique_ptr<AudioIODevice> outputDevice, std::unique_ptr<AudioIODevice> inputDevice)
        : AudioIODevice (outputDevice->getName(), typeName),
          output (std::move (outputDevice)), input (std::move (inputDevice)) {}

    ~DirectDevice() override { close(); }

    String getOutputName() const { return output->getName(); }
    String getInputName() const  { return input != nullptr ? input->getName() : String(); }

    StringArray getOutputChannelNames() override { return output->getOutputChannelNames(); }
    StringArray getInputChannelNames() override  { return input != nullptr ? input->getInputChannelNames() : StringArray(); }
    std::optional<BigInteger> getDefaultOutputChannels() const override { return output->getDefaultOutputChannels(); }
    std::optional<BigInteger> getDefaultInputChannels() const override
    {
        return input != nullptr ? input->getDefaultInputChannels() : std::optional<BigInteger>();
    }

    Array<double> getAvailableSampleRates() override
    {
        auto rates = output->getAvailableSampleRates();
        if (input != nullptr)
        {
            auto common = rates;
            common.removeValuesNotIn (input->getAvailableSampleRates());
            if (! common.isEmpty())
                return common;
        }
        return rates;
    }

    // The usual audio buffer sizes. The chosen size is the host's block size (the input is split
    // into blocks of it); the devices themselves use buffers that suit them (see deviceBufferFor) -
    // the FIFO between input and output takes up the difference.
    Array<int> getAvailableBufferSizes() override { return { 64, 128, 256, 512, 1024, 2048 }; }
    int getDefaultBufferSize() override           { return 256; }

    String open (const BigInteger& ins, const BigInteger& outs, double rate, int bufferSize) override
    {
        close();
        const int requested = bufferSize > 0 ? bufferSize : getDefaultBufferSize();
        lastError = output->open ({}, outs, rate, deviceBufferFor (*output, requested));
        if (lastError.isEmpty() && input != nullptr && ! ins.isZero())
        {
            lastError = input->open (ins, {}, rate, deviceBufferFor (*input, requested));
            if (lastError.isEmpty()
                && std::abs (input->getCurrentSampleRate() - output->getCurrentSampleRate()) > 0.5)
            {
                lastError = "Input (" + String (input->getCurrentSampleRate()) + " Hz) and output ("
                          + String (output->getCurrentSampleRate()) + " Hz) run at different sample rates. "
                            "Set both devices to the same rate in the system sound settings.";
                input->close();
            }
            inputOpen = lastError.isEmpty();
        }
        if (lastError.isNotEmpty())
        {
            output->close();
            inputOpen = false;
            return lastError;
        }

        const int numOut = output->getActiveOutputChannels().countNumberOfSetBits();
        const int inputBlock = inputOpen ? input->getCurrentBufferSizeSamples() : 0;
        fifo.prepare (numOut, output->getCurrentSampleRate(), inputBlock, output->getCurrentBufferSizeSamples());
        hostBlock = inputOpen ? requested : jmax (16, output->getCurrentBufferSizeSamples());
        hostOutput.setSize (fifo.getNumChannels(), hostBlock);
        opened = true;
        return {};
    }

    void close() override
    {
        stop();
        if (inputOpen)
            input->close();
        output->close();
        inputOpen = opened = false;
    }

    bool isOpen() override { return opened; }

    void start (AudioIODeviceCallback* callback) override
    {
        if (! opened || callback == nullptr)
            return;
        stop();
        fifo.reset();
        callback->audioDeviceAboutToStart (this);
        host = callback;
        playing = true;
        output->start (&outputSide);
        if (inputOpen)
            input->start (&inputSide);
    }

    void stop() override
    {
        if (! playing)
            return;
        if (inputOpen)
            input->stop();
        output->stop();
        playing = false;
        if (auto* callback = host.exchange (nullptr))
            callback->audioDeviceStopped();
    }

    bool isPlaying() override { return playing && output->isPlaying() && (! inputOpen || input->isPlaying()); }

    String getLastError() override
    {
        if (lastError.isNotEmpty())
            return lastError;
        if (output->getLastError().isNotEmpty())
            return output->getLastError();
        return input != nullptr ? input->getLastError() : String();
    }

    // The host runs on the input's clock (if there is an input).
    int getCurrentBufferSizeSamples() override   { return inputOpen ? hostBlock : output->getCurrentBufferSizeSamples(); }
    double getCurrentSampleRate() override       { return output->getCurrentSampleRate(); }
    int getCurrentBitDepth() override            { return output->getCurrentBitDepth(); }
    BigInteger getActiveOutputChannels() const override { return output->getActiveOutputChannels(); }
    BigInteger getActiveInputChannels() const override  { return inputOpen ? input->getActiveInputChannels() : BigInteger(); }
    int getOutputLatencyInSamples() override     { return output->getOutputLatencyInSamples() + (inputOpen ? fifo.getTargetFill() : 0); }
    int getInputLatencyInSamples() override      { return inputOpen ? input->getInputLatencyInSamples() : 0; }

    int getXRunCount() const noexcept override
    {
        return output->getXRunCount() + (inputOpen ? input->getXRunCount() : 0) + dropoutCount.load();
    }

private:
    // Input device thread: runs the host, puts its output into the FIFO.
    struct InputSide : public AudioIODeviceCallback
    {
        explicit InputSide (DirectDevice& d) : owner (d) {}
        void audioDeviceAboutToStart (AudioIODevice*) override {}
        void audioDeviceStopped() override {}
        void audioDeviceError (const String& e) override { owner.forwardError (e); }

        void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs, float* const*, int,
                                               int numSamples, const AudioIODeviceCallbackContext& context) override
        {
            auto* callback = owner.host.load();
            auto& out = owner.hostOutput;
            const int numIn = jmin (numInputs, (int) maxChannels);
            std::array<const float*, maxChannels> in {};
            for (int done = 0; done < numSamples;)
            {
                const int n = jmin (out.getNumSamples(), numSamples - done);
                for (int ch = 0; ch < numIn; ++ch)
                    in[(size_t) ch] = inputs[ch] != nullptr ? inputs[ch] + done : nullptr;
                for (int ch = 0; ch < out.getNumChannels(); ++ch)
                    FloatVectorOperations::clear (out.getWritePointer (ch), n);
                if (callback != nullptr)
                    callback->audioDeviceIOCallbackWithContext (in.data(), numIn, out.getArrayOfWritePointers(),
                                                                out.getNumChannels(), n, context);
                owner.fifo.write (out.getArrayOfReadPointers(), out.getNumChannels(), n);
                done += n;
            }
        }

        static constexpr int maxChannels = 64;
        DirectDevice& owner;
    };

    // Output device thread: reads the FIFO with drift correction (or runs the host directly
    // when there is no input).
    struct OutputSide : public AudioIODeviceCallback
    {
        explicit OutputSide (DirectDevice& d) : owner (d) {}
        void audioDeviceAboutToStart (AudioIODevice*) override {}
        void audioDeviceStopped() override {}
        void audioDeviceError (const String& e) override { owner.forwardError (e); }

        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs,
                                               int numSamples, const AudioIODeviceCallbackContext& context) override
        {
            if (! owner.inputOpen)
            {
                if (auto* callback = owner.host.load())
                {
                    callback->audioDeviceIOCallbackWithContext (nullptr, 0, outputs, numOutputs, numSamples, context);
                    return;
                }
                for (int ch = 0; ch < numOutputs; ++ch)
                    if (outputs[ch] != nullptr)
                        FloatVectorOperations::clear (outputs[ch], numSamples);
                return;
            }
            owner.fifo.read (outputs, numOutputs, numSamples);
            if (const int d = owner.fifo.takeDropouts(); d > 0)
                owner.dropoutCount.fetch_add (d);
        }

        DirectDevice& owner;
    };

    // Buffer for one of the two system devices. Windows (WASAPI shared): always the device's own
    // period - smaller buffers there crackle - while the host still runs in the chosen block size;
    // the FIFO takes up the difference. Mac: the smallest supported size of at least the request.
    static int deviceBufferFor (AudioIODevice& device, int requested)
    {
       #if JUCE_WINDOWS
        if (const int period = device.getDefaultBufferSize(); period > 0)
            return period;
       #endif
        auto sizes = device.getAvailableBufferSizes();
        if (sizes.isEmpty())
            return requested;
        sizes.sort();
        for (auto size : sizes)
            if (size >= requested)
                return size;
        return sizes.getLast();
    }

    void forwardError (const String& e)
    {
        if (auto* callback = host.load())
            callback->audioDeviceError (e);
    }

    std::unique_ptr<AudioIODevice> output, input;
    InputSide inputSide { *this };
    OutputSide outputSide { *this };
    DriftFifo fifo;
    AudioBuffer<float> hostOutput;   // one host block
    int hostBlock = 480;
    std::atomic<AudioIODeviceCallback*> host { nullptr };
    std::atomic<int> dropoutCount { 0 };
    String lastError;
    bool opened = false, inputOpen = false, playing = false;
};

#if JUCE_WINDOWS
//==============================================================================
// Windows: a playback device used as input - records what is played into it (WASAPI loopback).
// This is how "Hi-Fi Cable Input", the device games play into, is recorded. Listed among the
// inputs as "<name> [Loopback]".
class LoopbackDevice : public AudioIODevice
{
public:
    static constexpr const char* suffix = " [Loopback]";

    LoopbackDevice (const String& playbackName, const String& typeName)
        : AudioIODevice (playbackName + suffix, typeName), playback (playbackName)
    {
        loopback::queryFormat (playback.toStdString(), format);
    }

    ~LoopbackDevice() override { close(); }

    StringArray getOutputChannelNames() override { return {}; }
    StringArray getInputChannelNames() override
    {
        // Windows channel order; 7.1 = front, centre, LFE, rear (back), side.
        static const char* const names71[] = { "Left", "Right", "Centre", "LFE", "Rear Left", "Rear Right", "Side Left", "Side Right" };
        // Format unknown (device not answering yet): offer 7.1; opening reports the real problem.
        const int channels = format.channels > 0 ? format.channels : 8;
        StringArray names;
        for (int ch = 0; ch < channels; ++ch)
            names.add (channels == 8 ? String (names71[ch]) : "Input " + String (ch + 1));
        return names;
    }

    Array<double> getAvailableSampleRates() override { return format.sampleRate > 0 ? Array<double> { format.sampleRate } : Array<double>(); }
    Array<int> getAvailableBufferSizes() override    { return { getDefaultBufferSize() }; }
    int getDefaultBufferSize() override              { return format.periodFrames > 0 ? format.periodFrames : 480; }

    String open (const BigInteger& ins, const BigInteger&, double rate, int) override
    {
        close();
        if (! loopback::queryFormat (playback.toStdString(), format))
            return lastError = "Playback device not found: " + playback;
        if (rate > 0 && std::abs (rate - format.sampleRate) > 0.5)
            return lastError = playback + " runs at " + String (format.sampleRate) + " Hz. Set it to "
                             + String (rate) + " Hz in the Windows sound settings (Properties > Advanced).";
        active = ins;
        active.setRange (format.channels, jmax (0, active.getHighestBit() + 1 - format.channels), false);
        activeList.clear();
        for (int ch = 0; ch < format.channels; ++ch)
            if (active[ch])
                activeList.push_back (ch);
        lastError = {};
        opened = true;
        return {};
    }

    void close() override
    {
        stop();
        opened = false;
    }

    bool isOpen() override { return opened; }

    void start (AudioIODeviceCallback* callback) override
    {
        if (! opened || callback == nullptr)
            return;
        stop();
        callback->audioDeviceAboutToStart (this);
        host = callback;
        lastError = capture.start (playback.toStdString(),
                                   [this] (const float* const* data, int numChannels, int numFrames)
                                   {
                                       std::array<const float*, 64> in {};
                                       int n = 0;
                                       for (auto ch : activeList)
                                           if (ch < numChannels && n < (int) in.size())
                                               in[(size_t) n++] = data[ch];
                                       if (auto* cb = host.load())
                                           cb->audioDeviceIOCallbackWithContext (in.data(), n, nullptr, 0, numFrames, {});
                                   });
        if (lastError.isNotEmpty())
        {
            host = nullptr;
            callback->audioDeviceStopped();
        }
    }

    void stop() override
    {
        capture.stop();
        if (auto* callback = host.exchange (nullptr))
            callback->audioDeviceStopped();
    }

    bool isPlaying() override                            { return capture.isRunning(); }
    String getLastError() override                       { return lastError; }
    int getCurrentBufferSizeSamples() override           { return getDefaultBufferSize(); }
    double getCurrentSampleRate() override               { return format.sampleRate; }
    int getCurrentBitDepth() override                    { return 32; }
    BigInteger getActiveOutputChannels() const override  { return {}; }
    BigInteger getActiveInputChannels() const override   { return active; }
    int getOutputLatencyInSamples() override             { return 0; }
    int getInputLatencyInSamples() override              { return getDefaultBufferSize(); }

private:
    String playback, lastError;
    loopback::Format format;
    loopback::Capture capture;
    BigInteger active;
    std::vector<int> activeList;
    std::atomic<AudioIODeviceCallback*> host { nullptr };
    bool opened = false;
};
#endif

//==============================================================================
// Lists the devices of the system's own audio API (WASAPI shared / Core Audio) and creates a
// DirectDevice for the chosen output and input. On Windows, playback devices are listed among the
// inputs too (as "<name> [Loopback]"), so the device games play into can be recorded directly.
class DirectAudioType : public AudioIODeviceType, private AudioIODeviceType::Listener
{
public:
    static constexpr const char* name = "Direct with drift correction";

    static std::unique_ptr<AudioIODeviceType> createSystemType()
    {
       #if JUCE_WINDOWS
        return std::unique_ptr<AudioIODeviceType> (createAudioIODeviceType_WASAPI (WASAPIDeviceMode::shared));
       #elif JUCE_MAC
        return std::unique_ptr<AudioIODeviceType> (createAudioIODeviceType_CoreAudio());
       #else
        return {};
       #endif
    }

    explicit DirectAudioType (std::unique_ptr<AudioIODeviceType> systemType)
        : AudioIODeviceType (name), inner (std::move (systemType))
    {
        inner->addListener (this);
    }

    ~DirectAudioType() override { inner->removeListener (this); }

    void scanForDevices() override
    {
        inner->scanForDevices();
       #if JUCE_WINDOWS
        // Every playback device of the output list can be recorded as an input, too.
        loopbackInputs.clear();
        for (auto& n : inner->getDeviceNames (false))
            loopbackInputs.add (n + LoopbackDevice::suffix);
       #endif
    }

    StringArray getDeviceNames (bool forInput) const override
    {
        auto names = inner->getDeviceNames (forInput);
       #if JUCE_WINDOWS
        if (forInput)
            names.addArray (loopbackInputs);
       #endif
        return names;
    }

    int getDefaultDeviceIndex (bool forInput) const override     { return inner->getDefaultDeviceIndex (forInput); }
    bool hasSeparateInputsAndOutputs() const override            { return true; }

    int getIndexOfDevice (AudioIODevice* d, bool asInput) const override
    {
        if (auto* direct = dynamic_cast<DirectDevice*> (d))
            return getDeviceNames (asInput).indexOf (asInput ? direct->getInputName() : direct->getOutputName());
        return -1;
    }

    AudioIODevice* createDevice (const String& outputName, const String& inputName) override
    {
        if (outputName.isEmpty())
            return nullptr;
        std::unique_ptr<AudioIODevice> out (inner->createDevice (outputName, {}));
        if (out == nullptr)
            return nullptr;
        std::unique_ptr<AudioIODevice> in;
       #if JUCE_WINDOWS
        if (inputName.endsWith (LoopbackDevice::suffix))
            in = std::make_unique<LoopbackDevice> (inputName.dropLastCharacters ((int) strlen (LoopbackDevice::suffix)), getTypeName());
        else
       #endif
        if (inputName.isNotEmpty())
            in.reset (inner->createDevice ({}, inputName));
        return new DirectDevice (getTypeName(), std::move (out), std::move (in));
    }

private:
    void audioDeviceListChanged() override
    {
        scanForDevices();
        callDeviceChangeListeners();
    }

    std::unique_ptr<AudioIODeviceType> inner;
   #if JUCE_WINDOWS
    StringArray loopbackInputs;
   #endif
};
} // namespace hth
