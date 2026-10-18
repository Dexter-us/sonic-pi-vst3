// SPDX-License-Identifier: GPL-3.0-or-later
#include "socket_platform.h"
#include "sonic_pi_live_runtime.h"

#include "clockwork_tcp_osc_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <cwchar>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
extern char** environ;
#endif

#ifndef SONIC_PI_ROOT_DIRECTORY
#error "SONIC_PI_ROOT_DIRECTORY must point to the fetched Sonic Pi source tree"
#endif

namespace SonicPiVst3 {
namespace {

constexpr std::uint32_t kControlToken = 483901;
constexpr std::size_t kMaxCodeBytes = 32000;
constexpr auto kStartupTimeout = std::chrono::seconds(60);

void appendU32BE(std::vector<std::uint8_t>& packet, std::uint32_t value) {
    packet.push_back(static_cast<std::uint8_t>(value >> 24));
    packet.push_back(static_cast<std::uint8_t>(value >> 16));
    packet.push_back(static_cast<std::uint8_t>(value >> 8));
    packet.push_back(static_cast<std::uint8_t>(value));
}

void appendOscString(std::vector<std::uint8_t>& packet, const std::string& value) {
    packet.insert(packet.end(), value.begin(), value.end());
    packet.push_back(0);
    while ((packet.size() & 3u) != 0u) packet.push_back(0);
}

std::vector<std::uint8_t> makeRunCodePacket(const std::string& code) {
    std::vector<std::uint8_t> packet;
    packet.reserve(code.size() + 48);
    appendOscString(packet, "/run-code");
    appendOscString(packet, ",issi");
    appendU32BE(packet, kControlToken);
    appendOscString(packet, code);
    appendOscString(packet, "vst-editor");
    appendU32BE(packet, 0);
    return packet;
}

std::vector<std::uint8_t> makeControlPacket(const char* address) {
    std::vector<std::uint8_t> packet;
    packet.reserve(20);
    appendOscString(packet, address);
    appendOscString(packet, ",i");
    appendU32BE(packet, kControlToken);
    return packet;
}

std::uint16_t reserveUdpPort() {
    const auto fd = SocketPlatform::create(AF_INET, SOCK_DGRAM, 0);
    if (fd == SocketPlatform::kInvalidSocket) return 0;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    std::uint16_t result = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address),
               static_cast<SocketPlatform::SocketLength>(sizeof(address))) == 0) {
        SocketPlatform::SocketLength size = sizeof(address);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0) {
            result = ntohs(address.sin_port);
        }
    }
    SocketPlatform::close(fd);
    return result;
}

#if !defined(_WIN32)
std::vector<std::string> childEnvironment(const std::string& root,
                                          const std::string& userHome) {
    std::vector<std::string> environment;
    constexpr std::array<const char*, 3> replaced{
        "SONIC_PI_ROOT=", "SONIC_PI_ETC_PATH=", "SONIC_PI_HOME="};
    for (char** item = environ; item && *item; ++item) {
        const std::string entry(*item);
        const bool isReplaced = std::any_of(
            replaced.begin(), replaced.end(),
            [&entry](const char* prefix) {
                return entry.rfind(prefix, 0) == 0;
            });
        if (!isReplaced) environment.push_back(entry);
    }
    environment.push_back("SONIC_PI_ROOT=" + root);
    environment.push_back("SONIC_PI_ETC_PATH=" + root + "/etc");
    environment.push_back("SONIC_PI_HOME=" + userHome);
    return environment;
}
#else
std::filesystem::path localDataDirectory() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required > 1) {
        std::vector<wchar_t> value(required);
        const DWORD length =
            GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
        if (length > 0 && length < required) {
            return std::filesystem::path(value.data());
        }
    }
    return std::filesystem::temp_directory_path();
}

