// swapchain_probe.cpp — standalone (NOT loaded through the layer).
// Creates a hidden window + DXGI factory + D3D12 device + swapchain and
// presents one frame. Tells us whether ANY process on this PC can present
// right now (vs the failure being specific to the layer-loaded guest).
// Usage: swapchain_probe.exe  (prints result, exit 0 = present ok)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <d3d12.h>
#include <dxgi1_3.h>

int main() {
    printf("[probe] starting\n");
    const wchar_t* kClass = L"ProbeWnd";
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hi;
    wc.lpszClassName = kClass;
    if (!RegisterClassW(&wc)) {
        printf("[probe] RegisterClass failed (%lu)\n", GetLastError());
        return 1;
    }
    HWND hwnd = CreateWindowExW(0, kClass, L"Probe", WS_OVERLAPPEDWINDOW,
                                0, 0, 640, 360, nullptr, nullptr, hi, nullptr);
    if (!hwnd) {
        printf("[probe] CreateWindowEx failed (%lu)\n", GetLastError());
        return 1;
    }
    printf("[probe] window created %p (hidden)\n", hwnd);

    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&dev));
    if (FAILED(hr)) {
        printf("[probe] D3D12CreateDevice hr=0x%08x\n", (unsigned)hr);
        return 1;
    }
    printf("[probe] device %p\n", dev);

    IDXGIFactory2* factory = nullptr;
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        printf("[probe] CreateDXGIFactory2 hr=0x%08x\n", (unsigned)hr);
        return 1;
    }
    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    if (FAILED(hr)) {
        printf("[probe] CreateCommandQueue hr=0x%08x\n", (unsigned)hr);
        return 1;
    }
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 640;
    sd.Height = 360;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc = nullptr;
    hr = factory->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc);
    if (FAILED(hr)) {
        printf("[probe] CreateSwapChainForHwnd hr=0x%08x\n", (unsigned)hr);
        // Legacy fallback.
        DXGI_SWAP_CHAIN_DESC lsd = {};
        lsd.BufferCount = 2;
        lsd.BufferDesc.Width = 640;
        lsd.BufferDesc.Height = 360;
        lsd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        lsd.BufferDesc.RefreshRate.Numerator = 60;
        lsd.BufferDesc.RefreshRate.Denominator = 1;
        lsd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        lsd.OutputWindow = hwnd;
        lsd.SampleDesc.Count = 1;
        lsd.Windowed = TRUE;
        lsd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain* legacy = nullptr;
        hr = factory->CreateSwapChain(q, &lsd, &legacy);
        printf("[probe] legacy CreateSwapChain hr=0x%08x\n", (unsigned)hr);
        if (FAILED(hr)) return 1;
        hr = legacy->QueryInterface(IID_PPV_ARGS(&sc));
        legacy->Release();
        if (FAILED(hr)) {
            printf("[probe] QI swapchain1 hr=0x%08x\n", (unsigned)hr);
            return 1;
        }
    }
    printf("[probe] swapchain %p created\n", sc);
    ID3D12Resource* back = nullptr;
    hr = sc->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr)) {
        printf("[probe] GetBuffer hr=0x%08x\n", (unsigned)hr);
        return 1;
    }
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    if (FAILED(hr)) {
        printf("[probe] CreateDescriptorHeap hr=0x%08x\n", (unsigned)hr);
        return 1;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h =
        rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(back, nullptr, h);
    ID3D12CommandAllocator* alloc = nullptr;
    hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                     IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* list = nullptr;
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc,
                                    nullptr, IID_PPV_ARGS(&list));
    if (FAILED(hr)) {
        printf("[probe] list setup hr=0x%08x\n", (unsigned)hr);
        return 1;
    }
    FLOAT color[4] = {0.2f, 0.4f, 0.8f, 1.0f};
    list->ClearRenderTargetView(h, color, 0, nullptr);
    hr = list->Close();
    if (FAILED(hr)) {
        printf("[probe] Close hr=0x%08x\n", (unsigned)hr);
        return 1;
    }
    ID3D12CommandList* lists[] = {list};
    q->ExecuteCommandLists(1, lists);
    hr = sc->Present(1, 0);
    printf("[probe] Present hr=0x%08x\n", (unsigned)hr);
    if (FAILED(hr)) return 1;
    ID3D12Fence* fence = nullptr;
    hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (SUCCEEDED(hr)) {
        HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        q->Signal(fence, 1);
        fence->SetEventOnCompletion(1, ev);
        DWORD w = WaitForSingleObject(ev, 10000);
        printf("[probe] fence wait %s\n", w == WAIT_OBJECT_0 ? "OK" : "TIMEOUT");
        CloseHandle(ev);
        fence->Release();
        if (w != WAIT_OBJECT_0) return 1;
    }
    printf("[probe] PRESENT OK\n");
    return 0;
}
