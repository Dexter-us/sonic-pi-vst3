// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonic_pi_portable.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
using namespace SonicPiVst3;
namespace fs = std::filesystem;
namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
}
int main() {
    const auto scratch = fs::temp_directory_path() /
        ("sonic-pi-portable-test-" + std::to_string(std::random_device{}()));
    try {
        const auto root = scratch / "original.vst3";
        const auto module = root / "Contents" / "x86_64-win" / "sonic_pi_vst3.vst3";
        const auto paths = portablePathsFromModule(module);
        require(paths.root == root, "Windows binary mistaken for the bundle folder");
        require(portablePathsFromModule(root / "Contents" / "Resources" /
                                        "SonicPiVstRuntime").root == root,
                "Bundled runtime did not resolve its portable root");
        std::string error;
        require(isUtf8Source("# café 音\n") && !isUtf8Source("\xff") &&
                !isUtf8Source(std::string("x\0y", 3)) &&
                !isUtf8Source("\xed\xa0\x80") && !isUtf8Source("\xc0\x80"),
                "Invalid source text was accepted");
        require(ensurePortableDirectories(paths, error), "Cannot create portable directories");
        const auto file = paths.code() / fs::path(u8"test 音.rb");
        require(savePortableSource(file, paths.code(), "play 60\n", error),
                "Cannot save Unicode filename");
        require(savePortableSource(file, paths.code(), "# café\nplay 72\n", error) &&
                read(file) == "# café\nplay 72\n", "Cannot safely replace source");
        require(!savePortableSource(scratch / "outside.rb", paths.code(), "bad", error) &&
                !fs::exists(scratch / "outside.rb"), "Save escaped the portable directory");
        require(!pathIsInside(paths.code(), paths.code()) &&
                !pathIsInside(paths.code() / ".." / "escape.rb", paths.code()) &&
                !pathIsInside(root / "Files" / "Code-other" / "test.rb", paths.code()),
                "Path boundary check accepted an outside path");
        const auto occupied = paths.code() / "occupied";
        fs::create_directory(occupied);
        require(!savePortableSource(occupied, paths.code(), "data", error) &&
                fs::is_directory(occupied) && read(file) == "# café\nplay 72\n",
                "Failed save damaged existing files");
        std::ofstream(paths.snips() / "b.rb") << "play 64";
        std::ofstream(paths.snips() / "a.TXT") << "play 60";
        std::ofstream(paths.snips() / "ignored.md") << "# notes";
        fs::create_directories(paths.snips() / "nested");
        std::ofstream(paths.snips() / "nested" / "hidden.rb") << "play 50";
        auto snips = listSnips(paths);
        require(snips.size() == 2 && snips[0].filename() == "a.TXT",
                "Snippet discovery did not filter or sort regular source files");
        std::ofstream(paths.snips() / "new.rb") << "play 67";
        require(listSnips(paths).size() == 3, "Snippet discovery was cached");
        fs::create_directories(scratch / "outside");
        std::error_code ec;
        fs::create_directory_symlink(scratch / "outside", paths.code() / "link", ec);
        if (!ec) require(!pathIsInside(paths.code() / "link" / "file.rb", paths.code()),
                         "A symlink escaped the portable file boundary");
        const auto moved = scratch / fs::path(u8"moved 音.vst3");
        fs::copy(root, moved, fs::copy_options::recursive |
                              fs::copy_options::skip_symlinks);
        const auto movedPaths = portablePathsFromModule(
            moved / "Contents" / "x86_64-linux" / "sonic_pi_vst3.so");
        require(read(movedPaths.code() / file.filename()) == "# café\nplay 72\n" &&
                listSnips(movedPaths).size() == 3, "Moving the bundle lost its source/snips");
        fs::remove_all(scratch);
        std::cout << "PASS: portable layout, Unicode files, atomic save, boundaries, "
                     "failed-save preservation, refreshed Snips and relocated bundle\n";
        return 0;
    } catch (const std::exception& e) {
        std::error_code ignored;
        fs::remove_all(scratch, ignored);
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
