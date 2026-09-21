// SPDX-License-Identifier: GPL-2.0-or-later
#include "SysvThunk.h"
#include "core/platform_memory.h"

#include <windows.h>
#include <memoryapi.h>

#include <cstring>
#include <stdexcept>

namespace Lab {
namespace {

constexpr std::size_t PageSize = 4096;
constexpr std::uint32_t FrameSize = 0xE8;
constexpr std::uint8_t WindowsShadowSpace = 0x20;

void Byte(std::uint8_t* code, std::size_t& offset, std::uint8_t value) {
    code[offset++] = value;
}

void U32(std::uint8_t* code, std::size_t& offset, std::uint32_t value) {
    std::memcpy(code + offset, &value, sizeof(value));
    offset += sizeof(value);
}

void U64(std::uint8_t* code, std::size_t& offset, std::uint64_t value) {
    std::memcpy(code + offset, &value, sizeof(value));
    offset += sizeof(value);
}

void StoreGpr(std::uint8_t* code, std::size_t& offset, std::uint8_t reg, std::uint8_t displacement) {
    // mov [rsp+disp8], reg
    Byte(code, offset, static_cast<std::uint8_t>(0x48 | (reg >= 8 ? 0x04 : 0x00)));
    Byte(code, offset, 0x89);
    Byte(code, offset, static_cast<std::uint8_t>(0x44 | ((reg & 7u) << 3)));
    Byte(code, offset, 0x24);
    Byte(code, offset, displacement);
}

void StoreXmm(std::uint8_t* code, std::size_t& offset, std::uint8_t reg,
              std::uint32_t displacement) {
    // movdqu [rsp+disp32], xmmN
    Byte(code, offset, 0xF3);
    Byte(code, offset, 0x0F);
    Byte(code, offset, 0x7F);
    Byte(code, offset, static_cast<std::uint8_t>(0x84 | (reg << 3)));
    Byte(code, offset, 0x24);
    U32(code, offset, displacement);
}

constexpr std::uint64_t ValidationReturn = 0xA81B'C2D3'E4F5'0617ull;
constexpr std::uint64_t ValidationGpr[] = {
    0x1111'1111'1111'1111ull, 0x2222'2222'2222'2222ull,
    0x3333'3333'3333'3333ull, 0x4444'4444'4444'4444ull,
    0x5555'5555'5555'5555ull, 0x6666'6666'6666'6666ull};
constexpr std::uint64_t ValidationStack[] = {
    0x7777'7777'7777'7777ull, 0x8888'8888'8888'8888ull};
constexpr std::uint64_t ValidationXmm[] = {
    0x0123'4567'89AB'CDEFull, 0xFEDC'BA98'7654'3210ull};

std::uint64_t ValidateDispatch(void*, std::uint64_t slot, GuestCallFrame const* frame,
                               void* guestStack) noexcept {
    if (slot != 0 || !frame || !guestStack ||
        frame->guest_stack != reinterpret_cast<std::uint64_t>(guestStack))
        return 0;
    for (std::size_t index = 0; index != 6; ++index)
        if (frame->gpr[index] != ValidationGpr[index]) return 0;
    auto const* stack = static_cast<std::uint64_t const*>(guestStack);
    if (stack[1] != ValidationStack[0] || stack[2] != ValidationStack[1]) return 0;
    std::uint64_t xmm0{};
    std::uint64_t xmm1{};
    std::memcpy(&xmm0, frame->xmm[0], sizeof(xmm0));
    std::memcpy(&xmm1, frame->xmm[1], sizeof(xmm1));
    return xmm0 == ValidationXmm[0] && xmm1 == ValidationXmm[1] ? ValidationReturn : 0;
}

} // namespace

SysvThunkArena::~SysvThunkArena() {
    for (auto* page : pages_)
        Core::PlatformMemory::Free(GetCurrentProcess(), page, 0, MEM_RELEASE);
}

void* SysvThunkArena::Create(void* context, std::uint64_t slot, SysvDispatch dispatch) {
    if (!dispatch) throw std::invalid_argument("SysV thunk sem dispatcher.");
    auto* page = static_cast<std::uint8_t*>(Core::PlatformMemory::Allocate(
        GetCurrentProcess(), nullptr, PageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) throw std::runtime_error("Não foi possível reservar a página do thunk SysV.");

    std::size_t offset = 0;
    Byte(page, offset, 0x48); Byte(page, offset, 0x81); Byte(page, offset, 0xEC); // sub rsp, imm32
    U32(page, offset, FrameSize);
    // Keep the Windows 32-byte shadow space separate from GuestCallFrame;
    // the dispatcher is allowed to use that area for its own arguments.
    StoreGpr(page, offset, 7, WindowsShadowSpace + 0x00); // rdi
    StoreGpr(page, offset, 6, WindowsShadowSpace + 0x08); // rsi
    StoreGpr(page, offset, 2, WindowsShadowSpace + 0x10); // rdx
    StoreGpr(page, offset, 1, WindowsShadowSpace + 0x18); // rcx
    StoreGpr(page, offset, 8, WindowsShadowSpace + 0x20); // r8
    StoreGpr(page, offset, 9, WindowsShadowSpace + 0x28); // r9
    // Reconstruct the incoming stack pointer after reserving the Windows call
    // frame. Using RAX avoids destroying an incoming SysV argument register.
    Byte(page, offset, 0x48); Byte(page, offset, 0x8D); Byte(page, offset, 0x84);
    Byte(page, offset, 0x24); U32(page, offset, FrameSize); // lea rax,[rsp+FrameSize]
    StoreGpr(page, offset, 0, WindowsShadowSpace + 0x30);
    for (std::uint8_t index = 0; index != 8; ++index)
        StoreXmm(page, offset, index, WindowsShadowSpace + 0x40u + index * 16u);

    // Windows x64 dispatcher(context, slot, frame, originalGuestStack).
    Byte(page, offset, 0x48); Byte(page, offset, 0xB9); U64(page, offset, reinterpret_cast<std::uint64_t>(context)); // mov rcx,context
    Byte(page, offset, 0x48); Byte(page, offset, 0xBA); U64(page, offset, slot); // mov rdx,slot
    Byte(page, offset, 0x4C); Byte(page, offset, 0x8D); Byte(page, offset, 0x44);
    Byte(page, offset, 0x24); Byte(page, offset, WindowsShadowSpace); // lea r8,[rsp+20h]
    Byte(page, offset, 0x4C); Byte(page, offset, 0x8B); Byte(page, offset, 0x4C);
    Byte(page, offset, 0x24); Byte(page, offset, WindowsShadowSpace + 0x30); // mov r9,[rsp+50h]
    Byte(page, offset, 0x48); Byte(page, offset, 0xB8);
    U64(page, offset, reinterpret_cast<std::uint64_t>(dispatch)); // mov rax,dispatch
    Byte(page, offset, 0xFF); Byte(page, offset, 0xD0); // call rax
    Byte(page, offset, 0x48); Byte(page, offset, 0x81); Byte(page, offset, 0xC4); // add rsp,imm32
    U32(page, offset, FrameSize);
    Byte(page, offset, 0xC3); // ret

    DWORD previous{};
    if (!Core::PlatformMemory::Protect(GetCurrentProcess(), page, PageSize, PAGE_EXECUTE_READ,
                                       &previous)) {
        Core::PlatformMemory::Free(GetCurrentProcess(), page, 0, MEM_RELEASE);
        throw std::runtime_error("Não foi possível proteger o thunk SysV como RX.");
    }
    if (!FlushInstructionCache(GetCurrentProcess(), page, offset)) {
        Core::PlatformMemory::Free(GetCurrentProcess(), page, 0, MEM_RELEASE);
        throw std::runtime_error("Não foi possível limpar o cache de instruções do thunk SysV.");
    }
    pages_.push_back(page);
    return page;
}

SysvAbiValidation ValidateSysvThunkAbi() {
    SysvThunkArena arena;
    auto* thunk = arena.Create(nullptr, 0, &ValidateDispatch);
    auto* caller = static_cast<std::uint8_t*>(Core::PlatformMemory::Allocate(
        GetCurrentProcess(), nullptr, PageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!caller) throw std::runtime_error("Não foi possível reservar o validador da ABI SysV.");

    std::size_t offset = 0;
    Byte(caller, offset, 0x57); // preserve Windows nonvolatile RDI
    Byte(caller, offset, 0x56); // preserve Windows nonvolatile RSI
    Byte(caller, offset, 0x48); Byte(caller, offset, 0x83); Byte(caller, offset, 0xEC);
    Byte(caller, offset, 0x28); // align SysV call and reserve two stack arguments
    for (std::size_t index = 0; index != 2; ++index) {
        Byte(caller, offset, 0x48); Byte(caller, offset, 0xB8); U64(caller, offset, ValidationStack[index]);
        Byte(caller, offset, 0x48); Byte(caller, offset, 0x89);
        Byte(caller, offset, static_cast<std::uint8_t>(index == 0 ? 0x04 : 0x44));
        Byte(caller, offset, 0x24);
        if (index != 0) Byte(caller, offset, 0x08);
    }
    // SysV integer arguments: RDI, RSI, RDX, RCX, R8, R9.
    constexpr std::uint8_t movImmediatePrefixes[] = {0x48, 0x48, 0x48, 0x48, 0x49, 0x49};
    constexpr std::uint8_t movImmediateOpcodes[] = {0xBF, 0xBE, 0xBA, 0xB9, 0xB8, 0xB9};
    for (std::size_t index = 0; index != 6; ++index) {
        Byte(caller, offset, movImmediatePrefixes[index]);
        Byte(caller, offset, movImmediateOpcodes[index]);
        U64(caller, offset, ValidationGpr[index]);
    }
    // mov rax,pattern; movq xmmN,rax
    for (std::size_t index = 0; index != 2; ++index) {
        Byte(caller, offset, 0x48); Byte(caller, offset, 0xB8); U64(caller, offset, ValidationXmm[index]);
        Byte(caller, offset, 0x66); Byte(caller, offset, 0x48); Byte(caller, offset, 0x0F);
        Byte(caller, offset, 0x6E); Byte(caller, offset, static_cast<std::uint8_t>(0xC0 | (index << 3)));
    }
    Byte(caller, offset, 0x48); Byte(caller, offset, 0xB8);
    U64(caller, offset, reinterpret_cast<std::uint64_t>(thunk));
    Byte(caller, offset, 0xFF); Byte(caller, offset, 0xD0); // call rax
    Byte(caller, offset, 0x48); Byte(caller, offset, 0x83); Byte(caller, offset, 0xC4);
    Byte(caller, offset, 0x28);
    Byte(caller, offset, 0x5E); Byte(caller, offset, 0x5F); Byte(caller, offset, 0xC3);

    DWORD previous{};
    if (!Core::PlatformMemory::Protect(GetCurrentProcess(), caller, PageSize, PAGE_EXECUTE_READ,
                                       &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), caller, offset)) {
        Core::PlatformMemory::Free(GetCurrentProcess(), caller, 0, MEM_RELEASE);
        throw std::runtime_error("Não foi possível ativar o validador da ABI SysV.");
    }
    const auto value = reinterpret_cast<std::uint64_t (*)()>(caller)();
    Core::PlatformMemory::Free(GetCurrentProcess(), caller, 0, MEM_RELEASE);
    return SysvAbiValidation{value == ValidationReturn, value};
}

} // namespace Lab
