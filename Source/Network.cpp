#include "Network.h"

#if FLIPKIT_NETWORK
 #include <fcntl.h>
 #include <cerrno>
 extern "C" {
 #include <smb2/smb2.h>
 #include <smb2/libsmb2.h>
 }
#endif

using namespace juce;

namespace Net
{
// ============================================================================ helpers

bool isAvailable()
{
   #if FLIPKIT_NETWORK
    return true;
   #else
    return false;
   #endif
}

File cacheRoot()
{
    return File::getSpecialLocation (File::userApplicationDataDirectory)
               .getChildFile ("FlipKit64").getChildFile ("Network cache");
}

String joinPath (const String& a, const String& b)
{
    auto x = a.replaceCharacter ('\\', '/').trimCharactersAtStart ("/").trimCharactersAtEnd ("/");
    auto y = b.replaceCharacter ('\\', '/').trimCharactersAtStart ("/").trimCharactersAtEnd ("/");
    if (x.isEmpty()) return y;
    if (y.isEmpty()) return x;
    return x + "/" + y;
}

static String safePart (String s)
{
    s = s.trim();
    if (s.isEmpty() || s == "." || s == "..") s = "_";
    return s.replaceCharacter ('/', '_').replaceCharacter ('\\', '_').replaceCharacter (':', '_');
}

File cacheFileFor (const Location& loc, const String& remotePath)
{
    auto f = cacheRoot().getChildFile (safePart (loc.server.toLowerCase())).getChildFile (safePart (loc.share.toLowerCase()));
    for (auto& part : StringArray::fromTokens (remotePath.replaceCharacter ('\\', '/'), "/", ""))
        if (part.isNotEmpty()) f = f.getChildFile (safePart (part));
    return f;
}

bool fromCacheFile (const File& f, String& server, String& share, String& remotePath)
{
    if (! f.isAChildOf (cacheRoot())) return false;
    auto parts = StringArray::fromTokens (f.getRelativePathFrom (cacheRoot()).replaceCharacter ('\\', '/'), "/", "");
    parts.removeEmptyStrings();
    if (parts.size() < 3) return false;
    server = parts[0];
    share  = parts[1];
    parts.removeRange (0, 2);
    remotePath = parts.joinIntoString ("/");
    return true;
}

static String scramble (const String& s, bool encode)
{
    static const char* key = "FlipKit64-net";
    MemoryBlock data;
    if (encode) data = MemoryBlock (s.toRawUTF8(), s.getNumBytesAsUTF8());
    else { MemoryOutputStream mo; if (! Base64::convertFromBase64 (mo, s)) return {}; data = mo.getMemoryBlock(); }
    auto* p = static_cast<uint8*> (data.getData());
    for (size_t i = 0; i < data.getSize(); ++i) p[i] ^= (uint8) key[i % 13];
    return encode ? Base64::toBase64 (data.getData(), data.getSize())
                  : String::fromUTF8 (static_cast<const char*> (data.getData()), (int) data.getSize());
}

static bool isAudioName (const String& n)
{
    static const StringArray exts { ".wav", ".wave", ".aif", ".aiff", ".flac", ".ogg", ".mp3" };
    for (auto& e : exts) if (n.endsWithIgnoreCase (e)) return true;
    return false;
}

// ============================================================================ implementation

struct Library::Impl
{
    ThreadPool pool { ThreadPoolOptions{}.withThreadName ("FlipKit64 network").withNumberOfThreads (1) };

   #if FLIPKIT_NETWORK
    std::map<String, smb2_context*> conns;          // only touched on the pool thread

    ~Impl()
    {
        pool.removeAllJobs (true, 20000);
        for (auto& c : conns) { smb2_disconnect_share (c.second); smb2_destroy_context (c.second); }
    }

