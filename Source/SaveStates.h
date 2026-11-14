// Head Tracking Host
// Copyright (C) 2026 lumen-tools and contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Save states: each one is a single file ("<name>.hthstate", XML) in the folder "Save States"
// next to the program, holding every setting - plugin and its state, audio device, routing, head
// tracking, headphone correction, switches. One of them can be marked as the default, which is
// loaded at every start. Nothing is saved automatically.
//
// Besides the save states there is only a cache file ("Head Tracking Host.cache") with the plugin
// list of the VST3 scan and the startup crash guard - no settings. Deleting it is harmless.

#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <memory>
#include <vector>

namespace states
{
using namespace juce;

inline constexpr const char* extension = ".hthstate";
inline constexpr const char* rootTag = "HeadTrackingHostState";

// Portable: next to the program (EXE on Windows, .app on the Mac), so copying the folder takes
// everything along. Falls back to the user folder if that place is not writable (a protected
// folder, or macOS running the app from a hidden read-only "AppTranslocation" location).
inline File baseFolder()
{
    const auto app = File::getSpecialLocation (File::currentApplicationFile);
    const auto folder = app.getParentDirectory();
    const bool translocated = app.getFullPathName().contains ("/AppTranslocation/");
    if (! translocated && folder.hasWriteAccess())
        return folder;
   #if JUCE_MAC
    return File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("Application Support/Head Tracking Host");
   #else
    return File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("Head Tracking Host");
   #endif
}

inline File folder() { return baseFolder().getChildFile ("Save States"); }

inline File fileFor (const String& name)
{
    return folder().getChildFile (File::createLegalFileName (name.trim()) + extension);
}

struct Info
{
    String name;
    File file;
    bool isDefault = false;
};

inline std::unique_ptr<XmlElement> readXml (const File& file)
{
    auto xml = parseXML (file);
    if (xml == nullptr || ! xml->hasTagName (rootTag))
        return {};
    return xml;
}

// All save states, sorted by name.
inline std::vector<Info> list()
{
    std::vector<Info> result;
    for (auto& f : folder().findChildFiles (File::findFiles, false, String ("*") + extension))
    {
        Info info { f.getFileNameWithoutExtension(), f, false };
        if (auto xml = readXml (f))
            info.isDefault = xml->getBoolAttribute ("default", false);
        result.push_back (info);
    }
    std::sort (result.begin(), result.end(),
               [] (const Info& a, const Info& b) { return a.name.compareNatural (b.name) < 0; });
    return result;
}

inline File defaultFile()
{
    for (auto& s : list())
        if (s.isDefault)
            return s.file;
    return {};
}

// The settings stored in a save state, or nullptr if the file is missing or not a save state.
inline std::unique_ptr<PropertySet> read (const File& file)
{
    auto xml = readXml (file);
    if (xml == nullptr)
        return {};
    auto set = std::make_unique<PropertySet>();
    set->restoreFromXml (*xml);
    return set;
}

inline bool write (const File& file, const PropertySet& set, bool isDefault)
{
    auto xml = set.createXml (rootTag);
    xml->setAttribute ("version", 1);
    xml->setAttribute ("default", isDefault);
    return file.getParentDirectory().createDirectory().wasOk() && xml->writeTo (file); // writes via a temporary file
}

inline bool isDefault (const File& file)
{
    auto xml = readXml (file);
    return xml != nullptr && xml->getBoolAttribute ("default", false);
}

// Marks one save state as the default (none: an empty File) and unmarks all others.
inline void setDefault (const File& file)
{
    for (auto& s : list())
    {
        const bool wanted = s.file == file;
        if (s.isDefault == wanted)
            continue;
        if (auto xml = readXml (s.file))
        {
            xml->setAttribute ("default", wanted);
            xml->writeTo (s.file);
        }
    }
}

//==============================================================================
// The cache: plugin list of the VST3 scan and the startup crash guard.
inline std::unique_ptr<PropertiesFile> openCache()
{
    PropertiesFile::Options o;
    o.applicationName = "Head Tracking Host";
    o.storageFormat = PropertiesFile::storeAsXML;
    o.millisecondsBeforeSaving = 0;
    const auto file = baseFolder().getChildFile ("Head Tracking Host.cache");
    file.getParentDirectory().createDirectory();
    return std::make_unique<PropertiesFile> (file, o);
}

//==============================================================================
// Version 2.1 and older kept everything in "Head Tracking Host.settings" (settings of every
// plugin, five headphone correction slots, the plugin list). On the first start of this version
// it becomes the save state "Default" (with the plugin that was loaded last and the headphone
// correction that was chosen); the plugin list moves to the cache. The old files are removed.
inline File oldSettingsFile()
{
    const auto portable = baseFolder().getChildFile ("Head Tracking Host.settings");
    if (portable.existsAsFile())
        return portable;
    PropertiesFile::Options o;
    o.applicationName     = "Head Tracking Host";
    o.folderName          = "Head Tracking Host";
    o.filenameSuffix      = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    const auto user = o.getDefaultFile();
    return user.existsAsFile() ? user : File();
}

inline void migrateOldSettings (PropertiesFile& cache)
{
    const auto oldFile = oldSettingsFile();
    if (! oldFile.existsAsFile() || ! list().empty())
        return;

    PropertiesFile::Options o;
    o.applicationName = "Head Tracking Host";
    PropertiesFile old (oldFile, o);

    // Conversions older versions did at startup.
    if (! old.getBoolValue ("iemDefaults", false) && old.getIntValue ("oscPort", 8000) == 8000)
        old.setValue ("oscPort", 9000);
    if (! old.getBoolValue ("pitchConvention2", false))
    {
        for (auto& key : old.getAllProperties().getAllKeys())
            if (key.endsWith (".invertPitch"))
                old.setValue (key, ! old.getBoolValue (key));
        for (auto& key : old.getAllProperties().getAllKeys())
            if (key.endsWith (".paramPitch") || key.endsWith (".state"))
            {
                const auto prefix = key.upToLastOccurrenceOf (".", false, false);
                if (! old.containsKey (prefix + ".invertPitch"))
                    old.setValue (prefix + ".invertPitch", false);
            }
    }

    // Plugin list -> cache.
    if (auto xml = old.getXmlValue ("knownPlugins"))
        cache.setValue ("knownPlugins", xml.get());
    cache.setValue ("pluginsScanned", old.getBoolValue ("pluginsScanned", false));
    cache.saveIfNeeded();

    PropertySet state;
    for (auto key : { "layout", "speakerSim", "inputMap", "swapSurrounds", "autoeqEnabled", "volumeKeysFollow",
                      "headTracking", "oscPort", "headIndicator", "autoReconnect", "flexAsioStartSound",
                      "mixRearInto51", "audioDevice", "pluginPath", "pluginName" })
        if (old.containsKey (key))
            state.setValue (key, old.getValue (key));

    // The chosen headphone correction slot.
    const auto slot = "autoeqSlot" + String (old.getIntValue ("autoeqSlot", 0) + 1) + ".";
    if (old.containsKey (slot + "text"))
    {
        state.setValue ("eq.name", old.getValue (slot + "name"));
        state.setValue ("eq.text", old.getValue (slot + "text"));
    }

    // Settings of the plugin that was loaded last ("plugin.<id>.<setting>" -> "plugin.<setting>").
    const auto sub = old.getValue ("pluginName");
    const auto id = sub.isNotEmpty() ? sub : File (old.getValue ("pluginPath")).getFileNameWithoutExtension();
    if (id.isNotEmpty())
    {
        const auto prefix = "plugin." + id.removeCharacters (" .") + ".";
        for (auto& key : old.getAllProperties().getAllKeys())
            if (key.startsWith (prefix))
                state.setValue ("plugin." + key.substring (prefix.length()), old.getValue (key));
    }

    const bool written = write (fileFor ("Default"), state, true);
    old.setNeedsToBeSaved (false); // never write the converted values back into the old file
    if (written)
    {
        oldFile.deleteFile();
        for (auto name : { "Head Tracking Host AutoEq index.md", "Head Tracking Host AutoEq recommended.md" })
            oldFile.getSiblingFile (name).deleteFile();
    }
}
} // namespace states
