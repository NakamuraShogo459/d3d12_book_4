﻿#include "D3D12AppBase.h"
#include <exception>
#include <fstream>
#if _MSC_VER > 1922 && !defined(_SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING)
#define _SILENCE_EXPERIMENTAL_FILESYSTEM_DEPRECATION_WARNING
#endif

#include "d3dx12.h"
#include <DirectXTex.h>
#include "D3D12BookUtil.h"

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

#include <filesystem>
namespace fs = std::filesystem;

// シェーダーのコンパイル用.
#include <dxcapi.h>
#pragma comment(lib, "dxcompiler.lib")

using namespace std;
using namespace Microsoft::WRL;

D3D12AppBase::D3D12AppBase()
{
  m_frameIndex = 0;
  m_waitFence = CreateEvent(NULL, FALSE, FALSE, NULL);
}


D3D12AppBase::~D3D12AppBase()
{
  CloseHandle(m_waitFence);
}

void D3D12AppBase::SetTitle(const std::string& title)
{
  SetWindowTextA(m_hwnd, title.c_str());
}

void D3D12AppBase::Initialize(HWND hwnd, DXGI_FORMAT format, bool isFullscreen, bool useWaitableSwapchain)
{
  m_hwnd = hwnd;
  HRESULT hr;
  UINT dxgiFlags = 0;
#if defined(_DEBUG)
  ComPtr<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
  {
    debug->EnableDebugLayer();
    dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
  }
#endif
  ComPtr<IDXGIFactory5> factory;
  hr = CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&factory));
  ThrowIfFailed(hr, "CreateDXGIFactory2 失敗");

  // ハードウェアアダプタの検索
  ComPtr<IDXGIAdapter1> useAdapter;
  {
    UINT adapterIndex = 0;
    ComPtr<IDXGIAdapter1> adapter;
    while (DXGI_ERROR_NOT_FOUND != factory->EnumAdapters1(adapterIndex, &adapter))
    {
      DXGI_ADAPTER_DESC1 desc1{};
      adapter->GetDesc1(&desc1);
      ++adapterIndex;
      if (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        continue;

      // D3D12は使用可能か
      hr = D3D12CreateDevice(
        adapter.Get(),
        D3D_FEATURE_LEVEL_11_0,
        __uuidof(ID3D12Device), nullptr);
      if (SUCCEEDED(hr))
        break;
    }
    adapter.As(&useAdapter); // 使用するアダプター
  }

  hr = D3D12CreateDevice(useAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
  ThrowIfFailed(hr, "D3D12CreateDevice 失敗");
  m_adapter = useAdapter;

  // コマンドキューの生成
  D3D12_COMMAND_QUEUE_DESC queueDesc{
    D3D12_COMMAND_LIST_TYPE_DIRECT,
    0,
    D3D12_COMMAND_QUEUE_FLAG_NONE,
    0
  };
  hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
  ThrowIfFailed(hr, "CreateCommandQueue 失敗");

  // 各ディスクリプタヒープの準備.
  PrepareDescriptorHeaps();

  // HWND からクライアント領域サイズを判定する。
  // (ウィンドウサイズをもらってそれを使用するのもよい)
  RECT rect;
  GetClientRect(hwnd, &rect);
  m_width = rect.right - rect.left;
  m_height = rect.bottom - rect.top;

  bool useHDR = format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R10G10B10A2_UNORM;
  if (useHDR)
  {
    bool isDisplayHDR10 = false;
    UINT index = 0;
    ComPtr<IDXGIOutput> current;
    while (useAdapter->EnumOutputs(index, &current) != DXGI_ERROR_NOT_FOUND)
    {
      ComPtr<IDXGIOutput6> output6;
      current.As(&output6);

      DXGI_OUTPUT_DESC1 desc;
      output6->GetDesc1(&desc);
      isDisplayHDR10 |= desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
      ++index;
    }

    if (!isDisplayHDR10)
    {
      format = DXGI_FORMAT_R8G8B8A8_UNORM;
      useHDR = false;
    }
  }

  BOOL allowTearing = FALSE;
  hr = factory->CheckFeatureSupport(
    DXGI_FEATURE_PRESENT_ALLOW_TEARING,
    &allowTearing,
    sizeof(allowTearing)
  );
  m_isAllowTearing = SUCCEEDED(hr) && allowTearing;

  // スワップチェインの生成
  {
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.BufferCount = FrameBufferCount;
    scDesc.Width = m_width;
    scDesc.Height = m_height;
    scDesc.Format = format;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scDesc.SampleDesc.Count = 1;
    //scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;  // ディスプレイの解像度も変更する場合にはコメント解除。

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc{};
    fsDesc.Windowed = isFullscreen ? FALSE : TRUE;
    fsDesc.RefreshRate.Denominator = 1000;
    fsDesc.RefreshRate.Numerator = 60317;
    fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;

    if (useWaitableSwapchain) {
      scDesc.BufferCount = 2;
      scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      scDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }

    ComPtr<IDXGISwapChain1> swapchain;
    hr = factory->CreateSwapChainForHwnd(
      m_commandQueue.Get(),
      hwnd,
      &scDesc,
      &fsDesc,
      nullptr,
      &swapchain);
    ThrowIfFailed(hr, "CreateSwapChainForHwnd 失敗");
    m_swapchain = std::make_shared<Swapchain>(swapchain, m_heapRTV);

    if(useWaitableSwapchain) {
      ComPtr<IDXGISwapChain2> swapchain2;
      swapchain.As(&swapchain2);
      if (swapchain2) {
        swapchain2->SetMaximumFrameLatency(1);
        HANDLE waitableObj = swapchain2->GetFrameLatencyWaitableObject();

        m_swapchain->SetWaitableObject(waitableObj);
      }
    }
  
  }


  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  m_surfaceFormat = m_swapchain->GetFormat();

  //// デプスバッファ関連の準備.
  CreateDefaultDepthBuffer(m_width, m_height);

  // コマンドアロケータ－の準備.
  CreateCommandAllocators();

  // コマンドリストの生成.
  hr = m_device->CreateCommandList(
    0,
    D3D12_COMMAND_LIST_TYPE_DIRECT,
    m_commandAllocators[0].Get(),
    nullptr,
    IID_PPV_ARGS(&m_commandList)
  );
  ThrowIfFailed(hr, "CreateCommandList 失敗");
  m_commandList->Close();

  m_viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, float(m_width), float(m_height));
  m_scissorRect = CD3DX12_RECT(0, 0, LONG(m_width), LONG(m_height));

  Prepare();

  PrepareImGui();
}