    // Plain-English error. libsmb2 keeps the Windows status code (nt) even when its message
    // text has been overwritten by a later socket error, so the code is checked first.
    static String friendly (smb2_context* ctx, int rc, const Location& loc, bool connecting)
    {
        const auto nt = (uint32) smb2_get_nterror (ctx);
        switch (nt)
        {
            case 0xc000006d: case 0xc000006a: case 0xc0000064: case 0xc0000070: case 0xc000006e:
                return "Windows didn't accept that username or password. (With a Microsoft account, use its email and password, not your PIN.)";
            case 0xc0000072: return "That Windows account is disabled.";
            case 0xc000015b: return "Windows doesn't allow this account to sign in over the network.";
            case 0xc00000cc: return "The PC has no shared folder called \"" + loc.share + "\". Check the share name on the PC.";
            case 0xc0000034: case 0xc000003a: return "That folder or file isn't there any more.";
            case 0xc0000022: return connecting ? "Windows says this account isn't allowed to open \"" + loc.share + "\". Check the share's permissions on the PC."
                                               : "Windows says this account isn't allowed to open that.";
            default: break;
        }
        if (rc == -ECONNREFUSED || rc == -ETIMEDOUT || rc == -EHOSTUNREACH || rc == -ENETUNREACH)
            return "Couldn't reach the PC at \"" + loc.server + "\". Check that it's on, on the same network "
                   "(or Tailscale), and that file sharing is turned on.";
        return friendly (String::fromUTF8 (smb2_get_error (ctx)), loc, connecting);
    }

    static String friendly (const String& raw, const Location& loc, bool connecting)
    {
        const auto r = raw.toLowerCase();
        if (r.contains ("logon_failure") || r.contains ("logon failure") || (connecting && r.contains ("access_denied")))
            return "Windows didn't accept that username or password.";
        if (r.contains ("bad_network_name"))
            return "The PC has no shared folder called \"" + loc.share + "\". Check the share name on the PC.";
        if (r.contains ("resolv") || r.contains ("invalid address") || r.contains ("getaddrinfo") || r.contains ("name or service"))
            return "Couldn't find a PC called \"" + loc.server + "\". Use the PC's IP address instead "
                   "(on the PC: Settings > Network & internet > Properties > IPv4 address).";
        if (r.contains ("refused") || r.contains ("timed out") || r.contains ("timeout") || r.contains ("unreachable") || r.contains ("connect"))
            return "Couldn't reach the PC at \"" + loc.server + "\". Check that it's on, on the same network "
                   "(or Tailscale), and that file sharing is turned on.";
        if (r.contains ("object_name_not_found") || r.contains ("object_path_not_found"))
            return "That folder or file isn't there any more.";
        if (r.contains ("access_denied"))
            return "Windows says this account isn't allowed to open that.";
        return raw.isNotEmpty() ? raw : String ("Network error");
    }

    static String connKey (const Location& l)
    {
        return l.key() + "|" + l.user + "|" + String::toHexString (l.password.hashCode64());
    }

    void drop (const String& key)
    {
        auto it = conns.find (key);
        if (it == conns.end()) return;
        smb2_destroy_context (it->second);
        conns.erase (it);
    }

    smb2_context* get (const Location& loc, String& err)
    {
        auto it = conns.find (connKey (loc));
        if (it != conns.end()) return it->second;

        auto* ctx = smb2_init_context();
        if (ctx == nullptr) { err = "Out of memory"; return nullptr; }
        smb2_set_security_mode (ctx, SMB2_NEGOTIATE_SIGNING_ENABLED);
        smb2_set_timeout (ctx, 15);

        auto user = loc.user.trim();
        if (user.contains ("\\"))                   // DOMAIN\user
        {
            smb2_set_domain (ctx, user.upToFirstOccurrenceOf ("\\", false, false).toRawUTF8());
            user = user.fromFirstOccurrenceOf ("\\", false, false);
        }
        smb2_set_user (ctx, user.toRawUTF8());
        smb2_set_password (ctx, loc.password.toRawUTF8());

        const int rc = smb2_connect_share (ctx, loc.server.trim().toRawUTF8(), loc.share.trim().toRawUTF8(), user.toRawUTF8());
        if (rc < 0)
        {
            err = friendly (ctx, rc, loc, true);
            smb2_destroy_context (ctx);
            return nullptr;
        }
        conns[connKey (loc)] = ctx;
        return ctx;
    }

