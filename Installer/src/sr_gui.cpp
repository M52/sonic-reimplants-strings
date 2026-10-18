#include "sr_core.h"
#include "sr_gui.h"
#include "sr_window.h"

#include "imgui.h"
#include "imgui_sw.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

namespace {

enum Screen { SCREEN_START, SCREEN_SEARCHING, SCREEN_FOUND, SCREEN_BUILDING, SCREEN_DONE };
enum BrowseFor { BROWSE_SOURCE, BROWSE_OUTPUT };

const ImVec4 COL_ACCENT(0.91f, 0.64f, 0.24f, 1.0f);
const ImVec4 COL_ACCENT_HOVER(0.97f, 0.72f, 0.33f, 1.0f);
const ImVec4 COL_ACCENT_ACTIVE(0.80f, 0.54f, 0.17f, 1.0f);
const ImVec4 COL_TEXT_DIM(0.64f, 0.62f, 0.60f, 1.0f);
const ImVec4 COL_GOOD(0.55f, 0.80f, 0.45f, 1.0f);
const ImVec4 COL_BAD(0.95f, 0.45f, 0.40f, 1.0f);

struct Browser {
    bool open = false;
    bool request_open = false;
    BrowseFor mode = BROWSE_SOURCE;
    std::string dir;
    std::vector<SrDirEntry> entries;
    int selected = -1;
    char path_input[2048] = "";
    bool listing_ok = true;
};

struct App {
    float scale = 1.0f;
    ImFont* font_body = nullptr;
    ImFont* font_title = nullptr;
    ImFont* font_small = nullptr;

    Screen screen = SCREEN_START;
    Browser browser;
    std::string exe_dir, recipes_dir, out_dir, instruments_dir;
    std::vector<SrRecipe> recipes;
    std::vector<std::string> recipe_errors;
    SrRebuilder who;
    bool who_ok = false;

    std::string picked;
    std::vector<std::string> roots;
    std::thread search_thread;
    std::unique_ptr<SrSearchProgress> search;
    std::atomic<bool> search_done{false};
    std::vector<SrFound> found;

    int parts_total = 0, parts_found = 0, parts_other = 0;
    std::vector<std::string> missing;
    std::vector<SrRecipe*> buildable;
    int banks_present = 0;
    uint64_t build_bytes = 0;
    bool force = false;

    std::thread build_thread;
    std::unique_ptr<std::vector<SrBankProgress>> progress;
    std::vector<SrRecipe*> building;
    std::atomic<bool> cancel{false};
    std::atomic<bool> build_done{false};
    int threads = 1;
    std::chrono::steady_clock::time_point started, finished;

    int linked = 0;
    std::vector<std::string> link_problems;
};

std::string human_bytes(uint64_t b) {
    char buf[64];
    if (b >= 1000000000ull) snprintf(buf, sizeof(buf), "%.1f GB", b / 1e9);
    else snprintf(buf, sizeof(buf), "%.0f MB", b / 1e6);
    return buf;
}

std::string human_time(double seconds) {
    if (seconds < 0) seconds = 0;
    int s = (int)(seconds + 0.5);
    char buf[64];
    if (s >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

bool primary_button(App& app, const char* label, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, COL_ACCENT);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_ACCENT_HOVER);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, COL_ACCENT_ACTIVE);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.08f, 0.06f, 1.0f));
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

void text_wrapped_dim(const char* fmt, ...) {
    char buf[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    ImGui::PushStyleColor(ImGuiCol_Text, COL_TEXT_DIM);
    ImGui::TextWrapped("%s", buf);
    ImGui::PopStyleColor();
}

std::string n_banks(int n) {
    return std::to_string(n) + (n == 1 ? " bank" : " banks");
}

void title(App& app, const char* text) {
    ImGui::PushFont(app.font_title);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::Spacing();
}

void progress_bar(App& app, float fraction, const char* overlay, float height) {
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, COL_ACCENT);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * app.scale);
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, height), overlay);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void browser_go(Browser& b, const std::string& dir) {
    b.dir = dir;
    b.selected = -1;
    b.listing_ok = dir.empty() ? true : sr_list_dir(dir, &b.entries);
    if (dir.empty()) b.entries.clear();
    snprintf(b.path_input, sizeof(b.path_input), "%s", dir.c_str());
}

