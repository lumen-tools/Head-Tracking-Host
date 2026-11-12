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

#include <array>
#include <atomic>
#include <cmath>
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

    Array<int> getAvailableBufferSizes() override { return (input != nullptr ? *input : *output).getAvailableBufferSizes(); }
    int getDefaultBufferSize() override           { return (input != nullptr ? *input : *output).getDefaultBufferSize(); }

    String open (const BigInteger& ins, const BigInteger& outs, double rate, int bufferSize) override
    {
        close();
        lastError = output->open ({}, outs, rate, bufferSize);
        if (lastError.isEmpty() && input != nullptr && ! ins.isZero())
        {
            lastError = input->open (ins, {}, rate, bufferSize);
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
        hostOutput.setSize (fifo.getNumChannels(), jmax (4096, inputBlock * 2));
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
    int getCurrentBufferSizeSamples() override   { return inputOpen ? input->getCurrentBufferSizeSamples() : output->getCurrentBufferSizeSamples(); }
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

    void forwardError (const String& e)
    {
        if (auto* callback = host.load())
            callback->audioDeviceError (e);
    }

    std::unique_ptr<AudioIODevice> output, input;
    InputSide inputSide { *this };
    OutputSide outputSide { *this };
    DriftFifo fifo;
    AudioBuffer<float> hostOutput;
    std::atomic<AudioIODeviceCallback*> host { nullptr };
    std::atomic<int> dropoutCount { 0 };
    String lastError;
    bool opened = false, inputOpen = false, playing = false;
};

//==============================================================================
// Lists the devices of the system's own audio API (WASAPI shared / Core Audio) and creates a
// DirectDevice for the chosen output and input.
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

    void scanForDevices() override                               { inner->scanForDevices(); }
    StringArray getDeviceNames (bool forInput) const override    { return inner->getDeviceNames (forInput); }
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
        if (inputName.isNotEmpty())
            in.reset (inner->createDevice ({}, inputName));
        return new DirectDevice (getTypeName(), std::move (out), std::move (in));
    }

private:
    void audioDeviceListChanged() override { callDeviceChangeListeners(); }

    std::unique_ptr<AudioIODeviceType> inner;
};
} // namespace hth
