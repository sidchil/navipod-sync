// navipod-sync - sync an iPod Classic (6G/7G, stock firmware, "PC"/FAT32 format) with a Navidrome library. Not tested on Mac-formatted iPod.
//
// Talks to Navidrome through the Subsonic API and writes the iPod's iTunesDB via libgpod-ng (my libgpod fork)
// Anything on the iPod that navipod did not add itself (iTunes-synced music, videos, podcasts, the Photos folder / Photo Database) is never modified.
//

#include <gpod/itdb.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <tuple>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

static volatile std::sig_atomic_t g_stop = 0;
static int g_splWarnings = 0; // libgpod nonsense about iTunes smart playlists with rule types it doesn't know
static void logFilter(const gchar *dom, GLogLevelFlags lvl, const gchar *msg, gpointer)
{
    if (msg && (strstr(msg, "Unknown action type") || strstr(msg, "itdb_splr_validate")))
    {
        g_splWarnings++;
        return;
    }
    g_log_default_handler(dom, lvl, msg, nullptr);
}
static void onSignal(int) { g_stop = 1; }

// helpers
static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                   { return std::tolower(c); });
    return s;
}
static std::string humanSize(double b)
{
    const char *u[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (b >= 1024 && i < 4)
    {
        b /= 1024;
        i++;
    }
    char buf[32];
    snprintf(buf, sizeof buf, i ? "%.1f %s" : "%.0f %s", b, u[i]);
    return buf;
}
static std::string jstr(const json &j, const char *k)
{
    if (!j.contains(k) || j[k].is_null())
        return "";
    if (j[k].is_string())
        return j[k].get<std::string>();
    return j[k].dump();
}
static long long jint(const json &j, const char *k, long long d = 0)
{
    if (!j.contains(k) || j[k].is_null())
        return d;
    if (j[k].is_number())
        return j[k].get<long long>();
    try
    {
        return std::stoll(j[k].get<std::string>());
    }
    catch (...)
    {
        return d;
    }
}
static std::string readFile(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void writeFileAtomic(const fs::path &p, const std::string &data)
{
    fs::create_directories(p.parent_path());
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f << data;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec)
    {
        fs::remove(p, ec);
        fs::rename(tmp, p);
    }
}
static std::string promptHidden(const std::string &label)
{
    std::cerr << label << std::flush;
    std::string s;
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD m = 0;
    GetConsoleMode(h, &m);
    SetConsoleMode(h, m & ~ENABLE_ECHO_INPUT);
    std::getline(std::cin, s);
    SetConsoleMode(h, m);
#else
    termios t{};
    tcgetattr(STDIN_FILENO, &t);
    termios n = t;
    n.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &n);
    std::getline(std::cin, s);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
#endif
    std::cerr << "\n";
    return s;
}
static bool confirm(const std::string &q)
{
    std::cerr << q << " [y/N] " << std::flush;
    std::string s;
    std::getline(std::cin, s);
    return !s.empty() && (s[0] == 'y' || s[0] == 'Y');
}

// model
struct Song
{
    std::string id, title, artist, albumArtist, album, genre, suffix, coverArt;
    int year = 0, track = 0, disc = 0, duration = 0, bitRate = 0, userRating = 0;
    long long size = 0;
};
static Song parseSong(const json &j)
{
    Song s;
    s.id = jstr(j, "id");
    s.title = jstr(j, "title");
    s.artist = jstr(j, "artist");
    s.albumArtist = jstr(j, "albumArtist");
    s.album = jstr(j, "album");
    s.genre = jstr(j, "genre");
    s.suffix = lower(jstr(j, "suffix"));
    s.coverArt = jstr(j, "coverArt");
    s.year = (int)jint(j, "year");
    s.track = (int)jint(j, "track");
    s.disc = (int)jint(j, "discNumber");
    s.duration = (int)jint(j, "duration");
    s.bitRate = (int)jint(j, "bitRate");
    s.userRating = (int)jint(j, "userRating");
    s.size = jint(j, "size");
    if (s.albumArtist.empty())
        s.albumArtist = s.artist;
    return s;
}

struct Prefs
{
    bool all = false, starred = false, fill = false, manual = false, artwork = true, scrobble = true;
    std::vector<std::string> playlists, artists, albums, genres;
    int recent = 0, maxBitrate = 0, reserveMB = 512;
    std::string format = "mp3", model, guid;

    json toJson() const
    {
        return {{"all", all}, {"starred", starred}, {"fill", fill}, {"manual", manual}, {"artwork", artwork}, {"scrobble", scrobble}, {"playlists", playlists}, {"artists", artists}, {"albums", albums}, {"genres", genres}, {"recent", recent}, {"maxBitrate", maxBitrate}, {"reserveMB", reserveMB}, {"format", format}, {"model", model}, {"guid", guid}};
    }
    static Prefs fromJson(const json &j)
    {
        Prefs p;
        auto b = [&](const char *k, bool &v)
        { if (j.contains(k) && j[k].is_boolean()) v = j[k]; };
        auto v = [&](const char *k, std::vector<std::string> &o)
        { if (j.contains(k) && j[k].is_array()) o = j[k].get<std::vector<std::string>>(); };
        auto i = [&](const char *k, int &o)
        { if (j.contains(k) && j[k].is_number()) o = j[k]; };
        auto s = [&](const char *k, std::string &o)
        { if (j.contains(k) && j[k].is_string()) o = j[k]; };
        b("all", p.all);
        b("starred", p.starred);
        b("fill", p.fill);
        b("manual", p.manual);
        b("artwork", p.artwork);
        b("scrobble", p.scrobble);
        v("playlists", p.playlists);
        v("artists", p.artists);
        v("albums", p.albums);
        v("genres", p.genres);
        i("recent", p.recent);
        i("maxBitrate", p.maxBitrate);
        i("reserveMB", p.reserveMB);
        s("format", p.format);
        s("model", p.model);
        s("guid", p.guid);
        return p;
    }
};

// config (ONCE per computer)
struct Config
{
    std::string server, user, token, salt;
};