void browser_open(App& app, BrowseFor mode) {
    Browser& b = app.browser;
    b.mode = mode;
    std::string start = mode == BROWSE_OUTPUT ? app.out_dir : app.picked;
    if (!start.empty() && !sr_is_dir(start)) start = sr_path_parent(start);
    while (!start.empty() && !sr_is_dir(start)) start = sr_path_parent(start);
    if (start.empty()) start = sr_home_dir();
    browser_go(b, start);
    b.request_open = true;
}

void start_search(App& app, const std::string& picked);

void draw_browser(App& app) {
    Browser& b = app.browser;
    float s = app.scale;
    if (b.request_open) {
        b.request_open = false;
        b.open = true;
        ImGui::OpenPopup("###Browser");
    }
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(display.x - 60.0f * s, display.y - 60.0f * s), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    const char* heading = b.mode == BROWSE_SOURCE ? "Select the folder or a .gig file###Browser"
                                                  : "Select the output folder###Browser";
    if (!ImGui::BeginPopupModal(heading, &b.open, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) return;

    std::vector<std::string> roots = sr_roots();
    if (ImGui::Button("Home")) browser_go(b, sr_home_dir());
    for (const std::string& r : roots) {
        ImGui::SameLine();
        if (ImGui::Button(r.c_str())) browser_go(b, r);
    }
    ImGui::SameLine();
    std::string parent = sr_path_parent(b.dir);
    ImGui::BeginDisabled(parent.empty());
    if (ImGui::Button("Up")) browser_go(b, parent);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputText("##path", b.path_input, sizeof(b.path_input), ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string typed = b.path_input;
        if (sr_is_dir(typed)) browser_go(b, typed);
        else if (b.mode == BROWSE_SOURCE && sr_ends_with(typed, ".gig") && sr_file_size(typed) >= 0) {
            ImGui::CloseCurrentPopup();
            b.open = false;
            ImGui::EndPopup();
            start_search(app, typed);
            return;
        }
    }

    float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##entries", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (!b.listing_ok) ImGui::TextColored(COL_BAD, "Cannot read this folder.");
    std::string chosen_file;
    std::string enter_dir;
    int shown = 0;
    for (int i = 0; i < (int)b.entries.size(); ++i) {
        const SrDirEntry& e = b.entries[(size_t)i];
        bool gig = !e.dir && sr_ends_with(e.name, ".gig");
        if (!e.dir && (b.mode == BROWSE_OUTPUT || !gig)) continue;
        if (e.name[0] == '.') continue;
        shown++;
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text, e.dir ? COL_ACCENT : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        std::string label = e.dir ? e.name + "/" : e.name;
        if (ImGui::Selectable(label.c_str(), b.selected == i, ImGuiSelectableFlags_AllowDoubleClick)) {
            b.selected = i;
            if (ImGui::IsMouseDoubleClicked(0)) {
                if (e.dir) enter_dir = sr_path_join(b.dir, e.name);
                else chosen_file = sr_path_join(b.dir, e.name);
            }
        }
        ImGui::PopStyleColor();
        if (gig) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80.0f * s + ImGui::GetCursorPosX());
            ImGui::TextColored(COL_TEXT_DIM, "%s", human_bytes((uint64_t)e.size).c_str());
        }
        ImGui::PopID();
    }
    if (shown == 0 && b.listing_ok) ImGui::TextColored(COL_TEXT_DIM, "This folder has no folders or .gig files.");
    ImGui::EndChild();
    if (!enter_dir.empty()) browser_go(b, enter_dir);

    const SrDirEntry* sel = b.selected >= 0 && b.selected < (int)b.entries.size() ? &b.entries[(size_t)b.selected] : nullptr;
    std::string use_path = sel ? sr_path_join(b.dir, sel->name) : b.dir;
    bool use_file = sel && !sel->dir;
    if (b.mode == BROWSE_SOURCE) {
        text_wrapped_dim("Select the folder of the collection or one .gig file. Double-click a folder to open it.");
    } else {
        text_wrapped_dim("Select the Samples folder. It is next to the Instruments folder.");
    }
    std::string label = use_file ? "Use this file" : "Use this folder";
    ImGui::BeginDisabled(use_path.empty());
    if (primary_button(app, label.c_str()) || !chosen_file.empty()) {
        std::string path = !chosen_file.empty() ? chosen_file : use_path;
        ImGui::CloseCurrentPopup();
        b.open = false;
        ImGui::EndDisabled();
        ImGui::EndPopup();
        if (b.mode == BROWSE_SOURCE) start_search(app, path);
        else app.out_dir = path;
        return;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
        b.open = false;
    }
    ImGui::EndPopup();
}

