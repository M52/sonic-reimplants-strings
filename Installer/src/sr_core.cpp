#if !defined(_WIN32)
#define _FILE_OFFSET_BITS 64
#endif

#include "sr_core.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <thread>

extern "C" {
#include "wavpack.h"
#include "monocypher.h"
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#else
#include <dirent.h>
#include <errno.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/random.h>
#endif
#endif

#if defined(_WIN32)
static std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(wcslen(w.c_str()));
    return w;
}
static std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n : 1, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize(strlen(s.c_str()));
    return s;
}
static FILE* file_open(const std::string& path, const char* mode) {
    std::wstring wm(mode, mode + strlen(mode));
    return _wfopen(wide(path).c_str(), wm.c_str());
}
static bool file_seek(FILE* f, uint64_t pos) { return _fseeki64(f, (long long)pos, SEEK_SET) == 0; }
static bool file_replace(const std::string& from, const std::string& to) {
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
static void file_delete(const std::string& path) { DeleteFileW(wide(path).c_str()); }
static bool dir_make(const std::string& path) {
    return CreateDirectoryW(wide(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}
static const char SEP = '\\';
#else
static FILE* file_open(const std::string& path, const char* mode) { return fopen(path.c_str(), mode); }
static bool file_seek(FILE* f, uint64_t pos) { return fseeko(f, (off_t)pos, SEEK_SET) == 0; }
static bool file_replace(const std::string& from, const std::string& to) { return rename(from.c_str(), to.c_str()) == 0; }
static void file_delete(const std::string& path) { remove(path.c_str()); }
static bool dir_make(const std::string& path) { return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST; }
static const char SEP = '/';
#endif

static bool is_sep(char c) { return c == '/' || c == '\\'; }

std::string sr_path_join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return is_sep(a.back()) ? a + b : a + SEP + b;
}

std::string sr_path_base(const std::string& p) {
    size_t i = p.size();
    while (i > 0 && !is_sep(p[i - 1])) --i;
    return p.substr(i);
}

std::string sr_path_parent(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && is_sep(s.back())) s.pop_back();
    size_t i = s.size();
    while (i > 0 && !is_sep(s[i - 1])) --i;
    if (i == 0) return "";
    std::string parent = s.substr(0, i);
    if (parent.size() > 1 && is_sep(parent.back()) && !(parent.size() == 3 && parent[1] == ':')) parent.pop_back();
    if (parent == s) return "";
    return parent;
}

bool sr_name_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

bool sr_ends_with(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && sr_name_equal(s.substr(s.size() - n), suffix);
}

int64_t sr_file_size(const std::string& p) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(wide(p).c_str(), GetFileExInfoStandard, &d) ||
        (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    return (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
#else
    struct stat st;
    if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (int64_t)st.st_size;
#endif
}

bool sr_is_dir(const std::string& p) {
#if defined(_WIN32)
    DWORD a = GetFileAttributesW(wide(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static bool dir_make_all(const std::string& path) {
    if (sr_is_dir(path)) return true;
    std::string parent = sr_path_parent(path);
    if (!parent.empty() && parent != path) dir_make_all(parent);
    return dir_make(path);
}

bool sr_list_dir(const std::string& dir, std::vector<SrDirEntry>* out) {
    out->clear();
#if defined(_WIN32)
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wide(sr_path_join(dir, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        std::string name = narrow(fd.cFileName);
        if (name == "." || name == ".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        out->push_back({name, is_dir, is_dir ? 0 : (int64_t)(((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow)});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    while (struct dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.empty() || name[0] == '.') continue;
        struct stat st;
        if (stat(sr_path_join(dir, name).c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) out->push_back({name, true, 0});
        else if (S_ISREG(st.st_mode)) out->push_back({name, false, (int64_t)st.st_size});
    }
    closedir(d);
#endif
    std::sort(out->begin(), out->end(), [](const SrDirEntry& a, const SrDirEntry& b) {
        if (a.dir != b.dir) return a.dir;
        std::string x = a.name, y = b.name;
        for (char& c : x) c = (char)tolower((unsigned char)c);
        for (char& c : y) c = (char)tolower((unsigned char)c);
        return x < y;
    });
    return true;
}

std::vector<std::string> sr_roots() {
    std::vector<std::string> roots;
#if defined(_WIN32)
    wchar_t buf[512];
    DWORD n = GetLogicalDriveStringsW(511, buf);
    for (const wchar_t* p = buf; n && *p; p += wcslen(p) + 1) roots.push_back(narrow(p));
#else
    roots.push_back("/");
#endif
    return roots;
}

std::string sr_home_dir() {
#if defined(_WIN32)
    const wchar_t* h = _wgetenv(L"USERPROFILE");
    return h ? narrow(h) : "C:\\";
#else
    const char* h = getenv("HOME");
    return h ? h : "/";
#endif
}

void sr_reveal(const std::string& dir) {
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", wide(dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + dir + "\" &";
    if (system(cmd.c_str()) != 0) {}
#else
    std::string cmd = "xdg-open \"" + dir + "\" >/dev/null 2>&1 &";
    if (system(cmd.c_str()) != 0) {}
#endif
}

std::string sr_exe_dir() {
    std::string p;
#if defined(_WIN32)
    wchar_t w[4096];
    DWORD n = GetModuleFileNameW(nullptr, w, 4096);
    if (n > 0 && n < 4096) p = narrow(w);
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char real[4096];
        p = realpath(buf, real) ? real : buf;
    }
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) p.assign(buf, (size_t)n);
#endif
    std::string dir = sr_path_parent(p);
    return dir.empty() ? "." : dir;
}

static bool os_random(uint8_t* out, size_t size) {
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, out, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__linux__)
    while (size) {
        ssize_t n = getrandom(out, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        out += n;
        size -= (size_t)n;
    }
    return true;
#else
    while (size) {
        size_t n = size > 256 ? 256 : size;
        if (getentropy(out, n) != 0) return false;
        out += n;
        size -= n;
    }
    return true;
#endif
}

bool sr_rebuilder_init(SrRebuilder* who) {
    who->account.clear();
    who->display.clear();
    who->created = (uint64_t)time(nullptr);
#if defined(_WIN32)
    wchar_t name[1024];
    DWORD size = 1024;
    if (!GetUserNameW(name, &size)) return false;
    who->account = narrow(name);
    if (HMODULE security = LoadLibraryW(L"secur32.dll")) {
        typedef BOOLEAN(WINAPI * NameFn)(int, LPWSTR, PULONG);
        NameFn get_name = (NameFn)(void*)GetProcAddress(security, "GetUserNameExW");
        ULONG length = 1024;
        if (get_name && get_name(3  , name, &length)) who->display = narrow(name);
        FreeLibrary(security);
    }
    return true;
#else
    struct passwd entry, *result = nullptr;
    std::vector<char> buffer(32768);
    if (getpwuid_r(geteuid(), &entry, buffer.data(), buffer.size(), &result) != 0 || !result || !entry.pw_name)
        return false;
    who->account = entry.pw_name;
    if (entry.pw_gecos) who->display.assign(entry.pw_gecos, strcspn(entry.pw_gecos, ","));
    return true;
#endif
}

static const uint64_t P1 = 0x9E3779B185EBCA87ULL, P2 = 0xC2B2AE3D27D4EB4FULL, P3 = 0x165667B19E3779F9ULL,
                      P4 = 0x85EBCA77C2B2AE63ULL, P5 = 0x27D4EB2F165667C5ULL;

struct Xxh64 {
    uint64_t v[4], total;
    uint8_t buf[32];
    size_t used;
};

static uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
    return v;
}
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t xxh_round(uint64_t acc, uint64_t in) { return rotl64(acc + in * P2, 31) * P1; }
static uint64_t xxh_merge(uint64_t acc, uint64_t v) { return (acc ^ xxh_round(0, v)) * P1 + P4; }

static void xxh_begin(Xxh64* h) {
    h->v[0] = P1 + P2;
    h->v[1] = P2;
    h->v[2] = 0;
    h->v[3] = 0 - P1;
    h->total = 0;
    h->used = 0;
}

static void xxh_update(Xxh64* h, const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    h->total += size;
    if (h->used + size < 32) {
        memcpy(h->buf + h->used, p, size);
        h->used += size;
        return;
    }
    if (h->used) {
        size_t take = 32 - h->used;
        memcpy(h->buf + h->used, p, take);
        for (int i = 0; i < 4; ++i) h->v[i] = xxh_round(h->v[i], rd64(h->buf + 8 * i));
        p += take;
        size -= take;
        h->used = 0;
    }
    while (size >= 32) {
        h->v[0] = xxh_round(h->v[0], rd64(p));
        h->v[1] = xxh_round(h->v[1], rd64(p + 8));
        h->v[2] = xxh_round(h->v[2], rd64(p + 16));
        h->v[3] = xxh_round(h->v[3], rd64(p + 24));
        p += 32;
        size -= 32;
    }
    memcpy(h->buf, p, size);
    h->used = size;
}

static uint64_t xxh_end(const Xxh64* h) {
    uint64_t acc;
    const uint8_t* p = h->buf;
    size_t n = h->used;
    if (h->total >= 32) {
        acc = rotl64(h->v[0], 1) + rotl64(h->v[1], 7) + rotl64(h->v[2], 12) + rotl64(h->v[3], 18);
        for (int i = 0; i < 4; ++i) acc = xxh_merge(acc, h->v[i]);
    } else {
        acc = h->v[2] + P5;
    }
    acc += h->total;
    while (n >= 8) {
        acc ^= xxh_round(0, rd64(p));
        acc = rotl64(acc, 27) * P1 + P4;
        p += 8;
        n -= 8;
    }
    if (n >= 4) {
        acc ^= (uint64_t)rd32(p) * P1;
        acc = rotl64(acc, 23) * P2 + P3;
        p += 4;
        n -= 4;
    }
    while (n) {
        acc ^= (*p++) * P5;
        acc = rotl64(acc, 11) * P1;
        n--;
    }
    acc ^= acc >> 33;
    acc *= P2;
    acc ^= acc >> 29;
    acc *= P3;
    acc ^= acc >> 32;
    return acc;
}

static uint64_t xxh64(const void* data, size_t size) {
    Xxh64 h;
    xxh_begin(&h);
    xxh_update(&h, data, size);
    return xxh_end(&h);
}

static uint64_t fingerprint(const int32_t* values, size_t count) {
    Xxh64 h;
    uint8_t b[4096];
    size_t i = 0;
    xxh_begin(&h);
    while (i < count) {
        size_t n = 0;
        for (; i < count && n + 4 <= sizeof(b); ++i, n += 4) {
            uint32_t v = (uint32_t)values[i];
            b[n] = (uint8_t)v;
            b[n + 1] = (uint8_t)(v >> 8);
            b[n + 2] = (uint8_t)(v >> 16);
            b[n + 3] = (uint8_t)(v >> 24);
        }
        xxh_update(&h, b, n);
    }
    return xxh_end(&h);
}

static uint64_t le64(const uint8_t* p) { return rd64(p); }
static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t* p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list a;
    va_start(a, f);
    vsnprintf(buf, sizeof(buf), f, a);
    va_end(a);
    return buf;
}

static int b64_value(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static std::vector<std::string> split_words(const char* line) {
    std::vector<std::string> w;
    const char* p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') break;
        const char* start;
        if (*p == '"') {
            start = ++p;
            while (*p && *p != '"') p++;
        } else {
            start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        w.emplace_back(start, (size_t)(p - start));
        if (*p) p++;
    }
    return w;
}

struct Words {
    std::vector<std::string> w;
    bool ok = true;
    const std::string* after(const char* key) const {
        for (size_t i = 0; i + 1 < w.size(); ++i)
            if (w[i] == key) return &w[i + 1];
        return nullptr;
    }
    uint64_t num(const char* key) {
        const std::string* v = after(key);
        if (!v) {
            ok = false;
            return 0;
        }
        return strtoull(v->c_str(), nullptr, 10);
    }
    uint64_t hex(const char* key) {
        const std::string* v = after(key);
        if (!v) {
            ok = false;
            return 0;
        }
        return strtoull(v->c_str(), nullptr, 16);
    }
    std::string str(const char* key) const {
        const std::string* v = after(key);
        return v ? *v : std::string();
    }
};

static bool read_b64(FILE* f, uint8_t* out, size_t want) {
    char line[256];
    size_t got = 0;
    uint32_t acc = 0;
    int bits = 0;
    while (got < want && fgets(line, sizeof(line), f)) {
        for (char* p = line; *p; ++p) {
            int v = b64_value((unsigned char)*p);
            if (v < 0) continue;
            acc = acc << 6 | (uint32_t)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                if (got < want) out[got++] = (uint8_t)(acc >> bits);
            }
        }
    }
    return got == want;
}

static bool read_sketch(const Words& w, SrSample* s) {
    const std::string *env = w.after("env"), *pcm = w.after("pcm");
    size_t at = 0;
    while (at + 2 < w.w.size() && w.w[at] != "at") ++at;
    if (!env || !pcm || env->size() != 2 * SR_SKETCH_BLOCKS || at + 2 >= w.w.size()) return false;
    for (int k = 0; k < SR_SKETCH_BLOCKS; ++k) {
        char hex[3] = {(*env)[2 * k], (*env)[2 * k + 1], 0};
        if (!isxdigit((unsigned char)hex[0]) || !isxdigit((unsigned char)hex[1])) return false;
        s->env[k] = (uint8_t)strtoul(hex, nullptr, 16);
    }
    s->window_at[0] = strtoull(w.w[at + 1].c_str(), nullptr, 10);
    s->window_at[1] = strtoull(w.w[at + 2].c_str(), nullptr, 10);
    std::vector<uint8_t> bytes;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : *pcm) {
        int v = b64_value((unsigned char)c);
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back((uint8_t)(acc >> bits));
        }
    }
    uint64_t n = std::min<uint64_t>(s->frames, SR_SKETCH_WINDOW);
    if (s->channels < 1 || bytes.size() != 4 * n * (uint64_t)s->channels || s->window_at[0] + n > s->frames ||
        s->window_at[1] + n > s->frames)
        return false;
    s->window.resize(bytes.size() / 2);
    for (size_t i = 0; i < s->window.size(); ++i) s->window[i] = (int16_t)(bytes[2 * i] | bytes[2 * i + 1] << 8);
    s->has_sketch = true;
    return true;
}

static void fix_part_name(SrPart* p) {
    static const char* const fixes[][2] = {
        {"Violas Esprsessivo.gig", "Violas Espressivo.gig"},
    };
    for (const auto& f : fixes) {
        if (!sr_name_equal(p->name, f[0])) continue;
        p->alt_name = p->name;
        p->name = f[1];
        return;
    }
}

bool sr_recipe_read(const std::string& path, SrRecipe* r, std::string* err) {
    *r = SrRecipe{};
    r->file = path;
    FILE* f = file_open(path, "rb");
    if (!f) {
        *err = "Cannot open the file.";
        return false;
    }
    char line[4096];
    bool ok = true, have_head = false;
    uint64_t rows_hash = 0;
    while (ok && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        Words w;
        w.w = split_words(line);
        if (w.w.empty()) continue;
        const std::string& k = w.w[0];
        if (k == "recipe") {
            if (w.w.size() < 2 || atoi(w.w[1].c_str()) != 1) {
                *err = "It needs a newer version of this program.";
                ok = false;
            }
        } else if (k == "bank" && w.w.size() >= 2) {
            r->bank = w.w[1];
            r->bank_bytes = w.num("bytes");
            r->content_hash = w.hex("content_xxh64");
        } else if (k == "format") {
            r->rate = (uint32_t)w.num("rate");
            r->channels = (uint32_t)w.num("channels");
            r->bits = (uint32_t)w.num("bits");
            r->preload = (uint32_t)w.num("preload");
            r->block = (uint32_t)w.num("block");
            r->flags = (uint32_t)w.num("flags");
        } else if (k == "part" && w.w.size() >= 3) {
            SrPart p{};
            p.file = atoi(w.w[1].c_str());
            p.part = atoi(w.w[2].c_str());
            p.name = w.str("name");
            p.folder = w.str("folder");
            fix_part_name(&p);
            p.bytes = w.num("bytes");
            r->parts.push_back(p);
        } else if (k == "sample" && w.w.size() >= 2) {
            if (atoi(w.w[1].c_str()) != (int)r->samples.size()) {
                *err = "Its samples are in the wrong order.";
                ok = false;
                break;
            }
            SrSample s{};
            s.file = (int)w.num("file");
            s.gig = (int)w.num("gig");
            s.part = (int)w.num("part");
            s.offset = w.num("offset");
            s.bytes = w.num("bytes");
            s.frames = w.num("frames");
            s.channels = (int)w.num("channels");
            s.bits = (int)w.num("bits");
            s.rate = (uint32_t)w.num("rate");
            s.compressed = (int)w.num("compressed");
            s.source_fp = w.hex("source");
            s.steps = (int)w.num("steps");
            s.result_frames = (uint32_t)w.num("result_frames");
            s.result_bits = (int)w.num("result_bits");
            s.result_fp = w.hex("result");
            s.name = w.str("name");
            r->samples.push_back(s);
        } else if (k == "stream" && w.w.size() >= 2) {
            int i = atoi(w.w[1].c_str());
            if (i < 0 || i >= (int)r->samples.size()) {
                *err = "It has data for an unknown sample.";
                ok = false;
                break;
            }
            SrSample& s = r->samples[(size_t)i];
            s.has_stream = true;
            s.stream_offset = w.num("offset");
            s.stream_bytes = w.num("bytes");
            s.stream_first = w.num("first");
            s.stream_hash = w.hex("xxh64");
        } else if (k == "sketch" && w.w.size() >= 2) {
            int i = atoi(w.w[1].c_str());
            if (i < 0 || i >= (int)r->samples.size() || !read_sketch(w, &r->samples[(size_t)i])) {
                *err = "It has a damaged line.";
                ok = false;
                break;
            }
        } else if (k == "preload") {
            r->preload_offset = w.num("offset");
            r->preload_bytes = w.num("bytes");
            r->preload_hash = w.hex("xxh64");
        } else if (k == "head") {
            have_head = w.num("bytes") == 44 && read_b64(f, r->head, 44);
            if (!have_head) {
                *err = "Its bank header is damaged.";
                ok = false;
            }
        } else if (k == "rows") {
            uint64_t bytes = w.num("bytes");
            r->row_count = (uint32_t)w.num("count");
            rows_hash = w.hex("xxh64");
            if (!w.ok || bytes != (uint64_t)r->row_count * 128) {
                *err = "Its row table is damaged.";
                ok = false;
                break;
            }
            r->rows.resize((size_t)bytes);
            if (!read_b64(f, r->rows.data(), (size_t)bytes) || xxh64(r->rows.data(), (size_t)bytes) != rows_hash) {
                *err = "Its row table is damaged.";
                ok = false;
            }
        } else if (k == "end") {
            break;
        }
        if (!w.ok && ok) {
            *err = "It has a damaged line.";
            ok = false;
        }
    }
    fclose(f);
    if (ok && (!have_head || r->rows.empty() || r->bank.empty() || r->samples.empty())) {
        *err = "It is not complete.";
        ok = false;
    }
    return ok;
}

std::vector<SrRecipe> sr_recipes_load(const std::string& dir, std::vector<std::string>* errors) {
    std::vector<SrRecipe> out;
    std::vector<SrDirEntry> entries;
    if (!sr_list_dir(dir, &entries)) {
        if (errors) errors->push_back("Cannot read the recipes folder " + dir + ".");
        return out;
    }
    for (const SrDirEntry& e : entries) {
        if (e.dir || !sr_ends_with(e.name, ".recipe")) continue;
        SrRecipe r;
        std::string err;
        if (sr_recipe_read(sr_path_join(dir, e.name), &r, &err)) out.push_back(std::move(r));
        else if (errors) errors->push_back("Cannot use the recipe " + e.name + ". " + err);
    }
    std::sort(out.begin(), out.end(), [](const SrRecipe& a, const SrRecipe& b) { return a.bank < b.bank; });
    return out;
}

std::vector<std::string> sr_search_roots(const std::string& picked) {
    std::vector<std::string> roots;
    if (sr_is_dir(picked)) {
        roots.push_back(picked);
        return roots;
    }
    std::string dir = sr_path_parent(picked);
    for (int i = 0; i < 3 && !dir.empty(); ++i) {
        roots.push_back(dir);
        std::string up = sr_path_parent(dir);
        if (up == dir) break;
        dir = up;
    }
    return roots;
}

static void scan_dir(const std::string& dir, int depth, int max_depth, SrSearchProgress* p, std::vector<SrFound>* out) {
    if (depth > max_depth || (p && p->cancel.load())) return;
    std::vector<SrDirEntry> entries;
    if (!sr_list_dir(dir, &entries)) return;
    if (p) p->folders++;
    for (const SrDirEntry& e : entries) {
        std::string path = sr_path_join(dir, e.name);
        if (e.dir) {
            scan_dir(path, depth + 1, max_depth, p, out);
        } else if (sr_ends_with(e.name, ".gig") || sr_ends_with(e.name, ".gx01") || sr_ends_with(e.name, ".gx99")) {
            out->push_back({path, e.size});
            if (p) p->files++;
        }
    }
}

void sr_scan(const std::vector<std::string>& roots, int max_depth, SrSearchProgress* p, std::vector<SrFound>* out) {
    for (const std::string& root : roots) {
        std::vector<SrFound> found;
        scan_dir(root, 0, max_depth, p, &found);
        for (const SrFound& f : found) {
            bool have = false;
            for (const SrFound& g : *out)
                if (g.path == f.path) have = true;
            if (!have) out->push_back(f);
        }
    }
}

bool sr_match_parts(SrRecipe* r, const std::vector<SrFound>& files) {
    bool all = true;
    for (SrPart& p : r->parts) {
        int best = -1;
        const std::string* names[2] = {&p.name, &p.alt_name};
        for (int n = 0; n < 2 && best < 0; ++n) {
            if (names[n]->empty()) continue;
            for (int pass = 0; pass < 2 && best < 0; ++pass) {
                for (size_t j = 0; j < files.size(); ++j) {
                    const SrFound& f = files[j];
                    if ((pass == 0 && (uint64_t)f.size != p.bytes) || !sr_name_equal(sr_path_base(f.path), *names[n]))
                        continue;
                    if (best < 0 || sr_name_equal(sr_path_base(sr_path_parent(f.path)), p.folder)) best = (int)j;
                }
            }
        }
        p.path = best >= 0 ? files[(size_t)best].path : "";
        p.exact = best >= 0 && (uint64_t)files[(size_t)best].size == p.bytes;
        if (best < 0) all = false;
    }
    return all;
}

#define FSB_MAGIC 0x31425346u
#define PROV_MAGIC 0x52505346u
#define PROV_RECORD_BYTES 2160
#define PROV_PLAIN_BYTES 2048
#define ROW_BYTES 128

struct Row {
    uint64_t preload_frame_offset, preload_frame_count, total_frames, sample_frame_offset;
    uint32_t sample_index, sample_frames;
    uint64_t stream_offset, stream_size, stream_first_frame;
};

static Row row_at(const SrRecipe& r, uint32_t i) {
    const uint8_t* p = r.rows.data() + (size_t)i * ROW_BYTES;
    Row row;
    row.preload_frame_offset = le64(p + 64);
    row.preload_frame_count = le64(p + 72);
    row.total_frames = le64(p + 80);
    row.sample_frame_offset = le64(p + 88);
    row.sample_index = rd32(p + 96);
    row.sample_frames = rd32(p + 100);
    row.stream_offset = le64(p + 104);
    row.stream_size = le64(p + 112);
    row.stream_first_frame = le64(p + 120);
    return row;
}

static const char PROV_KEY_ID[] = "fsb-provenance-2026-01";
static const uint8_t PROV_PUBLIC_KEY[32] = {
    0x85, 0xa2, 0xce, 0x07, 0x23, 0x1a, 0xbc, 0x65, 0x75, 0x0d, 0xdb, 0xac, 0xd1, 0xd6, 0x1d, 0xbc,
    0x6e, 0x2c, 0x77, 0x7c, 0xb3, 0x53, 0xdf, 0xa0, 0x30, 0x12, 0x1a, 0x66, 0x5b, 0xa1, 0x19, 0x68,
};

enum {
    PF_VERSION = 0, PF_CREATED = 4, PF_CREATOR = 12, PF_CREATOR_VERSION = 44, PF_ACCOUNT = 108, PF_DISPLAY = 364,
    PF_SOURCE_KIND = 620, PF_SOURCE_FORMAT = 652, PF_SOURCE_ID = 668, PF_SOURCE_NAME = 796, PF_SOURCE_VENDOR = 924,
    PF_SOURCE_VERSION = 1052, PF_PRESET = 1116, PF_LIBRARY = 1372, PF_DISCLAIMER = 1628, PF_RESERVED = 1692
};

static size_t utf8_width(const uint8_t* s, size_t remaining) {
    if (!remaining) return 0;
    uint8_t c = s[0];
    if (c < 0x80) return 1;
    size_t n = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    if (!n || n > remaining) return 0;
    for (size_t i = 1; i < n; ++i)
        if ((s[i] & 0xc0) != 0x80) return 0;
    if (n == 3 && ((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] >= 0xa0))) return 0;
    if (n == 4 && ((c == 0xf0 && s[1] < 0x90) || (c == 0xf4 && s[1] >= 0x90))) return 0;
    return n;
}

static void prov_text(uint8_t* field, size_t capacity, const std::string& text) {
    size_t size = text.size(), at = 0, kept = 0;
    memset(field, 0, capacity);
    while (at < size) {
        size_t n = utf8_width((const uint8_t*)text.data() + at, size - at);
        if (!n) return;
        if (at + n < capacity) kept = at + n;
        at += n;
    }
    memcpy(field, text.data(), kept);
}

static bool prov_record(const SrRebuilder& who, const std::string& library, const std::string& source,
                        uint8_t record[PROV_RECORD_BYTES]) {
    uint8_t plain[PROV_PLAIN_BYTES] = {}, entropy[56], shared[32], session[32], zero[32] = {};
    crypto_blake2b_ctx hash;
    const char domain[] = "FSB-provenance-v1";
    put32(plain + PF_VERSION, 1);
    put64(plain + PF_CREATED, who.created);
    prov_text(plain + PF_CREATOR, 32, SR_CREATOR);
    prov_text(plain + PF_CREATOR_VERSION, 64, SR_VERSION);
    prov_text(plain + PF_ACCOUNT, 256, who.account);
    prov_text(plain + PF_DISPLAY, 256, who.display);
    prov_text(plain + PF_SOURCE_KIND, 32, "samples");
    prov_text(plain + PF_SOURCE_NAME, 128, source);
    prov_text(plain + PF_SOURCE_VENDOR, 128, "Sonic Implants");
    prov_text(plain + PF_LIBRARY, 256, library);

    memset(record, 0, PROV_RECORD_BYTES);
    if (!os_random(entropy, sizeof(entropy))) return false;
    put32(record, 1);
    put32(record + 4, 1);
    memcpy(record + 8, PROV_KEY_ID, sizeof(PROV_KEY_ID) - 1);
    crypto_x25519_public_key(record + 40, entropy);
    memcpy(record + 72, entropy + 32, 24);
    crypto_x25519(shared, entropy, PROV_PUBLIC_KEY);
    bool ok = crypto_verify32(shared, zero) != 0;
    if (ok) {
        crypto_blake2b_init(&hash, 32);
        crypto_blake2b_update(&hash, (const uint8_t*)domain, sizeof(domain) - 1);
        crypto_blake2b_update(&hash, shared, 32);
        crypto_blake2b_update(&hash, record + 40, 32);
        crypto_blake2b_update(&hash, PROV_PUBLIC_KEY, 32);
        crypto_blake2b_final(&hash, session);
        crypto_aead_lock(record + 112, record + 96, session, record + 72, record, 96, plain, sizeof(plain));
    }
    crypto_wipe(entropy, sizeof(entropy));
    crypto_wipe(shared, sizeof(shared));
    crypto_wipe(session, sizeof(session));
    crypto_wipe(plain, sizeof(plain));
    crypto_wipe(&hash, sizeof(hash));
    return ok;
}

struct Buffer {
    std::vector<uint8_t> data;
};

static int block_out(void* id, void* data, int32_t bcount) {
    Buffer* b = (Buffer*)id;
    const uint8_t* p = (const uint8_t*)data;
    b->data.insert(b->data.end(), p, p + bcount);
    return 1;
}

static bool pack_stream(const SrRecipe& r, int32_t* pcm, uint32_t frames, Buffer* out) {
    out->data.clear();
    WavpackConfig config;
    memset(&config, 0, sizeof(config));
    config.bits_per_sample = (int)r.bits;
    config.bytes_per_sample = r.bits == 16 ? 2 : 3;
    config.num_channels = (int)r.channels;
    config.sample_rate = (int32_t)r.rate;
    config.channel_mask = r.channels == 1 ? 4 : 3;
    config.block_samples = (int)r.block;
    config.flags = (int)r.flags;
    WavpackContext* wpc = WavpackOpenFileOutput(block_out, out, nullptr);
    if (!wpc) return false;
    bool ok = WavpackSetConfiguration(wpc, &config, frames) && WavpackPackInit(wpc);
    for (uint32_t done = 0; ok && done < frames;) {
        uint32_t n = frames - done;
        if (n > 65536) n = 65536;
        ok = WavpackPackSamples(wpc, pcm + (size_t)done * r.channels, n) != 0;
        done += n;
    }
    if (ok) ok = WavpackFlushSamples(wpc) != 0 && !out->data.empty();
    WavpackCloseFile(wpc);
    return ok;
}

static int part_index(const SrRecipe& r, int file, int part) {
    for (size_t i = 0; i < r.parts.size(); ++i)
        if (r.parts[i].file == file && r.parts[i].part == part) return (int)i;
    return -1;
}

static void decode_pcm(const uint8_t* src, int bits, size_t values, int32_t* out) {
    if (bits == 24) {
        for (size_t i = 0; i < values; ++i, src += 3)
            out[i] = (int32_t)((uint32_t)src[0] << 8 | (uint32_t)src[1] << 16 | (uint32_t)src[2] << 24) >> 8;
    } else {
        for (size_t i = 0; i < values; ++i, src += 2) out[i] = (int32_t)(int16_t)(src[0] | src[1] << 8);
    }
}

static void pack_pcm(const int32_t* src, size_t values, int bytes, uint8_t* out) {
    for (size_t i = 0; i < values; ++i) {
        uint32_t v = (uint32_t)src[i];
        *out++ = (uint8_t)v;
        *out++ = (uint8_t)(v >> 8);
        if (bytes == 3) *out++ = (uint8_t)(v >> 16);
    }
}

#define RIFF_ID(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define ID_RIFF RIFF_ID('R', 'I', 'F', 'F')
#define ID_LIST RIFF_ID('L', 'I', 'S', 'T')
#define ID_WVPL RIFF_ID('w', 'v', 'p', 'l')
#define ID_WAVE RIFF_ID('w', 'a', 'v', 'e')
#define ID_INFO RIFF_ID('I', 'N', 'F', 'O')
#define ID_INAM RIFF_ID('I', 'N', 'A', 'M')
#define ID_FMT RIFF_ID('f', 'm', 't', ' ')
#define ID_DATA RIFF_ID('d', 'a', 't', 'a')

struct GigWave {
    std::string name;
    uint64_t offset, bytes;
    uint32_t rate;
    int channels, bits;
};

struct GigSource {
    FILE* f = nullptr;
    uint64_t size = 0;
    bool indexed = false;
    std::vector<GigWave> waves;
};

struct RiffChunk {
    uint32_t id, type;
    uint64_t offset, size;
};

static bool riff_child(const GigSource& g, const RiffChunk& list, uint64_t* pos, RiffChunk* out) {
    uint64_t end = list.offset + list.size, header = g.size >= ((uint64_t)1 << 32) ? 12 : 8;
    uint8_t h[12];
    if (*pos >= end || end - *pos < header || !file_seek(g.f, *pos) || fread(h, 1, (size_t)header, g.f) != header)
        return false;
    uint64_t size = header == 12 ? rd64(h + 4) : rd32(h + 4), payload = *pos + header;
    if (size > end - payload) return false;
    out->id = rd32(h);
    out->type = 0;
    out->offset = payload;
    out->size = size;
    if (out->id == ID_LIST || out->id == ID_RIFF) {
        if (size < 4 || fread(h, 1, 4, g.f) != 4) return false;
        out->type = rd32(h);
        out->offset += 4;
        out->size -= 4;
    }
    *pos = payload + size + (size & 1);
    return true;
}

static std::string riff_text(const GigSource& g, const RiffChunk& ck) {
    std::string s((size_t)std::min<uint64_t>(ck.size, 1024), '\0');
    if (s.empty() || !file_seek(g.f, ck.offset) || fread(&s[0], 1, s.size(), g.f) != s.size()) return "";
    s.resize(strlen(s.c_str()));
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

static void gig_index(GigSource* g) {
    g->indexed = true;
    uint8_t h[4];
    uint64_t start = g->size >= ((uint64_t)1 << 32) ? 16 : 12;
    if (g->size < start || !file_seek(g->f, 0) || fread(h, 1, 4, g->f) != 4 || rd32(h) != ID_RIFF) return;
    RiffChunk root{ID_RIFF, 0, start, g->size - start}, wvpl{}, ck{}, sub{}, info{};
    uint64_t pos = root.offset;
    bool found = false;
    while (!found && riff_child(*g, root, &pos, &wvpl)) found = wvpl.id == ID_LIST && wvpl.type == ID_WVPL;
    if (!found) return;
    pos = wvpl.offset;
    while (riff_child(*g, wvpl, &pos, &ck)) {
        if (ck.id != ID_LIST || ck.type != ID_WAVE) continue;
        GigWave w{};
        bool has_data = false;
        uint64_t at = ck.offset;
        while (riff_child(*g, ck, &at, &sub)) {
            if (sub.id == ID_DATA) {
                w.offset = sub.offset;
                w.bytes = sub.size;
                has_data = true;
            } else if (sub.id == ID_FMT && sub.size >= 16) {
                uint8_t f[16];
                if (!file_seek(g->f, sub.offset) || fread(f, 1, 16, g->f) != 16) continue;
                w.channels = f[2] | f[3] << 8;
                w.rate = rd32(f + 4);
                w.bits = (f[0] | f[1] << 8) == 1 ? f[14] | f[15] << 8 : 0;
            } else if (sub.id == ID_LIST && sub.type == ID_INFO) {
                uint64_t in = sub.offset;
                while (riff_child(*g, sub, &in, &info))
                    if (info.id == ID_INAM) w.name = riff_text(*g, info);
            }
        }
        if (has_data) g->waves.push_back(w);
    }
}

#define SKETCH_TOLERANCE 6
#define SKETCH_LEVEL 3
#define SKETCH_FLOOR 168

static bool sketch_agrees(const SrSample& smp, const int32_t* v) {
    uint64_t frames = smp.frames, ch = (uint64_t)smp.channels;
    for (int k = 0; k < SR_SKETCH_BLOCKS; ++k) {
        uint64_t a = frames * k / SR_SKETCH_BLOCKS, b = frames * (k + 1) / SR_SKETCH_BLOCKS;
        double sum = 0.0;
        for (uint64_t i = a * ch; i < b * ch; ++i) sum += (double)v[i] * (double)v[i];
        int level = 255;
        if (b > a && sum > 0.0) {
            double db = 10.0 * log10(sum / (double)((b - a) * ch)) - 20.0 * log10(8388608.0);
            level = std::min(std::max((int)lround(-2.0 * db), 0), 255);
        }
        if (abs(std::min(level, SKETCH_FLOOR) - std::min((int)smp.env[k], SKETCH_FLOOR)) > SKETCH_LEVEL) return false;
    }
    uint64_t n = std::min<uint64_t>(frames, SR_SKETCH_WINDOW);
    const int16_t* want = smp.window.data();
    for (uint64_t start : smp.window_at) {
        for (uint64_t i = start * ch; i < (start + n) * ch; ++i) {
            int32_t q = std::min(std::max((v[i] + 128) >> 8, -32768), 32767);
            if (abs(q - *want++) > SKETCH_TOLERANCE) return false;
        }
    }
    return true;
}

enum SampleMatch {
    MATCH_SAME,
    MATCH_16BIT,
    MATCH_NAME,
};

static std::string read_sample(GigSource* g, const SrPart& part, const SrSample& smp, std::vector<uint8_t>* raw,
                               std::vector<int32_t>* pcm, int* match, std::string* warning) {
    *match = MATCH_SAME;
    warning->clear();
    size_t values = (size_t)smp.frames * (size_t)smp.channels;
    raw->resize((size_t)smp.bytes);
    pcm->resize(values);
    auto read_at = [&](uint64_t offset) {
        if (offset > g->size || smp.bytes > g->size - offset || !file_seek(g->f, offset) ||
            fread(raw->data(), 1, raw->size(), g->f) != raw->size())
            return false;
        decode_pcm(raw->data(), smp.bits, values, pcm->data());
        return fingerprint(pcm->data(), values) == smp.source_fp;
    };
    if (part.exact && read_at(smp.offset)) return "";
    if (!g->indexed) gig_index(g);
    auto fits = [&](const GigWave& w) {
        return w.bytes == smp.bytes && w.channels == smp.channels && w.bits == smp.bits && w.rate == smp.rate;
    };
    auto tried = [&](const GigWave& w) { return part.exact && w.offset == smp.offset; };
    bool named = false;
    for (const GigWave& w : g->waves) {
        if (!sr_name_equal(w.name, smp.name)) continue;
        named = true;
        if (fits(w) && !tried(w) && read_at(w.offset)) return "";
    }
    for (const GigWave& w : g->waves)
        if (!sr_name_equal(w.name, smp.name) && fits(w) && !tried(w) && read_at(w.offset)) return "";
    for (const GigWave& w : g->waves) {
        if (!smp.has_sketch || smp.bits != 24 || w.bits != 16 || !sr_name_equal(w.name, smp.name) ||
            w.channels != smp.channels || w.rate != smp.rate || w.bytes != values * 2)
            continue;
        raw->resize((size_t)w.bytes);
        if (w.offset > g->size || w.bytes > g->size - w.offset || !file_seek(g->f, w.offset) ||
            fread(raw->data(), 1, raw->size(), g->f) != raw->size())
            continue;
        decode_pcm(raw->data(), 16, values, pcm->data());
        for (int32_t& v : *pcm) v *= 256;
        if (sketch_agrees(smp, pcm->data())) {
            *match = MATCH_16BIT;
            return "";
        }
    }
    for (const GigWave& w : g->waves) {
        if (!sr_name_equal(w.name, smp.name) || w.channels != smp.channels || w.rate != smp.rate ||
            (w.bits != 16 && w.bits != 24))
            continue;
        uint64_t frame_bytes = (uint64_t)w.channels * (uint64_t)(w.bits / 8), frames = w.bytes / frame_bytes;
        uint64_t used = std::min<uint64_t>(frames, smp.frames);
        raw->resize((size_t)(used * frame_bytes));
        if (w.offset > g->size || raw->size() > g->size - w.offset || !file_seek(g->f, w.offset) ||
            fread(raw->data(), 1, raw->size(), g->f) != raw->size())
            continue;
        std::fill(pcm->begin(), pcm->end(), 0);
        decode_pcm(raw->data(), w.bits, (size_t)used * (size_t)smp.channels, pcm->data());
        if (w.bits < smp.bits)
            for (int32_t& v : *pcm) v *= 256;
        else if (w.bits > smp.bits)
            for (int32_t& v : *pcm) v = std::min((v + 128) >> 8, 32767);
        *match = MATCH_NAME;
        if (frames < smp.frames)
            *warning = fmt("Sample %s in %s is %llu frames shorter than the original sample. The program adds silence at the end.",
                           smp.name.c_str(), part.name.c_str(), (unsigned long long)(smp.frames - frames));
        else if (frames > smp.frames)
            *warning = fmt("Sample %s in %s is %llu frames longer than the original sample. The program removes the frames at the end.",
                           smp.name.c_str(), part.name.c_str(), (unsigned long long)(frames - smp.frames));
        else
            *warning = fmt("Sample %s in %s is different from the original sample.", smp.name.c_str(), part.name.c_str());
        return "";
    }
    if (named)
        return fmt("Sample %s in %s has a format that this program cannot use.", smp.name.c_str(), part.name.c_str());
    return fmt("Cannot find sample %s in %s.", smp.name.c_str(), part.name.c_str());
}

static void finish(SrBankProgress* p, int state, const std::string& error) {
    if (!error.empty()) p->error = error;
    p->state.store(state);
}

void sr_build_bank(const SrRecipe& r, const std::string& out_dir, const SrRebuilder& who, bool force,
                   const std::atomic<bool>* cancel, SrBankProgress* p) {
    p->samples_total = (int)r.samples.size();
    p->samples_done.store(0);
    std::string out_path = sr_path_join(out_dir, r.bank);
    std::string part_path = out_path + ".part";
    if (!force && sr_file_size(out_path) == (int64_t)r.bank_bytes) {
        p->samples_done.store(p->samples_total);
        finish(p, SR_BANK_SKIPPED, "");
        return;
    }
    p->state.store(SR_BANK_BUILDING);
    uint32_t header_zones = rd32(r.head + 20);
    uint64_t rows_offset = 44 + 16 + PROV_RECORD_BYTES, preload_offset = le64(r.head + 24);
    uint64_t preload_size = le64(r.head + 32);
    if (rd32(r.head) != FSB_MAGIC || rd32(r.head + 4) != 3 || header_zones != r.row_count ||
        preload_offset != rows_offset + (uint64_t)r.row_count * ROW_BYTES || preload_offset != r.preload_offset ||
        preload_size != r.preload_bytes || (r.bits != 16 && r.bits != 24) || (r.channels != 1 && r.channels != 2)) {
        finish(p, SR_BANK_FAILED, "The recipe has a bank layout that this program does not know.");
        return;
    }
    for (const SrPart& part : r.parts) {
        if (part.path.empty()) {
            finish(p, SR_BANK_FAILED, fmt("Cannot find %s.", part.name.c_str()));
            return;
        }
    }
    uint32_t bytes = r.bits == 16 ? 2 : 3;
    uint32_t frame_bytes = bytes * r.channels;

    uint8_t record[PROV_RECORD_BYTES], prov_head[16];
    std::string library = r.bank.size() > 4 ? r.bank.substr(0, r.bank.size() - 4) : r.bank;
    if (!prov_record(who, library, r.parts[0].name, record)) {
        finish(p, SR_BANK_FAILED, "Cannot encrypt the record. The system did not supply random numbers.");
        return;
    }
    put32(prov_head, PROV_MAGIC);
    put32(prov_head + 4, 1);
    put32(prov_head + 8, 1);
    put32(prov_head + 12, PROV_RECORD_BYTES);

    std::vector<uint8_t> preload((size_t)preload_size);
    std::vector<int32_t> pcm;
    std::vector<uint8_t> raw;
    Buffer stream;
    std::vector<GigSource> sources(r.parts.size());
    std::string error;
    int end_state = SR_BANK_FAILED;
    FILE* out = file_open(part_path, "wb");
    bool ok = out != nullptr;
    if (!ok) error = fmt("Cannot write %s.", part_path.c_str());
    if (ok) {
        ok = fwrite(r.head, 1, 44, out) == 44 && fwrite(prov_head, 1, 16, out) == 16 &&
             fwrite(record, 1, PROV_RECORD_BYTES, out) == PROV_RECORD_BYTES &&
             fwrite(r.rows.data(), ROW_BYTES, r.row_count, out) == r.row_count;
        if (!ok) error = "Cannot write the bank. The disk is possibly full.";
    }

    uint64_t expect_next = preload_offset + preload_size, recipe_next = expect_next;
    bool any_other = false;
    int sixteen = 0;
    std::vector<std::string> warnings;
    std::vector<uint64_t> stream_at(r.samples.size(), 0), stream_size(r.samples.size(), 0);
    for (size_t s = 0; ok && s < r.samples.size(); ++s) {
        if (cancel && cancel->load()) {
            ok = false;
            end_state = SR_BANK_CANCELLED;
            break;
        }
        const SrSample& smp = r.samples[s];
        int pi = part_index(r, smp.file, smp.part);
        size_t values = (size_t)smp.frames * (size_t)smp.channels;
        if (pi < 0 || smp.compressed || smp.steps != 0 || (uint32_t)smp.channels != r.channels ||
            (smp.bits != 16 && smp.bits != 24) || smp.rate != r.rate || smp.result_frames != smp.frames ||
            smp.bytes != values * (size_t)(smp.bits / 8)) {
            error = fmt("Sample %s needs a conversion that this program cannot do.", smp.name.c_str());
            ok = false;
            break;
        }
        const SrPart& part = r.parts[(size_t)pi];
        GigSource& src = sources[(size_t)pi];
        if (!src.f) {
            src.f = file_open(part.path, "rb");
            src.size = (uint64_t)std::max<int64_t>(sr_file_size(part.path), 0);
            if (!src.f) {
                error = fmt("Cannot open %s.", part.path.c_str());
                ok = false;
                break;
            }
        }
        int match = MATCH_SAME;
        std::string warning;
        error = read_sample(&src, part, smp, &raw, &pcm, &match, &warning);
        if (!error.empty()) {
            ok = false;
            break;
        }
        bool other = match != MATCH_SAME;
        any_other = any_other || other;
        if (match == MATCH_16BIT) sixteen++;
        if (!warning.empty()) warnings.push_back(warning);
        if ((int)r.bits > smp.bits)
            for (int32_t& v : pcm) v *= 256;

        int users = 0;
        for (uint32_t z = 0; ok && z < r.row_count; ++z) {
            Row row = row_at(r, z);
            if (row.sample_index != (uint32_t)s) continue;
            users++;
            if (row.sample_frames != smp.frames || row.sample_frame_offset + row.preload_frame_count > smp.frames ||
                (row.preload_frame_offset + row.preload_frame_count) * frame_bytes > preload_size) {
                error = fmt("Row %u is not correct for sample %s.", z, smp.name.c_str());
                ok = false;
                break;
            }
            pack_pcm(pcm.data() + (size_t)row.sample_frame_offset * r.channels,
                     (size_t)row.preload_frame_count * r.channels, (int)bytes,
                     preload.data() + (size_t)row.preload_frame_offset * frame_bytes);
        }
        if (!ok) break;

        if (smp.has_stream) {
            uint64_t end = smp.frames;
            if (users == 1) {
                for (uint32_t z = 0; z < r.row_count; ++z) {
                    Row row = row_at(r, z);
                    if (row.sample_index == (uint32_t)s) end = row.sample_frame_offset + row.total_frames;
                }
            }
            if (smp.stream_first >= end || end > smp.frames || smp.stream_offset != recipe_next) {
                error = fmt("The position of the data of sample %s is not correct.", smp.name.c_str());
                ok = false;
                break;
            }
            if (!pack_stream(r, pcm.data() + (size_t)smp.stream_first * r.channels,
                             (uint32_t)(end - smp.stream_first), &stream)) {
                error = fmt("Cannot compress sample %s.", smp.name.c_str());
                ok = false;
                break;
            }
            if (!other && (stream.data.size() != smp.stream_bytes ||
                                xxh64(stream.data.data(), stream.data.size()) != smp.stream_hash)) {
                error = fmt("The compressed data of sample %s is different from the original bank.", smp.name.c_str());
                ok = false;
                break;
            }
            if (!file_seek(out, expect_next) ||
                fwrite(stream.data.data(), 1, stream.data.size(), out) != stream.data.size()) {
                error = "Cannot write the bank. The disk is possibly full.";
                ok = false;
                break;
            }
            stream_at[s] = expect_next;
            stream_size[s] = stream.data.size();
            expect_next += stream.data.size();
            recipe_next += smp.stream_bytes;
        }
        p->samples_done.store((int)s + 1);
    }
    if (ok && !any_other && xxh64(preload.data(), preload.size()) != r.preload_hash) {
        error = "The first part of the samples is different from the original bank.";
        ok = false;
    }
    if (ok && recipe_next != r.bank_bytes) {
        error = fmt("The size of the bank is %llu bytes. The correct size is %llu bytes.", (unsigned long long)recipe_next,
                    (unsigned long long)r.bank_bytes);
        ok = false;
    }
    if (ok && any_other) {
        std::vector<uint8_t> rows = r.rows;
        for (uint32_t z = 0; z < r.row_count; ++z) {
            Row row = row_at(r, z);
            if (row.stream_size == 0) continue;
            if (row.sample_index >= r.samples.size() || stream_size[row.sample_index] == 0) {
                error = fmt("Row %u is not correct.", z);
                ok = false;
                break;
            }
            put64(rows.data() + (size_t)z * ROW_BYTES + 104, stream_at[row.sample_index]);
            put64(rows.data() + (size_t)z * ROW_BYTES + 112, stream_size[row.sample_index]);
        }
        if (ok && (!file_seek(out, rows_offset) || fwrite(rows.data(), ROW_BYTES, r.row_count, out) != r.row_count)) {
            error = "Cannot write the bank. The disk is possibly full.";
            ok = false;
        }
    }
    if (ok && (!file_seek(out, preload_offset) || fwrite(preload.data(), 1, preload.size(), out) != preload.size())) {
        error = "Cannot write the bank. The disk is possibly full.";
        ok = false;
    }
    for (GigSource& src : sources)
        if (src.f) fclose(src.f);
    if (out && fclose(out) != 0 && ok) {
        error = "Cannot write the bank. The disk is possibly full.";
        ok = false;
    }
    if (ok && !file_replace(part_path, out_path)) {
        error = fmt("Cannot replace %s. Close all programs that use this file.", out_path.c_str());
        ok = false;
    }
    if (!ok) {
        file_delete(part_path);
        finish(p, end_state, error);
        return;
    }
    p->samples_16bit = sixteen;
    p->warnings = warnings;
    finish(p, SR_BANK_DONE, "");
}

int sr_default_threads() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    if (n > 2) n -= 1;
    return (int)std::min(n, 12u);
}

void sr_build_all(const std::vector<SrRecipe*>& recipes, const std::string& out_dir, const SrRebuilder& who,
                  bool force, int threads, const std::atomic<bool>* cancel, std::vector<SrBankProgress>* progress) {
    dir_make_all(out_dir);
    std::vector<size_t> order(recipes.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
        (*progress)[i].samples_total = (int)recipes[i]->samples.size();
    }
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return recipes[a]->bank_bytes > recipes[b]->bank_bytes; });
    std::atomic<size_t> next{0};
    auto worker = [&]() {
        for (;;) {
            size_t k = next.fetch_add(1);
            if (k >= order.size()) return;
            size_t i = order[k];
            if (cancel && cancel->load()) {
                (*progress)[i].state.store(SR_BANK_CANCELLED);
                continue;
            }
            sr_build_bank(*recipes[i], out_dir, who, force, cancel, &(*progress)[i]);
        }
    };
    if (threads < 1) threads = 1;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
}

