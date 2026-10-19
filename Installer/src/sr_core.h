#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#define SR_VERSION "1.3"
#define SR_CREATOR "sonic-reimplants-installer"

std::string sr_exe_dir();
std::string sr_path_join(const std::string& a, const std::string& b);
std::string sr_path_base(const std::string& p);
std::string sr_path_parent(const std::string& p);
bool sr_is_dir(const std::string& p);
int64_t sr_file_size(const std::string& p);
bool sr_name_equal(const std::string& a, const std::string& b);
bool sr_ends_with(const std::string& s, const char* suffix);

struct SrDirEntry {
    std::string name;
    bool dir;
    int64_t size;
};
bool sr_list_dir(const std::string& dir, std::vector<SrDirEntry>* out);
std::vector<std::string> sr_roots();
std::string sr_home_dir();
void sr_reveal(const std::string& dir);

struct SrPart {
    int file, part;
    std::string name, folder;
    std::string alt_name;
    uint64_t bytes;
    std::string path;
    bool exact;
};

struct SrSample {
    int file, gig, part, channels, bits, steps, result_bits, compressed;
    uint32_t rate, result_frames;
    uint64_t offset, bytes, frames, source_fp, result_fp;
    std::string name;
    bool has_stream;
    uint64_t stream_offset, stream_bytes, stream_first, stream_hash;
};

struct SrRecipe {
    std::string file, bank;
    uint64_t bank_bytes, content_hash;
    uint32_t rate, channels, bits, preload, block, flags;
    std::vector<SrPart> parts;
    std::vector<SrSample> samples;
    uint64_t preload_offset, preload_bytes, preload_hash;
    uint8_t head[44];
    std::vector<uint8_t> rows;
    uint32_t row_count;
};

bool sr_recipe_read(const std::string& path, SrRecipe* r, std::string* err);
std::vector<SrRecipe> sr_recipes_load(const std::string& dir, std::vector<std::string>* errors);

struct SrFound {
    std::string path;
    int64_t size;
};

struct SrSearchProgress {
    std::atomic<int> folders{0};
    std::atomic<int> files{0};
    std::atomic<bool> cancel{false};
};

std::vector<std::string> sr_search_roots(const std::string& picked);
void sr_scan(const std::vector<std::string>& roots, int max_depth, SrSearchProgress* p, std::vector<SrFound>* out);
bool sr_match_parts(SrRecipe* r, const std::vector<SrFound>& files);

enum SrBankState { SR_BANK_WAITING, SR_BANK_BUILDING, SR_BANK_DONE, SR_BANK_SKIPPED, SR_BANK_FAILED, SR_BANK_CANCELLED };

struct SrBankProgress {
    std::atomic<int> state{SR_BANK_WAITING};
    std::atomic<int> samples_done{0};
    int samples_total = 0;
    std::string error;
};

struct SrRebuilder {
    std::string account, display;
    uint64_t created = 0;
};

bool sr_rebuilder_init(SrRebuilder* who);

void sr_build_bank(const SrRecipe& r, const std::string& out_dir, const SrRebuilder& who, bool force,
                   const std::atomic<bool>* cancel, SrBankProgress* p);

void sr_build_all(const std::vector<SrRecipe*>& recipes, const std::string& out_dir, const SrRebuilder& who,
                  bool force, int threads, const std::atomic<bool>* cancel, std::vector<SrBankProgress>* progress);
int sr_default_threads();

bool sr_verify_bank(const SrRecipe& r, const std::string& out_dir, std::string* err);

std::string sr_default_instruments_dir();
int sr_link_instruments(const std::string& instruments_dir, std::vector<std::string>* problems);
