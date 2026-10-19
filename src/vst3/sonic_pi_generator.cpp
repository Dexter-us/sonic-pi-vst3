// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonic_pi_generator.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <random>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace SonicPiVst3 {
namespace {
namespace fs = std::filesystem;
constexpr std::size_t kMaxDiagnosticBytes = 16384;
constexpr char kRunner[] = R"RUBY(
# encoding: UTF-8
wire = STDOUT
$stdout = STDERR
wire.binmode
source = File.binread(ARGV.fetch(0)).force_encoding(Encoding::UTF_8)
raise EncodingError, "Generator script is not valid UTF-8" unless source.valid_encoding?
value = eval(source, TOPLEVEL_BINDING, ARGV[0], 1)
raise TypeError, "Generator must return a String as its final expression" unless value.is_a?(String)
value = value.encode(Encoding::UTF_8)
raise EncodingError, "Generated source is not valid UTF-8" unless value.valid_encoding?
raise RangeError, "Generated source exceeds 32000 bytes" if value.bytesize > 32000
wire.write(value)
)RUBY";

struct ScratchDirectory {
    fs::path path;
    ScratchDirectory() {
        std::random_device random;
        const auto root = fs::temp_directory_path();
        for (int attempt = 0; attempt < 16; ++attempt) {
            auto candidate = root / ("dexter-sonic-pi-generator-" +
                std::to_string(random()) + "-" + std::to_string(random()));
            if (fs::create_directory(candidate)) {
                path = std::move(candidate);
#if !defined(_WIN32)
                fs::permissions(path, fs::perms::owner_all);
#endif
                return;
            }
        }
        throw std::runtime_error("Could not create a private generator directory");
    }
    ~ScratchDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void writeFile(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) throw std::runtime_error("Could not write generator input");
}

std::string readFile(const fs::path& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not read generator output");
    std::string text(limit + 1, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || text.size() > limit) {
        throw std::runtime_error("Generator output exceeded its size limit");
    }
    return text;
}

bool oversized(const fs::path& path, std::size_t limit) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    return !error && size > limit;
}

bool validSource(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (!first) return false;
        if (first < 0x80) continue;
        unsigned remaining = 0;
        std::uint32_t code = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1; code = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2; code = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3; code = first & 7; minimum = 0x10000;
        } else return false;
        if (remaining > text.size() - i) return false;
        while (remaining--) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0) != 0x80) return false;
            code = (code << 6) | (byte & 0x3f);
        }
        if (code < minimum || code > 0x10ffff ||
            (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

#if defined(_WIN32)
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

std::wstring quote(const std::wstring& arg) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (auto character : arg) {
        if (character == L'\\') { ++slashes; continue; }
        result.append(slashes * (character == L'"' ? 2 : 1), L'\\');
        if (character == L'"') result.push_back(L'\\');
        result.push_back(character);
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}

int moduleAnchor;
fs::path bundledRuby() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&moduleAnchor), &module)) {
        throw std::runtime_error("Could not locate the plug-in's Ruby runtime");
    }
    std::vector<wchar_t> name(32768);
    auto length = GetModuleFileNameW(module, name.data(), static_cast<DWORD>(name.size()));
    if (!length || length >= name.size()) {
        throw std::runtime_error("Could not locate the plug-in bundle");
    }
    return fs::path(std::wstring(name.data(), length)).parent_path().parent_path() /
        L"Resources" / L"SonicPiVstRuntime" / L"ruby" / L"bin" / L"ruby.exe";
}

std::vector<wchar_t> rubyEnvironment(const fs::path& ruby) {
    const auto bin = fs::absolute(ruby).parent_path();
    auto search = bin.native();
    for (const auto& extra : {bin.parent_path() / L"msys64" / L"ucrt64" / L"bin",
                              bin.parent_path() / L"msys64" / L"usr" / L"bin"}) {
        if (fs::is_directory(extra)) search += L";" + extra.native();
    }
    auto* source = GetEnvironmentStringsW();
    if (!source) throw std::runtime_error("Could not prepare Ruby's environment");
    std::vector<std::wstring> entries;
    bool pathFound = false;
    for (const auto* cursor = source; *cursor;) {
        std::wstring entry(cursor);
        cursor += entry.size() + 1;
        if (_wcsnicmp(entry.c_str(), L"PATH=", 5) == 0) {
            entry = L"PATH=" + search + L";" + entry.substr(5);
            pathFound = true;
        }
        entries.push_back(std::move(entry));
    }
    FreeEnvironmentStringsW(source);
    if (!pathFound) entries.push_back(L"PATH=" + search);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::vector<wchar_t> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// Only the three explicit stdio handles may be inherited from the DAW.
struct AttributeList {
    std::vector<unsigned char> bytes;
    LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
    ~AttributeList() { if (value) DeleteProcThreadAttributeList(value); }
    bool initialize(HANDLE* handles) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        bytes.resize(size);
        auto* candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        if (!InitializeProcThreadAttributeList(candidate, 1, 0, &size)) return false;
        value = candidate;
        return UpdateProcThreadAttribute(
            value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles, sizeof(HANDLE) * 3, nullptr, nullptr) != 0;
    }
};