void join_threads(App& app) {
    if (app.search_thread.joinable()) {
        if (app.search) app.search->cancel.store(true);
        app.search_thread.join();
    }
    if (app.build_thread.joinable()) {
        app.cancel.store(true);
        app.build_thread.join();
    }
}

void start_search(App& app, const std::string& picked) {
    if (app.search_thread.joinable()) {
        app.search->cancel.store(true);
        app.search_thread.join();
    }
    app.picked = picked;
    app.roots = sr_search_roots(picked);
    app.search.reset(new SrSearchProgress());
    app.search_done.store(false);
    app.found.clear();
    app.screen = SCREEN_SEARCHING;
    app.search_thread = std::thread([&app]() {
        int depth = sr_is_dir(app.picked) ? 6 : 2;
        sr_scan(app.roots, depth, app.search.get(), &app.found);
        app.search_done.store(true);
    });
}

void finish_search(App& app) {
    app.search_thread.join();
    app.parts_total = app.parts_found = app.parts_other = 0;
    app.missing.clear();
    app.buildable.clear();
    app.banks_present = 0;
    app.build_bytes = 0;
    std::vector<std::string> seen;
    for (SrRecipe& r : app.recipes) {
        bool all = sr_match_parts(&r, app.found);
        for (const SrPart& p : r.parts) {
            bool have = false;
            for (const std::string& s : seen)
                if (sr_name_equal(s, p.name)) have = true;
            if (have) continue;
            seen.push_back(p.name);
            app.parts_total++;
            if (!p.path.empty()) app.parts_found++;
            else app.missing.push_back(p.name + "   (" + p.folder + ")");
            if (!p.path.empty() && !p.exact) app.parts_other++;
        }
        if (all) {
            app.buildable.push_back(&r);
            app.build_bytes += r.bank_bytes;
            if (sr_file_size(sr_path_join(app.out_dir, r.bank)) == (int64_t)r.bank_bytes) app.banks_present++;
        }
    }
    app.screen = SCREEN_FOUND;
}

void start_build(App& app) {
    app.building = app.buildable;
    app.progress.reset(new std::vector<SrBankProgress>(app.building.size()));
    for (size_t i = 0; i < app.building.size(); ++i) (*app.progress)[i].samples_total = (int)app.building[i]->samples.size();
    app.cancel.store(false);
    app.build_done.store(false);
    app.threads = sr_default_threads();
    app.started = std::chrono::steady_clock::now();
    app.screen = SCREEN_BUILDING;
    app.build_thread = std::thread([&app]() {
        sr_build_all(app.building, app.out_dir, app.who, app.force, app.threads, &app.cancel, app.progress.get());
        app.link_problems.clear();
        app.linked = sr_link_instruments(app.instruments_dir, &app.link_problems);
        app.finished = std::chrono::steady_clock::now();
        app.build_done.store(true);
    });
}

