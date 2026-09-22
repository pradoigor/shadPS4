// SPDX-License-Identifier: GPL-2.0-or-later
#include "HomebrewRuntime.h"

#include "SysvThunk.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <windows.h>

namespace Lab {
namespace {

std::filesystem::path gCrashStateFile;
std::uint64_t UnixSeconds() noexcept;

int RecordGuestException(EXCEPTION_POINTERS *exception) noexcept {
  if (!exception || !exception->ExceptionRecord || !exception->ContextRecord ||
      gCrashStateFile.empty())
    return EXCEPTION_EXECUTE_HANDLER;
  char payload[512]{};
  const auto *record = exception->ExceptionRecord;
  const auto fault = record->NumberParameters > 1
                         ? record->ExceptionInformation[1]
                         : 0;
  const int length = std::snprintf(
      payload, sizeof(payload),
      "{\"stage\":\"guest_exception\",\"exception_code\":%lu,"
      "\"exception_address\":%llu,\"rip\":%llu,\"rsp\":%llu,"
      "\"fault_address\":%llu,\"timestamp\":%llu}",
      static_cast<unsigned long>(record->ExceptionCode),
      static_cast<unsigned long long>(
          reinterpret_cast<std::uintptr_t>(record->ExceptionAddress)),
      static_cast<unsigned long long>(exception->ContextRecord->Rip),
      static_cast<unsigned long long>(exception->ContextRecord->Rsp),
      static_cast<unsigned long long>(fault),
      static_cast<unsigned long long>(UnixSeconds()));
  if (length > 0) {
    CREATEFILE2_EXTENDED_PARAMETERS parameters{sizeof(parameters)};
    parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    HANDLE file = CreateFile2(gCrashStateFile.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ, CREATE_ALWAYS, &parameters);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD written{};
      WriteFile(file, payload, static_cast<DWORD>(length < 511 ? length : 511),
                &written, nullptr);
      FlushFileBuffers(file);
      CloseHandle(file);
    }
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

// Keep SEH in a function without C++ objects that require unwinding. UWP does
// not expose vectored exception registration, but MSVC SEH remains available.
std::uint64_t InvokeGuestProtected(void *entry, std::uint64_t argument0,
                                   std::uint64_t argument1,
                                   bool *crashed) noexcept {
  __try {
    return InvokeGuestSysv2(entry, argument0, argument1);
  } __except (RecordGuestException(GetExceptionInformation())) {
    *crashed = true;
    return 0;
  }
}

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
  stateFile_ = stateRoot / L"homebrew-runtime.json";
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
  dispatcher_->ConfigureTrace(stateRoot / L"homebrew-last-hle.json");
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
  gCrashStateFile = stateFile_;
  running_.store(true);
  worker_ = std::thread([this] { RunEntry(); });
}

void HomebrewRuntime::RunEntry() noexcept {
  Record("entry_started", "Controle transferido ao e_entry do homebrew.");
  try {
    bool crashed = false;
    const auto value = InvokeGuestProtected(
        reinterpret_cast<void *>(params_.entry_addr),
        reinterpret_cast<std::uint64_t>(&params_),
        reinterpret_cast<std::uint64_t>(&GuestExit), &crashed);
    if (!crashed)
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
