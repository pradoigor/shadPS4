// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// UWP adaptation of the PKG/PFSC format flow in AzaharPlus/shadPS4Plus
// 9d2e761. Bounds, directory traversal, streaming and commit handling are new.
#include "PkgExtractor.h"
#include "PkgCrypto.h"
#include <miniz_tinfl.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <functional>

namespace Lab {
namespace {
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
uint64_t Number(std::span<const unsigned char> data, size_t offset, size_t size, bool big = false) {
    Require(offset <= data.size() && size <= data.size() - offset, "Estrutura truncada.");
    uint64_t value = 0;
    for (size_t i = 0; i < size; ++i) value |= uint64_t(data[offset + i]) << (8 * (big ? size - 1 - i : i));
    return value;
}
void Range(uint64_t offset, uint64_t size, uint64_t limit) {
    Require(offset <= limit && size <= limit - offset, "Regiao declarada fora dos limites.");
}
class Reader {
    std::ifstream file;
public:
    uint64_t size;
    explicit Reader(const std::filesystem::path& path) : file(path, std::ios::binary), size(std::filesystem::file_size(path)) {
        Require(bool(file), "Nao foi possivel abrir o PKG.");
    }
    Bytes Read(uint64_t offset, uint64_t count) {
        Range(offset, count, size);
        Require(count <= 16 * 1024 * 1024 && offset <= INT64_MAX, "Leitura excede limite de memoria.");
        Bytes result(static_cast<size_t>(count));
        file.clear(); file.seekg(static_cast<std::streamoff>(offset));
        file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(count));
        Require(file && uint64_t(file.gcount()) == count, "Leitura incompleta do PKG.");
        return result;
    }
};
std::string SafeName(std::span<const unsigned char> value) {
    Require(!value.empty() && value.size() <= 240, "Nome de arquivo invalido.");
    std::string name(value.begin(), value.end());
    Require(name != "." && name != ".." && name.back() != '.' && name.back() != ' ', "Nome de arquivo inseguro.");
    for (auto byte : value) Require(byte >= 32 && byte != 127 && std::string("/\\:*?\"<>|").find(char(byte)) == std::string::npos,
        "Caminho inseguro dentro do PKG.");
    std::string stem = name.substr(0, name.find('.'));
    for (auto& c : stem) if (c >= 'a' && c <= 'z') c -= 32;
    Require(stem != "CON" && stem != "PRN" && stem != "AUX" && stem != "NUL" &&
        !(stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) && stem[3] >= '1' && stem[3] <= '9'),
        "Nome reservado pelo Windows.");
    return name;
}
struct Entry { Bytes raw; uint64_t id, offset, size; };
struct Node { uint64_t mode, size, blocks, location; };
class Extractor {
    Reader file;
    std::filesystem::path stage;
    const PackageKeys& keys;
    InstallProgress& progress;
    uint64_t pfsOffset{}, pfsSize{}, pfscOffset{}, plainSize{};
    bool pfsEncrypted{};
    Bytes xtsKey;
    std::vector<uint64_t> sectors;
    std::vector<Node> nodes;
    std::set<uint64_t> visited;
    uint64_t totalBytes{};
    void Cancel() { Require(!progress.cancel.load(), "Extracao cancelada."); }
    Bytes Pfs(uint64_t offset, uint64_t count) {
        Range(offset, count, pfsSize);
        if (!pfsEncrypted) return file.Read(pfsOffset + offset, count);
        const auto aligned = offset & ~uint64_t(4095);
        const auto length = ((offset - aligned + count + 4095) / 4096) * 4096;
        Range(aligned, length, pfsSize);
        auto encrypted = file.Read(pfsOffset + aligned, length);
        auto plain = PkgCrypto::Xts(encrypted, xtsKey, aligned / 4096);
        return Bytes(plain.begin() + (offset - aligned), plain.begin() + (offset - aligned + count));
    }
    Bytes Block(uint64_t index) {
        Cancel();
        Require(index + 1 < sectors.size(), "Indice de bloco PFSC invalido.");
        auto length = sectors[index + 1] - sectors[index];
        Require(length > 0 && length <= 65536, "Bloco PFSC nao suportado.");
        auto data = Pfs(pfscOffset + sectors[index], length);
        if (length == 65536) return data;
        Bytes output(65536);
        auto count = tinfl_decompress_mem_to_mem(output.data(), output.size(), data.data(), data.size(), TINFL_FLAG_PARSE_ZLIB_HEADER);
        Require(count == 65536, "Bloco zlib invalido ou incompleto.");
        return output;
    }
    void Write(const std::filesystem::path& path, uint64_t length,
               const std::function<Bytes(uint64_t, uint64_t)>& read) {
        Require(!std::filesystem::exists(path), "Caminho duplicado no pacote; instalacao interrompida.");
        Require(length <= (uint64_t(1) << 40) && totalBytes <= (uint64_t(1) << 40) - length, "Tamanho de extracao excedido.");
        totalBytes += length;
        Require(std::filesystem::space(stage).available >= length + 16 * 1024 * 1024, "Espaco insuficiente para extrair.");
        std::ofstream output(path, std::ios::binary);
        Require(bool(output), "Falha ao criar arquivo de destino.");
        for (uint64_t offset = 0; offset < length;) {
            Cancel();
            auto count = std::min<uint64_t>(65536, length - offset);
            auto data = read(offset, count);
            Require(data.size() == count, "Extracao produziu tamanho incorreto.");
            output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(count));
            Require(bool(output), "Falha ao gravar arquivo; verifique armazenamento.");
            offset += count; progress.bytes.fetch_add(count);
        }
        output.flush(); Require(bool(output), "Falha ao finalizar arquivo.");
        output.close(); Require(!output.fail(), "Falha ao fechar arquivo.");
        progress.files.fetch_add(1);
    }
    Bytes NodeBlock(uint64_t inode, uint64_t block) {
        Require(inode < nodes.size() && block < nodes[inode].blocks, "Referencia a inode invalida.");
        return Block(nodes[inode].location + block);
    }
    struct Dir { uint64_t inode, type; std::string name; };
    std::vector<Dir> Directory(uint64_t inode) {
        Require(inode < nodes.size() && (nodes[inode].mode & 0xF000) == 0x4000, "Diretorio PFS invalido.");
        const auto& node = nodes[inode];
        Require(node.size <= 16 * 1024 * 1024, "Diretorio grande demais.");
        std::vector<Dir> entries;
        for (uint64_t pos = 0; pos < node.size; pos += 65536) {
            auto block = NodeBlock(inode, pos / 65536);
            auto limit = std::min<uint64_t>(65536, node.size - pos);
            for (uint64_t at = 0; at + 16 <= limit;) {
                auto child = Number(block, at, 4), type = Number(block, at + 4, 4);
                auto nameSize = Number(block, at + 8, 4), recordSize = Number(block, at + 12, 4);
                if (!child && !recordSize) break;
                Require(recordSize >= 16 && recordSize % 4 == 0 && recordSize <= limit - at && nameSize <= recordSize - 16,
                    "Registro de diretorio PFS corrompido.");
                if (type != 4 && type != 5 && child) {
                    Require(type == 2 || type == 3, "Tipo de arquivo PFS nao suportado.");
                    entries.push_back({child, type, SafeName(std::span(block).subspan(at + 16, nameSize))});
                }
                at += recordSize;
            }
        }
        return entries;
    }
    void Walk(uint64_t inode, const std::filesystem::path& directory, unsigned depth) {
        Require(depth <= 64 && visited.insert(inode).second, "Ciclo ou profundidade excessiva no PFS.");
        for (const auto& entry : Directory(inode)) {
            Cancel();
            Require(entry.inode < nodes.size(), "Inode de arquivo fora dos limites.");
            auto path = directory / std::filesystem::path(std::u8string(entry.name.begin(), entry.name.end()));
            const auto& node = nodes[entry.inode];
            if (entry.type == 3) {
                Require(!std::filesystem::exists(path), "Diretorio duplicado no pacote.");
                std::filesystem::create_directory(path); Walk(entry.inode, path, depth + 1);
            } else {
                Require((node.mode & 0xF000) == 0x8000, "Tipo do inode nao corresponde ao arquivo.");
                Write(path, node.size, [&](uint64_t offset, uint64_t count) {
                    auto data = NodeBlock(entry.inode, offset / 65536); data.resize(static_cast<size_t>(count)); return data;
                });
            }
            progress.percent.store(static_cast<unsigned>(std::min<uint64_t>(95, 20 + progress.files.load() * 75 / std::max<size_t>(1, nodes.size()))));
        }
    }
