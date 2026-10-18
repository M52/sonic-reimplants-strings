#include "sr_core.h"
#if !defined(SR_NO_GUI)
#include "sr_gui.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

#if !defined(SR_NO_GUI)
static bool window_args(int argc, char** argv, SrGuiOptions* opt) {
    bool window = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--window") window = true;
        else if (a == "--recipes" && i + 1 < argc) opt->recipes_dir = argv[++i];
        else if (a == "--out" && i + 1 < argc) opt->out_dir = argv[++i];
        else if (a == "--instruments" && i + 1 < argc) opt->instruments_dir = argv[++i];
        else if (a == "--pick" && i + 1 < argc) opt->pick = argv[++i];
        else if (a == "--browse") opt->browse = true;
        else if (a == "--build") opt->build = true;
        else return false;
    }
    return window;
}
#endif

static void usage() {
    printf("sr_install %s builds the Sonic Reimplants sample banks from the original GigaStudio files.\n\n"
           "  sr_install                          Open the window.\n"
           "  sr_install --gig <folder|.gig> ...  Build the banks on the command line.\n\n"
           "  --gig <folder|.gig>  The folder of the Symphonic String Collection, or one .gig file of the\n"
           "                       collection. The program also searches the subfolders.\n"
           "  --out <folder>       The output folder. Default: the Samples folder next to the Installer folder.\n"
           "  --recipes <folder>   The recipes folder. Default: the recipes folder next to this program.\n"
           "  --instruments <folder>\n"
           "                       The instruments folder. Default: the Instruments folder next to the Installer\n"
           "                       folder. After a build, the program updates the instruments that use more than\n"
           "                       one bank, so that they use your banks.\n"
           "  --only <text>        Build only the banks with this text in their name.\n"
           "  --threads <n>        The number of banks to build at the same time. Default: %d.\n"
           "  --force              Build the banks again that are already in the output folder.\n"
           "  --verify             Do not build banks. Compare the banks in the output folder with the recipes.\n"
           "  --yes                Do not ask before you start.\n",
           SR_VERSION, sr_default_threads());
}

