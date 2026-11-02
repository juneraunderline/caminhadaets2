#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <atomic>
#include <mutex>

#include "dx11_fade.h"
#include "log.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace dx11_fade {
namespace {

typedef HRESULT (STDMETHODCALLTYPE *present_fn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *resize_fn)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);

present_fn original_present = NULL;
resize_fn original_resize = NULL;
void **swapchain_vtable = NULL;
std::atomic<float> alpha(0.0f);
std::atomic<int> transition(0); // 0 idle, 1 fading out, 2 fading in
std::atomic<bool> installed(false);
std::mutex resource_lock;

ID3D11Device *device = NULL;
ID3D11DeviceContext *context = NULL;
ID3D11RenderTargetView *target = NULL;
ID3D11VertexShader *vertex_shader = NULL;
ID3D11PixelShader *pixel_shader = NULL;
ID3D11Buffer *alpha_buffer = NULL;
ID3D11BlendState *blend_state = NULL;
ID3D11DepthStencilState *depth_state = NULL;
ID3D11RasterizerState *raster_state = NULL;
IDXGISwapChain *active_swapchain = NULL;

const char *const SHADER_SOURCE =
"struct FadeData { float alpha; float3 padding; };"
"cbuffer FadeConstants : register(b0) { FadeData fade; };"
"struct VSOut { float4 position : SV_Position; };"
"VSOut VSMain(uint id : SV_VertexID) {"
"  VSOut o; float2 p;"
"  p.x = (id == 2) ? 3.0 : -1.0;"
"  p.y = (id == 1) ? 3.0 : -1.0;"
"  o.position = float4(p, 0.0, 1.0); return o;"
"}"
"float4 PSMain(VSOut input) : SV_Target { return float4(0.0, 0.0, 0.0, saturate(fade.alpha)); }";

void release_resources(void)
{
    if (target) { target->Release(); target = NULL; }
    if (active_swapchain) { active_swapchain->Release(); active_swapchain = NULL; }
    if (device) { device->Release(); device = NULL; }
    if (context) { context->Release(); context = NULL; }
    if (vertex_shader) { vertex_shader->Release(); vertex_shader = NULL; }
    if (pixel_shader) { pixel_shader->Release(); pixel_shader = NULL; }
    if (alpha_buffer) { alpha_buffer->Release(); alpha_buffer = NULL; }
    if (blend_state) { blend_state->Release(); blend_state = NULL; }
    if (depth_state) { depth_state->Release(); depth_state = NULL; }
    if (raster_state) { raster_state->Release(); raster_state = NULL; }
}

bool make_resources(IDXGISwapChain *chain)
{
    ID3D11Device *new_device = NULL;
    if (FAILED(chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void **>(&new_device)))) return false;
    ID3D11DeviceContext *new_context = NULL;
    new_device->GetImmediateContext(&new_context);
    if (!new_context) { new_device->Release(); return false; }

    ID3DBlob *vs_blob = NULL, *ps_blob = NULL, *errors = NULL;
    HRESULT hr = D3DCompile(SHADER_SOURCE, lstrlenA(SHADER_SOURCE), NULL, NULL, NULL,
        "VSMain", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vs_blob, &errors);
    if (errors) errors->Release();
    if (FAILED(hr)) { new_context->Release(); new_device->Release(); return false; }
    hr = D3DCompile(SHADER_SOURCE, lstrlenA(SHADER_SOURCE), NULL, NULL, NULL,
        "PSMain", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &ps_blob, &errors);
    if (errors) errors->Release();
    if (FAILED(hr)) { vs_blob->Release(); new_context->Release(); new_device->Release(); return false; }

    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    hr = new_device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), NULL, &vs);
    if (SUCCEEDED(hr)) hr = new_device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), NULL, &ps);
    vs_blob->Release(); ps_blob->Release();
    if (FAILED(hr)) {
        if (vs) vs->Release(); if (ps) ps->Release();
        new_context->Release(); new_device->Release(); return false;
    }

    D3D11_BUFFER_DESC buffer_desc = {};
    buffer_desc.ByteWidth = 16;
    buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer *cb = NULL;
    hr = new_device->CreateBuffer(&buffer_desc, NULL, &cb);

    D3D11_BLEND_DESC blend_desc = {};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState *blend = NULL;
    if (SUCCEEDED(hr)) hr = new_device->CreateBlendState(&blend_desc, &blend);

    D3D11_DEPTH_STENCIL_DESC depth_desc = {};
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depth_desc.StencilEnable = FALSE;
    ID3D11DepthStencilState *depth = NULL;
    if (SUCCEEDED(hr)) hr = new_device->CreateDepthStencilState(&depth_desc, &depth);

    D3D11_RASTERIZER_DESC raster_desc = {};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;
    raster_desc.DepthClipEnable = TRUE;
    ID3D11RasterizerState *raster = NULL;
    if (SUCCEEDED(hr)) hr = new_device->CreateRasterizerState(&raster_desc, &raster);

    if (FAILED(hr)) {
        if (cb) cb->Release(); if (blend) blend->Release(); if (depth) depth->Release();
        if (raster) raster->Release(); vs->Release(); ps->Release();
        new_context->Release(); new_device->Release(); return false;
    }

    device = new_device; context = new_context; vertex_shader = vs; pixel_shader = ps;
    alpha_buffer = cb; blend_state = blend; depth_state = depth; raster_state = raster;
    return true;
}