std::wstring quoteWindowsArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::vector<wchar_t> childEnvironmentBlock(
    const std::filesystem::path& root,
    const std::filesystem::path& userHome,
    const std::filesystem::path& rubyBin) {
    std::vector<std::wstring> entries;
    LPWCH source = GetEnvironmentStringsW();
    if (!source) return {};
    bool replacedPath = false;
    std::wstring rubySearchPath;
    if (!rubyBin.empty()) {
        rubySearchPath = rubyBin.native();
        const auto rubyRoot = rubyBin.parent_path();
        for (const auto& extraPath : {
                 rubyRoot / L"msys64" / L"ucrt64" / L"bin",
                 rubyRoot / L"msys64" / L"usr" / L"bin"}) {
            if (std::filesystem::is_directory(extraPath)) {
                rubySearchPath += L";" + extraPath.native();
            }
        }
    }
    for (const wchar_t* current = source; *current;) {
        std::wstring entry(current);
        current += entry.size() + 1;
        if (_wcsnicmp(entry.c_str(), L"SONIC_PI_ROOT=", 14) == 0 ||
            _wcsnicmp(entry.c_str(), L"SONIC_PI_ETC_PATH=", 18) == 0 ||
            _wcsnicmp(entry.c_str(), L"SONIC_PI_HOME=", 14) == 0) {
            continue;
        }
        if (!rubyBin.empty() && _wcsnicmp(entry.c_str(), L"PATH=", 5) == 0) {
            entries.push_back(L"PATH=" + rubySearchPath + L";" +
                              entry.substr(5));
            replacedPath = true;
            continue;
        }
        entries.push_back(std::move(entry));
    }
    FreeEnvironmentStringsW(source);

    entries.push_back(L"SONIC_PI_ROOT=" + root.native());
    entries.push_back(L"SONIC_PI_ETC_PATH=" + (root / L"etc").native());
    entries.push_back(L"SONIC_PI_HOME=" + userHome.native());
    if (!rubyBin.empty() && !replacedPath) {
        entries.push_back(L"PATH=" + rubySearchPath);
    }
    std::sort(entries.begin(), entries.end(),
              [](const std::wstring& left, const std::wstring& right) {
                  return _wcsicmp(left.c_str(), right.c_str()) < 0;
              });

    std::vector<wchar_t> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (entries.empty()) block.push_back(L'\0');
    return block;
}

int runtimeModuleAnchor = 0;

std::filesystem::path currentModulePath() {
    HMODULE module = nullptr;
    const auto address = reinterpret_cast<LPCWSTR>(&runtimeModuleAnchor);
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            address, &module)) {
        return {};
    }

    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(
        module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::filesystem::path configuredRuntimeRoot() {
    const DWORD required =
        GetEnvironmentVariableW(L"SONIC_PI_VST_RUNTIME_ROOT", nullptr, 0);
    if (required <= 1) return {};
    std::vector<wchar_t> value(required);
    const DWORD length = GetEnvironmentVariableW(
        L"SONIC_PI_VST_RUNTIME_ROOT", value.data(), required);
    if (length == 0 || length >= required) return {};
    return std::filesystem::path(value.data());
}

std::filesystem::path bundledRuntimeRoot(
    const std::filesystem::path& modulePath) {
    if (modulePath.empty()) return {};
    const auto contentsDirectory = modulePath.parent_path().parent_path();
    return contentsDirectory / L"Resources" / L"SonicPiVstRuntime";
}

bool hasBundledRuntime(const std::filesystem::path& runtimeRoot) {
    return std::filesystem::is_regular_file(
               runtimeRoot / L"sonic-pi" / L"app" / L"server" / L"ruby" /
               L"bin" / L"spider-server.rb") &&
           std::filesystem::is_regular_file(
               runtimeRoot / L"ruby" / L"bin" / L"ruby.exe");
}

bool isVstModule(const std::filesystem::path& modulePath) {
    const auto extension = modulePath.extension().wstring();
    return _wcsicmp(extension.c_str(), L".vst3") == 0;
}

bool buildTreeRuntimeIsEnabled() {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(
               L"SONIC_PI_VST_ALLOW_BUILD_TREE_RUNTIME", value, 2) == 1 &&
           value[0] == L'1';
}
#endif

} // namespace

