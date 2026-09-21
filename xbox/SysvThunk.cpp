// SPDX-License-Identifier: GPL-2.0-or-later
#include "SysvThunk.h"

#include <windows.h>
#include <memoryapi.h>

#include <cstring>
#include <stdexcept>

namespace Lab {
namespace {

constexpr std::size_t PageSize = 4096;
constexpr std::uint32_t FrameSize = 0xE8;

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
    if (reg < 4) {
        Byte(code, offset, 0x48);
        Byte(code, offset, 0x89);
        Byte(code, offset, static_cast<std::uint8_t>(0x44 | (reg << 3)));
    } else {
        Byte(code, offset, 0x4C);
        Byte(code, offset, 0x89);
        Byte(code, offset, static_cast<std::uint8_t>(0x44 | ((reg - 4) << 3)));
    }
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

} // namespace

SysvThunkArena::~SysvThunkArena() {
    for (auto* page : pages_) VirtualFree(page, 0, MEM_RELEASE);
}

void* SysvThunkArena::Create(void* context, std::uint64_t slot, SysvDispatch dispatch) {
    if (!dispatch) throw std::invalid_argument("SysV thunk sem dispatcher.");
    auto* page = static_cast<std::uint8_t*>(
        VirtualAllocFromApp(nullptr, PageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!page) throw std::runtime_error("Não foi possível reservar a página do thunk SysV.");

    std::size_t offset = 0;
    // Preserve the incoming PS4 stack pointer before switching to the Windows
    // shadow-space frame used by the dispatcher.
    Byte(page, offset, 0x49); Byte(page, offset, 0x89); Byte(page, offset, 0xE3); // mov r11,rsp
    Byte(page, offset, 0x48); Byte(page, offset, 0x81); Byte(page, offset, 0xEC); // sub rsp, imm32
    U32(page, offset, FrameSize);
    StoreGpr(page, offset, 7, 0x00); // rdi
    StoreGpr(page, offset, 6, 0x08); // rsi
    StoreGpr(page, offset, 2, 0x10); // rdx
    StoreGpr(page, offset, 1, 0x18); // rcx
    StoreGpr(page, offset, 8, 0x20); // r8
    StoreGpr(page, offset, 9, 0x28); // r9
    StoreGpr(page, offset, 11, 0x30); // original rsp
    for (std::uint8_t index = 0; index != 8; ++index)
        StoreXmm(page, offset, index, 0x40u + index * 16u);

    // Windows x64 dispatcher(context, slot, frame, originalGuestStack).
    Byte(page, offset, 0x48); Byte(page, offset, 0xB9); U64(page, offset, reinterpret_cast<std::uint64_t>(context)); // mov rcx,context
    Byte(page, offset, 0x48); Byte(page, offset, 0xBA); U64(page, offset, slot); // mov rdx,slot
    Byte(page, offset, 0x4C); Byte(page, offset, 0x8D); Byte(page, offset, 0x04); Byte(page, offset, 0x24); // lea r8,[rsp]
    Byte(page, offset, 0x4D); Byte(page, offset, 0x89); Byte(page, offset, 0xD9); // mov r9,r11
    Byte(page, offset, 0x48); Byte(page, offset, 0xB8);
    U64(page, offset, reinterpret_cast<std::uint64_t>(dispatch)); // mov rax,dispatch
    Byte(page, offset, 0xFF); Byte(page, offset, 0xD0); // call rax
    Byte(page, offset, 0x48); Byte(page, offset, 0x81); Byte(page, offset, 0xC4); // add rsp,imm32
    U32(page, offset, FrameSize);
    Byte(page, offset, 0xC3); // ret

    DWORD previous{};
    if (!VirtualProtectFromApp(page, PageSize, PAGE_EXECUTE_READ, &previous)) {
        VirtualFree(page, 0, MEM_RELEASE);
        throw std::runtime_error("Não foi possível proteger o thunk SysV como RX.");
    }
    if (!FlushInstructionCache(GetCurrentProcess(), page, offset)) {
        VirtualFree(page, 0, MEM_RELEASE);
        throw std::runtime_error("Não foi possível limpar o cache de instruções do thunk SysV.");
    }
    pages_.push_back(page);
    return page;
}

} // namespace Lab