static fs::path configPath()
{
    fs::path base;
#ifdef _WIN32
    const char *a = getenv("APPDATA");
    base = a ? a : ".";
#elif defined(__APPLE__)
    const char *h = getenv("HOME");
    base = fs::path(h ? h : ".") / "Library/Application Support";
#else
    const char *x = getenv("XDG_CONFIG_HOME");
    const char *h = getenv("HOME");
    base = x ? fs::path(x) : fs::path(h ? h : ".") / ".config";
#endif
    return base / "navipod" / "config.json";
}
static bool loadConfig(Config &c)
{
    fs::path p = configPath();
    if (!fs::exists(p))
        return false;
    json j = json::parse(readFile(p), nullptr, false);
    if (j.is_discarded())
        return false;
    c.server = jstr(j, "server");
    c.user = jstr(j, "user");
    c.token = jstr(j, "token");
    c.salt = jstr(j, "salt");
    return !c.server.empty();
}
static void saveConfig(const Config &c)
{
    json j = {{"server", c.server}, {"user", c.user}, {"token", c.token}, {"salt", c.salt}};
    writeFileAtomic(configPath(), j.dump(2));
#ifndef _WIN32
    fs::permissions(configPath(), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
#endif
}

// Subsonic / Navidrome client
class Subsonic
{
public:
    explicit Subsonic(const Config &c) : cfg(c)
    {
        while (!cfg.server.empty() && cfg.server.back() == '/')
            cfg.server.pop_back();
        curl = curl_easy_init();
    }
    ~Subsonic()
    {
        if (curl)
            curl_easy_cleanup(curl);
    }
    using Params = std::vector<std::pair<std::string, std::string>>;

    std::string url(const std::string &ep, const Params &p = {})
    {
        std::string u = cfg.server + "/rest/" + ep + "?u=" + esc(cfg.user) + "&t=" + cfg.token + "&s=" + cfg.salt +
                        "&v=1.16.1&c=navipod&f=json";
        for (auto &kv : p)
            u += "&" + kv.first + "=" + esc(kv.second);
        return u;
    }
    json call(const std::string &ep, const Params &p = {})
    {
        std::string body;
        setup(url(ep, p));
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char *d, size_t s, size_t n, void *u)
                                                      {
            ((std::string *)u)->append(d, s * n); return s * n; });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        CURLcode rc = curl_easy_perform(curl);
        if (rc != CURLE_OK)
            throw std::runtime_error(std::string("network error: ") + curl_easy_strerror(rc));
        json j = json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.contains("subsonic-response"))
            throw std::runtime_error("unexpected response from server (is the URL right?)");
        json r = j["subsonic-response"];
        if (jstr(r, "status") != "ok")
            throw std::runtime_error("server error: " + (r.contains("error") ? jstr(r["error"], "message") : body));
        return r;
    }
    void download(const std::string &fullUrl, const fs::path &dest)
    {
        FILE *f = fopen(dest.string().c_str(), "wb");
        if (!f)
            throw std::runtime_error("cannot write " + dest.string());
        setup(fullUrl);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char *d, size_t s, size_t n, void *u)
                                                      { return fwrite(d, s, n, (FILE *)u); });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
        CURLcode rc = curl_easy_perform(curl);
        fclose(f);
        long code = 0;
        char *ct = nullptr;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);
        std::string type = ct ? lower(ct) : "";
        bool bad = rc != CURLE_OK || code != 200 || type.find("json") != std::string::npos ||
                   type.find("xml") != std::string::npos || type.find("html") != std::string::npos;
        if (bad)
        {
            std::error_code ec;
            fs::remove(dest, ec);
            throw std::runtime_error(rc != CURLE_OK ? curl_easy_strerror(rc) : "HTTP " + std::to_string(code) + " " + type);
        }
    }
    void ping() { call("ping"); }

    std::vector<Song> allSongs()
    {
        std::vector<Song> out;
        for (int off = 0;; off += 500)
        {
            json r = call("search3", {{"query", ""}, {"songCount", "500"}, {"songOffset", std::to_string(off)}, {"artistCount", "0"}, {"albumCount", "0"}});
            size_t n = 0;
            if (r.contains("searchResult3") && r["searchResult3"].contains("song"))
                for (auto &s : r["searchResult3"]["song"])
                {
                    out.push_back(parseSong(s));
                    n++;
                }
            if (n < 500)
                break;
        }
        if (out.empty())
        { // FALLBACK: walk albums
            for (auto &a : albums("alphabeticalByName", 100000))
                for (auto &s : albumSongs(a))
                    out.push_back(s);
        }
        return out;
    }
    std::vector<std::string> albums(const std::string &type, int count)
    {
        std::vector<std::string> ids;
        for (int off = 0; (int)ids.size() < count; off += 500)
        {
            int want = std::min(500, count - (int)ids.size());
            json r = call("getAlbumList2", {{"type", type}, {"size", std::to_string(want)}, {"offset", std::to_string(off)}});
            size_t n = 0;
            if (r.contains("albumList2") && r["albumList2"].contains("album"))
                for (auto &a : r["albumList2"]["album"])
                {
                    ids.push_back(jstr(a, "id"));
                    n++;
                }
            if ((int)n < want)
                break;
        }
        return ids;
    }
    std::vector<Song> albumSongs(const std::string &albumId)
    {
        std::vector<Song> out;
        json r = call("getAlbum", {{"id", albumId}});
        if (r.contains("album") && r["album"].contains("song"))
            for (auto &s : r["album"]["song"])
                out.push_back(parseSong(s));
        return out;
    }
    std::vector<Song> starred()
    {
        std::vector<Song> out;
        json r = call("getStarred2");
        if (r.contains("starred2") && r["starred2"].contains("song"))
            for (auto &s : r["starred2"]["song"])
                out.push_back(parseSong(s));
        return out;
    }
    std::vector<std::pair<std::string, std::string>> playlists()
    { // id, name
        std::vector<std::pair<std::string, std::string>> out;
        json r = call("getPlaylists");
        if (r.contains("playlists") && r["playlists"].contains("playlist"))
            for (auto &p : r["playlists"]["playlist"])
                out.push_back({jstr(p, "id"), jstr(p, "name")});
        return out;
    }
    std::vector<Song> playlistSongs(const std::string &id)
    {
        std::vector<Song> out;
        json r = call("getPlaylist", {{"id", id}});
        if (r.contains("playlist") && r["playlist"].contains("entry"))
            for (auto &s : r["playlist"]["entry"])
                out.push_back(parseSong(s));
        return out;
    }
    std::string md5(const std::string &s)
    {
        gchar *c = g_compute_checksum_for_string(G_CHECKSUM_MD5, s.c_str(), -1);
        std::string r = c;
        g_free(c);
        return r;
    }

private:
    Config cfg;
    CURL *curl = nullptr;
    std::string esc(const std::string &s)
    {
        char *e = curl_easy_escape(curl, s.c_str(), (int)s.size());
        std::string r = e;
        curl_free(e);
        return r;
    }
    void setup(const std::string &u)
    {
        curl_easy_reset(curl);
        curl_easy_setopt(curl, CURLOPT_URL, u.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "navipod/1.0");
    }
};

