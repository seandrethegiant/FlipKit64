#pragma once
#include <juce_core/juce_core.h>

// Filling many pads at once: drop a folder or several samples, or "Load folder as kit".
// Pure helpers (no processor), so they're easy to test.
namespace KitFill
{
    enum Group { kicks = 0, snares, hats, perc, other, melodic, numGroups };

    // Audio files in a folder (and one level of subfolders), or the files given; folders expanded.
    juce::Array<juce::File> audioFiles (const juce::Array<juce::File>& filesOrFolders);

    // "BD_02.wav" -> kicks, "Sub Bass C1" -> melodic ... from the file name.
    Group groupOf (const juce::String& name);

    // Natural order: "Hit 2" before "Hit 10".
    int naturalCompare (const juce::String& a, const juce::String& b);

    // Where each file goes, as slots in pad (note) order counted from the first pad: File() = leave empty.
    // smart: group by sound type, each group starting on a new row of 4; melodic sounds start on the
    // right half (note 68) when filling a whole kit from note 36.
    juce::Array<juce::File> layout (juce::Array<juce::File> files, bool smart, bool wholeKit);

    // "BD_02_hard" -> "BD 02 hard"
    juce::String padName (const juce::File& f);
}
