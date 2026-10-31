#include "KitFill.h"

using namespace juce;

namespace KitFill
{
    static bool isAudio (const File& f)
    {
        return f.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3");
    }

    Array<File> audioFiles (const Array<File>& in)
    {
        Array<File> out;
        for (auto& f : in)
        {
            if (f.isDirectory())
            {
                Array<File> found;
                for (const auto& e : RangedDirectoryIterator (f, false, "*", File::findFiles))
                    if (isAudio (e.getFile())) found.add (e.getFile());
                if (found.isEmpty())                                   // drum folders often hold one level of subfolders
                    for (const auto& e : RangedDirectoryIterator (f, true, "*", File::findFiles))
                        if (isAudio (e.getFile()) && e.getFile().getParentDirectory().getParentDirectory() == f) found.add (e.getFile());
                std::sort (found.begin(), found.end(), [] (const File& a, const File& b)
                           { return naturalCompare (a.getRelativePathFrom (a.getParentDirectory().getParentDirectory()),
                                                    b.getRelativePathFrom (b.getParentDirectory().getParentDirectory())) < 0; });
                out.addArray (found);
            }
            else if (f.existsAsFile() && isAudio (f)) out.add (f);
        }
        return out;
    }

    Group groupOf (const String& rawName)
    {
        const auto name = rawName.toLowerCase();
        StringArray tokens;
        tokens.addTokens (name.replaceCharacters ("_-.()[]&+,", "          "), " ", "");
        tokens.removeEmptyStrings();
        auto has = [&] (std::initializer_list<const char*> words)          // substring, for longer words
        {
            for (auto* w : words) if (name.contains (w)) return true;
            return false;
        };
        auto token = [&] (std::initializer_list<const char*> words)        // whole token, for short codes
        {
            for (auto& t : tokens)
                for (auto* w : words)
                {
                    const String ws (w);
                    if (t == ws) return true;
                    if (t.startsWith (ws) && t.substring (ws.length()).containsOnly ("0123456789")) return true;   // "bd02"
                }
            return false;
        };
        if (has ({ "kick", "kik", "bassdrum", "bass drum" }) || token ({ "bd", "kck" })) return kicks;
        if (has ({ "snare", "snr", "clap", "rimshot", "rim" }) || token ({ "sd", "cp", "clp", "sn" })) return snares;
        if (has ({ "hihat", "hi hat", "hat", "cymbal", "crash", "ride", "splash", "china" }) || token ({ "hh", "oh", "ch", "cym" })) return hats;
        if (has ({ "perc", "tom", "shaker", "shk", "conga", "bongo", "tamb", "cowbell", "bell", "block", "clave", "guiro", "cabasa", "snap", "fx" }))
            return perc;
        if (has ({ "bass", "sub", "keys", "piano", "rhodes", "organ", "pad", "synth", "chord", "lead", "pluck", "vox", "vocal", "string", "guitar", "brass" })
             || token ({ "808", "ep" }))
            return melodic;
        return other;
    }

    int naturalCompare (const String& a, const String& b)
    {
        auto pa = a.getCharPointer(), pb = b.getCharPointer();
        while (! pa.isEmpty() && ! pb.isEmpty())
        {
            if (pa.isDigit() && pb.isDigit())
            {
                int64 na = 0, nb = 0;
                while (pa.isDigit()) na = na * 10 + (*pa++ - '0');
                while (pb.isDigit()) nb = nb * 10 + (*pb++ - '0');
                if (na != nb) return na < nb ? -1 : 1;
                continue;
            }
            const auto ca = CharacterFunctions::toLowerCase (*pa), cb = CharacterFunctions::toLowerCase (*pb);
            if (ca != cb) return ca < cb ? -1 : 1;
            ++pa; ++pb;
        }
        return pa.isEmpty() ? (pb.isEmpty() ? 0 : -1) : 1;
    }

    Array<File> layout (Array<File> files, bool smart, bool wholeKit)
    {
        std::stable_sort (files.begin(), files.end(), [] (const File& a, const File& b)
                          { return naturalCompare (a.getFileNameWithoutExtension(), b.getFileNameWithoutExtension()) < 0; });
        if (! smart) return files;

        Array<File> groups[numGroups];
        for (auto& f : files) groups[groupOf (f.getFileNameWithoutExtension())].add (f);

        Array<File> slots;
        auto padTo = [&] (int n) { while (slots.size() < n) slots.add (File()); };
        for (int g = 0; g < numGroups; ++g)
        {
            if (groups[g].isEmpty()) continue;
            if (g == melodic && wholeKit && slots.size() <= 32) padTo (32);   // right half of the grid (note 68 up)
            else padTo ((slots.size() + 3) / 4 * 4);                            // each group starts on a new row of 4
            slots.addArray (groups[g]);
        }
        while (slots.size() > 0 && slots.getLast() == File()) slots.removeLast();
        return slots;
    }

    String padName (const File& f)
    {
        auto n = f.getFileNameWithoutExtension().replaceCharacters ("_", " ").trim();
        while (n.contains ("  ")) n = n.replace ("  ", " ");
        return n;
    }
}
