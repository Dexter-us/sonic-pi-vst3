// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonic_pi_portable.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <fstream>
#include <random>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <dlfcn.h>
#include <spawn.h>
#include <sys/wait.h>
#include <cerrno>
extern char** environ;
#endif

namespace SonicPiVst3 {
namespace {
int moduleAnchor;
std::string lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
    return text;
}
}
std::string pathUtf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
PortablePaths portablePathsFromModule(const std::filesystem::path& module) {
    // Begin at the parent: Windows' module file itself also ends in .vst3.
    for (auto directory = module.parent_path(); !directory.empty();) {
        if (lower(pathUtf8(directory.extension())) == ".vst3") return {directory};
        const auto parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    if (module.parent_path().empty()) {
        throw std::runtime_error("Cannot locate the portable plug-in folder");
    }
    return {module.parent_path() / "SonicPiPortable"};
}
PortablePaths portablePaths() {
#if defined(_WIN32)
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&moduleAnchor), &module)) {
        throw std::runtime_error("Cannot locate the plug-in module");
    }
    std::vector<wchar_t> name(32768);
    const auto count = GetModuleFileNameW(module, name.data(), name.size());
    if (!count || count >= name.size()) {
        throw std::runtime_error("Cannot read the plug-in module location");
    }
    return portablePathsFromModule(std::filesystem::path(name.data()));
#else
    Dl_info info{};
    if (!dladdr(&moduleAnchor, &info) || !info.dli_fname) {
        throw std::runtime_error("Cannot locate the plug-in module");
    }
    return portablePathsFromModule(std::filesystem::absolute(info.dli_fname));
#endif
}
bool ensurePortableDirectories(const PortablePaths& paths, std::string& error) {
    try {
        for (const auto& directory : {paths.snips(), paths.code(), paths.generators(),
                                       paths.help(), paths.runtime()}) {
            std::filesystem::create_directories(directory);
        }
        return true;
    } catch (const std::exception& e) {
        error = "Portable folder is unavailable. Move the complete .vst3 folder "
                "to a writable location. " + std::string(e.what());
        return false;
    }
}
bool pathIsInside(const std::filesystem::path& file,
                  const std::filesystem::path& directory) {
    try {
        const auto parent = std::filesystem::weakly_canonical(directory);
        const auto target = std::filesystem::weakly_canonical(file);
        auto p = parent.begin(), t = target.begin();
        for (; p != parent.end(); ++p, ++t) {
            if (t == target.end()) return false;
#if defined(_WIN32)
            if (_wcsicmp(p->c_str(), t->c_str()) != 0) return false;
#else
            if (*p != *t) return false;
#endif
        }
        return t != target.end();
    } catch (...) { return false; }
}
std::vector<std::filesystem::path> listSnips(const PortablePaths& paths) {
    return listSnipsDirectory(paths.snips());
}
std::vector<std::filesystem::path> listSnipsDirectory(
    const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_symlink() || !entry.is_regular_file()) continue;
        const auto extension = lower(pathUtf8(entry.path().extension()));
        if (extension != ".rb" && extension != ".txt") continue;
        if (files.size() == 128) {
            throw std::runtime_error("Snips folder exceeds the 128-file menu limit");
        }
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return pathUtf8(a.filename()) < pathUtf8(b.filename());
    });
    return files;
}
bool isUtf8Source(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (!first) return false;
        if (first < 0x80) continue;
        unsigned count = 0, code = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1; code = first & 31; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2; code = first & 15; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3; code = first & 7; minimum = 0x10000;
        } else return false;
        if (count > text.size() - i) return false;
        while (count--) {
            const auto c = static_cast<unsigned char>(text[i++]);
            if ((c & 0xc0) != 0x80) return false;
            code = (code << 6) | (c & 63);
        }
        if (code < minimum || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
bool savePortableSource(const std::filesystem::path& file,
                        const std::filesystem::path& directory,
                        const std::string& text, std::string& error) {
    std::filesystem::path scratch;
    try {
        if (!pathIsInside(file, directory)) {
            throw std::runtime_error("Choose a file inside " + pathUtf8(directory));
        }
        std::filesystem::create_directories(file.parent_path());
        for (int attempt = 0; attempt < 16; ++attempt) {
            auto candidate = file.parent_path() /
                (".sonic-pi-save-" + std::to_string(std::random_device{}()));
            if (std::filesystem::create_directory(candidate)) {
                scratch = std::move(candidate);
                break;
            }
        }
        if (scratch.empty()) throw std::runtime_error("Cannot create save staging folder");
        const auto temporary = scratch / "source.tmp";
        {
            std::ofstream output(temporary, std::ios::binary);
            output.write(text.data(), text.size());
            output.flush();
            if (!output.good()) throw std::runtime_error("Cannot write source file");
            output.close();
            if (output.fail()) throw std::runtime_error("Cannot close source file");
        }
#if defined(_WIN32)
        if (!MoveFileExW(temporary.c_str(), file.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("Cannot replace source file (Windows error " +
                                     std::to_string(GetLastError()) + ")");
        }
#else
        std::filesystem::rename(temporary, file);
#endif
        std::filesystem::remove(scratch);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        std::error_code ignored;
        if (!scratch.empty()) std::filesystem::remove_all(scratch, ignored);
        return false;
    }
}
bool openPortablePath(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return false;
#if defined(_WIN32)
    return reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    std::string text = pathUtf8(path);
    char shell[] = "sh";
    char option[] = "-c";
#if defined(__APPLE__)
    char command[] = "command -v open >/dev/null 2>&1 || exit 127; open \"$1\" >/dev/null 2>&1 &";
#else
    char command[] = "command -v xdg-open >/dev/null 2>&1 || exit 127; xdg-open \"$1\" >/dev/null 2>&1 &";
#endif
    char name[] = "sonic-pi-open";
    // The path is a separate positional argument, never interpolated into a
    // shell command. Reap the short-lived launcher without leaving a thread
    // executing plug-in code after the host unloads the library.
    char* args[]{shell, option, command, name, text.data(), nullptr};
    pid_t pid = -1;
    if (posix_spawnp(&pid, shell, nullptr, nullptr, args, environ) != 0) return false;
    int status = 0;
    pid_t waited = -1;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}
} // namespace SonicPiVst3