    // Runs op with a cached connection; on failure reconnects once and retries.
    bool withConn (const Location& loc, String& err, const std::function<bool (smb2_context*, String&)>& op)
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            err.clear();
            auto* ctx = get (loc, err);
            if (ctx == nullptr) return false;
            if (op (ctx, err)) return true;
            if (smb2_get_nterror (ctx) != 0) return false;   // the PC answered (e.g. "not found"): connection is fine
            drop (connKey (loc));                    // stale or broken connection: start fresh
        }
        return false;
    }

    bool doList (const Location& loc, const String& path, Array<Entry>& out, String& err)
    {
        return withConn (loc, err, [&] (smb2_context* ctx, String& e)
        {
            out.clear();
            auto* dir = smb2_opendir (ctx, path.toRawUTF8());
            if (dir == nullptr) { e = friendly (ctx, 0, loc, false); return false; }
            while (auto* ent = smb2_readdir (ctx, dir))
            {
                const auto name = String::fromUTF8 (ent->name);
                if (name == "." || name == ".." || name.startsWith (".") || name.startsWith ("$")
                    || name == "System Volume Information" || name.equalsIgnoreCase ("desktop.ini") || name.equalsIgnoreCase ("thumbs.db"))
                    continue;
                Entry en;
                en.name = name;
                en.isDir = ent->st.smb2_type == SMB2_TYPE_DIRECTORY;
                en.size = (int64) ent->st.smb2_size;
                if (en.isDir || isAudioName (name)) out.add (en);
            }
            smb2_closedir (ctx, dir);
            std::sort (out.begin(), out.end(), [] (const Entry& a, const Entry& b)
            {
                if (a.isDir != b.isDir) return a.isDir;
                return a.name.compareNatural (b.name) < 0;
            });
            return true;
        });
    }

    bool doFetch (const Location& loc, const String& remotePath, File& result, String& err)
    {
        const auto dest = cacheFileFor (loc, remotePath);
        result = dest;
        return withConn (loc, err, [&] (smb2_context* ctx, String& e)
        {
            auto* fh = smb2_open (ctx, remotePath.toRawUTF8(), O_RDONLY);
            if (fh == nullptr) { e = friendly (ctx, 0, loc, false); return false; }

            struct smb2_stat_64 st {};
            const bool haveStat = smb2_fstat (ctx, fh, &st) == 0;
            if (haveStat && dest.existsAsFile() && dest.getSize() == (int64) st.smb2_size)
            {
                smb2_close (ctx, fh);                 // already cached and unchanged
                return true;
            }

            dest.getParentDirectory().createDirectory();
            const auto part = dest.getSiblingFile (dest.getFileName() + ".part");
            part.deleteFile();
            bool ok = true;
            {
                FileOutputStream out (part);
                if (! out.openedOk()) { e = "Can't write to the network cache folder."; ok = false; }
                const uint32 chunk = jlimit (65536u, 1u << 20, smb2_get_max_read_size (ctx));
                HeapBlock<uint8> buf (chunk);
                uint64_t pos = 0;
                while (ok)
                {
                    const int n = smb2_pread (ctx, fh, buf, chunk, pos);
                    if (n == -EAGAIN) continue;
                    if (n < 0) { e = friendly (ctx, n, loc, false); ok = false; break; }
                    if (n == 0) break;
                    if (! out.write (buf, (size_t) n)) { e = "Ran out of space for the network cache."; ok = false; break; }
                    pos += (uint64_t) n;
                }
                out.flush();
            }
            smb2_close (ctx, fh);
            if (! ok) { part.deleteFile(); return false; }
            dest.deleteFile();
            if (! part.moveFileTo (dest)) { e = "Couldn't save the downloaded file."; return false; }
            return true;
        });
    }
   #else
    ~Impl() { pool.removeAllJobs (true, 2000); }
   #endif
};

// ============================================================================ Library