struct SonicPiLiveRuntime::Impl {
    ClockworkTcpOscBridge bridge;
    std::filesystem::path root;
    std::filesystem::path runtimeBundleRoot;
    std::filesystem::path rubyExecutable;
    std::filesystem::path userHome;
    std::filesystem::path logPath;
    std::uint16_t controlPort = 0;
    std::uint16_t guiPort = 0;
    std::uint16_t cuePort = 0;
    std::atomic<bool> isReady{false};
    std::atomic<bool> startupFailed{false};
    std::atomic<bool> monitorRunning{false};
    std::atomic<RuntimeStatus> uiStatus{RuntimeStatus::inactive};
    mutable std::mutex commandMutex;
    std::string pendingCode;
    mutable std::mutex errorMutex;
    std::string errorText;
    std::thread readinessThread;

    void setError(std::string message) noexcept {
        if (!message.empty()) {
            uiStatus.store(RuntimeStatus::failed);
        }
        std::lock_guard<std::mutex> lock(errorMutex);
        errorText = std::move(message);
    }

    std::string getError() const {
        std::lock_guard<std::mutex> lock(errorMutex);
        return errorText;
    }

    bool sendRunCodePacket(const std::string& code) noexcept {
        if (!SocketPlatform::initialize()) return false;
        const auto packet = makeRunCodePacket(code);
        const auto fd = SocketPlatform::create(AF_INET, SOCK_DGRAM, 0);
        if (fd == SocketPlatform::kInvalidSocket) return false;

        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        destination.sin_port = htons(controlPort);
        const auto sent = SocketPlatform::sendTo(
            fd, packet.data(), packet.size(), 0,
            reinterpret_cast<const sockaddr*>(&destination),
            static_cast<SocketPlatform::SocketLength>(sizeof(destination)));
        SocketPlatform::close(fd);
        return sent == static_cast<std::ptrdiff_t>(packet.size());
    }

    bool configureRuntimePaths() {
#if defined(_WIN32)
        const auto configuredRoot = configuredRuntimeRoot();
        if (!configuredRoot.empty()) {
            if (!hasBundledRuntime(configuredRoot)) {
                setError("The configured Sonic Pi VST runtime is incomplete: " +
                         configuredRoot.string());
                return false;
            }
            runtimeBundleRoot = configuredRoot;
        } else {
            const auto modulePath = currentModulePath();
            const auto bundledRoot = bundledRuntimeRoot(modulePath);
            if (hasBundledRuntime(bundledRoot)) {
                runtimeBundleRoot = bundledRoot;
            } else if (isVstModule(modulePath) &&
                       !buildTreeRuntimeIsEnabled()) {
                setError("The Sonic Pi VST runtime is missing from the plug-in "
                         "bundle. Reinstall the complete Windows package.");
                return false;
            } else {
                // Executable probes and local development builds use the
                // source tree fetched by CMake. A VST module never falls back
                // to this machine-specific build path.
                root = SONIC_PI_ROOT_DIRECTORY;
            }
        }

        if (!runtimeBundleRoot.empty()) {
            root = runtimeBundleRoot / L"sonic-pi";
            rubyExecutable = runtimeBundleRoot / L"ruby" / L"bin" / L"ruby.exe";
        }
#else
        root = SONIC_PI_ROOT_DIRECTORY;
#endif
        const auto spiderServer = root / "app/server/ruby/bin/spider-server.rb";
        if (!std::filesystem::is_regular_file(spiderServer)) {
            setError("Sonic Pi Ruby server is missing from the selected runtime tree");
            return false;
        }
#if defined(_WIN32)
        if (!rubyExecutable.empty() &&
            !std::filesystem::is_regular_file(rubyExecutable)) {
            setError("Bundled ruby.exe is missing from the Sonic Pi VST runtime");
            return false;
        }
#endif
        return true;
    }

#if defined(_WIN32)
    HANDLE workerProcess = nullptr;
#else
    pid_t workerPid = -1;
#endif