// iPod discovery/state
static bool isIpod(const fs::path &p)
{
    std::error_code ec;
    return fs::exists(p / "iPod_Control", ec);
}
static std::vector<fs::path> findIpods()
{
    std::vector<fs::path> out;
    std::vector<fs::path> roots;
    std::error_code ec;
#ifdef _WIN32
    for (char d = 'D'; d <= 'Z'; d++)
    {
        fs::path p = std::string(1, d) + ":\\";
        if (isIpod(p))
            out.push_back(p);
    }
    return out;
#else
    roots = {"/Volumes", "/media", "/mnt", "/run/media"};
    for (auto &r : roots)
    {
        if (!fs::is_directory(r, ec))
            continue;
        for (auto &e : fs::directory_iterator(r, ec))
        {
            if (isIpod(e.path()))
                out.push_back(e.path());
            else if (fs::is_directory(e.path(), ec)) // /media/user/label
                for (auto &e2 : fs::directory_iterator(e.path(), ec))
                    if (isIpod(e2.path()))
                        out.push_back(e2.path());
        }
    }
    return out;
#endif
}
static fs::path stateDir(const fs::path &m) { return m / "iPod_Control" / "navipod"; }
static json loadState(const fs::path &m)
{
    fs::path p = stateDir(m) / "state.json";
    if (fs::exists(p))
    {
        json j = json::parse(readFile(p), nullptr, false);
        if (!j.is_discarded())
            return j;
    }
    return json::object();
}
static void saveState(const fs::path &m, const json &j) { writeFileAtomic(stateDir(m) / "state.json", j.dump(1)); }

// CLI options
struct Opts
{
    std::string cmd, mount, server, user, password;
    bool dryRun = false, yes = false, eject = false, erase = false, force = false;
    Prefs p;
    bool selGiven = false, formatGiven = false, bitrateGiven = false, reserveGiven = false, manualGiven = false,
         artGiven = false, scrobbleGiven = false, fillGiven = false;
    std::vector<std::string> selAll; // placeholder
};

static void usage()
{
    std::cout <<
        R"(navipod - sync an iPod Classic with a Navidrome library

  navipod pair   --server URL --user NAME [--password PW] [--mount PATH] [--force]
        Save server login on this computer and link the iPod to this library.
  navipod info   [--mount PATH]          Show iPod model, space, contents and sync settings.
  navipod sync   [--mount PATH] [selection] [options]
  navipod status [same as sync]          Show what a sync would do (= sync --dry-run).
  navipod eject  [--mount PATH]          Safely eject the iPod.
  navipod unpair [--erase] [--mount PATH]  Forget the link; --erase also removes songs navipod added.

Selection (like iTunes' "Music" tab; given flags replace the saved selection, which is remembered per iPod):
  --all                  entire library
  --playlist NAME        sync a Navidrome playlist as an iPod playlist ("*" = all playlists). Repeatable.
  --artist NAME          all songs by artist.  Repeatable.
  --album NAME           album by name.        Repeatable.
  --genre NAME           genre.                Repeatable.
  --starred              favorites
  --recent N             N most recently added albums
  --fill                 afterwards fill remaining free space with random songs ("auto-fill")
  --clear                forget the saved selection
Options:
  --max-bitrate KBPS     transcode anything above this on the server ("convert higher bitrate songs")
  --format mp3|<fmt>     server transcoding format for files the iPod can't play (FLAC/OGG/OPUS...) [mp3]
  --manual               never delete songs from the iPod ("manually manage")      (--no-manual to undo)
  --reserve MB           keep this much space free [512]
  --no-artwork / --artwork
  --no-scrobble / --scrobble   report iPod play counts to Navidrome and sync ratings both ways
  --dry-run              change nothing      --yes   don't ask before deleting
  --eject                eject when finished
  --model MODEL          override model number (e.g. MB565) if the iPod isn't recognized after serial edits
  --guid HEX             override FireWire GUID used for the Classic's database signature (see README)
)";
}

