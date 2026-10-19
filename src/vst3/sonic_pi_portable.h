// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace SonicPiVst3 {
struct PortablePaths {
    std::filesystem::path root;
    std::filesystem::path snips() const { return root / "Snips"; }
    std::filesystem::path code() const { return root / "Files" / "Code"; }
    std::filesystem::path generators() const { return root / "Files" / "Generators"; }
    std::filesystem::path help() const { return root / "Help"; }
    std::filesystem::path runtime() const { return root / "RuntimeData"; }
};
PortablePaths portablePathsFromModule(const std::filesystem::path& module);
PortablePaths portablePaths();
std::string pathUtf8(const std::filesystem::path& path);
bool ensurePortableDirectories(const PortablePaths& paths, std::string& error);
bool pathIsInside(const std::filesystem::path& file,
                  const std::filesystem::path& directory);
std::vector<std::filesystem::path> listSnips(const PortablePaths& paths);
std::vector<std::filesystem::path> listSnipsDirectory(
    const std::filesystem::path& directory);
bool isUtf8Source(std::string_view text);
bool openPortablePath(const std::filesystem::path& path);
bool savePortableSource(const std::filesystem::path& file,
                        const std::filesystem::path& directory,
                        const std::string& text, std::string& error);
} // namespace SonicPiVst3
