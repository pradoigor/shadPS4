// SPDX-License-Identifier: GPL-2.0-or-later
#include "RuntimeGate.h"

#include <cstddef>
#include <set>
#include <utility>

namespace Lab {
namespace {

std::wstring LibraryName(std::string const& encoded) {
    const auto equals = encoded.find('=');
    const auto at = encoded.find('@', equals == std::string::npos ? 0 : equals + 1);
    const auto begin = equals == std::string::npos ? 0 : equals + 1;
    return std::wstring(encoded.begin() + static_cast<std::ptrdiff_t>(begin),
                        encoded.begin() + static_cast<std::ptrdiff_t>(
                                             at == std::string::npos ? encoded.size() : at));
}

void Add(std::set<std::wstring>& unique, std::vector<std::wstring>& blockers,
         std::wstring message) {
    if (unique.insert(message).second) blockers.push_back(std::move(message));
}

} // namespace

RuntimeGateResult EvaluateRuntimeGate(ControlledLoadResult const& load) {
    RuntimeGateResult result;
    std::set<std::wstring> unique;

    if (!load.validated || !load.inner_mapped)
        Add(unique, result.blockers, L"A imagem ELF/SELF ainda não foi validada e mapeada.");
    if (!load.relocation_data_valid || load.unsupported_relocations != 0 ||
        load.relocation_targets_outside_segments != 0)
        Add(unique, result.blockers, L"As tabelas de relocação não satisfazem o gate de segurança.");
    if (load.symbol_relocations_invalid != 0 || load.hle_symbols_unknown != 0)
        Add(unique, result.blockers, L"Há símbolos importados sem NID válido ou sem correspondência AeroLib.");
    if (load.tls_relocations_pending != 0)
        Add(unique, result.blockers, L"A inicialização TLS do convidado ainda está pendente.");

    // These are implementation gates, not format failures. They remain until
    // the UWP runtime supplies a real SysV thunk and host-backed HLE address.
    Add(unique, result.blockers, L"A ponte ABI SysV do PS4 para o ABI x64 do Xbox ainda não está ligada.");
    Add(unique, result.blockers, L"As funções HLE ainda não possuem endereços executáveis no runtime UWP.");

    for (auto const& encoded : load.import_library_names) {
        const auto name = LibraryName(encoded);
        if (name == L"libScePigletv2VSH")
            Add(unique, result.blockers, L"O renderer Piglet EGL/OpenGL ainda precisa de backend Direct3D UWP.");
        else if (name == L"libSceFreeType")
            Add(unique, result.blockers, L"O backend FreeType importado pelo homebrew ainda não está ligado ao UWP.");
        else if (name == L"libSceRegMgr")
            Add(unique, result.blockers, L"O serviço RegMgr ainda não possui uma implementação HLE UWP.");
    }

    result.ready = result.blockers.empty();
    return result;
}

} // namespace Lab