Library::Library() : impl (std::make_unique<Impl>())
{
    PropertiesFile::Options o;
    o.applicationName = "FlipKit64-network";
    o.folderName = "FlipKit64";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    store = std::make_unique<PropertiesFile> (o);

    if (auto xml = store->getXmlValue ("locations"))
        for (auto* e : xml->getChildIterator())
        {
            Location l;
            l.name     = e->getStringAttribute ("name");
            l.server   = e->getStringAttribute ("server");
            l.share    = e->getStringAttribute ("share");
            l.folder   = e->getStringAttribute ("folder");
            l.user     = e->getStringAttribute ("user");
            l.password = scramble (e->getStringAttribute ("pw"), false);
            if (l.server.isNotEmpty() && l.share.isNotEmpty()) locations.push_back (l);
        }
}

Library::~Library() = default;

void Library::persist()
{
    XmlElement xml ("Locations");
    for (auto& l : locations)
    {
        auto* e = xml.createNewChildElement ("Location");
        e->setAttribute ("name", l.name);
        e->setAttribute ("server", l.server);
        e->setAttribute ("share", l.share);
        e->setAttribute ("folder", l.folder);
        e->setAttribute ("user", l.user);
        e->setAttribute ("pw", scramble (l.password, true));
    }
    store->setValue ("locations", &xml);
    store->saveIfNeeded();
}

std::vector<Location> Library::getLocations() const { const ScopedLock sl (storeLock); return locations; }

void Library::saveLocation (const Location& loc)
{
    const ScopedLock sl (storeLock);
    for (auto& l : locations)
        if (l.key() == loc.key()) { l = loc; persist(); return; }
    locations.push_back (loc);
    persist();
}

void Library::removeLocation (const String& key)
{
    const ScopedLock sl (storeLock);
    locations.erase (std::remove_if (locations.begin(), locations.end(), [&] (const Location& l) { return l.key() == key; }), locations.end());
    persist();
}

bool Library::findLocation (const String& key, Location& out) const
{
    const ScopedLock sl (storeLock);
    for (auto& l : locations)
        if (l.key() == key.toLowerCase()) { out = l; return true; }
    return false;
}

void Library::list (const Location& loc, const String& path, ListCallback cb)
{
   #if FLIPKIT_NETWORK
    impl->pool.addJob ([this, loc, path, cb]
    {
        Array<Entry> entries; String err;
        const bool ok = impl->doList (loc, path, entries, err);
        MessageManager::callAsync ([cb, ok, entries, err] { cb (ok, entries, err); });
    });
   #else
    ignoreUnused (loc, path);
    MessageManager::callAsync ([cb] { cb (false, {}, "Network locations aren't available in this version."); });
   #endif
}

void Library::fetch (const Location& loc, const String& remotePath, FetchCallback cb)
{
   #if FLIPKIT_NETWORK
    impl->pool.addJob ([this, loc, remotePath, cb]
    {
        File f; String err;
        if (! impl->doFetch (loc, remotePath, f, err)) f = File();
        MessageManager::callAsync ([cb, f, err] { cb (f, err); });
    });
   #else
    ignoreUnused (loc, remotePath);
    MessageManager::callAsync ([cb] { cb ({}, "Network locations aren't available in this version."); });
   #endif
}

File Library::fetchNow (const Location& loc, const String& remotePath, String& error, int timeoutMs)
{
   #if FLIPKIT_NETWORK
    struct Shared { WaitableEvent done; File file; String err; };
    auto shared = std::make_shared<Shared>();
    impl->pool.addJob ([this, loc, remotePath, shared]
    {
        if (! impl->doFetch (loc, remotePath, shared->file, shared->err)) shared->file = File();
        shared->done.signal();
    });
    if (! shared->done.wait (timeoutMs)) { error = "The download took too long."; return {}; }
    error = shared->err;
    return shared->file;
   #else
    ignoreUnused (loc, remotePath, timeoutMs);
    error = "Network locations aren't available in this version.";
    return {};
   #endif
}

} // namespace Net