void D3D12AppBase::Terminate()
{
  WaitForIdleGPU();
  Cleanup();

  CleanupImGui();
}


void D3D12AppBase::Render()
{
  m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();

  m_commandAllocators[m_frameIndex]->Reset();
  m_commandList->Reset(
    m_commandAllocators[m_frameIndex].Get(),
    nullptr
  );

  // スワップチェイン表示可能からレンダーターゲット描画可能へ
  auto barrierToRT = m_swapchain->GetBarrierToRenderTarget();
  m_commandList->ResourceBarrier(1, &barrierToRT);

  auto rtv = m_swapchain->GetCurrentRTV();
  auto dsv = m_defaultDepthDSV;

  // カラーバッファ(レンダーターゲットビュー)のクリア
  const float clearColor[] = { 0.5f,0.75f,1.0f,0.0f }; // クリア色
  m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
  
  // デプスバッファ(デプスステンシルビュー)のクリア
  m_commandList->ClearDepthStencilView(
    dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

  // 描画先をセット
  D3D12_CPU_DESCRIPTOR_HANDLE handleRtvs[] = { rtv };
  D3D12_CPU_DESCRIPTOR_HANDLE handleDsv = dsv;
  m_commandList->OMSetRenderTargets(1, handleRtvs, FALSE, &handleDsv);

  ID3D12DescriptorHeap* heaps[] = { m_heap->GetHeap().Get() };
  m_commandList->SetDescriptorHeaps(_countof(heaps), heaps);

  // レンダーターゲットからスワップチェイン表示可能へ
  auto barrierToPresent = m_swapchain->GetBarrierToPresent();
  m_commandList->ResourceBarrier(1, &barrierToPresent);

  m_commandList->Close();

  ID3D12CommandList* lists[] = { m_commandList.Get() };

  m_commandQueue->ExecuteCommandLists(1, lists);

  m_swapchain->Present(1, 0);
  m_swapchain->WaitPreviousFrame(m_commandQueue, m_frameIndex, GpuWaitTimeout);

}

D3D12AppBase::ComPtr<ID3D12Resource1> D3D12AppBase::CreateResource(
  const CD3DX12_RESOURCE_DESC& desc,
  D3D12_RESOURCE_STATES resourceStates,
  const D3D12_CLEAR_VALUE* clearValue,
  D3D12_HEAP_TYPE heapType )
{
  HRESULT hr;
  ComPtr<ID3D12Resource1> ret;
  const auto heapProps = CD3DX12_HEAP_PROPERTIES(heapType);
  hr = m_device->CreateCommittedResource(
    &heapProps,
    D3D12_HEAP_FLAG_NONE,
    &desc,
    resourceStates,
    clearValue,
    IID_PPV_ARGS(&ret)
  );
  ThrowIfFailed(hr, "CreateCommittedResource Failed.");
  return ret;
}

std::vector<ComPtr<ID3D12Resource1>> D3D12AppBase::CreateConstantBuffers(const CD3DX12_RESOURCE_DESC& desc, int count)
{
  vector<ComPtr<ID3D12Resource1>> buffers;
  for (int i = 0; i < count; ++i)
  {
    buffers.emplace_back(
      CreateResource(desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, D3D12_HEAP_TYPE_UPLOAD)
    );
  }
  return buffers;
}


D3D12AppBase::ComPtr<ID3D12GraphicsCommandList> D3D12AppBase::CreateCommandList()
{
  HRESULT hr;
  ComPtr<ID3D12GraphicsCommandList> command;
  hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_oneshotCommandAllocator.Get(), nullptr, IID_PPV_ARGS(&command));
  ThrowIfFailed(hr, "CreateCommandList(OneShot) Failed.");
  command->SetName(L"OneShotCommand");
  
  return command;
}