void draw_output_line(App& app, bool can_change) {
    ImGui::TextColored(COL_TEXT_DIM, "Output folder:");
    ImGui::TextWrapped("%s", app.out_dir.c_str());
    if (can_change) {
        if (ImGui::Button("Change...")) browser_open(app, BROWSE_OUTPUT);
    }
}

void draw_start(App& app) {
    float s = app.scale;
    title(app, "Sonic Reimplants");
    ImGui::TextColored(COL_ACCENT, "Sample bank installer");
    ImGui::Dummy(ImVec2(0, 14.0f * s));
    ImGui::TextWrapped("This program builds the sample banks for the Sonic Reimplants instruments. It uses the .gig "
                       "files of the Sonic Implants Symphonic String Collection.");
    ImGui::Dummy(ImVec2(0, 6.0f * s));
    ImGui::TextWrapped("Select the folder of the collection, or select one .gig file of the collection. The program "
                       "then finds the other .gig files.");
    ImGui::Dummy(ImVec2(0, 22.0f * s));
    if (primary_button(app, "Select the folder...")) browser_open(app, BROWSE_SOURCE);
#if defined(_WIN32)
    ImGui::Dummy(ImVec2(0, 6.0f * s));
    text_wrapped_dim("You can also drag the folder or a .gig file onto this window.");
#endif
    ImGui::Dummy(ImVec2(0, 26.0f * s));
    draw_output_line(app, true);
    if (!app.recipe_errors.empty()) {
        ImGui::Dummy(ImVec2(0, 12.0f * s));
        for (const std::string& e : app.recipe_errors) ImGui::TextColored(COL_BAD, "%s", e.c_str());
    }
}

void draw_searching(App& app) {
    float s = app.scale;
    title(app, "Searching for the .gig files");
    for (const std::string& r : app.roots) text_wrapped_dim("%s", r.c_str());
    ImGui::Dummy(ImVec2(0, 10.0f * s));
    float t = (float)ImGui::GetTime();
    float phase = t - floorf(t);
    char overlay[128];
    snprintf(overlay, sizeof(overlay), "%d folders searched. %d .gig files found.", app.search->folders.load(),
             app.search->files.load());
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = 26.0f * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f * s);
    float x0 = pos.x + (w * 1.25f) * phase - w * 0.25f;
    float x1 = x0 + w * 0.25f;
    if (x0 < pos.x) x0 = pos.x;
    if (x1 > pos.x + w) x1 = pos.x + w;
    if (x1 > x0) dl->AddRectFilled(ImVec2(x0, pos.y), ImVec2(x1, pos.y + h), ImGui::GetColorU32(COL_ACCENT), 4.0f * s);
    ImVec2 ts = ImGui::CalcTextSize(overlay);
    dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), overlay);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::Dummy(ImVec2(0, 14.0f * s));
    if (ImGui::Button("Stop")) app.search->cancel.store(true);
}

