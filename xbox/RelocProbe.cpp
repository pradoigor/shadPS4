// SPDX-License-Identifier: GPL-2.0-or-later
#include "RelocProbe.h"

#include "core/loader/elf.h"

#include <array>
#include <cstring>
#include <windows.h>
#include <fileapifromapp.h>
#include <winrt/base.h>

namespace Lab {
namespace {

constexpr std::uint64_t RelocationBase = 0x10000000ull;
constexpr std::uint64_t RelativeAddend = 0x1234ull;
constexpr std::uint64_t LocalSymbolOffset = 0x5678ull;
constexpr std::uint64_t LocalSymbolAddend = 0x20ull;
constexpr std::uint64_t TlsModuleId = 7ull;

template <typename T>
void Store(std::array<std::uint8_t, 32>& image, std::size_t offset, const T& value) {
    std::memcpy(image.data() + offset, &value, sizeof(value));
}

template <typename T>
T Load(const std::array<std::uint8_t, 32>& image, std::size_t offset) {
    T value{};
    std::memcpy(&value, image.data() + offset, sizeof(value));
    return value;
}

} // namespace

RelocProbeResult ProbeOriginalRelocationModel(const std::wstring& directory) {
    elf_symbol local_symbol{};
    local_symbol.st_info = static_cast<std::uint8_t>((STB_LOCAL << 4u) | STT_OBJECT);
    local_symbol.st_value = LocalSymbolOffset;

    std::array<elf_relocation, 3> records{};
    records[0] = {.rel_offset = 0, .rel_info = R_X86_64_RELATIVE, .rel_addend = RelativeAddend};
    records[1] = {.rel_offset = 8, .rel_info = (1ull << 32u) | R_X86_64_64, .rel_addend = LocalSymbolAddend};
    records[2] = {.rel_offset = 16, .rel_info = R_X86_64_DTPMOD64, .rel_addend = 0};

    std::array<std::uint8_t, 32> image{};
    for (const auto& relocation : records) {
        const auto type = relocation.GetType();
        std::uint64_t value{};
        switch (type) {
        case R_X86_64_RELATIVE:
            value = RelocationBase + static_cast<std::uint64_t>(relocation.rel_addend);
            break;
        case R_X86_64_64:
            if (relocation.GetSymbol() != 1 || local_symbol.GetBind() != STB_LOCAL ||
                local_symbol.GetType() != STT_OBJECT) {
                throw winrt::hresult_error(E_FAIL, L"Símbolo local de relocação não foi interpretado.");
            }
            value = RelocationBase + local_symbol.st_value +
                    static_cast<std::uint64_t>(relocation.rel_addend);
            break;
        case R_X86_64_DTPMOD64:
            value = TlsModuleId;
            break;
        default:
            throw winrt::hresult_error(E_FAIL, L"Tipo de relocação original não reconhecido.");
        }
        Store(image, static_cast<std::size_t>(relocation.rel_offset), value);
    }

    const auto relative = Load<std::uint64_t>(image, 0);
    const auto local = Load<std::uint64_t>(image, 8);
    const auto tls = Load<std::uint64_t>(image, 16);
    const auto expected_relative = RelocationBase + RelativeAddend;
    const auto expected_local = RelocationBase + LocalSymbolOffset + LocalSymbolAddend;

    const auto path = directory + L"\\relocation-core-probe.bin";
    CREATEFILE2_EXTENDED_PARAMETERS params{sizeof(params)};
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    winrt::handle writer{CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, &params)};
    if (!writer) winrt::throw_last_error();
    DWORD written{};
    winrt::check_bool(WriteFile(writer.get(), records.data(), static_cast<DWORD>(sizeof(records)), &written, nullptr));
    winrt::check_bool(FlushFileBuffers(writer.get()));

    RelocProbeResult result;
    result.record_count = static_cast<std::uint32_t>(records.size());
    result.relative_value = static_cast<std::uint32_t>(relative);
    result.local_symbol_value = static_cast<std::uint32_t>(local);
    result.tls_module_value = static_cast<std::uint32_t>(tls);
    result.encoded_size = written;
    result.passed = written == sizeof(records) && relative == expected_relative &&
                    local == expected_local && tls == TlsModuleId;
    return result;
}

} // namespace Lab