void D3D12AppBase::FinishCommandList(ComPtr<ID3D12GraphicsCommandList>& command)
{
  ID3D12CommandList* commandList[] = {
    command.Get()
  };
  command->Close();
  m_commandQueue->ExecuteCommandLists(1, commandList);
  HRESULT hr;
  ComPtr<ID3D12Fence1> fence;
  hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  ThrowIfFailed(hr, "CreateFence Failed.");
  const UINT64 expectValue = 1;
  m_commandQueue->Signal(fence.Get(), expectValue);
  do
  {
  } while (fence->GetCompletedValue() != expectValue);
  m_oneshotCommandAllocator->Reset();
}

ComPtr<ID3D12GraphicsCommandList> D3D12AppBase::CreateBundleCommandList()
{
  ComPtr<ID3D12GraphicsCommandList> command;
  m_device->CreateCommandList(
    0, D3D12_COMMAND_LIST_TYPE_BUNDLE,
    m_bundleCommandAllocator.Get(),
    nullptr, IID_PPV_ARGS(&command)
  );
  return command;
}

void D3D12AppBase::WriteToUploadHeapMemory(ID3D12Resource1* resource, uint32_t size, const void* data)
{
  void* mapped;
  HRESULT hr = resource->Map(0, nullptr, &mapped);
  if (SUCCEEDED(hr))
  {
    memcpy(mapped, data, size);
    resource->Unmap(0, nullptr);
  }
  ThrowIfFailed(hr, "Map Failed.");
}