bool ensure_target(IDXGISwapChain *chain)
{
    if (active_swapchain != chain) {
        if (target) { target->Release(); target = NULL; }
        if (active_swapchain) { active_swapchain->Release(); active_swapchain = NULL; }
        if (device) release_resources();
        if (!make_resources(chain)) return false;
        active_swapchain = chain;
        active_swapchain->AddRef();
    }
    if (target) return true;
    ID3D11Texture2D *backbuffer = NULL;
    HRESULT hr = chain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&backbuffer));
    if (FAILED(hr)) return false;
    hr = device->CreateRenderTargetView(backbuffer, NULL, &target);
    backbuffer->Release();
    return SUCCEEDED(hr);
}

template<class T> void release_if(T *p) { if (p) p->Release(); }

void draw_fade(IDXGISwapChain *chain)
{
    const float a = alpha.load(std::memory_order_relaxed);
    if (a <= 0.001f) return;
    std::lock_guard<std::mutex> guard(resource_lock);
    if (!ensure_target(chain)) return;

    ID3D11RenderTargetView *old_targets[ D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT ] = {};
    ID3D11DepthStencilView *old_depth = NULL;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_targets, &old_depth);

    ID3D11BlendState *old_blend = NULL;
    FLOAT old_blend_factor[4] = {};
    UINT old_sample_mask = 0;
    context->OMGetBlendState(&old_blend, old_blend_factor, &old_sample_mask);
    ID3D11DepthStencilState *old_depth_state = NULL;
    UINT old_stencil_ref = 0;
    context->OMGetDepthStencilState(&old_depth_state, &old_stencil_ref);
    ID3D11RasterizerState *old_raster = NULL;
    context->RSGetState(&old_raster);
    D3D11_VIEWPORT old_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT old_viewport_count = _countof(old_viewports);
    context->RSGetViewports(&old_viewport_count, old_viewports);

    ID3D11InputLayout *old_layout = NULL;
    context->IAGetInputLayout(&old_layout);
    D3D11_PRIMITIVE_TOPOLOGY old_topology;
    context->IAGetPrimitiveTopology(&old_topology);

    ID3D11VertexShader *old_vs = NULL;
    ID3D11PixelShader *old_ps = NULL;
    ID3D11GeometryShader *old_gs = NULL;
    context->VSGetShader(&old_vs, NULL, NULL);
    context->PSGetShader(&old_ps, NULL, NULL);
    context->GSGetShader(&old_gs, NULL, NULL);
    ID3D11Buffer *old_ps_cb = NULL;
    context->PSGetConstantBuffers(0, 1, &old_ps_cb);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context->Map(alpha_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        *reinterpret_cast<float *>(mapped.pData) = a;
        context->Unmap(alpha_buffer, 0);

        D3D11_VIEWPORT viewport = {};
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(chain->GetDesc(&desc))) {
            viewport.Width = static_cast<float>(desc.BufferDesc.Width);
            viewport.Height = static_cast<float>(desc.BufferDesc.Height);
            viewport.MinDepth = 0.0f; viewport.MaxDepth = 1.0f;
            context->OMSetRenderTargets(1, &target, NULL);
            context->OMSetBlendState(blend_state, NULL, 0xffffffff);
            context->OMSetDepthStencilState(depth_state, 0);
            context->RSSetState(raster_state);
            context->RSSetViewports(1, &viewport);
            context->IASetInputLayout(NULL);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vertex_shader, NULL, 0);
            context->PSSetShader(pixel_shader, NULL, 0);
            context->PSSetConstantBuffers(0, 1, &alpha_buffer);
            context->GSSetShader(NULL, NULL, 0);
            context->Draw(3, 0);
        }
    }

    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, old_targets, old_depth);
    context->OMSetBlendState(old_blend, old_blend_factor, old_sample_mask);
    context->OMSetDepthStencilState(old_depth_state, old_stencil_ref);
    context->RSSetState(old_raster);
    if (old_viewport_count) context->RSSetViewports(old_viewport_count, old_viewports);
    context->IASetInputLayout(old_layout);
    context->IASetPrimitiveTopology(old_topology);
    context->VSSetShader(old_vs, NULL, 0);
    context->PSSetShader(old_ps, NULL, 0);
    context->GSSetShader(old_gs, NULL, 0);
    context->PSSetConstantBuffers(0, 1, &old_ps_cb);

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) release_if(old_targets[i]);
    release_if(old_depth); release_if(old_blend); release_if(old_depth_state); release_if(old_raster);
    release_if(old_layout); release_if(old_vs); release_if(old_ps); release_if(old_gs); release_if(old_ps_cb);
}

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain *chain, UINT sync, UINT flags)
{
    draw_fade(chain);
    return original_present ? original_present(chain, sync, flags) : S_OK;
}

