#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <iostream>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

int main(int argc, char** argv) {
    std::cout << "[MiamiTestApp:Graphics] Testing guest Direct3D 12 device acquisition...\n";

    IDXGIFactory4* pFactory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&pFactory)))) {
        std::cerr << "[MiamiTestApp:Graphics] Failed to create DXGI factory\n";
        return 1;
    }

    IDXGIAdapter1* pAdapter = nullptr;
    if (FAILED(pFactory->EnumAdapters1(0, &pAdapter))) {
        std::cerr << "[MiamiTestApp:Graphics] No DXGI adapter found\n";
        pFactory->Release();
        return 2;
    }

    DXGI_ADAPTER_DESC1 desc;
    pAdapter->GetDesc1(&desc);
    std::wcout << L"[MiamiTestApp:Graphics] Adapter: " << desc.Description << L"\n";

    ID3D12Device* pDevice = nullptr;
    HRESULT hr = D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&pDevice));
    pAdapter->Release();
    pFactory->Release();

    if (FAILED(hr) || !pDevice) {
        std::cerr << "[MiamiTestApp:Graphics] Failed to create D3D12 device\n";
        return 3;
    }

    // Create a command queue
    D3D12_COMMAND_QUEUE_DESC qdesc = {};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* pQueue = nullptr;
    hr = pDevice->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&pQueue));
    if (FAILED(hr) || !pQueue) {
        std::cerr << "[MiamiTestApp:Graphics] Failed to create D3D12 command queue\n";
        pDevice->Release();
        return 4;
    }

    pQueue->Release();
    pDevice->Release();
    std::cout << "[MiamiTestApp:Graphics] D3D12 device & command queue verified successfully.\n";
    return 0;
}