D3D12AppBase::Texture D3D12AppBase::LoadTexture(
  std::string filename, 
  D3D12_RESOURCE_STATES resourceStates,
  ComPtr<ID3D12GraphicsCommandList> commandList)
{
  using namespace DirectX;
  auto it = m_textureDatabase.find(filename);
  if (it != m_textureDatabase.end()) {
    return it->second;
  }

  fs::path loadPath(filename);

  TexMetadata metadata{};
  ScratchImage image;
  HRESULT hr = E_FAIL;
  if (loadPath.extension().string() == ".tga") {
    hr = DirectX::LoadFromTGAFile(loadPath.c_str(), &metadata, image);
  }
  if (loadPath.extension().string() == ".jpg" || loadPath.extension().string() == ".png") {
    WIC_FLAGS flags = WIC_FLAGS_NONE;
    hr = DirectX::LoadFromWICFile(loadPath.c_str(), flags, &metadata, image);
  }
  if (loadPath.extension().string() == ".dds") {
    DDS_FLAGS flags = DDS_FLAGS_NONE;
    hr = DirectX::LoadFromDDSFile(loadPath.c_str(), flags, &metadata, image);
  }
  if (FAILED(hr)) {
    throw std::runtime_error("Texture file not found.");
  }

  ComPtr<ID3D12Resource> texRes = nullptr;
  std::vector<D3D12_SUBRESOURCE_DATA> subresources;
  CreateTexture(m_device.Get(), metadata, &texRes);

  // アップロードヒープの準備.
  DirectX::PrepareUpload(m_device.Get(), image.GetImages(), image.GetImageCount(), metadata, subresources);

  auto totalBytes = GetRequiredIntermediateSize(texRes.Get(), 0, UINT(subresources.size()));
  auto desc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
  auto stagingTex = CreateResource(desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, D3D12_HEAP_TYPE_UPLOAD);

  if (commandList) {
    m_delayReleaseList.push_back(stagingTex);

    UpdateSubresources(
      commandList.Get(),
      texRes.Get(),
      stagingTex.Get(),
      0, 0,
      uint32_t(subresources.size()), subresources.data()
    );

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
      texRes.Get(),
      D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    commandList->ResourceBarrier(1, &barrier);

  } else {
    commandList = CreateCommandList();
    UpdateSubresources(
      commandList.Get(),
      texRes.Get(),
      stagingTex.Get(),
      0, 0,
      uint32_t(subresources.size()), subresources.data()
    );
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
      texRes.Get(),
      D3D12_RESOURCE_STATE_COPY_DEST,
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    );
    commandList->ResourceBarrier(1, &barrier);
    commandList->Close();

    ID3D12CommandList* lists[] = { commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
    WaitForIdleGPU();
  }

  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
  srvDesc.Format = metadata.format;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = UINT(metadata.mipLevels);
  srvDesc.Texture2D.MostDetailedMip = 0;
 
  // 登録処理.
  Texture texture;
  texRes.As(&texture.res);
  texture.srv = GetDescriptorManager()->Alloc();
  m_device->CreateShaderResourceView(texRes.Get(), &srvDesc, texture.srv);

  m_textureDatabase[filename] = texture;
  return texture;
}



void D3D12AppBase::PrepareDescriptorHeaps()
{
  const int MaxDescriptorCount = 2048; // SRV,CBV,UAV など.
  const int MaxDescriptorCountRTV = 100;
  const int MaxDescriptorCountDSV = 100;

  // RTV のディスクリプタヒープ
  D3D12_DESCRIPTOR_HEAP_DESC heapDescRTV{
    D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
    MaxDescriptorCountRTV,
    D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    0
  };
  m_heapRTV = std::make_shared<DescriptorManager>(m_device, heapDescRTV);

  // DSV のディスクリプタヒープ
  D3D12_DESCRIPTOR_HEAP_DESC heapDescDSV{
    D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
    MaxDescriptorCountDSV,
    D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
    0
  };
  m_heapDSV = std::make_shared<DescriptorManager>(m_device, heapDescDSV);

  // SRV のディスクリプタヒープ
  D3D12_DESCRIPTOR_HEAP_DESC heapDesc{
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
    MaxDescriptorCount,
    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    0
  };
  m_heap = std::make_shared<DescriptorManager>(m_device, heapDesc);
}