void draw_found(App& app) {
    float s = app.scale;
    int banks = (int)app.recipes.size();
    char line[256];
    if (app.parts_found == app.parts_total && app.parts_total == 1) {
        title(app, "Found the .gig file");
    } else if (app.parts_found == app.parts_total) {
        snprintf(line, sizeof(line), "Found all %d .gig files", app.parts_total);
        title(app, line);
    } else {
        snprintf(line, sizeof(line), "Found %d of %d .gig files", app.parts_found, app.parts_total);
        title(app, line);
    }
    text_wrapped_dim("Folder: %s", app.picked.c_str());
    if (app.parts_other == 1)
        text_wrapped_dim("1 file is a different edition. The program finds its samples by name.");
    else if (app.parts_other > 1)
        text_wrapped_dim("%d files are a different edition. The program finds their samples by name.", app.parts_other);
    ImGui::Dummy(ImVec2(0, 8.0f * s));

    if (!app.missing.empty()) {
        ImGui::TextColored(COL_BAD, "Not found:");
        float h = std::min(150.0f * s, (ImGui::GetTextLineHeightWithSpacing() * (float)app.missing.size()) + 8.0f * s);
        ImGui::BeginChild("##missing", ImVec2(0, h), ImGuiChildFlags_Borders);
        for (const std::string& m : app.missing) ImGui::TextUnformatted(m.c_str());
        ImGui::EndChild();
        text_wrapped_dim("The program cannot build the banks that use these files. Select a higher folder, or the "
                         "folder with these files.");
        ImGui::Dummy(ImVec2(0, 6.0f * s));
    }

    if (app.buildable.empty()) {
        ImGui::TextWrapped("The program cannot build banks with these files.");
    } else {
        int ready = (int)app.buildable.size();
        ImGui::TextWrapped("%d of %s %s ready to build (%s).", ready, n_banks(banks).c_str(), ready == 1 ? "is" : "are",
                           human_bytes(app.build_bytes).c_str());
        if (app.banks_present == 1) {
            ImGui::TextWrapped("1 of these banks is already in the output folder. The program does not build it again.");
            ImGui::Checkbox("Build this bank again", &app.force);
        } else if (app.banks_present > 1) {
            ImGui::TextWrapped("%d of these banks are already in the output folder. The program does not build them "
                               "again.",
                               app.banks_present);
            ImGui::Checkbox("Build these banks again", &app.force);
        }
    }
    ImGui::Dummy(ImVec2(0, 8.0f * s));
    draw_output_line(app, true);
    if (!app.who_ok) {
        ImGui::Dummy(ImVec2(0, 8.0f * s));
        ImGui::TextColored(COL_BAD, "Cannot read the name of your account.");
    }
    ImGui::Dummy(ImVec2(0, 14.0f * s));
    ImGui::BeginDisabled(app.buildable.empty() || !app.who_ok);
    snprintf(line, sizeof(line), "Build %s", n_banks((int)app.buildable.size()).c_str());
    if (primary_button(app, line)) start_build(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Select a different folder...")) browser_open(app, BROWSE_SOURCE);
}

struct Tally {
    int done = 0, skipped = 0, failed = 0, building = 0, waiting = 0, cancelled = 0;
    long long samples_done = 0, samples_total = 0;
};

Tally tally(App& app) {
    Tally t;
    for (size_t i = 0; i < app.progress->size(); ++i) {
        const SrBankProgress& p = (*app.progress)[i];
        int st = p.state.load();
        t.samples_total += p.samples_total;
        if (st == SR_BANK_DONE || st == SR_BANK_SKIPPED) t.samples_done += p.samples_total;
        else t.samples_done += p.samples_done.load();
        if (st == SR_BANK_DONE) t.done++;
        else if (st == SR_BANK_SKIPPED) t.skipped++;
        else if (st == SR_BANK_FAILED) t.failed++;
        else if (st == SR_BANK_BUILDING) t.building++;
        else if (st == SR_BANK_CANCELLED) t.cancelled++;
        else t.waiting++;
    }
    return t;
}

void draw_failures(App& app) {
    for (size_t i = 0; i < app.progress->size(); ++i) {
        const SrBankProgress& p = (*app.progress)[i];
        if (p.state.load() != SR_BANK_FAILED) continue;
        ImGui::TextColored(COL_BAD, "%s", app.building[i]->bank.c_str());
        ImGui::Indent();
        text_wrapped_dim("%s", p.error.c_str());
        ImGui::Unindent();
    }
}

void draw_building(App& app) {
    float s = app.scale;
    Tally t = tally(app);
    title(app, app.cancel.load() ? "Stopping..." : "Building the sample banks");
    int total = (int)app.progress->size();
    float fraction = t.samples_total ? (float)((double)t.samples_done / (double)t.samples_total) : 0.0f;
    char overlay[64];
    snprintf(overlay, sizeof(overlay), "%.0f%%", fraction * 100.0f);
    progress_bar(app, fraction, overlay, 30.0f * s);

    int finished = t.done + t.skipped + t.failed + t.cancelled;
    ImGui::Text("%d of %s complete   |   %d in progress   |   %d waiting", finished, n_banks(total).c_str(),
                t.building, t.waiting);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - app.started).count();
    char time_line[160];
    if (t.samples_done > 0 && fraction > 0.01f) {
        double left = elapsed * (1.0 - fraction) / fraction;
        snprintf(time_line, sizeof(time_line), "%lld of %lld samples   |   Time: %s   |   Time remaining: approximately %s",
                 t.samples_done, t.samples_total, human_time(elapsed).c_str(), human_time(left).c_str());
    } else {
        snprintf(time_line, sizeof(time_line), "%lld of %lld samples   |   Time: %s", t.samples_done, t.samples_total,
                 human_time(elapsed).c_str());
    }
    ImGui::TextColored(COL_TEXT_DIM, "%s", time_line);
    ImGui::Dummy(ImVec2(0, 10.0f * s));

    int parallel = std::min(app.threads, total);
    if (parallel > 1) ImGui::TextColored(COL_ACCENT, "In progress (%d banks at the same time)", parallel);
    else ImGui::TextColored(COL_ACCENT, "In progress");
    float list_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() - 10.0f * s;
    ImGui::BeginChild("##now", ImVec2(0, list_h), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##banks", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("bank", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("count", ImGuiTableColumnFlags_WidthFixed,
                                ImGui::CalcTextSize("0000 of 0000 samples").x + 12.0f * s);
        ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthStretch, 0.4f);
        for (size_t i = 0; i < app.progress->size(); ++i) {
            const SrBankProgress& p = (*app.progress)[i];
            if (p.state.load() != SR_BANK_BUILDING) continue;
            int d = p.samples_done.load();
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(app.building[i]->bank.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(COL_TEXT_DIM, "%d of %d samples", d, p.samples_total);
            ImGui::TableSetColumnIndex(2);
            progress_bar(app, p.samples_total ? (float)d / (float)p.samples_total : 0.0f, "", 0.0f);
        }
        ImGui::EndTable();
    }
    if (t.failed) {
        ImGui::Dummy(ImVec2(0, 8.0f * s));
        draw_failures(app);
    }
    ImGui::EndChild();
    ImGui::BeginDisabled(app.cancel.load());
    if (ImGui::Button("Stop")) app.cancel.store(true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextColored(COL_TEXT_DIM, "If you stop, the program keeps the banks that are complete.");

    if (app.build_done.load()) {
        app.build_thread.join();
        app.screen = SCREEN_DONE;
    }
}