    bool start(ClockworkClient* client, double engineSampleRate) noexcept {
        if (isReady.load() || monitorRunning.load()) return true;
        startupFailed.store(false);
        setError({});
        uiStatus.store(RuntimeStatus::starting);
        if (!configureRuntimePaths()) return false;
        const auto spiderServer = root / "app/server/ruby/bin/spider-server.rb";

#if defined(_WIN32)
        if (!SocketPlatform::initialize()) {
            setError("Could not initialize Windows socket support");
            return false;
        }
        userHome = localDataDirectory() / L"sonic-pi-vst";
#else
        const char* homeEnv = std::getenv("HOME");
        const auto base = homeEnv && *homeEnv
            ? std::filesystem::path(homeEnv)
            : std::filesystem::temp_directory_path();
        userHome = base / ".sonic-pi-vst";
#endif
        logPath = userHome / "spider-runtime.log";

        try {
            std::filesystem::create_directories(userHome);
        } catch (const std::exception& exception) {
            setError(std::string("Cannot create Sonic Pi runtime directory: ") +
                     exception.what());
            return false;
        }

        controlPort = reserveUdpPort();
        guiPort = reserveUdpPort();
        cuePort = reserveUdpPort();
        if (controlPort == 0 || guiPort == 0 || cuePort == 0) {
            setError("Could not reserve local OSC control ports");
            return false;
        }
        if (!bridge.start(client, engineSampleRate)) {
            setError("Could not start the local SuperSonic OSC bridge");
            return false;
        }

        if (!launchSpider(spiderServer)) {
            bridge.stop();
            return false;
        }
        monitorRunning.store(true);
        try {
            readinessThread = std::thread([this] { monitorSpider(); });
        } catch (const std::exception& exception) {
            setError(std::string("Could not monitor the Sonic Pi runtime: ") +
                     exception.what());
            stop();
            return false;
        }
        return true;
    }

    bool launchSpider(const std::filesystem::path& spiderServer) {
#if defined(_WIN32)
        const auto wideNumber = [](std::uint16_t value) {
            const auto number = std::to_string(value);
            return std::wstring(number.begin(), number.end());
        };
        const std::vector<std::wstring> arguments{
            rubyExecutable.empty() ? L"ruby.exe" : rubyExecutable.native(),
            L"--disable=gems",
            spiderServer.native(),
            L"-u",
            wideNumber(controlPort),
            wideNumber(guiPort),
            wideNumber(bridge.port()),
            wideNumber(bridge.port()),
            wideNumber(cuePort),
            std::wstring(std::to_wstring(kControlToken)),
        };
        std::wstring commandLine;
        for (const auto& argument : arguments) {
            if (!commandLine.empty()) commandLine.push_back(L' ');
            commandLine += quoteWindowsArgument(argument);
        }
        auto environment = childEnvironmentBlock(
            root, userHome,
            rubyExecutable.empty() ? std::filesystem::path{}
                                  : rubyExecutable.parent_path());
        if (environment.empty()) {
            setError("Could not copy the Windows environment for Ruby");
            return false;
        }

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        HANDLE logHandle = CreateFileW(
            logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logHandle == INVALID_HANDLE_VALUE) {
            setError("Could not create the Sonic Pi runtime log file");
            return false;
        }
        HANDLE nullInput = CreateFileW(
            L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (nullInput == INVALID_HANDLE_VALUE) {
            CloseHandle(logHandle);
            setError("Could not open the Windows null input device");
            return false;
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nullInput;
        startup.hStdOutput = logHandle;
        startup.hStdError = logHandle;
        PROCESS_INFORMATION process{};
        const BOOL created = CreateProcessW(
            rubyExecutable.empty() ? nullptr : rubyExecutable.c_str(),
            commandLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            environment.data(), root.c_str(), &startup, &process);
        const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
        CloseHandle(nullInput);
        CloseHandle(logHandle);
        if (!created) {
            setError("Could not start Ruby on Windows (CreateProcess error " +
                     std::to_string(createError) + ")");
            return false;
        }
        workerProcess = process.hProcess;
        CloseHandle(process.hThread);
        return true;
#else
        std::vector<std::string> arguments{
            "ruby",
            "--disable=gems",
            spiderServer.string(),
            "-u",
            std::to_string(controlPort),
            std::to_string(guiPort),
            std::to_string(bridge.port()),
            std::to_string(bridge.port()),
            std::to_string(cuePort),
            std::to_string(kControlToken),
        };
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);

        auto environmentStrings = childEnvironment(root.string(), userHome.string());
        std::vector<char*> envp;
        envp.reserve(environmentStrings.size() + 1);
        for (auto& entry : environmentStrings) envp.push_back(entry.data());
        envp.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) {
            setError("Could not initialize Ruby process file actions");
            return false;
        }
        const int logFd = posix_spawn_file_actions_addopen(
            &actions, STDOUT_FILENO, logPath.c_str(),
            O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int errFd = logFd == 0
            ? posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO)
            : logFd;

        int spawnResult = errFd;
        if (spawnResult == 0) {
            spawnResult = posix_spawnp(&workerPid, "ruby", &actions, nullptr,
                                       argv.data(), envp.data());
        }
        posix_spawn_file_actions_destroy(&actions);
        if (spawnResult != 0) {
            workerPid = -1;
            setError(std::string("Could not start Ruby: ") +
                     std::strerror(spawnResult));
            return false;
        }
        return true;
#endif
    }

