// SPDX-License-Identifier: GPL-2.0-or-later
#include "PkgExtractor.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace Lab;
Bytes Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary); return Bytes(std::istreambuf_iterator<char>(in), {});
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("fixture directory required");
        std::filesystem::path root(argv[1]);
        PackageKeys keys;
        std::ifstream input(root / "keys.txt"); std::string set, field, hex;
        while (input >> set >> field >> hex) {
            Bytes bytes; for (size_t i = 0; i < hex.size(); i += 2) bytes.push_back(static_cast<unsigned char>(std::stoul(hex.substr(i,2), nullptr, 16)));
            (set == "FakeKeyset" ? keys.fake : keys.derived)[field] = bytes;
        }
        ValidatePackageKeys(keys);
        unsigned passed{};
        for (auto name : {"valid", "traversal", "cycle", "bad_dirent", "bad_inode", "bad_zlib", "bad_map", "bad_table", "bad_rsa", "truncated", "missing_keys", "cancel"}) {
            auto target = root / (std::string("output-") + name); std::filesystem::create_directory(target);
            InstallProgress progress; progress.cancel = std::string(name) == "cancel";
            bool success = false;
            try {
                auto source = root / (std::string((std::string(name) == "missing_keys" || std::string(name) == "cancel") ? "valid" : name) + ".pkg");
                ExtractPackage(source, target, std::string(name) == "missing_keys" ? PackageKeys{} : keys, progress);
                success = true;
            } catch (const std::exception& e) { std::cout << name << ": " << e.what() << '\n'; }
            if (success != (std::string(name) == "valid")) throw std::runtime_error(std::string("unexpected result: ") + name);
            if (success) {
                if (Read(target / "eboot.bin") != Read(root / "expected-eboot.bin") ||
                    Read(target / "assets/payload.dat") != Read(root / "expected-payload.dat")) throw std::runtime_error("extracted bytes differ");
                if (progress.files != 3) throw std::runtime_error("unexpected file count");
            }
            ++passed;
        }
        if (std::filesystem::exists(root / "outside")) throw std::runtime_error("path escaped staging");
        std::cout << passed << " extraction cases passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
