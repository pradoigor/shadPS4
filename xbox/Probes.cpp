// SPDX-License-Identifier: GPL-2.0-or-later
#include "Probes.h"

#include "ControlledLoader.h"
#include "GuestMemory.h"
#include "HleDispatcher.h"
#include "RuntimeGate.h"

#include <windows.h>
#include <cstring>
#include <filesystem>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>

namespace Lab {

void RunProbe(Test& test, std::wstring const& executablePath, std::wstring const& directory) {
    if (test.id != L"controlled_execution")
        throw winrt::hresult_error(E_INVALIDARG, L"Teste automático desconhecido.");
    if (executablePath.empty())
        throw winrt::hresult_error(E_INVALIDARG, L"Selecione um ELF/SELF ou extraia um PKG antes de validar.");

    auto result = LoadControlled(executablePath);
    HleDispatcher hleDispatcher;
    const auto hleBindings = hleDispatcher.Bind(result.pending_symbol_names);
    result.hle_addresses_created = hleBindings.executable_addresses;
    result.hle_handlers_implemented = hleBindings.implemented_handlers;
    result.hle_handlers_unimplemented = hleBindings.unimplemented_handlers;
    for (auto const& relocation : result.pending_symbol_relocations) {
        auto* address = hleDispatcher.AddressFor(relocation.symbol);
        if (!address || relocation.target < result.min_virtual_address) {
            ++result.hle_relocations_unresolved;
            continue;
        }
        const auto target = relocation.target - result.min_virtual_address;
        if (target > result.private_image.size() ||
            sizeof(std::uint64_t) > result.private_image.size() - target) {
            ++result.hle_relocations_unresolved;
            continue;
        }
        const auto value = reinterpret_cast<std::uint64_t>(address) +
                           static_cast<std::uint64_t>(relocation.addend);
        std::memcpy(result.private_image.data() + target, &value, sizeof(value));
        ++result.hle_relocations_applied;
    }
    GuestMemory guestMemory;
    result.guest_memory_mapped = guestMemory.MapReadOnly(
        result.private_image, result.min_virtual_address);
    if (result.guest_memory_mapped) {
        result.guest_memory_bytes = guestMemory.size();
        result.guest_memory_host_address = guestMemory.hostAddress();
    }
    hleDispatcher.AttachGuestMemory(&guestMemory);
    auto gate = EvaluateRuntimeGate(result);
    result.runtime_preflight_ready = gate.ready;
    result.runtime_blockers = gate.blockers;
    using winrt::Windows::Data::Json::JsonValue;
    test.measurements.Insert(L"file_size", JsonValue::CreateNumberValue(static_cast<double>(result.file_size)));
    test.measurements.Insert(L"self", JsonValue::CreateBooleanValue(result.self));
    test.measurements.Insert(L"validated", JsonValue::CreateBooleanValue(result.validated));
    test.measurements.Insert(L"mapped", JsonValue::CreateBooleanValue(result.mapped));
    test.measurements.Insert(L"inner_elf", JsonValue::CreateBooleanValue(result.inner_elf));
    test.measurements.Insert(L"inner_mapped", JsonValue::CreateBooleanValue(result.inner_mapped));
    test.measurements.Insert(L"protected_segments", JsonValue::CreateBooleanValue(result.protected_segments));
    test.measurements.Insert(L"segment_count", JsonValue::CreateNumberValue(static_cast<double>(result.segment_count)));
    test.measurements.Insert(L"load_segments", JsonValue::CreateNumberValue(static_cast<double>(result.load_segments)));
    test.measurements.Insert(L"mapped_bytes", JsonValue::CreateNumberValue(static_cast<double>(result.mapped_bytes)));
    test.measurements.Insert(L"entry", JsonValue::CreateNumberValue(static_cast<double>(result.entry)));
    test.measurements.Insert(L"min_virtual_address", JsonValue::CreateNumberValue(static_cast<double>(result.min_virtual_address)));
    test.measurements.Insert(L"max_virtual_address", JsonValue::CreateNumberValue(static_cast<double>(result.max_virtual_address)));
    test.measurements.Insert(L"checksum_fnv1a", JsonValue::CreateNumberValue(static_cast<double>(result.checksum)));
    test.measurements.Insert(L"inner_segment_count", JsonValue::CreateNumberValue(static_cast<double>(result.inner_segment_count)));
    test.measurements.Insert(L"inner_load_segments", JsonValue::CreateNumberValue(static_cast<double>(result.inner_load_segments)));
    test.measurements.Insert(L"inner_entry", JsonValue::CreateNumberValue(static_cast<double>(result.inner_entry)));
    test.measurements.Insert(L"inner_mapped_bytes", JsonValue::CreateNumberValue(static_cast<double>(result.inner_mapped_bytes)));
    test.measurements.Insert(L"inner_checksum_fnv1a", JsonValue::CreateNumberValue(static_cast<double>(result.inner_checksum)));
    test.measurements.Insert(L"dynamic_segments", JsonValue::CreateNumberValue(static_cast<double>(result.dynamic_segments)));
    test.measurements.Insert(L"tls_segments", JsonValue::CreateNumberValue(static_cast<double>(result.tls_segments)));
    test.measurements.Insert(L"dynamic_entries", JsonValue::CreateNumberValue(static_cast<double>(result.dynamic_entries)));
    test.measurements.Insert(L"rela_entries", JsonValue::CreateNumberValue(static_cast<double>(result.rela_entries)));
    test.measurements.Insert(L"jmp_rela_entries", JsonValue::CreateNumberValue(static_cast<double>(result.jmp_rela_entries)));
    test.measurements.Insert(L"import_libraries", JsonValue::CreateNumberValue(static_cast<double>(result.import_libraries)));
    test.measurements.Insert(L"needed_modules", JsonValue::CreateNumberValue(static_cast<double>(result.needed_modules)));
    test.measurements.Insert(L"supported_relocations", JsonValue::CreateNumberValue(static_cast<double>(result.supported_relocations)));
    test.measurements.Insert(L"unsupported_relocations", JsonValue::CreateNumberValue(static_cast<double>(result.unsupported_relocations)));
    test.measurements.Insert(L"relocation_targets_outside_segments", JsonValue::CreateNumberValue(static_cast<double>(result.relocation_targets_outside_segments)));
    test.measurements.Insert(L"relative_relocations_applied", JsonValue::CreateNumberValue(static_cast<double>(result.relative_relocations_applied)));
    test.measurements.Insert(L"symbol_relocations_pending", JsonValue::CreateNumberValue(static_cast<double>(result.symbol_relocations_pending)));
    test.measurements.Insert(L"tls_relocations_pending", JsonValue::CreateNumberValue(static_cast<double>(result.tls_relocations_pending)));
    test.measurements.Insert(L"symbol_relocations_valid", JsonValue::CreateNumberValue(static_cast<double>(result.symbol_relocations_valid)));
    test.measurements.Insert(L"symbol_relocations_invalid", JsonValue::CreateNumberValue(static_cast<double>(result.symbol_relocations_invalid)));
    test.measurements.Insert(L"hle_symbols_known", JsonValue::CreateNumberValue(static_cast<double>(result.hle_symbols_known)));
    test.measurements.Insert(L"hle_symbols_unknown", JsonValue::CreateNumberValue(static_cast<double>(result.hle_symbols_unknown)));
    test.measurements.Insert(L"hle_addresses_created", JsonValue::CreateNumberValue(static_cast<double>(result.hle_addresses_created)));
    test.measurements.Insert(L"hle_handlers_implemented", JsonValue::CreateNumberValue(static_cast<double>(result.hle_handlers_implemented)));
    test.measurements.Insert(L"hle_handlers_unimplemented", JsonValue::CreateNumberValue(static_cast<double>(result.hle_handlers_unimplemented)));
    test.measurements.Insert(L"hle_relocations_applied", JsonValue::CreateNumberValue(static_cast<double>(result.hle_relocations_applied)));
    test.measurements.Insert(L"hle_relocations_unresolved", JsonValue::CreateNumberValue(static_cast<double>(result.hle_relocations_unresolved)));
    test.measurements.Insert(L"guest_memory_mapped", JsonValue::CreateBooleanValue(result.guest_memory_mapped));
    test.measurements.Insert(L"guest_memory_bytes", JsonValue::CreateNumberValue(static_cast<double>(result.guest_memory_bytes)));
    test.measurements.Insert(L"guest_memory_host_address", JsonValue::CreateNumberValue(static_cast<double>(result.guest_memory_host_address)));
    test.measurements.Insert(L"runtime_preflight_ready", JsonValue::CreateBooleanValue(result.runtime_preflight_ready));
    winrt::Windows::Data::Json::JsonArray symbolNames;
    for (auto const& name : result.pending_symbol_names)
        symbolNames.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
    test.measurements.Insert(L"pending_symbol_names", symbolNames);
    winrt::Windows::Data::Json::JsonArray hleMappings;
    for (auto const& mapping : result.hle_symbol_mappings)
        hleMappings.Append(JsonValue::CreateStringValue(winrt::to_hstring(mapping)));
    test.measurements.Insert(L"hle_symbol_mappings", hleMappings);
    winrt::Windows::Data::Json::JsonArray hleUnmapped;
    for (auto const& name : result.hle_unmapped_symbols)
        hleUnmapped.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
    test.measurements.Insert(L"hle_unmapped_symbols", hleUnmapped);
    winrt::Windows::Data::Json::JsonArray runtimeBlockers;
    for (auto const& blocker : result.runtime_blockers)
        runtimeBlockers.Append(JsonValue::CreateStringValue(blocker));
    test.measurements.Insert(L"runtime_blockers", runtimeBlockers);
    winrt::Windows::Data::Json::JsonArray libraryIds;
    for (auto const& id : result.import_library_ids)
        libraryIds.Append(JsonValue::CreateStringValue(winrt::to_hstring(id)));
    test.measurements.Insert(L"import_library_ids", libraryIds);
    winrt::Windows::Data::Json::JsonArray libraryNames;
    for (auto const& name : result.import_library_names)
        libraryNames.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
    test.measurements.Insert(L"import_library_names", libraryNames);
    winrt::Windows::Data::Json::JsonArray moduleIds;
    for (auto const& id : result.needed_module_ids)
        moduleIds.Append(JsonValue::CreateStringValue(winrt::to_hstring(id)));
    test.measurements.Insert(L"needed_module_ids", moduleIds);
    winrt::Windows::Data::Json::JsonArray moduleNames;
    for (auto const& name : result.needed_module_names)
        moduleNames.Append(JsonValue::CreateStringValue(winrt::to_hstring(name)));
    test.measurements.Insert(L"needed_module_names", moduleNames);
    test.measurements.Insert(L"relocation_dry_run_checksum", JsonValue::CreateNumberValue(static_cast<double>(result.relocation_dry_run_checksum)));
    test.measurements.Insert(L"has_dynamic", JsonValue::CreateBooleanValue(result.has_dynamic));
    test.measurements.Insert(L"has_tls", JsonValue::CreateBooleanValue(result.has_tls));
    test.measurements.Insert(L"has_relocations", JsonValue::CreateBooleanValue(result.has_relocations));
    test.measurements.Insert(L"has_imports", JsonValue::CreateBooleanValue(result.has_imports));
    test.measurements.Insert(L"relocation_data_valid", JsonValue::CreateBooleanValue(result.relocation_data_valid));
    auto execution = ExecuteGeneratedProbe(std::filesystem::path(directory));
    test.measurements.Insert(L"execution_returned_value", JsonValue::CreateNumberValue(execution.returned_value));
    test.measurements.Insert(L"hle_thunk_returned_value", JsonValue::CreateNumberValue(static_cast<double>(execution.hle_thunk_returned_value)));
    test.measurements.Insert(L"execution_address", JsonValue::CreateNumberValue(static_cast<double>(execution.executable_address)));
    test.measurements.Insert(L"execution_elf_file_size", JsonValue::CreateNumberValue(static_cast<double>(execution.elf_file_size)));
    test.status = L"passed";
    std::wstring gateDetail = L"\nGate de runtime: ";
    gateDetail += result.runtime_preflight_ready ? L"pronto." : L"bloqueado.";
    for (auto const& blocker : result.runtime_blockers)
        gateDetail += L"\n- " + blocker;
    test.detail = result.detail + L"\nMetadados runtime: dynamic=" + std::to_wstring(result.dynamic_entries) +
                  L", relocations=" + std::to_wstring(result.rela_entries + result.jmp_rela_entries) +
                  L", imports=" + std::to_wstring(result.import_libraries + result.needed_modules) +
                  L", TLS=" + std::to_wstring(result.tls_segments) +
                  L", relocation types supported=" + std::to_wstring(result.supported_relocations) +
                  L", unsupported=" + std::to_wstring(result.unsupported_relocations) +
                  L", targets outside mapped segments=" + std::to_wstring(result.relocation_targets_outside_segments) +
                  L", relative applied in private dry-run=" + std::to_wstring(result.relative_relocations_applied) +
                  L", symbol pending=" + std::to_wstring(result.symbol_relocations_pending) +
                  L", symbol names valid=" + std::to_wstring(result.symbol_relocations_valid) +
                  L", invalid=" + std::to_wstring(result.symbol_relocations_invalid) +
                  L", NIDs conhecidos no registro AeroLib=" + std::to_wstring(result.hle_symbols_known) +
                  L", NIDs sem correspondência=" + std::to_wstring(result.hle_symbols_unknown) +
                  L", endereços HLE criados=" + std::to_wstring(result.hle_addresses_created) +
                  L", handlers implementados=" + std::to_wstring(result.hle_handlers_implemented) +
                  L", handlers pendentes=" + std::to_wstring(result.hle_handlers_unimplemented) +
                  L", relocations HLE aplicadas em cópia privada=" + std::to_wstring(result.hle_relocations_applied) +
                  L", relocations HLE sem resolução=" + std::to_wstring(result.hle_relocations_unresolved) +
                  L", memória convidada somente leitura=" + (result.guest_memory_mapped ? L"sim" : L"não") +
                  L", TLS pending=" + std::to_wstring(result.tls_relocations_pending) +
                  std::wstring(L". Gate de runtime=") +
                  (result.runtime_preflight_ready ? L"pronto" : L"bloqueado") +
                  L". Os thunks temporários foram usados para aplicar relocations somente em uma cópia privada; o homebrew não foi executado.\n" +
                  gateDetail + L"\n" +
                  execution.detail + L"\nArquivo: " + executablePath;
}

} // namespace Lab