struct ChildProcess {
    Handle process;
    Handle job;
    int exitCode = -1;
    ~ChildProcess() {
        // This kills descendants too, even when the generator exits normally.
        if (job.value) TerminateJobObject(job.value, 1);
        if (process.value) {
            TerminateProcess(process.value, 1);
            WaitForSingleObject(process.value, INFINITE);
        }
    }
    bool start(const fs::path& ruby, const fs::path& dir, std::string& error) {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        Handle output{CreateFileW(
            (dir / L"output.txt").c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        Handle diagnostic{CreateFileW(
            (dir / L"error.txt").c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &security, OPEN_EXISTING, 0, nullptr)};
        if (output.value == INVALID_HANDLE_VALUE ||
            diagnostic.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE) {
            error = "Could not open generator process streams";
            return false;
        }
        HANDLE handles[]{input.value, output.value, diagnostic.value};
        AttributeList attributes;
        if (!attributes.initialize(handles)) {
            error = "Could not isolate generator process handles";
            return false;
        }
        job.value = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job.value || !SetInformationJobObject(
                job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            error = "Could not create generator process job";
            return false;
        }
        auto command = quote(ruby.native()) +
            L" --disable=rubyopt -EUTF-8:UTF-8 " +
            quote((dir / L"runner.rb").native()) + L" " +
            quote((dir / L"generator.rb").native());
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.value;
        startup.StartupInfo.hStdOutput = output.value;
        startup.StartupInfo.hStdError = diagnostic.value;
        startup.lpAttributeList = attributes.value;
        PROCESS_INFORMATION info{};
        auto environment = rubyEnvironment(ruby);
        if (!CreateProcessW(
                ruby.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT |
                    CREATE_UNICODE_ENVIRONMENT,
                environment.data(), dir.c_str(), &startup.StartupInfo, &info)) {
            error = "Could not start Ruby (Windows error " +
                    std::to_string(GetLastError()) + ")";
            return false;
        }
        process.value = info.hProcess;
        Handle thread{info.hThread};
        if (!AssignProcessToJobObject(job.value, process.value) ||
            ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
            error = "Could not isolate/start the Ruby generator process";
            return false;
        }
        return true;
    }
    bool exited() {
        const auto result = WaitForSingleObject(process.value, 0);
        if (result == WAIT_FAILED) throw std::runtime_error("Could not wait for Ruby");
        if (result == WAIT_TIMEOUT) return false;
        DWORD code = 1;
        if (!GetExitCodeProcess(process.value, &code)) {
            throw std::runtime_error("Could not read Ruby exit status");
        }
        exitCode = static_cast<int>(code);
        TerminateJobObject(job.value, 1);
        return true;
    }
};
#else
struct ChildProcess {
    pid_t pid = -1;
    bool reaped = false;
    int exitCode = -1;
    ~ChildProcess() {
        if (pid <= 0) return;
        kill(-pid, SIGKILL);
        if (!reaped) {
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    bool start(const fs::path& ruby, const fs::path& dir, std::string& error) {
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        int result = posix_spawn_file_actions_init(&actions);
        if (result) { error = std::strerror(result); return false; }
        result = posix_spawnattr_init(&attributes);
        if (result) {
            posix_spawn_file_actions_destroy(&actions);
            error = std::strerror(result);
            return false;
        }
        auto check = [&result](int next) { if (!result) result = next; };
        check(posix_spawn_file_actions_addopen(
            &actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
        check(posix_spawn_file_actions_addopen(
            &actions, STDOUT_FILENO, (dir / "output.txt").c_str(),
            O_WRONLY | O_CREAT | O_TRUNC, 0600));
        check(posix_spawn_file_actions_addopen(
            &actions, STDERR_FILENO, (dir / "error.txt").c_str(),
            O_WRONLY | O_CREAT | O_TRUNC, 0600));
        check(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
        check(posix_spawnattr_setpgroup(&attributes, 0));
        std::vector<std::string> args{ruby.string(), "--disable=rubyopt",
            "-EUTF-8:UTF-8", (dir / "runner.rb").string(),
            (dir / "generator.rb").string()};
        std::vector<char*> argv;
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        if (!result) {
            result = posix_spawnp(&pid, argv[0], &actions, &attributes,
                                  argv.data(), environ);
        }
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        if (result) {
            pid = -1;
            error = std::string("Could not start Ruby: ") + std::strerror(result);
            return false;
        }
        return true;
    }
    bool exited() {
        int status = 0;
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == 0 || (result < 0 && errno == EINTR)) return false;
        if (result < 0) throw std::runtime_error("Could not wait for Ruby");
        reaped = true;
        kill(-pid, SIGKILL);
        exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return true;
    }
};
#endif
} // namespace

GeneratorResult runRubyGenerator(
    const std::string& source, const GeneratorOptions& options,
    const std::atomic<bool>& cancel) {
    GeneratorResult result;
    try {
        if (source.size() > kMaxGeneratorSourceBytes) {
            throw std::runtime_error("Generator script exceeds 64 KiB");
        }
        if (cancel.load()) return {false, true, {}, "Generation cancelled"};
        ScratchDirectory scratch;
        writeFile(scratch.path / "runner.rb", kRunner);
        writeFile(scratch.path / "generator.rb", source);
        auto ruby = options.rubyExecutable;
        if (ruby.empty()) {
#if defined(_WIN32)
            ruby = bundledRuby();
            if (!fs::is_regular_file(ruby)) {
                throw std::runtime_error("Bundled Ruby is missing; reinstall the complete VST3 bundle");
            }
#else
            ruby = "ruby";
#endif
        }
        ChildProcess process;
        if (!process.start(ruby, scratch.path, result.error)) return result;
        const auto deadline = std::chrono::steady_clock::now() + options.timeout;
        while (true) {
            if (cancel.load()) return {false, true, {}, "Generation cancelled"};
            if (oversized(scratch.path / "output.txt", kMaxGeneratorOutputBytes) ||
                oversized(scratch.path / "error.txt", kMaxDiagnosticBytes)) {
                throw std::runtime_error("Generator exceeded its output/diagnostic size limit");
            }
            if (process.exited()) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("Generator timed out after " +
                    std::to_string(options.timeout.count()) + " ms");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto diagnostics = readFile(scratch.path / "error.txt", kMaxDiagnosticBytes);
        if (process.exitCode != 0) {
            result.error = diagnostics.empty()
                ? "Ruby generator exited with code " + std::to_string(process.exitCode)
                : diagnostics;
            return result;
        }
        result.output = readFile(scratch.path / "output.txt", kMaxGeneratorOutputBytes);
        if (!validSource(result.output)) {
            result.output.clear();
            throw std::runtime_error("Generated source must be valid UTF-8 without NUL bytes");
        }
        result.success = true;
        // Preserve nonfatal diagnostics for display alongside the preview.
        result.error = diagnostics;
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    return result;
}

RubyGenerator::~RubyGenerator() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

bool RubyGenerator::start(std::string source, GeneratorOptions options) {
    if (running_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    cancel_.store(false);
    {
        std::lock_guard<std::mutex> lock(resultMutex_);
        result_.reset();
    }
    try {
        worker_ = std::thread([this, source = std::move(source), options] {
            auto result = runRubyGenerator(source, options, cancel_);
            {
                std::lock_guard<std::mutex> lock(resultMutex_);
                result_ = std::move(result);
            }
            running_.store(false);
        });
    } catch (...) {
        running_.store(false);
        return false;
    }
    return true;
}

std::optional<GeneratorResult> RubyGenerator::takeResult() {
    std::optional<GeneratorResult> result;
    {
        std::lock_guard<std::mutex> lock(resultMutex_);
        result.swap(result_);
    }
    if (result && worker_.joinable()) worker_.join();
    return result;
}

} // namespace SonicPiVst3
