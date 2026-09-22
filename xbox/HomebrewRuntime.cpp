// SPDX-License-Identifier: GPL-2.0-or-later
#include "HomebrewRuntime.h"

#include "SysvThunk.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <intrin.h>
#include <stdexcept>
#include <windows.h>

namespace Lab {
namespace {

std::filesystem::path gCrashStateFile;
std::uint64_t gGuestHostBase{};
std::uint64_t gGuestVirtualBase{};
std::uint64_t gGuestImageSize{};
std::uint32_t gGuestTlsSlot{UINT32_MAX};
std::uint64_t gExpectedTcb{};
std::uint64_t UnixSeconds() noexcept;

std::uint32_t PatchFsTcbReads(std::vector<std::uint8_t>& image,
                              std::uint64_t virtualBase,
                              std::vector<GuestSegmentInfo> const& segments,
                              std::uint32_t tlsSlot) {
  // On Windows x64 the TEB is addressed through GS. Its first 64 TLS slots
  // begin at 0x1480. Replace the nine-byte PS4 `mov reg, fs:[0]` form with the
  // same-length `mov reg, gs:[tebTlsSlot]`; this is the compact form used by
  // the upstream shadPS4 CPU patcher for Windows TCB access.
  if (tlsSlot >= 64)
    throw std::runtime_error("O slot TLS do Windows não cabe no acesso GS direto.");
  const auto tebOffset = 0x1480u + tlsSlot * sizeof(void*);
  std::uint32_t patched{};
  for (auto const& segment : segments) {
    if ((segment.flags & 0x1u) == 0 || segment.address < virtualBase)
      continue;
    const auto begin64 = segment.address - virtualBase;
    if (begin64 >= image.size()) continue;
    const auto begin = static_cast<std::size_t>(begin64);
    const auto available = image.size() - begin;
    const auto bytes = static_cast<std::size_t>((std::min<std::uint64_t>)(
        segment.size, available));
    if (bytes < 9) continue;
    for (std::size_t index = begin; index + 9 <= begin + bytes; ++index) {
      if (image[index] != 0x64 || image[index + 1] < 0x48 ||
          image[index + 1] > 0x4F || image[index + 2] != 0x8B ||
          (image[index + 3] & 0xC7u) != 0x04u ||
          image[index + 4] != 0x25)
        continue;
      std::uint32_t displacement{};
      std::memcpy(&displacement, image.data() + index + 5,
                  sizeof(displacement));
      if (displacement != 0) continue;
      image[index] = 0x65;
      std::memcpy(image.data() + index + 5, &tebOffset, sizeof(tebOffset));
      ++patched;
      index += 8;
    }
  }
  return patched;
}

int RecordGuestException(EXCEPTION_POINTERS *exception) noexcept {
  if (!exception || !exception->ExceptionRecord || !exception->ContextRecord ||
      gCrashStateFile.empty())
    return EXCEPTION_EXECUTE_HANDLER;
  char payload[768]{};
  const auto *record = exception->ExceptionRecord;
  const auto fault = record->NumberParameters > 1
                         ? record->ExceptionInformation[1]
                         : 0;
  const auto rip = static_cast<std::uint64_t>(exception->ContextRecord->Rip);
  const auto tlsOffset = gGuestTlsSlot < 64
                             ? 0x1480u + gGuestTlsSlot * sizeof(void*)
                             : 0u;
  const auto observedTcb = tlsOffset ? __readgsqword(tlsOffset) : 0;
  const auto guestRip = rip >= gGuestHostBase &&
                                rip - gGuestHostBase < gGuestImageSize
                            ? gGuestVirtualBase + (rip - gGuestHostBase)
                            : 0;
  const auto guestFault = fault >= gGuestHostBase &&
                                  fault - gGuestHostBase < gGuestImageSize
                              ? gGuestVirtualBase + (fault - gGuestHostBase)
                              : 0;
  const int length = std::snprintf(
      payload, sizeof(payload),
      "{\"stage\":\"guest_exception\",\"exception_code\":%lu,"
      "\"exception_address\":%llu,\"rip\":%llu,\"rsp\":%llu,"
      "\"rcx\":%llu,\"tls_slot\":%lu,\"expected_tcb\":%llu,"
      "\"observed_tcb\":%llu,"
      "\"fault_address\":%llu,\"guest_virtual_rip\":%llu,"
      "\"guest_virtual_fault\":%llu,\"timestamp\":%llu}",
      static_cast<unsigned long>(record->ExceptionCode),
      static_cast<unsigned long long>(
          reinterpret_cast<std::uintptr_t>(record->ExceptionAddress)),
      static_cast<unsigned long long>(rip),
      static_cast<unsigned long long>(exception->ContextRecord->Rsp),
      static_cast<unsigned long long>(exception->ContextRecord->Rcx),
      static_cast<unsigned long>(gGuestTlsSlot),
      static_cast<unsigned long long>(gExpectedTcb),
      static_cast<unsigned long long>(observedTcb),
      static_cast<unsigned long long>(fault),
      static_cast<unsigned long long>(guestRip),
      static_cast<unsigned long long>(guestFault),
      static_cast<unsigned long long>(UnixSeconds()));
  if (length > 0) {
    CREATEFILE2_EXTENDED_PARAMETERS parameters{sizeof(parameters)};
    parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    HANDLE file = CreateFile2(gCrashStateFile.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ, CREATE_ALWAYS, &parameters);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD written{};
      WriteFile(file, payload, static_cast<DWORD>(length < 767 ? length : 767),
                &written, nullptr);
      FlushFileBuffers(file);
      CloseHandle(file);
    }
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

LONG CALLBACK RecordGuestVectoredException(EXCEPTION_POINTERS *exception) noexcept {
  if (!exception || !exception->ExceptionRecord)
    return EXCEPTION_CONTINUE_SEARCH;
  switch (exception->ExceptionRecord->ExceptionCode) {
  case EXCEPTION_ACCESS_VIOLATION:
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
  case EXCEPTION_DATATYPE_MISALIGNMENT:
  case EXCEPTION_ILLEGAL_INSTRUCTION:
  case EXCEPTION_IN_PAGE_ERROR:
  case EXCEPTION_INT_DIVIDE_BY_ZERO:
  case EXCEPTION_STACK_OVERFLOW:
    RecordGuestException(exception);
    break;
  default:
    break;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// Keep SEH in a function without C++ objects that require unwinding. The
// vectored handler records faults before Windows attempts to unwind guest code;
// this wrapper still handles faults that can cross the generated launcher.
std::uint64_t InvokeGuestProtected(void *entry, std::uint64_t argument0,
                                   bool *crashed, bool *exited) noexcept {
  __try {
    return InvokeGuestEntry(entry, argument0, exited);
  } __except (RecordGuestException(GetExceptionInformation())) {
    *crashed = true;
    return 0;
  }
}

std::uint64_t UnixSeconds() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
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
  if (!running_.load() && tlsSlot_ != UINT32_MAX)
    TlsFree(tlsSlot_);
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
  if (tlsSlot_ != UINT32_MAX) {
    TlsFree(tlsSlot_);
    tlsSlot_ = UINT32_MAX;
  }

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

  tlsSlot_ = TlsAlloc();
  if (tlsSlot_ == TLS_OUT_OF_INDEXES)
    throw std::runtime_error("Não foi possível reservar o slot TLS convidado.");
  patchedFsReads_ = PatchFsTcbReads(load_.private_image,
                                    load_.min_virtual_address,
                                    load_.guest_segments, tlsSlot_);
  if (patchedFsReads_ == 0)
    throw std::runtime_error("Nenhum acesso PS4 fs:[0] foi localizado para tradução.");

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
  gGuestHostBase = memory_->RuntimeAddress(load_.min_virtual_address);
  gGuestVirtualBase = load_.min_virtual_address;
  gGuestImageSize = load_.private_image.size();
  Record("ready", "Imagem real relocada; iniciando o ponto de entrada.");
  gCrashStateFile = stateFile_;
  running_.store(true);
  worker_ = std::thread([this] { RunEntry(); });
}

void HomebrewRuntime::RunEntry() noexcept {
  Record("entry_started", "Controle transferido ao e_entry do homebrew.");
  auto* tcb = mainTlsBlock_.data() + 64;
  mainDtv_ = {1, 1, 0, 0};
  std::memcpy(tcb, &tcb, sizeof(tcb));
  auto* dtv = mainDtv_.data();
  std::memcpy(tcb + 8, &dtv, sizeof(dtv));
  std::memcpy(tcb + 16, &tcb, sizeof(tcb));
  const auto threadId = static_cast<std::uint32_t>(GetCurrentThreadId());
  std::memcpy(tcb + 56, &threadId, sizeof(threadId));
  if (!TlsSetValue(tlsSlot_, tcb)) {
    Record("host_exception", "Não foi possível ativar a base TLS convidada.");
    running_.store(false);
    return;
  }
  gGuestTlsSlot = tlsSlot_;
  gExpectedTcb = reinterpret_cast<std::uint64_t>(tcb);
  const auto tlsOffset = 0x1480u + tlsSlot_ * sizeof(void*);
  const auto observedTcb = __readgsqword(tlsOffset);
  if (TlsGetValue(tlsSlot_) != tcb || observedTcb != gExpectedTcb) {
    Record("tls_invalid", "A leitura GS do slot TLS não corresponde ao TCB instalado.");
    running_.store(false);
    return;
  }
  using AddVectoredHandler = PVOID(WINAPI *)(
      ULONG, PVECTORED_EXCEPTION_HANDLER);
  using RemoveVectoredHandler = ULONG(WINAPI *)(PVOID);
  auto module = GetModuleHandleW(L"kernelbase.dll");
  auto addVectoredHandler = module
      ? reinterpret_cast<AddVectoredHandler>(
            GetProcAddress(module, "AddVectoredExceptionHandler"))
      : nullptr;
  auto removeVectoredHandler = module
      ? reinterpret_cast<RemoveVectoredHandler>(
            GetProcAddress(module, "RemoveVectoredExceptionHandler"))
      : nullptr;
  auto *vectoredHandler = addVectoredHandler
      ? addVectoredHandler(1, &RecordGuestVectoredException)
      : nullptr;
  try {
    bool crashed = false;
    bool exited = false;
    const auto value = InvokeGuestProtected(
        reinterpret_cast<void *>(params_.entry_addr),
        reinterpret_cast<std::uint64_t>(&params_), &crashed, &exited);
    if (exited)
      Record("entry_exited", "O runtime do homebrew solicitou encerramento controlado.");
    else if (!crashed)
      Record("entry_returned", "O e_entry retornou ao host com código " +
                                   std::to_string(value) + ".");
  } catch (std::exception const &error) {
    Record("host_exception", error.what());
  } catch (...) {
    Record("host_exception", "Exceção desconhecida durante a execução.");
  }
  if (vectoredHandler && removeVectoredHandler)
    removeVectoredHandler(vectoredHandler);
  running_.store(false);
}

} // namespace Lab