public:
    Extractor(const std::filesystem::path& source, const std::filesystem::path& target,
              const PackageKeys& supplied, InstallProgress& state) : file(source), stage(target), keys(supplied), progress(state) {}
    void Run() {
        Require(std::filesystem::is_directory(stage) && std::filesystem::is_empty(stage), "Destino temporario deve estar vazio.");
        Cancel();
        auto header = file.Read(0, 4096);
        Require(Number(header, 0, 4, true) == 0x7F434E54, "Arquivo nao e PKG PS4.");
        auto count = Number(header, 0x10, 4, true), table = Number(header, 0x18, 4, true);
        pfsOffset = Number(header, 0x410, 8, true);
        pfsSize = Number(header, 0x418, 8, true);
        Range(pfsOffset, pfsSize, file.size);
        Require(pfsSize >= 0x20, "Imagem PFS invalida.");
        auto pfsHeader = file.Read(pfsOffset, 0x20);
        pfsEncrypted = (Number(pfsHeader, 0x1C, 2) & 0x0004) != 0;
        Require(count > 0 && count <= 100000, "Tabela PKG invalida.");
        Range(table, count * 32, file.size);
        std::vector<Entry> entries;
        std::set<uint64_t> ids;
        for (uint64_t i = 0; i < count; ++i) {
            auto raw = file.Read(table + i * 32, 32);
            Entry entry{raw, Number(raw, 0, 4, true), Number(raw, 16, 4, true), Number(raw, 20, 4, true)};
            Range(entry.offset, entry.size, file.size);
            Require(ids.insert(entry.id).second, "ID duplicado na tabela PKG."); entries.push_back(std::move(entry));
        }
        const bool hasKeyEntries = std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
            return entry.id == 0x10 || entry.id == 0x20;
        });
        auto get = [&](uint64_t id) -> const Entry& {
            auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.id == id; });
            Require(it != entries.end(), "Estrutura criptografica PKG ausente ou nao suportada."); return *it;
        };
        if (pfsEncrypted) {
            Require(hasKeyEntries, "Imagem PFS criptografada sem entry_keys/image_key.");
            ValidatePackageKeys(keys);
            const auto& entryKeys = get(0x10);
            Require(entryKeys.size >= 32 + 7 * 32 + 4 * 256, "entry_keys truncado.");
            auto derived = PkgCrypto::Rsa(file.Read(entryKeys.offset + 32 + 7 * 32 + 3 * 256, 256), keys.derived);
            const auto& image = get(0x20);
            Require(image.size == 256, "image_key invalido.");
            auto hashInput = image.raw; hashInput.insert(hashInput.end(), derived.begin(), derived.end());
            auto ivKey = PkgCrypto::Hash(hashInput);
            auto imageKey = PkgCrypto::Aes(file.Read(image.offset, 256), std::span(ivKey).subspan(16), std::span(ivKey).first(16));
            auto ekpfs = PkgCrypto::Rsa(imageKey, keys.fake);
            auto seed = file.Read(pfsOffset + 0x370, 16);
            Bytes hmacInput{1, 0, 0, 0}; hmacInput.insert(hmacInput.end(), seed.begin(), seed.end());
            xtsKey = PkgCrypto::Hash(hmacInput, ekpfs);
        }
        Require(pfsSize >= 65536 && pfsSize % 4096 == 0, "Imagem PFS invalida.");
        // Reference layout has PFSC aligned to 64 KiB. Search a bounded prefix.
        bool found = false;
        for (uint64_t pos = 0x20000; pos + 4096 <= std::min<uint64_t>(pfsSize, 16 * 1024 * 1024); pos += 65536) {
            Cancel(); auto candidate = Pfs(pos, 48);
            if (Number(candidate, 0, 4) != 0x43534650) continue;
            Require(Number(candidate, 12, 4) == 65536 && Number(candidate, 16, 8) == 65536, "Variante PFSC nao suportada.");
            pfscOffset = pos; plainSize = Number(candidate, 40, 8);
            auto mapOffset = Number(candidate, 24, 8), dataStart = Number(candidate, 32, 8);
            Require(plainSize > 0 && plainSize % 65536 == 0 && plainSize / 65536 <= 1000000, "Tamanho PFSC nao suportado.");
            auto mapBytes = (plainSize / 65536 + 1) * 8;
            Range(mapOffset, mapBytes, pfsSize - pos);
            auto map = Pfs(pos + mapOffset, mapBytes);
            for (uint64_t i = 0; i < mapBytes; i += 8) sectors.push_back(Number(map, i, 8));
            Require(sectors.front() >= dataStart && sectors.front() >= mapOffset + mapBytes, "Mapa PFSC sobrepoe metadados.");
            for (size_t i = 0; i + 1 < sectors.size(); ++i) {
                Require(sectors[i + 1] > sectors[i] && sectors[i + 1] - sectors[i] <= 65536, "Mapa PFSC invalido.");
                Range(sectors[i], sectors[i + 1] - sectors[i], pfsSize - pos);
            }
            found = true; break;
        }
        Require(found, "PFSC nao encontrado: chaves incompativeis ou variante PKG nao suportada.");
        auto super = Block(0);
        Require(Number(super, 0x20, 4) == 65536, "Tamanho de bloco PFS nao suportado.");
        auto inodeCount = Number(super, 0x30, 8), root = Number(super, 0x48, 8);
        Require(inodeCount > 0 && inodeCount <= 100000 && root < inodeCount, "Cabecalho de inodes invalido.");
        constexpr uint64_t stride = 0xA8, perBlock = 65536 / stride;
        Bytes inodeBlock;
        for (uint64_t i = 0; i < inodeCount; ++i) {
            if (i % perBlock == 0) inodeBlock = Block(1 + i / perBlock);
            const auto at = (i % perBlock) * stride;
            Node node{Number(inodeBlock, at, 2), Number(inodeBlock, at + 8, 8), Number(inodeBlock, at + 96, 4), Number(inodeBlock, at + 100, 4)};
            Require(node.size <= (uint64_t(1) << 40) && node.blocks <= plainSize / 65536, "Tamanho de inode invalido.");
            Require(node.size <= node.blocks * 65536, "Inode incompleto.");
            if (node.blocks) Range(node.location, node.blocks, plainSize / 65536);
            // The PFSC sector map resolves the logical block sequence. Bytes after
            // loc are inode metadata/reserved space, not direct block pointers.
            nodes.push_back(node);
        }
        // The super-root stores internal metadata and the user root (uroot).
        auto roots = Directory(root);
        auto userRoot = std::find_if(roots.begin(), roots.end(), [](const auto& e) { return e.name == "uroot" && e.type == 3; });
        Require(userRoot != roots.end(), "Diretorio uroot ausente; variante PFS nao suportada.");
        progress.percent.store(20);
        Walk(userRoot->inode, stage, 0);
        // Public system entries: fixed names, never paths from package metadata.
        auto system = stage / "sce_sys";
        std::filesystem::create_directories(system);
        for (auto [id, name] : {std::pair{0x1000u, "param.sfo"}, {0x1200u, "icon0.png"}, {0x1220u, "pic0.png"}, {0x1006u, "pic1.png"}}) {
            auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.id == id; });
            if (it == entries.end() || std::filesystem::exists(system / name)) continue;
            Require((Number(it->raw, 8, 4, true) & 0x80000000u) == 0, "Entrada de sistema criptografada nao suportada.");
            Write(system / name, it->size, [&](uint64_t offset, uint64_t count) { return file.Read(it->offset + offset, count); });
        }
        Require(std::filesystem::is_regular_file(stage / "eboot.bin") && std::filesystem::is_regular_file(system / "param.sfo"),
            "Extracao sem eboot.bin/param.sfo: atualizacao, DLC ou variante ainda nao suportada.");
        std::ifstream sfo(system / "param.sfo", std::ios::binary);
        std::array<unsigned char, 4> signature{}; sfo.read(reinterpret_cast<char*>(signature.data()), 4);
        Require(sfo && Number(signature, 0, 4) == 0x46535000, "param.sfo extraido invalido.");
        Cancel(); progress.percent.store(99);
    }
};
}
void ExtractPackage(const std::filesystem::path& package, const std::filesystem::path& staging,
                    const PackageKeys& keys, InstallProgress& progress) {
    Extractor(package, staging, keys, progress).Run();
}
bool PackageNeedsKeys(const std::filesystem::path& path) {
    Reader file(path);
    auto header = file.Read(0, 4096);
    Require(Number(header, 0, 4, true) == 0x7F434E54, "Arquivo nao e PKG PS4.");
    const auto count = Number(header, 0x10, 4, true);
    const auto table = Number(header, 0x18, 4, true);
    Require(count > 0 && count <= 100000, "Tabela PKG invalida.");
    Range(table, count * 32, file.size);
    bool encryptedPfs = false;
    for (uint64_t i = 0; i < count; ++i) {
        auto entry = file.Read(table + i * 32, 32);
        const auto id = Number(entry, 0, 4, true);
        const auto offset = Number(entry, 16, 4, true);
        const auto size = Number(entry, 20, 4, true);
        Range(offset, size, file.size);
        (void)id;
    }
    const auto pfsOffset = Number(header, 0x410, 8, true);
    const auto pfsSize = Number(header, 0x418, 8, true);
    Range(pfsOffset, pfsSize, file.size);
    if (pfsSize >= 0x20) {
        const auto pfsHeader = file.Read(pfsOffset, 0x20);
        encryptedPfs = (Number(pfsHeader, 0x1C, 2) & 0x0004) != 0;
    }
    return encryptedPfs;
}
}