void draw_done(App& app) {
    float s = app.scale;
    Tally t = tally(app);
    int built = t.done, total = (int)app.progress->size();
    char line[256];
    int ready = built + t.skipped;
    const char* verb = ready == 1 ? "is" : "are";
    if (t.failed == 0 && t.cancelled == 0 && total == 1) snprintf(line, sizeof(line), "The bank is ready");
    else if (t.failed == 0 && t.cancelled == 0) snprintf(line, sizeof(line), "All %d banks are ready", total);
    else if (t.cancelled) snprintf(line, sizeof(line), "Stopped. %d of %s %s ready", ready, n_banks(total).c_str(), verb);
    else snprintf(line, sizeof(line), "%d of %s %s ready", ready, n_banks(total).c_str(), verb);
    title(app, line);
    double elapsed = std::chrono::duration<double>(app.finished - app.started).count();
    ImGui::TextWrapped("The program built %s in %s.", n_banks(built).c_str(), human_time(elapsed).c_str());
    if (t.skipped)
        ImGui::TextWrapped("%s already in the output folder.", (n_banks(t.skipped) + (t.skipped == 1 ? " was" : " were")).c_str());
    if (app.linked)
        ImGui::TextWrapped("The program updated %d %s to use your banks.", app.linked,
                           app.linked == 1 ? "instrument" : "instruments");
    for (const std::string& p : app.link_problems) ImGui::TextColored(COL_BAD, "%s", p.c_str());
    if (t.failed) ImGui::TextColored(COL_BAD, "The program cannot build %s:", n_banks(t.failed).c_str());
    if (t.failed) {
        float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 4.0f;
        ImGui::BeginChild("##failed", ImVec2(0, h > 60.0f * s ? h : 60.0f * s), ImGuiChildFlags_Borders);
        draw_failures(app);
        ImGui::EndChild();
    } else if (t.failed == 0 && t.cancelled == 0) {
        ImGui::Dummy(ImVec2(0, 6.0f * s));
        ImGui::TextColored(COL_GOOD, "You can now load the instruments in FastSampler.");
    }
    ImGui::Dummy(ImVec2(0, 10.0f * s));
    draw_output_line(app, false);
    ImGui::Dummy(ImVec2(0, 14.0f * s));
    if (primary_button(app, "Open the output folder")) sr_reveal(app.out_dir);
    ImGui::SameLine();
    if (ImGui::Button("Start again")) app.screen = SCREEN_START;
}

