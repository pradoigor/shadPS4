// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Lab {

struct PendingSymbolRelocation {
    std::uint64_t target{};
    std::int64_t addend{};
    std::string symbol;
};

struct GuestSegmentInfo {
    std::uint64_t address{};
    std::uint64_t size{};
    std::uint32_t flags{};
};

struct ControlledLoadResult {
    bool recognized{};
    bool self{};
    bool validated{};
    bool mapped{};
    bool inner_elf{};
    bool inner_mapped{};
    bool protected_segments{};
    std::uint64_t file_size{};
    std::uint64_t segment_count{};
    std::uint64_t load_segments{};
    std::uint64_t mapped_bytes{};
    std::uint64_t entry{};
    std::uint64_t min_virtual_address{};
    std::uint64_t max_virtual_address{};
    std::uint64_t checksum{};
    std::uint64_t inner_segment_count{};
    std::uint64_t inner_load_segments{};
    std::uint64_t inner_entry{};
    std::uint64_t inner_mapped_bytes{};
    std::uint64_t inner_checksum{};
    std::uint64_t dynamic_segments{};
    std::uint64_t tls_segments{};
    std::uint64_t dynamic_entries{};
    std::uint64_t rela_entries{};
    std::uint64_t jmp_rela_entries{};
    std::uint64_t import_libraries{};
    std::uint64_t needed_modules{};
    std::uint64_t supported_relocations{};
    std::uint64_t unsupported_relocations{};
    std::uint64_t relocation_targets_outside_segments{};
    std::uint64_t relative_relocations_applied{};
    std::uint64_t symbol_relocations_pending{};
    std::uint64_t tls_relocations_pending{};
    std::uint64_t symbol_relocations_valid{};
    std::uint64_t symbol_relocations_invalid{};
    // Matches against the upstream AeroLib NID registry are inventory only:
    // they do not provide an executable address or patch the guest image.
    std::uint64_t hle_symbols_known{};
    std::uint64_t hle_symbols_unknown{};
    std::uint64_t hle_addresses_created{};
    std::uint64_t hle_handlers_implemented{};
    std::uint64_t hle_handlers_unimplemented{};
    std::uint64_t hle_relocations_applied{};
    std::uint64_t hle_relocations_unresolved{};
    std::uint64_t guest_memory_bytes{};
    std::uint64_t guest_memory_writable_bytes{};
    std::uint64_t guest_memory_host_address{};
    bool guest_memory_mapped{};
    bool hle_pointer_probe_passed{};
    std::uint64_t hle_pointer_probe_return{};
    std::uint64_t hle_pointer_probe_guest_address{};
    std::uint64_t relocation_dry_run_checksum{};
    std::vector<std::string> pending_symbol_names;
    std::vector<std::string> hle_symbol_mappings;
    std::vector<std::string> hle_unmapped_symbols;
    std::vector<std::string> import_library_ids;
    std::vector<std::string> needed_module_ids;
    std::vector<std::string> import_library_names;
    std::vector<std::string> needed_module_names;
    std::vector<PendingSymbolRelocation> pending_symbol_relocations;
    std::vector<GuestSegmentInfo> guest_segments;
    // Private, non-executable image used only for relocation dry-runs.
    std::vector<std::uint8_t> private_image;
    bool runtime_preflight_ready{};
    std::vector<std::wstring> runtime_blockers;
    bool has_dynamic{};
    bool has_tls{};
    bool has_relocations{};
    bool has_imports{};
    bool relocation_data_valid{};
    std::wstring detail;
};

// Validates an ELF/SELF and maps only validated PT_LOAD bytes into a private,
// non-executable buffer. It never transfers control to the input file.
ControlledLoadResult LoadControlled(std::filesystem::path const& path);

struct GeneratedExecutionResult {
    bool passed{};
    int returned_value{};
    std::uint64_t hle_thunk_returned_value{};
    std::uint64_t executable_address{};
    std::uint64_t elf_file_size{};
    std::wstring detail;
};

// Builds an ELF containing a project-owned `return 42` stub, validates it with
// the same loader, then executes only that known six-byte sequence.
GeneratedExecutionResult ExecuteGeneratedProbe(std::filesystem::path const& directory);

} // namespace Lab
