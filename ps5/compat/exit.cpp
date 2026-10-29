// llama.cpp's tools leave through _Exit once main returns (issue #3).
//
// After main, the SDK's start code unloads the Sony modules the payload loaded
// and calls Sony's exit() (ps5-payload-dev/sdk, crt/crt.c), which then runs
// every C++ static destructor registered with __cxa_atexit while other threads
// may still run. "[SceLibc] A heap error is detected" shows up at exit, after
// the work is done, which points at this phase. Native PS5 apps never return
// from main (ProsperoAI,
// ps5-native-app-boilerplate); a payload can end with _Exit, as SDK payloads
// did until 2024 (sdk commit f19dd3e).
//
// ps5/CMakeLists.txt links this into the tools with --wrap=main: the start
// code's call to main lands here, and __real_main is the tool's own main.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

extern "C" int __real_main(int argc, char ** argv, char ** envp);
extern "C" int __wrap_main(int argc, char ** argv, char ** envp);

// PLK's payload manager (port 8084) launches payloads without arguments, so a
// tool started that way reads its command line from a file instead: one
// argument per line, blank lines and lines that start with '#' ignored. This
// lets llama-cli/llama-server/llama-bench run as PLK payloads with full args.
static const char * kArgsFile = "/data/PS5LM/cli-args.txt";

static bool load_args_from_file(const char * self, int * out_argc, char *** out_argv) {
    FILE * f = std::fopen(kArgsFile, "r");
    if (!f) {
        return false;
    }
    int    cap  = 8;
    int    n    = 1;  // slot 0 is argv[0]
    char **argv = (char **) std::malloc(sizeof(char *) * cap);
    if (!argv) { std::fclose(f); return false; }
    argv[0] = const_cast<char *>(self ? self : "llama-cli");

    char   line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                           line[len - 1] == ' '  || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }
        char * s = line;
        while (*s == ' ' || *s == '\t') { s++; }
        if (*s == '\0' || *s == '#') { continue; }
        if (n + 1 >= cap) {
            cap *= 2;
            char ** grown = (char **) std::realloc(argv, sizeof(char *) * cap);
            if (!grown) { break; }
            argv = grown;
        }
        argv[n++] = strdup(s);
    }
    std::fclose(f);
    argv[n] = nullptr;
    *out_argc = n;
    *out_argv = argv;
    return n > 1;
}

int __wrap_main(int argc, char ** argv, char ** envp) {
    // PLK-launched payloads have no console; capture everything to a file we
    // can read back over FTP.
    std::freopen("/data/PS5LM/cli-out.log", "w", stdout);
    std::freopen("/data/PS5LM/cli-out.log", "a", stderr);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc <= 1) {
        int     file_argc = 0;
        char ** file_argv = nullptr;
        if (load_args_from_file(argc > 0 ? argv[0] : nullptr, &file_argc, &file_argv)) {
            std::printf("ps5lm: using %d args from %s\n", file_argc - 1, kArgsFile);
            std::fflush(stdout);
            argc = file_argc;
            argv = file_argv;
        }
    }
    int rc = 0;
    try {
        rc = __real_main(argc, argv, envp);
    } catch (const std::exception & e) {
        std::fprintf(stderr, "FATAL EXCEPTION: %s\n", e.what());
        rc = 1;
    } catch (...) {
        std::fprintf(stderr, "FATAL UNKNOWN EXCEPTION\n");
        rc = 1;
    }
    // llama.cpp logs through a worker thread: drain it, then stdio.
    common_log_flush(common_log_main());
    std::fflush(nullptr);
    _Exit(rc);
}
