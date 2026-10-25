#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

/**
    User presets as XML files in
    <user application data>/NovaTone/Presets/<name>.xml.

    Uses a dedicated "NovaTone" application data folder so presets never
    collide with HarmonyEngine.
*/
class PresetManager
{
public:
    static constexpr const char* initPresetName = "Default Vocal";

    PresetManager(juce::AudioProcessorValueTreeState& stateToManage,
                  std::function<void(const juce::ValueTree&)> applyStateFn,
                  std::function<void()> resetToDefaultsFn)
        : apvts(stateToManage), applyState(std::move(applyStateFn)), resetToDefaults(std::move(resetToDefaultsFn)) {}

    static juce::File getPresetDirectory()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("NovaTone").getChildFile("Presets");
    }

    juce::StringArray getPresetNames() const
    {
        juce::StringArray names;
        for (const auto& file : getPresetDirectory().findChildFiles(juce::File::findFiles, false, "*" + juce::String(extension)))
            names.add(file.getFileNameWithoutExtension());
        names.sortNatural();
        return names;
    }

    juce::String getCurrentPresetName() const
    {
        return apvts.state.getProperty(presetNameProperty, initPresetName).toString();
    }

    bool savePreset(const juce::String& requestedName)
    {
        const auto name = juce::File::createLegalFileName(requestedName.trim());
        if (name.isEmpty() || name == initPresetName)
            return false;

        const auto directory = getPresetDirectory();
        if (!directory.createDirectory())
            return false;

        apvts.state.setProperty(presetNameProperty, name, nullptr);
        const auto xml = apvts.copyState().createXml();
        return xml != nullptr && xml->writeTo(directory.getChildFile(name + extension));
    }

    bool loadPreset(const juce::String& name)
    {
        if (name == initPresetName)
        {
            resetToDefaults();
            apvts.state.setProperty(presetNameProperty, initPresetName, nullptr);
            return true;
        }

        const auto xml = juce::XmlDocument::parse(getPresetDirectory().getChildFile(name + extension));
        if (xml == nullptr || !xml->hasTagName(apvts.state.getType()))
            return false;

        auto tree = juce::ValueTree::fromXml(*xml);
        tree.setProperty(presetNameProperty, name, nullptr);
        applyState(tree);
        return true;
    }

private:
    static constexpr const char* extension = ".xml";
    static inline const juce::Identifier presetNameProperty { "presetName" };

    juce::AudioProcessorValueTreeState& apvts;
    std::function<void(const juce::ValueTree&)> applyState;
    std::function<void()> resetToDefaults;
};
