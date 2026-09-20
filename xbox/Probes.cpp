// SPDX-License-Identifier: GPL-2.0-or-later
#include "Probes.h"
#include <d3d12.h>
#include <memoryapi.h>
#include <fileapifromapp.h>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.System.h>

using namespace winrt;
using namespace Windows::Data::Json;
namespace Lab {
namespace {
struct Allocation { void* p{}; ~Allocation() { if (p) VirtualFree(p, 0, MEM_RELEASE); } };
struct View { void* p{}; ~View() { if (p) UnmapViewOfFile(p); } };
void Number(Test& t, wchar_t const* name, double value) { t.measurements.Insert(name, JsonValue::CreateNumberValue(value)); }
void String(Test& t, wchar_t const* name, std::wstring const& value) { t.measurements.Insert(name, JsonValue::CreateStringValue(value)); }
std::vector<char> Shader(wchar_t const* name) {
    auto path = std::filesystem::path(Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()) / L"Shaders" / name;
    std::ifstream file(path, std::ios::binary);
    if (!file) throw hresult_error(E_FAIL, L"Shader pré-compilado ausente no pacote.");
    return {std::istreambuf_iterator<char>(file), {}};
}
struct D3D11 {
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    D3D11() {
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, device.put(), &level, context.put()));
    }
};
void Compute(Test& t) {
    D3D11 gpu;
    Number(t, L"feature_level", gpu.level);
    auto dxgi = gpu.device.as<IDXGIDevice>();
    com_ptr<IDXGIAdapter> adapter; check_hresult(dxgi->GetAdapter(adapter.put()));
    DXGI_ADAPTER_DESC desc{}; check_hresult(adapter->GetDesc(&desc));
    String(t, L"adapter", desc.Description);
    auto code = Shader(L"ProbeCS.cso");
    com_ptr<ID3D11ComputeShader> shader;
    check_hresult(gpu.device->CreateComputeShader(code.data(), code.size(), nullptr, shader.put()));
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = 4; bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS; bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride = 4;
    com_ptr<ID3D11Buffer> buffer; check_hresult(gpu.device->CreateBuffer(&bufferDesc, nullptr, buffer.put()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC viewDesc{};
    viewDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; viewDesc.Buffer.NumElements = 1;
    com_ptr<ID3D11UnorderedAccessView> view;
    check_hresult(gpu.device->CreateUnorderedAccessView(buffer.get(), &viewDesc, view.put()));
    auto* raw = view.get(); gpu.context->CSSetUnorderedAccessViews(0, 1, &raw, nullptr);
    gpu.context->CSSetShader(shader.get(), nullptr, 0); gpu.context->Dispatch(1, 1, 1);
    raw = nullptr; gpu.context->CSSetUnorderedAccessViews(0, 1, &raw, nullptr);
    bufferDesc.Usage = D3D11_USAGE_STAGING; bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    bufferDesc.BindFlags = 0; bufferDesc.MiscFlags = 0; bufferDesc.StructureByteStride = 0;
    com_ptr<ID3D11Buffer> staging; check_hresult(gpu.device->CreateBuffer(&bufferDesc, nullptr, staging.put()));
    gpu.context->CopyResource(staging.get(), buffer.get()); gpu.context->Flush();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr{};
    do {
        hr = gpu.context->Map(staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    check_hresult(hr);
    uint32_t value{}; std::memcpy(&value, mapped.pData, sizeof(value)); gpu.context->Unmap(staging.get(), 0);
    Number(t, L"shader_readback", value);
    if (value != 0x53484144) throw hresult_error(E_FAIL, L"Shader retornou valor incorreto.");
    t.detail = L"Shader de computação executado por hardware; valor 0x53484144 confirmado por leitura de volta. Não valida shaders PS4.";
}
}
void RunProbe(Test& t, std::wstring const& directory) {
    t.status = L"passed";
    if (t.id == L"storage") {
        auto path = directory + L"\\storage-probe.txt";
        std::string payload = "shadPS4 Xbox storage probe " + std::to_string(Now());
        WriteDurable(path, payload);
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        std::string read{std::istreambuf_iterator<char>(input), {}};
        if (read != payload) throw hresult_error(E_FAIL, L"Conteúdo lido difere do conteúdo gravado.");
        t.detail = L"Arquivo gravado, sincronizado e relido em LocalState.";
    } else if (t.id == L"budget") {
        Number(t, L"app_memory_usage_bytes", Windows::System::MemoryManager::AppMemoryUsage());
        Number(t, L"app_memory_limit_bytes", Windows::System::MemoryManager::AppMemoryUsageLimit());
        t.detail = L"Orçamento consultado no sistema. Este resultado não confirma espaço suficiente para um jogo; o núcleo reserva grandes regiões e memória de backing adicional.";
    } else if (t.id == L"d3d11") Compute(t);
    else if (t.id == L"d3d12") {
        com_ptr<ID3D12Device> device;
        check_hresult(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), device.put_void()));
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        check_hresult(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
        Number(t, L"resource_binding_tier", options.ResourceBindingTier);
        Number(t, L"tiled_resources_tier", options.TiledResourcesTier);
        t.detail = L"Dispositivo Direct3D 12 criado e recursos consultados. Não testa apresentação D3D12 nem substitui o renderizador Vulkan.";
    } else if (t.id == L"mapping") {
        handle mapping{CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 65536, nullptr)};
        if (!mapping) throw_last_error();
        View a{MapViewOfFileFromApp(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 65536)};
        if (!a.p) throw_last_error();
        View b{MapViewOfFileFromApp(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 65536)};
        if (!b.p) throw_last_error();
        *static_cast<volatile uint32_t*>(a.p) = 0x53484144;
        if (*static_cast<volatile uint32_t*>(b.p) != 0x53484144) throw hresult_error(E_FAIL, L"Visões não compartilham os mesmos bytes.");
        t.detail = L"Duas visões RW compartilham 64 KiB. Não comprova MapViewOfFile3, placeholders, alias executável ou backing completo do PS4.";
    } else if (t.id == L"address") {
        JsonArray attempts;
        bool all = true;
        // Small reservations only; never claim these prove the entire PS4 virtual address layout.
        for (uintptr_t address : {uintptr_t(0x400000), uintptr_t(0x800000000), uintptr_t(0x1000000000)}) {
            Allocation allocation{VirtualAllocFromApp(reinterpret_cast<void*>(address), 65536, MEM_RESERVE, PAGE_NOACCESS)};
            DWORD error = allocation.p ? 0 : GetLastError();
            JsonObject entry; entry.Insert(L"address", JsonValue::CreateNumberValue(static_cast<double>(address)));
            entry.Insert(L"reserved", JsonValue::CreateBooleanValue(allocation.p == reinterpret_cast<void*>(address)));
            entry.Insert(L"win32_error", JsonValue::CreateNumberValue(error)); attempts.Append(entry);
            all = all && allocation.p == reinterpret_cast<void*>(address);
        }
        t.measurements.Insert(L"reservations", attempts);
        t.status = all ? L"passed" : L"unavailable";
        t.detail = L"Reservas de 64 KiB em três endereços representativos. Falha pode indicar endereço ocupado; sucesso não valida a reserva integral usada pelo shadPS4.";
    } else if (t.id == L"protection" || t.id == L"codegen") {
        Allocation allocation{VirtualAllocFromApp(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)};
        if (!allocation.p) throw_last_error();
        DWORD old{};
        if (t.id == L"protection") {
            *static_cast<uint32_t*>(allocation.p) = 42;
            check_bool(VirtualProtectFromApp(allocation.p, 4096, PAGE_READONLY, &old));
            auto value = *static_cast<volatile uint32_t*>(allocation.p);
            check_bool(VirtualProtectFromApp(allocation.p, 4096, PAGE_READWRITE, &old));
            if (value != 42) throw hresult_error(E_FAIL, L"Valor incorreto após mudança de proteção.");
            t.detail = L"Transição RW → R → RW validada sem provocar violação de acesso. Tratamento de exceções do núcleo ainda não validado.";
        } else {
            // mov eax,42; ret — generated by this project, no downloaded or guest code.
            constexpr std::array<unsigned char, 6> code{0xB8, 0x2A, 0, 0, 0, 0xC3};
            std::memcpy(allocation.p, code.data(), code.size());
            check_bool(VirtualProtectFromApp(allocation.p, 4096, PAGE_EXECUTE_READ, &old));
            check_bool(FlushInstructionCache(GetCurrentProcess(), allocation.p, code.size()));
            auto fn = reinterpret_cast<int (*)()>(allocation.p);
            auto value = fn(); Number(t, L"returned_value", value);
            if (value != 42) throw hresult_error(E_FAIL, L"Código x64 retornou valor incorreto.");
            t.detail = L"Código x64 próprio retornou 42 após transição RW → RX. Não valida ABI, TLS, exceções ou execução de binários PS4.";
        }
    } else throw hresult_error(E_INVALIDARG, L"Teste automático desconhecido.");
}
com_ptr<IDXGISwapChain1> RenderTriangle() {
    D3D11 gpu;
    auto dxgi = gpu.device.as<IDXGIDevice>();
    com_ptr<IDXGIAdapter> adapter; check_hresult(dxgi->GetAdapter(adapter.put()));
    com_ptr<IDXGIFactory2> factory; check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = 640; desc.Height = 240; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2; desc.Scaling = DXGI_SCALING_STRETCH; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    com_ptr<IDXGISwapChain1> chain;
    check_hresult(factory->CreateSwapChainForComposition(gpu.device.get(), &desc, nullptr, chain.put()));
    com_ptr<ID3D11Texture2D> buffer; check_hresult(chain->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void()));
    com_ptr<ID3D11RenderTargetView> target;
    check_hresult(gpu.device->CreateRenderTargetView(buffer.get(), nullptr, target.put()));
    auto vsCode = Shader(L"TriangleVS.cso"), psCode = Shader(L"TrianglePS.cso");
    com_ptr<ID3D11VertexShader> vs; com_ptr<ID3D11PixelShader> ps;
    check_hresult(gpu.device->CreateVertexShader(vsCode.data(), vsCode.size(), nullptr, vs.put()));
    check_hresult(gpu.device->CreatePixelShader(psCode.data(), psCode.size(), nullptr, ps.put()));
    const float clear[]{0.025f, 0.04f, 0.08f, 1}; gpu.context->ClearRenderTargetView(target.get(), clear);
    auto raw = target.get(); gpu.context->OMSetRenderTargets(1, &raw, nullptr);
    D3D11_VIEWPORT viewport{0, 0, 640, 240, 0, 1}; gpu.context->RSSetViewports(1, &viewport);
    gpu.context->VSSetShader(vs.get(), nullptr, 0); gpu.context->PSSetShader(ps.get(), nullptr, 0);
    gpu.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.context->Draw(3, 0); gpu.context->Flush();
    return chain;
}
}