static int run_cli(int argc, char** argv) {
    std::string here = sr_exe_dir();
    std::string recipes_dir = sr_path_join(here, "recipes");
    std::string parent = sr_path_parent(here);
    std::string out_dir = sr_path_join(parent.empty() ? std::string("..") : parent, "Samples");
    std::string instruments_dir = sr_default_instruments_dir();
    std::vector<std::string> gigs;
    const char* only = nullptr;
    bool verify = false, yes = false, force = false;
    int threads = sr_default_threads();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--gig" && i + 1 < argc) gigs.push_back(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--recipes" && i + 1 < argc) recipes_dir = argv[++i];
        else if (a == "--instruments" && i + 1 < argc) instruments_dir = argv[++i];
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--threads" && i + 1 < argc) threads = atoi(argv[++i]);
        else if (a == "--force") force = true;
        else if (a == "--verify") verify = true;
        else if (a == "--yes") yes = true;
        else {
            usage();
            return 2;
        }
    }
    if (!verify && gigs.empty()) {
        usage();
        return 2;
    }
    std::vector<std::string> errors;
    std::vector<SrRecipe> recipes = sr_recipes_load(recipes_dir, &errors);
    for (const std::string& e : errors) fprintf(stderr, "%s\n", e.c_str());
    std::vector<SrRecipe*> chosen;
    for (SrRecipe& r : recipes)
        if (!only || r.bank.find(only) != std::string::npos) chosen.push_back(&r);
    if (chosen.empty()) {
        fprintf(stderr, "Cannot find recipes in %s.\n", recipes_dir.c_str());
        return 1;
    }

    if (verify) {
        int good = 0;
        for (SrRecipe* r : chosen) {
            std::string err;
            if (sr_verify_bank(*r, out_dir, &err)) {
                printf("%s: The bank is the same as the original bank. Only the provenance record is different.\n",
                       r->bank.c_str());
                good++;
            } else {
                printf("%s: %s\n", r->bank.c_str(), err.c_str());
            }
        }
        printf("\n%d of %d banks are the same as the original banks.\n", good, (int)chosen.size());
        return good == (int)chosen.size() ? 0 : 1;
    }

    SrRebuilder who;
    if (!sr_rebuilder_init(&who)) {
        fprintf(stderr, "Cannot read the name of your account.\n");
        return 1;
    }
    printf("Each bank includes a record of who built it: your account name \"%s\"%s%s%s, the date, and the name\n"
           "of this program. The record is encrypted. Only the FastSampler maintainer can read it.\n"
           "The program does not send data.\n",
           who.account.c_str(), who.display.empty() ? "" : " and your display name \"", who.display.c_str(),
           who.display.empty() ? "" : "\"");
    printf("Output folder: %s\n", out_dir.c_str());
    if (!yes) {
        char answer[16] = "";
        printf("Continue? [y/N] ");
        fflush(stdout);
        if (!fgets(answer, sizeof(answer), stdin) || (answer[0] != 'y' && answer[0] != 'Y')) return 1;
    }

    std::vector<SrFound> found;
    for (const std::string& g : gigs) {
        std::vector<SrFound> some;
        sr_scan(sr_search_roots(g), sr_is_dir(g) ? 6 : 2, nullptr, &some);
        found.insert(found.end(), some.begin(), some.end());
    }
    std::vector<SrRecipe*> buildable;
    std::vector<std::string> told;
    for (SrRecipe* r : chosen) {
        bool all = sr_match_parts(r, found);
        for (const SrPart& p : r->parts) {
            if (p.path.empty() || p.exact || std::find(told.begin(), told.end(), p.path) != told.end()) continue;
            told.push_back(p.path);
            printf("%s is a different edition. The program finds its samples by name.\n", p.path.c_str());
        }
        if (all) {
            buildable.push_back(r);
            continue;
        }
        for (const SrPart& p : r->parts)
            if (p.path.empty())
                fprintf(stderr, "%s: Cannot find %s (%llu bytes, in the folder \"%s\").\n", r->bank.c_str(), p.name.c_str(),
                        (unsigned long long)p.bytes, p.folder.c_str());
    }

    std::vector<SrBankProgress> progress(buildable.size());
    std::atomic<bool> cancel{false};
    printf("Building %d banks, %d at the same time...\n", (int)buildable.size(), threads);
    fflush(stdout);
    sr_build_all(buildable, out_dir, who, force, threads, &cancel, &progress);
    int built = 0, skipped = 0, failed = (int)(chosen.size() - buildable.size());
    for (size_t i = 0; i < buildable.size(); ++i) {
        int st = progress[i].state.load();
        if (st == SR_BANK_DONE) {
            printf("%s\n", buildable[i]->bank.c_str());
            built++;
        } else if (st == SR_BANK_SKIPPED) {
            printf("%s: The bank is already in the output folder. Use --force to build it again.\n",
                   buildable[i]->bank.c_str());
            skipped++;
        } else {
            printf("%s: Cannot build the bank. %s\n", buildable[i]->bank.c_str(), progress[i].error.c_str());
            failed++;
        }
    }
    printf("\n%d banks built. %d banks were already in the output folder. %d banks failed.\n", built, skipped, failed);

    std::vector<std::string> problems;
    int linked = sr_link_instruments(instruments_dir, &problems);
    if (linked) printf("%d instruments updated to use your banks.\n", linked);
    for (const std::string& p : problems) fprintf(stderr, "%s\n", p.c_str());
    return failed || !problems.empty() ? 1 : 0;
}

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? (size_t)n : 1, '\0');
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
        s.resize(strlen(s.c_str()));
        args.push_back(s);
    }
    LocalFree(wargv);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(&a[0]);
#if !defined(SR_NO_GUI)
    SrGuiOptions opt;
    if (argc <= 1 || window_args(argc, argv.data(), &opt)) return sr_gui_run(opt);
#endif
    if (argc > 1) {
        auto unset = [](DWORD which) {
            HANDLE h = GetStdHandle(which);
            return h == nullptr || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN;
        };
        bool out = unset(STD_OUTPUT_HANDLE), err = unset(STD_ERROR_HANDLE), in = unset(STD_INPUT_HANDLE);
        if ((out || err || in) && (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())) {
            FILE* f;
            if (out) freopen_s(&f, "CONOUT$", "w", stdout);
            if (err) freopen_s(&f, "CONOUT$", "w", stderr);
            if (in) freopen_s(&f, "CONIN$", "r", stdin);
            SetConsoleOutputCP(CP_UTF8);
        }
        int rc = run_cli(argc, argv.data());
        fflush(stdout);
        return rc;
    }
    return run_cli(argc, argv.data());
}
#else
int main(int argc, char** argv) {
#if !defined(SR_NO_GUI)
    SrGuiOptions opt;
    if (argc <= 1 || window_args(argc, argv, &opt)) return sr_gui_run(opt);
#endif
    return run_cli(argc, argv);
}
#endif
