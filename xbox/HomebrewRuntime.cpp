// SPDX-License-Identifier: GPL-2.0-or-later
#include "HomebrewRuntime.h"

#include "SysvThunk.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace Lab {
namespace {

std::uint64_t UnixSeconds() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

void GuestExit() noexcept {
  // The PS4 kernel normally terminates the guest process here. Returning lets
  // the experimental host worker record the result without killing the UWP UI.
}

} // namespace

HomebrewRuntime::~HomebrewRuntime() {
  // A running guest cannot be cancelled safely. Keep its complete ownership
  // alive until process teardown; a returned entry is joined normally.
  if (worker_.joinable()) {
    if (running_.load())
      worker_.detach();
    else
      worker_.join();
  }
}

void HomebrewRuntime::Record(std::string const &stage,
                             std::string const &detail) noexcept {
  try {
    auto temporary = stateFile_;
    temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << "{\"stage\":\"" << stage << "\",\"detail\":\"" << detail
           << "\",\"timestamp\":" << UnixSeconds() << "}";
    output.flush();
    output.close();
    std::error_code ignored;
    std::filesystem::remove(stateFile_, ignored);
    std::filesystem::rename(temporary, stateFile_);
  } catch (...) {
  }
}

void HomebrewRuntime::Start(std::filesystem::path executable,
                            std::filesystem::path stateRoot) {
  if (running_.load())
    throw std::runtime_error("Um homebrew já está em execução.");
  if (worker_.joinable())
    worker_.join();

  executable_ = std::move(executable);
  stateFile_ = std::move(stateRoot) / L"homebrew-runtime.json";
  Record("loading", "Carregando o eboot.bin real.");
  load_ = LoadControlled(executable_);
  if (!load_.validated || !load_.mapped || load_.private_image.empty())
    throw std::runtime_error("O ELF/SELF não produziu uma imagem executável válida.");
  if (load_.unsupported_relocations != 0 ||
      load_.relocation_targets_outside_segments != 0 ||
      load_.symbol_relocations_invalid != 0)
    throw std::runtime_error("A imagem contém relocações que este runtime ainda não aceita.");

  dispatcher_ = std::make_unique<HleDispatcher>();
  dispatcher_->ConfigureFileSystem(executable_.parent_path(),
                                    executable_.parent_path() / L"RuntimeData");
  const auto bindings = dispatcher_->Bind(load_.pending_symbol_names);
  if (bindings.executable_addresses != load_.pending_symbol_names.size())
    throw std::runtime_error("Nem todos os imports receberam thunk HLE.");

  for (auto const &relocation : load_.pending_symbol_relocations) {
    auto *address = dispatcher_->AddressFor(relocation.symbol);
    if (!address || relocation.target < load_.min_virtual_address)
      throw std::runtime_error("Import HLE sem endereço executável.");
    const auto offset = relocation.target - load_.min_virtual_address;
    if (offset > load_.private_image.size() ||
        sizeof(std::uint64_t) > load_.private_image.size() - offset)
      throw std::runtime_error("Relocação HLE fora da imagem.");
    const auto value = reinterpret_cast<std::uint64_t>(address) +
                       static_cast<std::uint64_t>(relocation.addend);
    std::memcpy(load_.private_image.data() + offset, &value, sizeof(value));
  }

  memory_ = std::make_unique<GuestMemory>();
  if (!memory_->MapValidated(load_.private_image, load_.min_virtual_address,
                             load_.guest_segments,
                             load_.pending_relative_relocations))
    throw std::runtime_error("Não foi possível mapear a imagem real do homebrew.");
  dispatcher_->AttachGuestMemory(memory_.get());
  const auto entry = memory_->RuntimeAddress(load_.entry);
  if (!memory_->IsExecutable(entry))
    throw std::runtime_error("O ponto de entrada não pertence a um segmento executável.");

  guestPath_ = "/app0/eboot.bin";
  params_ = {};
  params_.argc = 1;
  params_.argv[0] = guestPath_.c_str();
  params_.entry_addr = entry;
  Record("ready", "Imagem real relocada; iniciando o ponto de entrada.");
  running_.store(true);
  worker_ = std::thread([this] { RunEntry(); });
}

void HomebrewRuntime::RunEntry() noexcept {
  Record("entry_started", "Controle transferido ao e_entry do homebrew.");
  try {
    const auto value = InvokeGuestSysv2(
        reinterpret_cast<void *>(params_.entry_addr),
        reinterpret_cast<std::uint64_t>(&params_),
        reinterpret_cast<std::uint64_t>(&GuestExit));
    Record("entry_returned", "O e_entry retornou ao host com código " +
                                 std::to_string(value) + ".");
  } catch (std::exception const &error) {
    Record("host_exception", error.what());
  } catch (...) {
    Record("host_exception", "Exceção desconhecida durante a execução.");
  }
  running_.store(false);
}

} // namespace Lab