    void monitorSpider() noexcept {
        const auto deadline = std::chrono::steady_clock::now() + kStartupTimeout;
        while (monitorRunning.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::ifstream log(logPath, std::ios::binary);
            if (log) {
                const std::string content(
                    (std::istreambuf_iterator<char>(log)),
                    std::istreambuf_iterator<char>());
                if (content.find("Spider - Booted Successfully.") !=
                    std::string::npos) {
                    std::string codeToRun;
                    {
                        std::lock_guard<std::mutex> lock(commandMutex);
                        isReady.store(true);
                        codeToRun.swap(pendingCode);
                        uiStatus.store(codeToRun.empty()
                            ? RuntimeStatus::ready
                            : RuntimeStatus::queued);
                    }
                    if (!codeToRun.empty()) {
                        if (sendRunCodePacket(codeToRun)) {
                            uiStatus.store(RuntimeStatus::submitted);
                        } else {
                            setError("Could not send queued Run code to Spider");
                            uiStatus.store(RuntimeStatus::error);
                        }
                    }
                    return;
                }
                if (content.find("Spider - Failed to start server:") !=
                    std::string::npos) {
                    setError("Sonic Pi failed during startup; inspect " +
                             logPath.string());
                    startupFailed.store(true);
                    monitorRunning.store(false);
                    return;
                }
            }
#if defined(_WIN32)
            if (workerProcess &&
                WaitForSingleObject(workerProcess, 0) == WAIT_OBJECT_0) {
                DWORD exitCode = 0;
                GetExitCodeProcess(workerProcess, &exitCode);
                setError("Sonic Pi Ruby server exited during startup with code " +
                         std::to_string(exitCode) + "; inspect " +
                         logPath.string());
                startupFailed.store(true);
                monitorRunning.store(false);
                return;
            }
#endif
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (monitorRunning.load()) {
            setError("Sonic Pi startup timed out; inspect " + logPath.string());
            startupFailed.store(true);
            monitorRunning.store(false);
        }
    }

    void stopWorker(bool graceful) noexcept {
#if defined(_WIN32)
        if (!workerProcess) return;
        if (graceful) {
            sendControl("/stop-all-jobs");
            sendControl("/exit");
            if (WaitForSingleObject(workerProcess, 600) == WAIT_OBJECT_0) {
                CloseHandle(workerProcess);
                workerProcess = nullptr;
                return;
            }
        }
        TerminateProcess(workerProcess, 1);
        WaitForSingleObject(workerProcess, 600);
        CloseHandle(workerProcess);
        workerProcess = nullptr;
#else
        if (workerPid <= 0) return;
        if (graceful) {
            sendControl("/stop-all-jobs");
            sendControl("/exit");
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(600);
            while (std::chrono::steady_clock::now() < deadline) {
                int status = 0;
                const auto result = waitpid(workerPid, &status, WNOHANG);
                if (result == workerPid || (result < 0 && errno == ECHILD)) {
                    workerPid = -1;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }

        kill(workerPid, SIGTERM);
        const auto termDeadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(600);
        while (std::chrono::steady_clock::now() < termDeadline) {
            int status = 0;
            const auto result = waitpid(workerPid, &status, WNOHANG);
            if (result == workerPid || (result < 0 && errno == ECHILD)) {
                workerPid = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(workerPid, SIGKILL);
        int status = 0;
        while (waitpid(workerPid, &status, 0) < 0 && errno == EINTR) {}
        workerPid = -1;
#endif
    }

    bool sendControl(const char* address) noexcept {
        if (controlPort == 0) return false;
        if (!SocketPlatform::initialize()) return false;
        const auto packet = makeControlPacket(address);
        const auto fd = SocketPlatform::create(AF_INET, SOCK_DGRAM, 0);
        if (fd == SocketPlatform::kInvalidSocket) return false;

        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        destination.sin_port = htons(controlPort);
        const auto sent = SocketPlatform::sendTo(
            fd, packet.data(), packet.size(), 0,
            reinterpret_cast<const sockaddr*>(&destination),
            static_cast<SocketPlatform::SocketLength>(sizeof(destination)));
        SocketPlatform::close(fd);
        return sent == static_cast<std::ptrdiff_t>(packet.size());
    }

    void stop() noexcept {
        const bool wasReady = isReady.exchange(false);
        monitorRunning.store(false);
        {
            std::lock_guard<std::mutex> lock(commandMutex);
            pendingCode.clear();
        }
        if (readinessThread.joinable()) readinessThread.join();
        stopWorker(wasReady);
        bridge.stop();
        controlPort = 0;
        guiPort = 0;
        cuePort = 0;
        uiStatus.store(RuntimeStatus::inactive);
    }
};

SonicPiLiveRuntime::SonicPiLiveRuntime()
    : impl_(std::make_unique<Impl>()) {}

SonicPiLiveRuntime::~SonicPiLiveRuntime() {
    stop();
}

bool SonicPiLiveRuntime::start(ClockworkClient* client,
                               double sampleRate) noexcept {
    return impl_->start(client, sampleRate);
}

void SonicPiLiveRuntime::stop() noexcept {
    impl_->stop();
}

bool SonicPiLiveRuntime::runCode(const std::string& code) noexcept {
    if (code.empty() || code.size() > kMaxCodeBytes ||
        impl_->startupFailed.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->commandMutex);
        if (!impl_->isReady.load()) {
            if (!impl_->monitorRunning.load() ||
                impl_->startupFailed.load()) {
                return false;
            }
            // Keep only the newest editor buffer if Run is clicked more than
            // once while Spider is still booting.
            impl_->pendingCode = code;
            impl_->uiStatus.store(RuntimeStatus::queued);
            return true;
        }
    }

    if (impl_->sendRunCodePacket(code)) {
        impl_->uiStatus.store(RuntimeStatus::submitted);
        return true;
    }
    impl_->uiStatus.store(RuntimeStatus::error);
    return false;
}

bool SonicPiLiveRuntime::stopAllJobs() noexcept {
    bool cancelledQueuedCode = false;
    {
        std::lock_guard<std::mutex> lock(impl_->commandMutex);
        cancelledQueuedCode = !impl_->pendingCode.empty();
        impl_->pendingCode.clear();
        if (cancelledQueuedCode) {
            impl_->uiStatus.store(RuntimeStatus::stopped);
        }
    }
    if (!impl_->isReady.load()) return cancelledQueuedCode;
    if (!impl_->sendControl("/stop-all-jobs")) {
        impl_->uiStatus.store(RuntimeStatus::error);
        return false;
    }
    impl_->uiStatus.store(RuntimeStatus::stopped);
    return true;
}

bool SonicPiLiveRuntime::ready() const noexcept {
    return impl_->isReady.load();
}

bool SonicPiLiveRuntime::failed() const noexcept {
    return impl_->startupFailed.load();
}

RuntimeStatus SonicPiLiveRuntime::status() const noexcept {
    return impl_->uiStatus.load();
}

std::string SonicPiLiveRuntime::error() const {
    return impl_->getError();
}

} // namespace SonicPiVst3