const char* system_font() {
    static const char* candidates[] = {
#if defined(_WIN32)
        "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
#elif defined(__APPLE__)
        "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
        "/System/Library/Fonts/Geneva.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    };
    for (const char* c : candidates)
        if (sr_file_size(c) > 0) return c;
    return nullptr;
}

void setup_fonts_and_style(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    float s = app.scale;
    io.Fonts->TexMinWidth = 2048;
    io.Fonts->TexMinHeight = 2048;
    const char* path = system_font();
    ImFontConfig cfg;
    cfg.OversampleH = 1;
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    if (path) {
        app.font_body = io.Fonts->AddFontFromFileTTF(path, 17.0f * s, &cfg);
        app.font_title = io.Fonts->AddFontFromFileTTF(path, 30.0f * s, &cfg);
        app.font_small = io.Fonts->AddFontFromFileTTF(path, 14.0f * s, &cfg);
    }
    if (!app.font_body || !app.font_title) {
        cfg.SizePixels = 15.0f * s;
        app.font_body = io.Fonts->AddFontDefault(&cfg);
        cfg.SizePixels = 26.0f * s;
        app.font_title = io.Fonts->AddFontDefault(&cfg);
        cfg.SizePixels = 13.0f * s;
        app.font_small = io.Fonts->AddFontDefault(&cfg);
    }
    io.FontDefault = app.font_body;

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.ChildRounding = 6.0f;
    st.FrameRounding = 5.0f;
    st.PopupRounding = 6.0f;
    st.GrabRounding = 4.0f;
    st.WindowPadding = ImVec2(28.0f, 24.0f);
    st.FramePadding = ImVec2(14.0f, 8.0f);
    st.ItemSpacing = ImVec2(10.0f, 8.0f);
    st.WindowBorderSize = 0.0f;
    st.PopupBorderSize = 1.0f;
    st.ScaleAllSizes(s);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.085f, 0.08f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.11f, 0.10f, 1.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.10f, 0.095f, 1.0f);
    c[ImGuiCol_Border] = ImVec4(0.25f, 0.23f, 0.21f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.16f, 0.15f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.20f, 0.18f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.24f, 0.21f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.19f, 0.17f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.27f, 0.25f, 0.22f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.32f, 0.29f, 0.25f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.91f, 0.64f, 0.24f, 0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.91f, 0.64f, 0.24f, 0.20f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.91f, 0.64f, 0.24f, 0.40f);
    c[ImGuiCol_CheckMark] = COL_ACCENT;
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = ImVec4(0.14f, 0.13f, 0.12f, 1.0f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.91f, 0.88f, 1.0f);
    c[ImGuiCol_PlotHistogram] = COL_ACCENT;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
}

}