bool sr_verify_bank(const SrRecipe& r, const std::string& out_dir, std::string* err) {
    std::string path = sr_path_join(out_dir, r.bank);
    if (sr_file_size(path) != (int64_t)r.bank_bytes) {
        *err = "The bank is missing or has the wrong size.";
        return false;
    }
    FILE* f = file_open(path, "rb");
    uint8_t head[60];
    if (!f || fread(head, 1, 60, f) != 60) {
        if (f) fclose(f);
        *err = "Cannot read the bank.";
        return false;
    }
    uint64_t pos = 60 + (uint64_t)rd32(head + 52) * PROV_RECORD_BYTES;
    Xxh64 h;
    xxh_begin(&h);
    xxh_update(&h, head, 60);
    std::vector<uint8_t> buf(1 << 20);
    bool ok = file_seek(f, pos);
    while (ok && pos < r.bank_bytes) {
        size_t n = (size_t)std::min<uint64_t>(buf.size(), r.bank_bytes - pos);
        ok = fread(buf.data(), 1, n, f) == n;
        if (ok) xxh_update(&h, buf.data(), n);
        pos += n;
    }
    fclose(f);
    if (!ok || xxh_end(&h) != r.content_hash) {
        *err = "The bank is different from the original bank.";
        return false;
    }
    return true;
}

