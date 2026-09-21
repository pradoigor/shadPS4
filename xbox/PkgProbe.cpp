// SPDX-License-Identifier: GPL-2.0-or-later
#include "PkgProbe.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>

namespace Lab {
namespace {

constexpr std::uint32_t PkgMagic = 0x7F434E54u;
constexpr std::uint32_t ParamSfoEntry = 0x1000u;
constexpr std::uint32_t EntrySize = 0x20u;
constexpr std::uint32_t HeaderSize = 0x1000u;
constexpr std::uint32_t MaxEntries = 100000u;

std::uint32_t ReadBE32(std::array<unsigned char, 8> const& bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24u) |
        (static_cast<std::uint32_t>(bytes[1]) << 16u) |
        (static_cast<std::uint32_t>(bytes[2]) << 8u) |
        static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t ReadBE64(std::array<unsigned char, 8> const& bytes) {
    std::uint64_t value{};
    for (auto byte : bytes)
        value = (value << 8u) | static_cast<std::uint64_t>(byte);
    return value;
}

bool ReadAt(std::ifstream& input, std::uint64_t offset, void* destination, std::size_t size) {
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max()))
        return false;
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input)
        return false;
    input.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
}

std::wstring Hex(std::uint64_t value) {
    std::wstringstream stream;
    stream << std::hex << std::uppercase << value;
    return stream.str();
}

bool RangeFits(std::uint64_t offset, std::uint64_t size, std::uint64_t fileSize) {
    return offset <= fileSize && size <= fileSize - offset;
}

} // namespace

PkgProbeResult ProbePkg(std::filesystem::path const& path) {
    PkgProbeResult result;
    std::error_code error;
    const auto fileSize = std::filesystem::file_size(path, error);
    if (error || fileSize < 4)
        return result;

    std::ifstream input(path, std::ios::binary);
    if (!input)
        return result;

    std::array<unsigned char, 8> bytes{};
    if (!ReadAt(input, 0, bytes.data(), 4) || ReadBE32(bytes) != PkgMagic)
        return result;
    result.recognized = true;

    if (fileSize < HeaderSize) {
        result.detail = L"PKG PS4 reconhecido, mas o cabeçalho está incompleto (" +
            std::to_wstring(fileSize) + L" bytes).";
        return result;
    }

    std::array<unsigned char, 0x100> header{};
    if (!ReadAt(input, 0, header.data(), header.size())) {
        result.detail = L"PKG PS4 reconhecido, mas não foi possível ler o cabeçalho.";
        return result;
    }
    auto U32 = [&header](std::size_t offset) {
        std::array<unsigned char, 8> value{};
        std::copy_n(header.data() + offset, 4, value.data());
        return ReadBE32(value);
    };
    auto U64 = [&header](std::size_t offset) {
        std::array<unsigned char, 8> value{};
        std::copy_n(header.data() + offset, 8, value.data());
        return ReadBE64(value);
    };

    const auto packageType = U32(0x04);
    const auto fileCount = U32(0x0C);
    const auto tableCount = U32(0x10);
    const auto tableOffset = U32(0x18);
    const auto bodyOffset = U64(0x20);
    const auto bodySize = U64(0x28);
    const auto contentOffset = U64(0x30);
    const auto contentSize = U64(0x38);
    const auto contentFlags = U32(0x78);

    std::string contentId;
    for (std::size_t i = 0; i < 0x24; ++i) {
        const auto character = static_cast<char>(header[0x40 + i]);
        if (character == '\0')
            break;
        contentId.push_back(character);
    }
    std::string titleId;
    if (contentId.size() >= 16)
        titleId = contentId.substr(7, 9);

    const auto tableBytes = static_cast<std::uint64_t>(tableCount) * EntrySize;
    const bool tableFits = tableCount <= MaxEntries && RangeFits(tableOffset, tableBytes, fileSize);
    const bool bodyFits = RangeFits(bodyOffset, bodySize, fileSize);
    const bool contentFits = RangeFits(contentOffset, contentSize, fileSize);

    std::uint32_t validEntries{};
    std::uint32_t invalidEntries{};
    std::uint32_t paramSfoSize{};
    if (tableFits) {
        for (std::uint32_t index = 0; index < tableCount; ++index) {
            std::array<unsigned char, EntrySize> entry{};
            const auto entryOffset = static_cast<std::uint64_t>(tableOffset) +
                static_cast<std::uint64_t>(index) * EntrySize;
            if (!ReadAt(input, entryOffset, entry.data(), entry.size())) {
                ++invalidEntries;
                continue;
            }
            std::array<unsigned char, 8> value{};
            std::copy_n(entry.data(), 4, value.data());
            const auto id = ReadBE32(value);
            std::copy_n(entry.data() + 0x10, 4, value.data());
            const auto payloadOffset = ReadBE32(value);
            std::copy_n(entry.data() + 0x14, 4, value.data());
            const auto payloadSize = ReadBE32(value);
            if (!RangeFits(payloadOffset, payloadSize, fileSize)) {
                ++invalidEntries;
                continue;
            }
            ++validEntries;
            if (id == ParamSfoEntry)
                paramSfoSize = payloadSize;
        }
    }

    std::wstringstream detail;
    detail << L"PKG PS4 reconhecido · " << fileSize << L" bytes\n"
           << L"Content ID: " << std::wstring(contentId.begin(), contentId.end()) << L"\n";
    if (!titleId.empty())
        detail << L"Title ID: " << std::wstring(titleId.begin(), titleId.end()) << L"\n";
    detail << L"Tipo: 0x" << Hex(packageType) << L" · Entradas declaradas: " << fileCount
           << L" · Tabela: " << tableCount << L"\n"
           << L"Body: " << bodySize << L" bytes em 0x" << Hex(bodyOffset)
           << L" · Conteúdo: " << contentSize << L" bytes em 0x"
           << Hex(contentOffset) << L"\n"
           << L"Flags de conteúdo: 0x" << Hex(contentFlags) << L"\n";
    if (!tableFits)
        detail << L"Tabela de entradas fora dos limites do arquivo ou grande demais.\n";
    else
        detail << L"Entradas dentro dos limites: " << validEntries << L"; inválidas: " << invalidEntries << L".\n";
    if (!bodyFits || !contentFits)
        detail << L"Aviso: uma região declarada ultrapassa o tamanho do arquivo.\n";
    if (paramSfoSize != 0)
        detail << L"param.sfo presente: " << paramSfoSize << L" bytes.\n";
    else
        detail << L"param.sfo não localizado na tabela.\n";
    detail << L"Metadados lidos sem descriptografar ou extrair o conteúdo.";
    result.detail = detail.str();
    return result;
}

} // namespace Lab