void D3D12AppBase::CreateDefaultDepthBuffer(int width, int height)
{
  // デプスバッファの生成
  auto depthBufferDesc = CD3DX12_RESOURCE_DESC::Tex2D(
    DXGI_FORMAT_D32_FLOAT,
    width,
    height,
    1, 1,
    1, 0,
    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
  );
  D3D12_CLEAR_VALUE depthClearValue{};
  depthClearValue.Format = depthBufferDesc.Format;
  depthClearValue.DepthStencil.Depth = 1.0f;
  depthClearValue.DepthStencil.Stencil = 0;

  const auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

  HRESULT hr;
  hr = m_device->CreateCommittedResource(
    &heapProps,
    D3D12_HEAP_FLAG_NONE,
    &depthBufferDesc,
    D3D12_RESOURCE_STATE_DEPTH_WRITE,
    &depthClearValue,
    IID_PPV_ARGS(&m_depthBuffer)
  );
  ThrowIfFailed(hr, "CreateCommittedResource 失敗");

  // デプスステンシルビュー生成
  m_defaultDepthDSV = m_heapDSV->Alloc();
  D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc
  {
    DXGI_FORMAT_D32_FLOAT,          // Format
    D3D12_DSV_DIMENSION_TEXTURE2D,  // ViewDimension
    D3D12_DSV_FLAG_NONE,            // Flags
    {       // D3D12_TEX2D_DSV
      0     // MipSlice
    }
  };
  m_device->CreateDepthStencilView(m_depthBuffer.Get(), &dsvDesc, m_defaultDepthDSV);
}

void D3D12AppBase::CreateCommandAllocators()
{
  HRESULT hr;
  m_commandAllocators.resize(FrameBufferCount);
  for (UINT i = 0; i < FrameBufferCount; ++i)
  {
    hr = m_device->CreateCommandAllocator(
      D3D12_COMMAND_LIST_TYPE_DIRECT,
      IID_PPV_ARGS(&m_commandAllocators[i])
    );
    if (FAILED(hr))
    {
      throw std::runtime_error("Failed CreateCommandAllocator");
    }
  }
  hr = m_device->CreateCommandAllocator(
    D3D12_COMMAND_LIST_TYPE_DIRECT,
    IID_PPV_ARGS(&m_oneshotCommandAllocator)
  );
  ThrowIfFailed(hr, "CreateCommandAllocator Failed(oneShot)");

  hr = m_device->CreateCommandAllocator(
    D3D12_COMMAND_LIST_TYPE_BUNDLE,
    IID_PPV_ARGS(&m_bundleCommandAllocator)
  );
  ThrowIfFailed(hr, "CreateCommandAllocator Failed(bundle)");
}
 
