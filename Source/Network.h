#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include <functional>
#include <map>

// ---------------------------------------------------------------------------------------------
// Network locations (Windows file shares over SMB, via libsmb2).
//
// Files are never streamed during playback: a sample is downloaded once into a local cache
// (mirroring server/share/path), and from then on it is an ordinary local file. Kits store the
// cache path; if the cache is cleared, the sample is fetched again automatically.
// All network I/O runs on one background thread.
// ---------------------------------------------------------------------------------------------
namespace Net
{
    struct Location
    {
        juce::String name, server, share, folder, user, password;
        juce::String key() const { return server.trim().toLowerCase() + "/" + share.trim().toLowerCase(); }
        juce::String displayPath() const
        {
            auto p = "\\\\" + server + "\\" + share;
            if (folder.isNotEmpty()) p << "\\" << folder.replaceCharacter ('/', '\\');
            return p;
        }
    };

    struct Entry
    {
        juce::String name;
        bool isDir = false;
        juce::int64 size = 0;
    };

    // True when this build includes SMB support (not on Windows, which reaches shares natively).
    bool isAvailable();

    juce::File cacheRoot();
    juce::File cacheFileFor (const Location&, const juce::String& remotePath);
    // Reverse of cacheFileFor: is this a cached network file, and where did it come from?
    bool fromCacheFile (const juce::File&, juce::String& server, juce::String& share, juce::String& remotePath);

    // Joins share-relative paths with '/', ignoring empty parts.
    juce::String joinPath (const juce::String& a, const juce::String& b);

    class Library
    {
    public:
        Library();
        ~Library();

        // ---- saved locations (credentials stay on this device)
        std::vector<Location> getLocations() const;
        void saveLocation (const Location&);           // add or replace (matched by key)
        void removeLocation (const juce::String& key);
        bool findLocation (const juce::String& key, Location& out) const;

        // ---- async operations; callbacks arrive on the message thread
        using ListCallback  = std::function<void (bool ok, juce::Array<Entry>, juce::String error)>;
        using FetchCallback = std::function<void (juce::File file, juce::String error)>;
        void list  (const Location&, const juce::String& path, ListCallback);
        void fetch (const Location&, const juce::String& remotePath, FetchCallback);

        // Blocking download for when a file is needed right now (drag & drop, loading a kit).
        // Must not be called from the network thread itself.
        juce::File fetchNow (const Location&, const juce::String& remotePath, juce::String& error, int timeoutMs = 60000);

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
        std::unique_ptr<juce::PropertiesFile> store;
        mutable juce::CriticalSection storeLock;
        std::vector<Location> locations;
        void persist();
        JUCE_DECLARE_NON_COPYABLE (Library)
    };

    // One shared instance for the whole app (editor and processor).
    using SharedLibrary = juce::SharedResourcePointer<Library>;
}
