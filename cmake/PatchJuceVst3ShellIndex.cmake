# Fix for VST3 shell plugins (e.g. Waves' WaveShell) in JUCE 8.0.12:
# JUCE remembers the chosen plugin only by its position in the shell's class list. Some shells
# change that list once the host context is set, so the position then points to a neighbour and
# the wrong plugin is created (e.g. "5.1/Stereo" instead of "7.1/Stereo"). The patch also
# remembers the class ID and looks the class up by that ID every time.
set(file "${JUCE_SOURCE}/modules/juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h")
file(READ "${file}" text)

if(text MATCHES "HTH_SHELL_INDEX_FIX")
    return()
endif()

function(replace_once old new)
    string(FIND "${text}" "${old}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "JUCE VST3 shell patch: text not found:\n${old}")
    endif()
    string(REPLACE "${old}" "${new}" text "${text}")
    set(text "${text}" PARENT_SCOPE)
endfunction()

replace_once([[
        result.classIndex = findClassMatchingDescription (factory, desc);

        if (result.classIndex == numClasses)
            return {};
]] [[
        result.classIndex = findClassMatchingDescription (factory, desc);

        if (result.classIndex == numClasses)
            return {};

        // HTH_SHELL_INDEX_FIX: remember the class by its ID, not only by its position.
        {
            PClassInfo info{};
            if (factory->getClassInfo (result.classIndex, &info) == kResultOk)
            {
                std::memcpy (result.classCid, info.cid, sizeof (TUID));
                result.hasClassCid = true;
            }
        }
]])

replace_once([[
    Steinberg::int32 getClassIndex() const
    {
        return classIndex;
    }
]] [[
    Steinberg::int32 getClassIndex() const
    {
        // HTH_SHELL_INDEX_FIX: shells may reorder their classes; find the class by its ID again.
        auto factory = getPluginFactory();

        if (hasClassCid && factory != nullptr)
        {
            const auto num = factory->countClasses();

            for (Steinberg::int32 i = 0; i < num; ++i)
            {
                PClassInfo info{};
                if (factory->getClassInfo (i, &info) == kResultOk
                    && std::memcmp (info.cid, classCid, sizeof (TUID)) == 0)
                    return i;
            }
        }

        return classIndex;
    }
]])

replace_once([[        factory->getClassInfo (classIndex, &info);]]
             [[        factory->getClassInfo (getClassIndex(), &info);]])

replace_once([[
    RefCountedDllHandle::Ptr handle;
    Steinberg::int32 classIndex{};
]] [[
    RefCountedDllHandle::Ptr handle;
    Steinberg::int32 classIndex{};
    TUID classCid{};
    bool hasClassCid = false;
]])

file(WRITE "${file}" "${text}")
message(STATUS "Applied JUCE VST3 shell plugin fix")
