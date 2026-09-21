// SPDX-License-Identifier: GPL-2.0-or-later
#include "IFileProbe.h"

#include "core/file_sys/ifile.h"
#include "core/loader/elf.h"

#include <cstring>
#include <windows.h>
#include <fileapifromapp.h>
#include <memory>
#include <winrt/base.h>
#include <vector>

namespace Lab {
namespace {

class UwpFile final : public Core::FileSys::IFile {
public:
    explicit UwpFile(const std::wstring& path) {
        CREATEFILE2_EXTENDED_PARAMETERS params{sizeof(params)};
        params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        m_file = winrt::handle{CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                            OPEN_EXISTING, &params)};
    }

    s64 Read(void* dst, u64 size) override {
        if (!m_file || size > UINT32_MAX) return -1;
        DWORD count{};
        if (!ReadFile(m_file.get(), dst, static_cast<DWORD>(size), &count, nullptr)) return -1;
        return static_cast<s64>(count);
    }
    s64 Write(const void*, u64) override { return -1; }
    bool Seek(s64 offset, Common::FS::SeekOrigin origin) override {
        LARGE_INTEGER distance{};
        distance.QuadPart = offset;
        DWORD method = FILE_BEGIN;
        if (origin == Common::FS::SeekOrigin::CurrentPosition) method = FILE_CURRENT;
        if (origin == Common::FS::SeekOrigin::End) method = FILE_END;
        return SetFilePointerEx(m_file.get(), distance, nullptr, method) != FALSE;
    }
    u64 Tell() const override {
        LARGE_INTEGER distance{};
        LARGE_INTEGER current{};
        if (!SetFilePointerEx(m_file.get(), distance, &current, FILE_CURRENT)) return 0;
        return static_cast<u64>(current.QuadPart);
    }
    u64 Size() const override {
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(m_file.get(), &size) || size.QuadPart < 0) return 0;
        return static_cast<u64>(size.QuadPart);
    }
    bool Flush() override { return m_file && FlushFileBuffers(m_file.get()) != FALSE; }
    bool IsOpen() const override { return static_cast<bool>(m_file); }

private:
    winrt::handle m_file;
};

std::vector<std::uint8_t> MakeSyntheticSelfElf() {
    self_header self{};
    self.magic = self_header::signature;
    self.version = 0;
    self.mode = 1;
    self.endian = 1;
    self.attributes = 0x12;
    self.category = 1;
    self.program_type = 1;
    self.segment_count = 1;
    self.unknown1A = 0x22;

    self_segment_header segment{};
    segment.flags = (0xABCuLL << 20u) | 0x800u | 0xFu;
    segment.file_offset = sizeof(self_header) + sizeof(self_segment_header) + sizeof(elf_header);
    segment.file_size = 0x1000;
    segment.memory_size = 0x2000;

    elf_header elf{};
    elf.e_ident.magic[EI_MAG0] = ELFMAG0;
    elf.e_ident.magic[EI_MAG1] = ELFMAG1;
    elf.e_ident.magic[EI_MAG2] = ELFMAG2;
    elf.e_ident.magic[EI_MAG3] = ELFMAG3;
    elf.e_ident.ei_class = ELF_CLASS_64;
    elf.e_ident.ei_data = ELF_DATA_2LSB;
    elf.e_ident.ei_version = ELF_VERSION_CURRENT;
    elf.e_type = ET_SCE_EXEC;
    elf.e_machine = EM_X86_64;
    elf.e_version = EV_CURRENT;
    elf.e_entry = 0x400000;
    elf.e_ehsize = sizeof(elf_header);
    elf.e_phentsize = sizeof(elf_program_header);
    elf.e_phnum = 1;

    std::vector<std::uint8_t> bytes(sizeof(self) + sizeof(segment) + sizeof(elf));
    std::memcpy(bytes.data(), &self, sizeof(self));
    std::memcpy(bytes.data() + sizeof(self), &segment, sizeof(segment));
    std::memcpy(bytes.data() + sizeof(self) + sizeof(segment), &elf, sizeof(elf));
    return bytes;
}

} // namespace

std::unique_ptr<Core::FileSys::IFile> MakeUwpFile(const std::wstring& path) {
    return std::make_unique<UwpFile>(path);
}

IFileProbeResult ProbeUwpIFileAdapter(const std::wstring& directory) {
    const auto bytes = MakeSyntheticSelfElf();
    const auto path = directory + L"\\ifile-core-probe.bin";
    CREATEFILE2_EXTENDED_PARAMETERS params{sizeof(params)};
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    winrt::handle writer{CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                     CREATE_ALWAYS, &params)};
    if (!writer) winrt::throw_last_error();
    DWORD written{};
    winrt::check_bool(WriteFile(writer.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                                &written, nullptr));
    winrt::check_bool(FlushFileBuffers(writer.get()));
    writer.close();

    auto backend = std::make_unique<UwpFile>(path);
    if (!backend->IsOpen()) throw winrt::hresult_error(E_FAIL, L"Backend IFile UWP não abriu o arquivo.");
    Core::FileSys::FileReader reader(std::move(backend));
    self_header self{};
    self_segment_header segment{};
    elf_header elf{};
    const bool read_self = reader.ReadObject(self);
    const bool read_segment = reader.ReadObject(segment);
    const bool read_elf = reader.ReadObject(elf);

    IFileProbeResult result;
    result.file_size = static_cast<std::uint32_t>(bytes.size());
    result.bytes_read = static_cast<std::uint32_t>(reader.Tell());
    result.segment_id = segment.GetId();
    result.elf_entry = elf.e_entry;
    result.passed = written == bytes.size() && read_self && read_segment && read_elf &&
                    result.bytes_read == bytes.size() && self.magic == self_header::signature &&
                    segment.IsBlocked() && result.segment_id == 0xABC &&
                    elf.e_ident.magic[EI_MAG0] == ELFMAG0 && elf.e_ident.magic[EI_MAG3] == ELFMAG3 &&
                    elf.e_type == ET_SCE_EXEC && elf.e_machine == EM_X86_64 &&
                    elf.e_entry == 0x400000;
    return result;
}

} // namespace Lab