static Opts parseArgs(int argc, char **argv)
{
    Opts o;
    if (argc < 2)
    {
        usage();
        exit(1);
    }
    o.cmd = argv[1];
    for (int i = 2; i < argc; i++)
    {
        std::string a = argv[i];
        auto val = [&]() -> std::string
        {
            if (i + 1 >= argc)
            {
                std::cerr << "missing value for " << a << "\n";
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--mount")
            o.mount = val();
        else if (a == "--server")
            o.server = val();
        else if (a == "--user")
            o.user = val();
        else if (a == "--password")
            o.password = val();
        else if (a == "--all")
        {
            o.p.all = true;
            o.selGiven = true;
        }
        else if (a == "--playlist")
        {
            o.p.playlists.push_back(val());
            o.selGiven = true;
        }
        else if (a == "--artist")
        {
            o.p.artists.push_back(val());
            o.selGiven = true;
        }
        else if (a == "--album")
        {
            o.p.albums.push_back(val());
            o.selGiven = true;
        }
        else if (a == "--genre")
        {
            o.p.genres.push_back(val());
            o.selGiven = true;
        }
        else if (a == "--starred")
        {
            o.p.starred = true;
            o.selGiven = true;
        }
        else if (a == "--recent")
        {
            o.p.recent = std::stoi(val());
            o.selGiven = true;
        }
        else if (a == "--clear")
            o.selGiven = true;
        else if (a == "--fill")
        {
            o.p.fill = true;
            o.fillGiven = true;
        }
        else if (a == "--max-bitrate")
        {
            o.p.maxBitrate = std::stoi(val());
            o.bitrateGiven = true;
        }
        else if (a == "--format")
        {
            o.p.format = lower(val());
            o.formatGiven = true;
        }
        else if (a == "--manual")
        {
            o.p.manual = true;
            o.manualGiven = true;
        }
        else if (a == "--no-manual")
        {
            o.p.manual = false;
            o.manualGiven = true;
        }
        else if (a == "--reserve")
        {
            o.p.reserveMB = std::stoi(val());
            o.reserveGiven = true;
        }
        else if (a == "--no-artwork")
        {
            o.p.artwork = false;
            o.artGiven = true;
        }
        else if (a == "--artwork")
        {
            o.p.artwork = true;
            o.artGiven = true;
        }
        else if (a == "--no-scrobble")
        {
            o.p.scrobble = false;
            o.scrobbleGiven = true;
        }
        else if (a == "--scrobble")
        {
            o.p.scrobble = true;
            o.scrobbleGiven = true;
        }
        else if (a == "--dry-run")
            o.dryRun = true;
        else if (a == "--yes" || a == "-y")
            o.yes = true;
        else if (a == "--eject")
            o.eject = true;
        else if (a == "--erase")
            o.erase = true;
        else if (a == "--force")
            o.force = true;
        else if (a == "--model")
            o.p.model = val();
        else if (a == "--guid")
            o.p.guid = val();
        else if (a == "-h" || a == "--help")
        {
            usage();
            exit(0);
        }
        else
        {
            std::cerr << "unknown option " << a << "\n";
            exit(2);
        }
    }
    return o;
}

static fs::path resolveMount(const Opts &o)
{
    if (!o.mount.empty())
    {
        if (!isIpod(o.mount))
            throw std::runtime_error(o.mount + " has no iPod_Control folder");
        return o.mount;
    }
    auto v = findIpods();
    if (v.empty())
        throw std::runtime_error("no iPod found; plug it in / mount it, or pass --mount PATH");
    if (v.size() > 1)
    {
        std::string m = "multiple iPods found, pass --mount:";
        for (auto &p : v)
            m += "\n  " + p.string();
        throw std::runtime_error(m);
    }
    return v[0];
}

// libgpod wrappers
struct Db
{
    Itdb_iTunesDB *db = nullptr;
    fs::path mount;
    ~Db()
    {
        if (db)
            itdb_free(db);
    }
    void write()
    {
        GError *err = nullptr;
        if (!itdb_write(db, &err))
        {
            std::string m = err ? err->message : "unknown error";
            if (err)
                g_error_free(err);
            throw std::runtime_error("writing iTunesDB failed: " + m);
        }
    }
};
static void openDb(Db &d, const fs::path &mount, const Prefs &p)
{
    GError *err = nullptr;
    d.mount = mount;
    d.db = itdb_parse(mount.string().c_str(), &err);
    if (!d.db)
    {
        std::string m = err ? err->message : "unknown";
        if (err)
            g_error_free(err);
        throw std::runtime_error("cannot read the iPod database: " + m +
                                 "\n(If the iPod is brand new / was restored, let iTunes or the Apple updater initialize it once first.)");
    }
    if (!p.model.empty())
        itdb_device_set_sysinfo(d.db->device, "ModelNumStr", p.model.c_str());
    if (!p.guid.empty())
        itdb_device_set_sysinfo(d.db->device, "FirewireGuid", p.guid.c_str());
}

// uhhh
static void removeTrack(Itdb_Track *t)
{
    itdb_playlist_remove_track(NULL, t);
    itdb_track_unlink(t);
    itdb_track_free(t);
}
static bool isManaged(const json &st, guint64 dbid, std::string *songId = nullptr)
{
    if (!st.contains("tracks"))
        return false;
    for (auto &kv : st["tracks"].items())
        if (jstr(kv.value(), "dbid") == std::to_string(dbid))
        {
            if (songId)
                *songId = kv.key();
            return true;
        }
    return false;
}

// transcoding decisions
struct Variant
{
    bool orig;
    std::string fmt, ext, key;
    int br;
};
static Variant pickVariant(const Song &s, const Prefs &p)
{
    static const std::set<std::string> native = {"mp3", "m4a", "m4b", "wav", "aif", "aiff"};
    bool tooBig = p.maxBitrate > 0 && s.bitRate > p.maxBitrate;
    Variant v;
    if (native.count(s.suffix) && !tooBig)
    {
        v = {true, "", s.suffix, "orig", s.bitRate};
        return v;
    }
    int br = p.maxBitrate > 0 ? p.maxBitrate : 320;
    std::string ext = (p.format == "aac" || p.format == "m4a") ? "m4a" : p.format;
    return {false, p.format, ext, p.format + "@" + std::to_string(br), br};
}
static long long estimateSize(const Song &s, const Variant &v)
{
    return v.orig ? s.size : (long long)s.duration * v.br * 125;
}

static void tuneFiletype(Itdb_Track *t, const Song &s, const Variant &v)
{
    std::string e = v.ext;
    if (e == "mp3")
    {
        t->filetype = g_strdup("MPEG audio file");
        t->filetype_marker = 0x4d503320;
    }
    else if (e == "m4a" || e == "m4b")
    {
        bool alac = v.orig && s.bitRate > 700;
        t->filetype = g_strdup(alac ? "Apple Lossless audio file" : "AAC audio file");
        t->filetype_marker = e == "m4b" ? 0x4d344220 : 0x4d344120;
    }
    else if (e == "wav")
    {
        t->filetype = g_strdup("WAV audio file");
        t->filetype_marker = 0x57415620;
    }
    else
    {
        t->filetype = g_strdup("AIFF audio file");
        t->filetype_marker = 0x41494646;
    }
}

// commands
static void checkDevice(Db &d, bool needWrite, const Prefs &p)
{
    const Itdb_IpodInfo *inf = itdb_device_get_ipod_info(d.db->device);
    auto gen = inf ? inf->ipod_generation : ITDB_IPOD_GENERATION_UNKNOWN;
    bool classic = gen == ITDB_IPOD_GENERATION_CLASSIC_1 || gen == ITDB_IPOD_GENERATION_CLASSIC_2 ||
                   gen == ITDB_IPOD_GENERATION_CLASSIC_3;
    gchar *guid = itdb_device_get_sysinfo(d.db->device, "FirewireGuid");
    bool haveGuid = guid && *guid;
    g_free(guid);
    if (needWrite)
    {
        if (!classic && p.model.empty())
            throw std::runtime_error(
                "this iPod isn't recognized as a Classic (the edited serial number probably hides the model).\n"
                "Pass --model with your original model number, e.g. --model MB565 (160GB 6G) or MC297 (160GB 2009).\n"
                "Writing a database with the wrong signature type would make the iPod show an empty library.");
        if (!haveGuid)
            throw std::runtime_error(
                "no FireWire GUID found in iPod_Control/Device/SysInfo(Extended); the Classic needs it to sign the database.\n"
                "Pass it with --guid (16 hex chars; see README).");
    }
}

static int cmdInfo(const Opts &o)
{
    fs::path m = resolveMount(o);
    json st = loadState(m);
    Prefs p = Prefs::fromJson(st.value("prefs", json::object()));
    if (!o.p.model.empty())
        p.model = o.p.model;
    if (!o.p.guid.empty())
        p.guid = o.p.guid;
    Db d;
    openDb(d, m, p);
    const Itdb_IpodInfo *inf = itdb_device_get_ipod_info(d.db->device);
    gchar *guid = itdb_device_get_sysinfo(d.db->device, "FirewireGuid");
    gchar *serial = itdb_device_get_sysinfo(d.db->device, "pszSerialNumber");
    std::error_code ec;
    auto sp = fs::space(m, ec);
    int audio = 0, video = 0, other = 0, managed = 0;
    for (GList *l = d.db->tracks; l; l = l->next)
    {
        Itdb_Track *t = (Itdb_Track *)l->data;
        if (isManaged(st, t->dbid))
            managed++;
        if (t->mediatype == ITDB_MEDIATYPE_AUDIO || t->mediatype == 0)
            audio++;
        else if (t->mediatype & (ITDB_MEDIATYPE_MOVIE | ITDB_MEDIATYPE_MUSICVIDEO | ITDB_MEDIATYPE_TVSHOW))
            video++;
        else
            other++;
    }
    std::cout << "Mount:         " << m << "\n"
              << "Model:         " << (inf && inf->model_number ? inf->model_number : "unknown")
              << (inf ? " (" + std::to_string((int)inf->capacity) + " GB model)" : "") << "\n"
              << "Generation:    " << (inf ? (int)inf->ipod_generation : -1) << " (libgpod enum)\n"
              << "Serial:        " << (serial ? serial : "?") << "\n"
              << "FireWire GUID: " << (guid && *guid ? guid : "NOT FOUND") << "\n"
              << "Space:         " << humanSize((double)sp.available) << " free of " << humanSize((double)sp.capacity) << "\n"
              << "Artwork:       " << (itdb_device_supports_artwork(d.db->device) ? "supported" : "not supported") << "\n"
              << "Tracks:        " << audio << " audio, " << video << " video, " << other << " other (" << managed << " added by navipod)\n"
              << "Playlists:     " << g_list_length(d.db->playlists) - 1 << "\n";
    g_free(guid);
    g_free(serial);
    if (st.contains("pair"))
        std::cout << "Paired with:   " << jstr(st["pair"], "server") << " as " << jstr(st["pair"], "user")
                  << " (since " << jstr(st["pair"], "since") << ")\n";
    else
        std::cout << "Paired with:   (not paired)\n";
    if (st.contains("prefs"))
        std::cout << "Saved sync settings:\n"
                  << st["prefs"].dump(2) << "\n";
    return 0;
}

static int cmdPair(const Opts &o)
{
    Config c;
    loadConfig(c);
    if (!o.server.empty())
        c.server = o.server;
    if (!o.user.empty())
        c.user = o.user;
    if (c.server.empty() || c.user.empty())
    {
        std::cerr << "need --server and --user\n";
        return 2;
    }
    std::string pw = o.password;
    if (pw.empty() && (o.server.size() || o.user.size() || c.token.empty()))
        pw = promptHidden("Navidrome password for " + c.user + ": ");
    if (!pw.empty())
    {
        Subsonic tmp(c);
        c.salt = tmp.md5(std::to_string(std::random_device{}()) + std::to_string(time(nullptr))).substr(0, 12);
        c.token = tmp.md5(pw + c.salt);
    }
    Subsonic api(c);
    api.ping();
    saveConfig(c);
    std::cout << "Logged in to " << c.server << " as " << c.user << ".\n";
    fs::path m = resolveMount(o);
    json st = loadState(m);
    if (st.contains("pair") && (jstr(st["pair"], "server") != c.server || jstr(st["pair"], "user") != c.user) && !o.force)
    {
        std::cerr << "This iPod is already linked to " << jstr(st["pair"], "server") << " (" << jstr(st["pair"], "user")
                  << "). Use --force to relink it (songs from the old library stay until the next sync removes them).\n";
        return 1;
    }
    char ts[32];
    time_t now = time(nullptr);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", localtime(&now));
    st["pair"] = {{"server", c.server}, {"user", c.user}, {"since", ts}};
    if (!st.contains("tracks"))
        st["tracks"] = json::object();
    saveState(m, st);
    std::cout << "iPod at " << m << " is now linked to this Navidrome library.\n"
              << "Next: navipod sync --all   (or --playlist/--artist/--album/--genre/--starred/--recent)\n";
    return 0;
}

static int cmdEject(const Opts &o)
{
    fs::path m = resolveMount(o);
    std::string path = m.string();
    for (char ch : path)
        if (ch == '\'' || ch == '"' || ch == '`' || ch == '$')
        {
            std::cerr << "unsafe path\n";
            return 1;
        }
#ifdef _WIN32
    std::string drive = path.substr(0, 2);
    std::string cmd = "powershell -NoProfile -Command \"(New-Object -comObject Shell.Application).Namespace(17).ParseName('" + drive + "').InvokeVerb('Eject')\"";
#elif defined(__APPLE__)
    std::string cmd = "diskutil eject '" + path + "'";
#else
    std::string cmd = "sync && (udisksctl unmount -p '" + path + "' >/dev/null 2>&1 || umount '" + path + "')";
#endif
    int rc = std::system(cmd.c_str());
    std::cout << (rc == 0 ? "Ejected. Safe to unplug.\n" : "Eject failed; unmount manually before unplugging.\n");
    return rc == 0 ? 0 : 1;
}

static int cmdUnpair(const Opts &o)
{
    fs::path m = resolveMount(o);
    json st = loadState(m);
    Prefs p = Prefs::fromJson(st.value("prefs", json::object()));
    if (!o.p.model.empty())
        p.model = o.p.model;
    if (!o.p.guid.empty())
        p.guid = o.p.guid;
    if (o.erase && st.contains("tracks") && !st["tracks"].empty())
    {
        Db d;
        openDb(d, m, p);
        checkDevice(d, true, p);
        std::vector<Itdb_Track *> kill;
        for (GList *l = d.db->tracks; l; l = l->next)
            if (isManaged(st, ((Itdb_Track *)l->data)->dbid))
                kill.push_back((Itdb_Track *)l->data);
        if (!o.yes && !confirm("Remove " + std::to_string(kill.size()) + " songs added by navipod?"))
            return 1;
        for (auto *t : kill)
        {
            gchar *f = itdb_filename_on_ipod(t);
            if (f)
            {
                g_remove(f);
                g_free(f);
            }
            removeTrack(t);
        }
        if (st.contains("playlists"))
            for (auto &n : st["playlists"])
            {
                std::string name = n;
                Itdb_Playlist *pl = itdb_playlist_by_name(d.db, (gchar *)name.c_str());
                if (pl && !itdb_playlist_is_mpl(pl))
                    itdb_playlist_remove(pl);
            }
        d.write();
    }
    std::error_code ec;
    fs::remove_all(stateDir(m), ec);
    std::cout << "iPod unlinked.\n";
    return 0;
}

struct Plan
{
    std::vector<Song> desired;
    std::vector<std::pair<std::string, std::vector<std::string>>> playlists;
};

static int cmdSync(Opts &o)
{
    bool dry = o.dryRun || o.cmd == "status";
    fs::path m = resolveMount(o);
    Config cfg;
    if (!loadConfig(cfg))
    {
        std::cerr << "not logged in: run `navipod pair --server URL --user NAME` first\n";
        return 1;
    }
    json st = loadState(m);
    if (!st.contains("pair"))
    {
        std::cerr << "this iPod isn't paired yet: run `navipod pair`\n";
        return 1;
    }
    if (jstr(st["pair"], "server") != cfg.server || jstr(st["pair"], "user") != cfg.user)
    {
        std::cerr << "This iPod is linked to " << jstr(st["pair"], "server") << " (" << jstr(st["pair"], "user")
                  << "), not to the library configured on this computer.\nRun `navipod pair --force` to relink it.\n";
        return 1;
    }

    // merge saved prefs with command-line flags
    Prefs p = Prefs::fromJson(st.value("prefs", json::object()));
    if (o.selGiven)
    {
        p.all = o.p.all;
        p.starred = o.p.starred;
        p.recent = o.p.recent;
        p.playlists = o.p.playlists;
        p.artists = o.p.artists;
        p.albums = o.p.albums;
        p.genres = o.p.genres;
        if (!o.fillGiven)
            p.fill = false;
    }
    if (o.fillGiven)
        p.fill = true;
    if (o.formatGiven)
        p.format = o.p.format;
    if (o.bitrateGiven)
        p.maxBitrate = o.p.maxBitrate;
    if (o.reserveGiven)
        p.reserveMB = o.p.reserveMB;
    if (o.manualGiven)
        p.manual = o.p.manual;
    if (o.artGiven)
        p.artwork = o.p.artwork;
    if (o.scrobbleGiven)
        p.scrobble = o.p.scrobble;
    if (!o.p.model.empty())
        p.model = o.p.model;
    if (!o.p.guid.empty())
        p.guid = o.p.guid;
    if (!(p.all || p.starred || p.recent || p.fill || !p.playlists.empty() || !p.artists.empty() || !p.albums.empty() || !p.genres.empty()))
    {
        std::cerr << "Nothing selected. Use --all, --playlist, --artist, --album, --genre, --starred, --recent or --fill.\n";
        return 1;
    }
    if (p.format != "mp3")
        std::cerr << "note: format '" << p.format << "' must be a transcoding configured in Navidrome that yields a file the iPod can play (mp3 is the safe choice)\n";

    Db d;
    openDb(d, m, p);
    checkDevice(d, !dry, p);
    Subsonic api(cfg);
    api.ping();

    // build list
    std::unordered_map<std::string, Song> byId;
    std::vector<Song> library;
    bool haveLib = false;
    auto lib = [&]() -> std::vector<Song> &
    {
        if (!haveLib)
        {
            std::cout << "Reading library...\n";
            library = api.allSongs();
            haveLib = true;
            for (auto &s : library)
                byId[s.id] = s;
        }
        return library;
    };
    Plan plan;
    std::unordered_set<std::string> seen;
    auto want = [&](const Song &s)
    { byId[s.id] = s; if (seen.insert(s.id).second) plan.desired.push_back(s); };
    auto ordered = [](std::vector<Song> v)
    {
        std::stable_sort(v.begin(), v.end(), [](const Song &a, const Song &b)
                         { return std::tie(a.albumArtist, a.album, a.disc, a.track) < std::tie(b.albumArtist, b.album, b.disc, b.track); });
        return v;
    };
    if (p.all)
        for (auto &s : ordered(lib()))
            want(s);
    auto matchAny = [](const std::vector<std::string> &names, const std::string &v)
    {
        for (auto &n : names) { if (lower(n) == lower(v)) return true; }
        return false; };
    if (!p.artists.empty() || !p.albums.empty() || !p.genres.empty())
    {
        std::vector<Song> sel;
        for (auto &s : lib())
            if (matchAny(p.artists, s.artist) || matchAny(p.artists, s.albumArtist) || matchAny(p.albums, s.album) || matchAny(p.genres, s.genre))
                sel.push_back(s);
        for (auto &s : ordered(sel))
            want(s);
        for (auto &n : p.artists)
            if (std::none_of(library.begin(), library.end(), [&](const Song &s)
                             { return lower(s.artist) == lower(n) || lower(s.albumArtist) == lower(n); }))
                std::cerr << "warning: no artist '" << n << "'\n";
        for (auto &n : p.albums)
            if (std::none_of(library.begin(), library.end(), [&](const Song &s)
                             { return lower(s.album) == lower(n); }))
                std::cerr << "warning: no album '" << n << "'\n";
        for (auto &n : p.genres)
            if (std::none_of(library.begin(), library.end(), [&](const Song &s)
                             { return lower(s.genre) == lower(n); }))
                std::cerr << "warning: no genre '" << n << "'\n";
    }
    if (p.starred)
        for (auto &s : api.starred())
            want(s);
    if (p.recent > 0)
        for (auto &id : api.albums("newest", p.recent))
            for (auto &s : api.albumSongs(id))
                want(s);
    if (!p.playlists.empty())
    {
        auto all = api.playlists();
        bool star = std::find(p.playlists.begin(), p.playlists.end(), "*") != p.playlists.end();
        for (auto &n : p.playlists)
            if (n != "*" && std::none_of(all.begin(), all.end(), [&](auto &x)
                                         { return lower(x.second) == lower(n); }))
                std::cerr << "warning: no playlist '" << n << "'\n";
        for (auto &pl : all)
        {
            if (!star && !matchAny(p.playlists, pl.second))
                continue;
            std::vector<std::string> ids;
            for (auto &s : api.playlistSongs(pl.first))
            {
                want(s);
                ids.push_back(s.id);
            }
            plan.playlists.push_back({pl.second, ids});
        }
    }

    // compare with what navipod previously put on the iPod
    std::unordered_map<guint64, Itdb_Track *> byDbid;
    for (GList *l = d.db->tracks; l; l = l->next)
        byDbid[((Itdb_Track *)l->data)->dbid] = (Itdb_Track *)l->data;
    json &tr = st["tracks"];
    if (!tr.is_object())
        tr = json::object();
    auto dbidOf = [&](const json &e) -> guint64
    { try { return std::stoull(jstr(e, "dbid")); } catch (...) { return 0; } };

    std::unordered_set<std::string> wantedIds;
    for (auto &s : plan.desired)
        wantedIds.insert(s.id);

    std::vector<std::string> toRemove;
    for (auto &kv : tr.items())
    {
        bool present = byDbid.count(dbidOf(kv.value())) > 0;
        if (!present)
        {
            toRemove.push_back(kv.key());
            continue;
        } // deleted on the iPod; forget (re-added if wanted)
        if (!wantedIds.count(kv.key()))
        {
            if (!p.manual)
                toRemove.push_back(kv.key());
            continue;
        }
        Variant v = pickVariant(byId[kv.key()], p);
        if (jstr(kv.value(), "variant") != v.key)
            toRemove.push_back(kv.key());
    }
    std::unordered_set<std::string> removeSet(toRemove.begin(), toRemove.end());

    if (p.fill)
    { // auto-fill random songs from the rest of the library
        std::vector<Song> rest;
        for (auto &s : lib())
            if (!seen.count(s.id))
                rest.push_back(s);
        std::mt19937 rng(std::random_device{}());
        std::shuffle(rest.begin(), rest.end(), rng);
        // keep previous random picks stable-ish: prefer ones already on the iPod
        std::stable_partition(rest.begin(), rest.end(), [&](const Song &s)
                              { return tr.contains(s.id) && !removeSet.count(s.id); });
        for (auto &s : rest)
            want(s);
    }

    std::error_code ec;
    auto sp = fs::space(m, ec);
    long long reserve = (long long)p.reserveMB * 1024 * 1024;
    long long budget = (long long)sp.available - reserve;
    for (auto &id : toRemove)
    {
        auto it = byDbid.find(dbidOf(tr[id]));
        if (it != byDbid.end())
            budget += it->second->size;
    }

    std::vector<std::pair<Song, Variant>> toAdd;
    long long addBytes = 0;
    int skippedSpace = 0, keep = 0;
    for (auto &s : plan.desired)
    {
        Variant v = pickVariant(s, p);
        bool onIpod = tr.contains(s.id) && !removeSet.count(s.id);
        if (onIpod)
        {
            keep++;
            continue;
        }
        long long est = estimateSize(s, v);
        if (addBytes + est > budget)
        {
            skippedSpace++;
            continue;
        }
        addBytes += est;
        toAdd.push_back({s, v});
    }
    std::cout << "Plan: " << toAdd.size() << " to add (" << humanSize((double)addBytes) << "), " << toRemove.size()
              << " to remove, " << keep << " already on iPod";
    if (skippedSpace)
        std::cout << ", " << skippedSpace << " skipped (not enough space)";
    std::cout << "\n";
    if (dry)
    {
        int n = 0;
        for (auto &a : toAdd)
        {
            if (++n > 25)
            {
                std::cout << "  ...\n";
                break;
            }
            std::cout << "  + " << a.first.artist << " - " << a.first.title << " [" << a.second.key << "]\n";
        }
        n = 0;
        for (auto &id : toRemove)
        {
            if (++n > 25)
            {
                std::cout << "  ...\n";
                break;
            }
            std::cout << "  - song " << id << "\n";
        }
        return 0;
    }
    if (!toRemove.empty() && !o.yes && !confirm("Remove " + std::to_string(toRemove.size()) + " songs from the iPod?"))
        return 1;

    // go
    st["prefs"] = p.toJson();
    saveState(m, st);
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // backup the databases (keep last 3)
    {
        char ts[32];
        time_t now = time(nullptr);
        strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", localtime(&now));
        fs::path bdir = stateDir(m) / "backup" / ts;
        fs::create_directories(bdir);
        for (const char *f : {"iTunes/iTunesDB", "iTunes/iTunesCDB", "iTunes/Play Counts", "iTunes/iTunesPrefs", "iTunes/iTunesStats"})
        {
            fs::path src = m / "iPod_Control" / f;
            if (fs::exists(src))
                fs::copy_file(src, bdir / src.filename(), fs::copy_options::overwrite_existing, ec);
        }
        std::vector<fs::path> bs;
        for (auto &e : fs::directory_iterator(stateDir(m) / "backup"))
            bs.push_back(e.path());
        std::sort(bs.begin(), bs.end());
        while (bs.size() > 3)
        {
            fs::remove_all(bs.front(), ec);
            bs.erase(bs.begin());
        }
    }

    // push iPod play counts/ratings to Navidrome
    json &pend = st["pendingScrobbles"];
    if (!pend.is_array())
        pend = json::array();
    if (p.scrobble)
    {
        for (auto &kv : tr.items())
        {
            auto it = byDbid.find(dbidOf(kv.value()));
            if (it == byDbid.end())
                continue;
            Itdb_Track *t = it->second;
            for (guint32 i = 0; i < t->recent_playcount; i++)
            {
                time_t when = t->time_played ? itdb_time_mac_to_host(t->time_played) : time(nullptr);
                pend.push_back({{"id", kv.key()}, {"time", (long long)when * 1000}});
            }
            t->recent_playcount = 0;
            if (t->rating != t->app_rating)
            {
                try
                {
                    api.call("setRating", {{"id", kv.key()}, {"rating", std::to_string(std::min<int>(5, t->rating / 20))}});
                    t->app_rating = (guint8)t->rating;
                }
                catch (std::exception &e)
                {
                    std::cerr << "rating sync failed: " << e.what() << "\n";
                }
            }
        }
        json left = json::array();
        size_t sent = 0;
        for (auto &s : pend)
        {
            try
            {
                api.call("scrobble", {{"id", jstr(s, "id")}, {"time", std::to_string(jint(s, "time"))}, {"submission", "true"}});
                sent++;
            }
            catch (std::exception &)
            {
                left.push_back(s);
            }
        }
        if (sent)
            std::cout << "Reported " << sent << " plays to Navidrome\n";
        pend = left;
    }

    // remove
    for (auto &id : toRemove)
    {
        auto it = byDbid.find(dbidOf(tr[id]));
        if (it != byDbid.end())
        {
            Itdb_Track *t = it->second;
            gchar *f = itdb_filename_on_ipod(t);
            if (f)
            {
                g_remove(f);
                g_free(f);
            }
            removeTrack(t);
            byDbid.erase(it);
        }
        tr.erase(id);
    }
    // drop previously created playlists; recreated below
    if (st.contains("playlists"))
        for (auto &n : st["playlists"])
        {
            std::string name = n;
            Itdb_Playlist *pl = itdb_playlist_by_name(d.db, (gchar *)name.c_str());
            if (pl && !itdb_playlist_is_mpl(pl))
                itdb_playlist_remove(pl);
        }
    st["playlists"] = json::array();

    // add
    fs::path tmp = fs::temp_directory_path() / ("navipod-" + std::to_string(std::random_device{}()));
    fs::create_directories(tmp);
    bool artOk = p.artwork && itdb_device_supports_artwork(d.db->device);
    std::map<std::string, fs::path> covers;
    Itdb_Playlist *mpl = itdb_playlist_mpl(d.db);
    std::mt19937_64 rng64(std::random_device{}());
    int done = 0, failed = 0, sinceWrite = 0, n = 0;
    for (auto &[s, v] : toAdd)
    {
        if (g_stop)
            break;
        n++;
        std::cout << "[" << n << "/" << toAdd.size() << "] " << s.artist << " - " << s.title << (v.orig ? "" : " (" + v.key + ")") << std::endl;
        fs::path file = tmp / (s.id + "." + v.ext);
        try
        {
            std::error_code e2;
            auto sp2 = fs::space(m, e2);
            if ((long long)sp2.available - reserve < estimateSize(s, v))
            {
                std::cerr << "  out of space, stopping\n";
                break;
            }
            std::string u = v.orig ? api.url("download", {{"id", s.id}})
                                   : api.url("stream", {{"id", s.id}, {"format", v.fmt}, {"maxBitRate", std::to_string(v.br)}, {"estimateContentLength", "true"}});
            api.download(u, file);
            Itdb_Track *t = itdb_track_new();
            t->title = g_strdup(s.title.c_str());
            t->artist = g_strdup(s.artist.c_str());
            t->albumartist = g_strdup(s.albumArtist.c_str());
            t->album = g_strdup(s.album.c_str());
            t->genre = g_strdup(s.genre.c_str());
            t->year = s.year;
            t->track_nr = s.track;
            t->cd_nr = s.disc ? s.disc : 1;
            t->tracklen = s.duration * 1000;
            t->bitrate = v.orig ? s.bitRate : v.br;
            t->samplerate = 44100;
            t->mediatype = ITDB_MEDIATYPE_AUDIO;
            t->dbid = rng64() | (1ULL << 62);
            t->dbid2 = t->dbid;
            t->time_added = itdb_time_get_mac_time();
            t->time_modified = t->time_added;
            t->rating = t->app_rating = (guint8)std::min(100, s.userRating * 20);
            t->compilation = lower(s.albumArtist) == "various artists" ? 1 : 0;
            tuneFiletype(t, s, v);
            itdb_track_add(d.db, t, -1);
            itdb_playlist_add_track(mpl, t, -1);
            GError *err = nullptr;
            if (!itdb_cp_track_to_ipod(t, file.string().c_str(), &err))
            {
                std::string msg = err ? err->message : "copy failed";
                if (err)
                    g_error_free(err);
                removeTrack(t);
                throw std::runtime_error(msg);
            }
            if (artOk && !s.coverArt.empty())
            {
                if (!covers.count(s.coverArt))
                {
                    fs::path cf = tmp / ("cover-" + std::to_string(covers.size()) + ".jpg");
                    try
                    {
                        api.download(api.url("getCoverArt", {{"id", s.coverArt}, {"size", "600"}}), cf);
                        covers[s.coverArt] = cf;
                    }
                    catch (std::exception &)
                    {
                        covers[s.coverArt] = "";
                    }
                }
                if (!covers[s.coverArt].empty())
                    itdb_track_set_thumbnails(t, covers[s.coverArt].string().c_str());
            }
            tr[s.id] = {{"dbid", std::to_string(t->dbid)}, {"variant", v.key}};
            byDbid[t->dbid] = t;
            done++;
            sinceWrite++;
        }
        catch (std::exception &e)
        {
            std::cerr << "  failed: " << e.what() << "\n";
            failed++;
        }
        std::error_code e3;
        fs::remove(file, e3);
        if (sinceWrite >= 250)
        { // checkpoint, interrupted sync never leaves orphaned files
            d.write();
            st["tracks"] = tr;
            saveState(m, st);
            sinceWrite = 0;
        }
    }

    // ratings from server for existing tracks (iPod-side edits already pushed above)
    for (auto &s : plan.desired)
    {
        if (!tr.contains(s.id))
            continue;
        auto it = byDbid.find(dbidOf(tr[s.id]));
        if (it == byDbid.end())
            continue;
        Itdb_Track *t = it->second;
        if (t->rating == t->app_rating && (int)t->rating != s.userRating * 20)
            t->rating = t->app_rating = (guint8)std::min(100, s.userRating * 20);
    }

    // playlists
    for (auto &pl : plan.playlists)
    {
        std::string name = pl.first;
        Itdb_Playlist *clash = itdb_playlist_by_name(d.db, (gchar *)name.c_str());
        if (clash)
            name += " (Navidrome)";
        Itdb_Playlist *ip = itdb_playlist_new(name.c_str(), FALSE);
        itdb_playlist_add(d.db, ip, -1);
        int cnt = 0;
        for (auto &sid : pl.second)
        {
            if (!tr.contains(sid))
                continue;
            auto it = byDbid.find(dbidOf(tr[sid]));
            if (it != byDbid.end())
            {
                itdb_playlist_add_track(ip, it->second, -1);
                cnt++;
            }
        }
        st["playlists"].push_back(name);
        std::cout << "Playlist '" << name << "': " << cnt << " tracks\n";
    }

    d.write();
    st["tracks"] = tr;
    saveState(m, st);
    std::error_code e4;
    fs::remove_all(tmp, e4);
    std::cout << (g_stop ? "Interrupted - progress saved. " : "") << "Done: " << done << " added, " << toRemove.size()
              << " removed, " << failed << " failed.\n";
    if (o.eject && !g_stop)
        return cmdEject(o);
    return failed ? 3 : 0;
}

int main(int argc, char **argv)
{
    try
    {
        Opts o = parseArgs(argc, argv);
        g_log_set_default_handler(logFilter, nullptr);
        struct SplNote
        {
            ~SplNote()
            {
                if (g_splWarnings)
                    std::cerr << "note: " << g_splWarnings << " libgpod warnings about existing smart playlists with unsupported rules were hidden (harmless for sync; check those playlists on the iPod afterwards).\n";
            }
        } splNote;
        curl_global_init(CURL_GLOBAL_DEFAULT);
        if (o.cmd == "help" || o.cmd == "--help" || o.cmd == "-h")
        {
            usage();
            return 0;
        }
        if (o.cmd == "pair")
            return cmdPair(o);
        if (o.cmd == "info")
            return cmdInfo(o);
        if (o.cmd == "sync" || o.cmd == "status")
            return cmdSync(o);
        if (o.cmd == "eject")
            return cmdEject(o);
        if (o.cmd == "unpair")
            return cmdUnpair(o);
        usage();
        return 2;
    }
    catch (std::exception &e)
    {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