std::string sr_default_instruments_dir() {
    std::string parent = sr_path_parent(sr_exe_dir());
    return sr_path_join(parent.empty() ? std::string("..") : parent, "Instruments");
}

#define FSI_MAGIC 0x50495346u
#define FSI_FIELD_BANKS 19
#define FSI_BANK_FIELD_PATH 3
#define FSI_BANK_FIELD_TABLE_DIGEST 9
#define FSI_BANK_FIELD_PRELOAD_DIGEST 10

static bool pb_varint(const uint8_t* p, size_t end, size_t* at, uint64_t* out) {
    uint64_t v = 0;
    for (int shift = 0; shift < 64 && *at < end; shift += 7) {
        uint8_t b = p[(*at)++];
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

static bool pb_skip(const uint8_t* p, size_t end, size_t* at, int wire) {
    uint64_t n = 0;
    switch (wire) {
    case 0: return pb_varint(p, end, at, &n);
    case 1: *at += 8; return *at <= end;
    case 2:
        if (!pb_varint(p, end, at, &n) || n > end - *at) return false;
        *at += (size_t)n;
        return true;
    case 5: *at += 4; return *at <= end;
    default: return false;
    }
}

struct FsiBankRef {
    std::string path;
    size_t table_at = 0, preload_at = 0;
};

static bool fsi_bank_refs(const std::vector<uint8_t>& data, std::vector<FsiBankRef>* out) {
    if (data.size() < 8 || rd32(data.data()) != FSI_MAGIC) return false;
    size_t end = 8 + (size_t)rd32(data.data() + 4);
    if (end > data.size()) return false;
    const uint8_t* p = data.data();
    size_t at = 8;
    while (at < end) {
        uint64_t key = 0;
        if (!pb_varint(p, end, &at, &key)) return false;
        int field = (int)(key >> 3), wire = (int)(key & 7);
        if (field != FSI_FIELD_BANKS || wire != 2) {
            if (!pb_skip(p, end, &at, wire)) return false;
            continue;
        }
        uint64_t len = 0;
        if (!pb_varint(p, end, &at, &len) || len > end - at) return false;
        size_t bank_end = at + (size_t)len;
        FsiBankRef ref;
        while (at < bank_end) {
            uint64_t k = 0;
            if (!pb_varint(p, bank_end, &at, &k)) return false;
            int f = (int)(k >> 3), w = (int)(k & 7);
            if (f == FSI_BANK_FIELD_PATH && w == 2) {
                uint64_t n = 0;
                if (!pb_varint(p, bank_end, &at, &n) || n > bank_end - at) return false;
                ref.path.assign((const char*)p + at, (size_t)n);
                at += (size_t)n;
            } else if ((f == FSI_BANK_FIELD_TABLE_DIGEST || f == FSI_BANK_FIELD_PRELOAD_DIGEST) && w == 1) {
                if (bank_end - at < 8) return false;
                (f == FSI_BANK_FIELD_TABLE_DIGEST ? ref.table_at : ref.preload_at) = at;
                at += 8;
            } else if (!pb_skip(p, bank_end, &at, w)) {
                return false;
            }
        }
        out->push_back(ref);
    }
    return true;
}

static std::string fsi_bank_path(const std::string& fsi_dir, const std::string& path) {
    bool absolute = !path.empty() && (is_sep(path[0]) || (path.size() > 1 && path[1] == ':'));
    std::string p = absolute ? path : sr_path_join(fsi_dir, path);
#if defined(_WIN32)
    for (char& c : p) if (c == '/') c = '\\';
#else
    for (char& c : p) if (c == '\\') c = '/';
#endif
    return p;
}

static bool bank_digests(const std::string& path, bool want_preload, uint64_t* table, uint64_t* preload) {
    FILE* f = file_open(path, "rb");
    if (!f) return false;
    uint8_t head[60];
    bool ok = fread(head, 1, 60, f) == 60 && rd32(head) == 0x31425346u;
    uint32_t version = ok ? rd32(head + 4) : 0, rows = ok ? rd32(head + 20) : 0;
    uint64_t preload_offset = ok ? le64(head + 24) : 0, preload_size = ok ? le64(head + 32) : 0;
    uint32_t frame = ok ? rd32(head + 12) * (rd32(head + 40) / 8) : 0;
    uint64_t table_bytes = version == 3 ? 60 + (uint64_t)rd32(head + 52) * PROV_RECORD_BYTES + 128ull * rows
                         : version == 2 ? 44 + 128ull * rows
                         : version == 1 ? 44 + 104ull * rows : 0;
    ok = ok && frame > 0 && table_bytes > 0 && table_bytes < ((uint64_t)1 << 30) && preload_offset >= table_bytes;
    std::vector<uint8_t> buf;
    if (ok) {
        buf.resize((size_t)table_bytes);
        ok = file_seek(f, 0) && fread(buf.data(), 1, buf.size(), f) == buf.size();
    }
    if (ok) *table = xxh64(buf.data(), buf.size());
    if (ok && want_preload) {
        ok = file_seek(f, preload_offset);
        uint64_t left = preload_size / frame * frame;
        Xxh64 h;
        xxh_begin(&h);
        buf.resize(1 << 20);
        while (ok && left > 0) {
            size_t n = (size_t)std::min<uint64_t>(buf.size(), left);
            ok = fread(buf.data(), 1, n, f) == n;
            if (ok) xxh_update(&h, buf.data(), n);
            left -= n;
        }
        if (ok) *preload = xxh_end(&h);
    }
    fclose(f);
    return ok;
}

static bool link_instrument(const std::string& fsi, const std::vector<std::string>& built,
                            std::vector<std::string>* problems, bool* changed) {
    *changed = false;
    int64_t size = sr_file_size(fsi);
    if (size < 8 || size > (64 << 20)) return false;
    std::vector<uint8_t> data((size_t)size);
    FILE* f = file_open(fsi, "rb");
    bool ok = f && fread(data.data(), 1, data.size(), f) == data.size();
    if (f) fclose(f);
    std::vector<FsiBankRef> refs;
    if (!ok || !fsi_bank_refs(data, &refs) || refs.empty()) return ok;

    std::string dir = sr_path_parent(fsi);
    for (const FsiBankRef& ref : refs) {
        if (!ref.table_at || !ref.preload_at || ref.path.empty()) continue;
        std::string bank = fsi_bank_path(dir, ref.path);
        if (sr_file_size(bank) < 0) continue;
        uint64_t table = 0, preload = 0;
        if (!bank_digests(bank, false, &table, &preload)) {
            problems->push_back(fmt("%s: Cannot read %s.", sr_path_base(fsi).c_str(), sr_path_base(bank).c_str()));
            continue;
        }
        if (table == rd64(data.data() + ref.table_at)) continue;
        bool checked = std::any_of(built.begin(), built.end(),
                                   [&](const std::string& name) { return sr_name_equal(name, sr_path_base(bank)); });
        if (!bank_digests(bank, true, &table, &preload) ||
            (!checked && preload != rd64(data.data() + ref.preload_at))) {
            problems->push_back(fmt("%s: %s is not the bank this instrument was made with. Build the bank again.",
                                    sr_path_base(fsi).c_str(), sr_path_base(bank).c_str()));
            continue;
        }
        put64(data.data() + ref.table_at, table);
        put64(data.data() + ref.preload_at, preload);
        *changed = true;
    }
    if (!*changed) return true;

    std::string part = fsi + ".part";
    FILE* out = file_open(part, "wb");
    ok = out && fwrite(data.data(), 1, data.size(), out) == data.size();
    if (out) ok = (fclose(out) == 0) && ok;
    if (!ok || !file_replace(part, fsi)) {
        file_delete(part);
        problems->push_back(fmt("%s: Cannot write the instrument. Close all programs that use this file.",
                                sr_path_base(fsi).c_str()));
        *changed = false;
    }
    return true;
}

int sr_link_instruments(const std::string& instruments_dir, const std::vector<std::string>& built,
                        std::vector<std::string>* problems) {
    std::vector<std::string> dirs = { instruments_dir };
    std::vector<SrDirEntry> entries;
    if (!sr_list_dir(instruments_dir, &entries)) return 0;
    for (const SrDirEntry& e : entries)
        if (e.dir) dirs.push_back(sr_path_join(instruments_dir, e.name));
    int changed_count = 0;
    for (const std::string& d : dirs) {
        if (!sr_list_dir(d, &entries)) continue;
        for (const SrDirEntry& e : entries) {
            if (e.dir || !sr_ends_with(e.name, ".fsi")) continue;
            bool changed = false;
            link_instrument(sr_path_join(d, e.name), built, problems, &changed);
            if (changed) changed_count++;
        }
    }
    return changed_count;
}