int sr_gui_run(const SrGuiOptions& opt) {
    const std::string& recipes_dir = opt.recipes_dir;
    const std::string& out_dir = opt.out_dir;
    SrWindow* win = sr_window_open("Sonic Reimplants installer", 780, 600);
    if (!win) {
        fprintf(stderr, "Cannot open the window. Use the command line: sr_install --gig <folder>\n");
        return 1;
    }
    std::unique_ptr<App> holder(new App());
    App& app = *holder;
    app.scale = sr_window_scale(win);
    app.exe_dir = sr_exe_dir();
    app.recipes_dir = recipes_dir.empty() ? sr_path_join(app.exe_dir, "recipes") : recipes_dir;
    std::string parent = sr_path_parent(app.exe_dir);
    app.out_dir = out_dir.empty() ? sr_path_join(parent.empty() ? std::string("..") : parent, "Samples") : out_dir;
    app.instruments_dir = opt.instruments_dir.empty() ? sr_default_instruments_dir() : opt.instruments_dir;
    app.recipes = sr_recipes_load(app.recipes_dir, &app.recipe_errors);
    if (app.recipes.empty() && app.recipe_errors.empty())
        app.recipe_errors.push_back("Cannot find recipes in " + app.recipes_dir + ".");
    app.who_ok = sr_rebuilder_init(&app.who);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    setup_fonts_and_style(app);
    imgui_sw::bind_imgui_painting();

    std::vector<uint32_t> pixels;
    auto last = std::chrono::steady_clock::now();
    bool auto_build = opt.build;
    if (!opt.pick.empty()) start_search(app, opt.pick);
    else if (opt.browse) browser_open(app, BROWSE_SOURCE);
    bool running = true;
    while (running) {
        bool busy = app.screen == SCREEN_SEARCHING || app.screen == SCREEN_BUILDING;
        if (!sr_window_pump(win, busy ? 33 : 250)) break;
        std::string drop = sr_window_take_drop(win);
        if (!drop.empty() && app.screen != SCREEN_BUILDING) start_search(app, drop);
        if (app.screen == SCREEN_SEARCHING && app.search_done.load()) finish_search(app);
        if (auto_build && app.screen == SCREEN_FOUND && !app.buildable.empty()) {
            auto_build = false;
            start_build(app);
        }

        int w = 0, h = 0;
        sr_window_size(win, &w, &h);
        if (w <= 0 || h <= 0) continue;
        auto now = std::chrono::steady_clock::now();
        io.DeltaTime = std::max(0.0001f, std::chrono::duration<float>(now - last).count());
        last = now;
        io.DisplaySize = ImVec2((float)w, (float)h);
        io.DisplayFramebufferScale = ImVec2(1, 1);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        switch (app.screen) {
            case SCREEN_START: draw_start(app); break;
            case SCREEN_SEARCHING: draw_searching(app); break;
            case SCREEN_FOUND: draw_found(app); break;
            case SCREEN_BUILDING: draw_building(app); break;
            case SCREEN_DONE: draw_done(app); break;
        }
        draw_browser(app);
        ImGui::End();
        ImGui::Render();
        pixels.assign((size_t)w * (size_t)h, 0xFF141312u);
        imgui_sw::paint_imgui(pixels.data(), w, h);
        sr_window_present(win, pixels.data(), w, h);
    }
    join_threads(app);
    imgui_sw::unbind_imgui_painting();
    ImGui::DestroyContext();
    sr_window_close(win);
    return 0;
}