void D3D12AppBase::WaitForIdleGPU()
{
  // 全ての発行済みコマンドの終了を待つ.
  ComPtr<ID3D12Fence1> fence;
  const UINT64 expectValue = 1;
  HRESULT hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  ThrowIfFailed(hr, "CreateFence 失敗");

  m_commandQueue->Signal(fence.Get(), expectValue);
  if (fence->GetCompletedValue() != expectValue)
  {
    fence->SetEventOnCompletion(expectValue, m_waitFence);
    WaitForSingleObject(m_waitFence, INFINITE);
  }
}
void D3D12AppBase::OnSizeChanged(UINT width, UINT height, bool isMinimized)
{
  m_width = width;
  m_height = height;
  if (!m_swapchain || isMinimized)
    return;

  // 処理の完了を待ってからサイズ変更の処理を開始.
  WaitForIdleGPU();
  m_swapchain->ResizeBuffers(width, height);

  // デプスバッファの作り直し.
  m_depthBuffer.Reset();
  m_heapDSV->Free(m_defaultDepthDSV);
  CreateDefaultDepthBuffer(m_width, m_height);

  m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();

  m_viewport.Width = float(m_width);
  m_viewport.Height = float(m_height);
  m_scissorRect.right = m_width;
  m_scissorRect.bottom = m_height;
}
void D3D12AppBase::ToggleFullscreen()
{
  if (m_swapchain->IsFullScreen())
  {
    // FullScreen -> Windowed
    m_swapchain->SetFullScreen(false);
    SetWindowLong(m_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
    ShowWindow(m_hwnd, SW_NORMAL);
  }
  else
  {
    // Windowed -> FullScreen
    DXGI_MODE_DESC desc;
    desc.Format = m_surfaceFormat;
    desc.Width = m_width;
    desc.Height = m_height;
    desc.RefreshRate.Denominator = 1;
    desc.RefreshRate.Numerator = 60;
    desc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    desc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    m_swapchain->ResizeTarget(&desc);
    m_swapchain->SetFullScreen(true);
  }
  OnSizeChanged(m_width, m_height, false);
}

void D3D12AppBase::PrepareImGui()
{
  auto descriptorImGui = m_heap->Alloc();
  CD3DX12_CPU_DESCRIPTOR_HANDLE hCpu(descriptorImGui);
  CD3DX12_GPU_DESCRIPTOR_HANDLE hGpu(descriptorImGui);


  ImGui_ImplDX12_Init(
    m_device.Get(),
    FrameBufferCount,
    m_surfaceFormat,
    m_heap->GetHeap().Get(),
    hCpu, hGpu);
}

void D3D12AppBase::CleanupImGui()
{
  ImGui_ImplDX12_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
}


void Shader::load(const std::wstring& fileName, Stage stage,
  const std::wstring& entryPoint,
  const std::vector<std::wstring>& flags,
  const std::vector<DefineMacro>& defines)
{
  fs::path filePath(fileName);
  std::ifstream infile(filePath, std::ios::binary);
  std::vector<char> sourceCode;
  if (!infile)
    throw std::runtime_error("shader not found");
  sourceCode.resize(uint32_t(infile.seekg(0, infile.end).tellg()));
  infile.seekg(0, infile.beg).read(sourceCode.data(), sourceCode.size());

  // DXC によるコンパイル処理
  ComPtr<IDxcLibrary> library;
  ComPtr<IDxcCompiler> compiler;
  ComPtr<IDxcBlobEncoding> source;
  ComPtr<IDxcOperationResult> dxcResult;

  DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library));
  library->CreateBlobWithEncodingFromPinned(sourceCode.data(), UINT(sourceCode.size()), CP_UTF8, &source);
  DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));

  wstring profile;
  switch (stage)
  {
  default:
  case Shader::Vertex:
    profile = L"vs_6_0";
    break;
  case Shader::Geometry:
    profile = L"gs_6_0";
    break;
  case Shader::Pixel:
    profile = L"ps_6_0";
    break;
  case Shader::Domain:
    profile = L"ds_6_0";
    break;
  case Shader::Hull:
    profile = L"hs_6_0";
    break;
  case Shader::Compute:
    profile = L"cs_6_0";
    break;
  }
  vector<LPCWSTR> compilerFlags;
  for (auto& v : flags)
  {
    compilerFlags.push_back(v.c_str());
  }
#if _DEBUG
  compilerFlags.push_back(L"/Zi");
  compilerFlags.push_back(L"/O0");
  compilerFlags.push_back(L"-Qembed_debug");
#else
  compilerFlags.push_back(L"/O2");
#endif
  vector<DxcDefine> defineMacros;
  for (const auto & v : defines)
  {
    defineMacros.push_back(
      DxcDefine{
        v.Name.c_str(),
        v.Value.c_str()
      });
  }

  compiler->Compile(
    source.Get(), filePath.wstring().c_str(),
    entryPoint.c_str(),
    profile.c_str(),
    compilerFlags.data(), UINT(compilerFlags.size()),
    defineMacros.data(), UINT(defineMacros.size()),
    nullptr,
    &dxcResult
  );

  ComPtr<IDxcBlobEncoding> errBlob;
  HRESULT hr;

  hr = dxcResult->GetErrorBuffer(&errBlob);
  if (SUCCEEDED(hr))
  {
    ComPtr<IDxcBlobEncoding> msgBlob;
    library->GetBlobAsUtf16(errBlob.Get(), &msgBlob);

    std::wstring msg((const wchar_t*)msgBlob->GetBufferPointer());
    OutputDebugStringW(msg.c_str());
  }

  dxcResult->GetStatus(&hr);
  if (SUCCEEDED(hr)) {
    hr = dxcResult->GetResult(
      reinterpret_cast<IDxcBlob**>(m_code.GetAddressOf())
    );
  }
  if (FAILED(hr))
  {
    throw runtime_error("shader compile failed.");
  }
}