HRESULT STDMETHODCALLTYPE hooked_resize(IDXGISwapChain *chain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    {
        std::lock_guard<std::mutex> guard(resource_lock);
        if (chain == active_swapchain && target) { target->Release(); target = NULL; }
    }
    return original_resize ? original_resize(chain, count, width, height, format, flags) : E_FAIL;
}

LRESULT CALLBACK dummy_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool patch_slot(void **slot, void *replacement, void **original)
{
    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protect)) return false;
    *original = *slot;
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void *), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
    return true;
}

bool install_hook(void)
{
    HINSTANCE instance = GetModuleHandleW(NULL);
    const wchar_t *class_name = L"TakeAWalkDX11Dummy";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = dummy_proc; wc.hInstance = instance; wc.lpszClassName = class_name;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, class_name, L"", WS_OVERLAPPEDWINDOW, 0, 0, 32, 32, NULL, NULL, instance, NULL);
    if (!hwnd) { UnregisterClassW(class_name, instance); return false; }

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 32; desc.BufferDesc.Height = 32;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain *chain = NULL;
    ID3D11Device *dummy_device = NULL;
    ID3D11DeviceContext *dummy_context = NULL;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        NULL, 0, D3D11_SDK_VERSION, &desc, &chain, &dummy_device, &level, &dummy_context);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            NULL, 0, D3D11_SDK_VERSION, &desc, &chain, &dummy_device, &level, &dummy_context);
    }
    if (FAILED(hr) || !chain) {
        if (dummy_context) dummy_context->Release();
        if (dummy_device) dummy_device->Release();
        if (chain) chain->Release();
        DestroyWindow(hwnd); UnregisterClassW(class_name, instance); return false;
    }

    swapchain_vtable = *reinterpret_cast<void ***>(chain);
    bool ok = patch_slot(&swapchain_vtable[8], reinterpret_cast<void *>(hooked_present), reinterpret_cast<void **>(&original_present));
    if (ok) ok = patch_slot(&swapchain_vtable[13], reinterpret_cast<void *>(hooked_resize), reinterpret_cast<void **>(&original_resize));
    if (!ok && original_present) {
        DWORD old_protect = 0;
        if (VirtualProtect(&swapchain_vtable[8], sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protect)) {
            swapchain_vtable[8] = reinterpret_cast<void *>(original_present);
            DWORD ignored = 0; VirtualProtect(&swapchain_vtable[8], sizeof(void *), old_protect, &ignored);
        }
    }
    dummy_context->Release(); dummy_device->Release(); chain->Release();
    DestroyWindow(hwnd); UnregisterClassW(class_name, instance);
    return ok;
}

} // namespace

bool init(void)
{
    if (installed.load()) return true;
    if (!install_hook()) {
        log_message(SCS_LOG_TYPE_warning, "DX11 fade hook could not be installed; in-game fade is unavailable");
        return false;
    }
    installed.store(true);
    log_message(SCS_LOG_TYPE_message, "DX11 Present fade hook installed");
    return true;
}

void shutdown(void)
{
    transition.store(0);
    alpha.store(0.0f);
    if (swapchain_vtable && original_present) {
        DWORD old_protect = 0;
        if (VirtualProtect(&swapchain_vtable[8], sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protect)) {
            swapchain_vtable[8] = reinterpret_cast<void *>(original_present);
            DWORD ignored = 0; VirtualProtect(&swapchain_vtable[8], sizeof(void *), old_protect, &ignored);
        }
    }
    if (swapchain_vtable && original_resize) {
        DWORD old_protect = 0;
        if (VirtualProtect(&swapchain_vtable[13], sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protect)) {
            swapchain_vtable[13] = reinterpret_cast<void *>(original_resize);
            DWORD ignored = 0; VirtualProtect(&swapchain_vtable[13], sizeof(void *), old_protect, &ignored);
        }
    }
    installed.store(false);
    std::lock_guard<std::mutex> guard(resource_lock);
    release_resources();
    swapchain_vtable = NULL; original_present = NULL; original_resize = NULL;
}

void fade_out(void)
{
    alpha.store(0.0f);
    transition.store(1);
}

void fade_in(void)
{
    transition.store(2);
}

bool is_black(void)
{
    return alpha.load() >= 0.999f;
}

} // namespace dx11_fade

