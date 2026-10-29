// Test helper: writes a small lossless scRGB JPEG XR (64bppRGBAHalf) with known values
// and a JSON list of those pixels (as stored in half precision).
// Usage: make_test_jxr out.jxr out.json
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <DirectXPackedVector.h>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
using DirectX::PackedVector::HALF;
using DirectX::PackedVector::XMConvertFloatToHalf;
using DirectX::PackedVector::XMConvertHalfToFloat;

int wmain(int argc, wchar_t** argv) {
  if (argc != 3) return 2;
  const UINT w = 16, h = 4;
  std::vector<HALF> px(w * h * 4);
  auto set = [&](UINT x, UINT y, float r, float g, float b) {
    HALF* p = &px[(y * w + x) * 4];
    p[0] = XMConvertFloatToHalf(r);
    p[1] = XMConvertFloatToHalf(g);
    p[2] = XMConvertFloatToHalf(b);
    p[3] = XMConvertFloatToHalf(1.0f);
  };
  for (UINT x = 0; x < w; ++x) {
    float v = 12.5f * x / (w - 1);           // Row 0: grey ramp 0..1000 nits.
    set(x, 0, v, v, v);
    float k = 0.25f + 0.75f * x;             // Row 1: primaries.
    set(x, 1, x % 3 == 0 ? k : 0, x % 3 == 1 ? k : 0, x % 3 == 2 ? k : 0);
    set(x, 2, -0.05f * (x + 1), 1.0f + 0.5f * x, 0.3f - 0.1f * x);  // Row 2: out-of-gamut negatives.
    set(x, 3, 10.0f * (x + 1) * (x + 1), 0.5f, 125.0f);             // Row 3: up to 32000 nits, clamped.
  }

  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IWICImagingFactory> f;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
  ComPtr<IWICStream> s;
  if (SUCCEEDED(hr)) hr = f->CreateStream(&s);
  if (SUCCEEDED(hr)) hr = s->InitializeFromFilename(argv[1], GENERIC_WRITE);
  ComPtr<IWICBitmapEncoder> enc;
  if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatWmp, nullptr, &enc);
  if (SUCCEEDED(hr)) hr = enc->Initialize(s.Get(), WICBitmapEncoderNoCache);
  ComPtr<IWICBitmapFrameEncode> fr;
  ComPtr<IPropertyBag2> bag;
  if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, &bag);
  if (SUCCEEDED(hr)) {
    PROPBAG2 opt = {};
    opt.pstrName = const_cast<wchar_t*>(L"Lossless");
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    hr = bag->Write(1, &opt, &v);
  }
  if (SUCCEEDED(hr)) hr = fr->Initialize(bag.Get());
  if (SUCCEEDED(hr)) hr = fr->SetSize(w, h);
  WICPixelFormatGUID fmt = GUID_WICPixelFormat64bppRGBAHalf;
  if (SUCCEEDED(hr)) hr = fr->SetPixelFormat(&fmt);
  if (SUCCEEDED(hr) && !IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBAHalf)) hr = E_FAIL;
  if (SUCCEEDED(hr)) hr = fr->WritePixels(h, w * 8, UINT(px.size() * 2), reinterpret_cast<BYTE*>(px.data()));
  if (SUCCEEDED(hr)) hr = fr->Commit();
  if (SUCCEEDED(hr)) hr = enc->Commit();
  if (FAILED(hr)) {
    wprintf(L"failed 0x%08X\n", unsigned(hr));
    return 1;
  }

  FILE* j = _wfopen(argv[2], L"w");
  fprintf(j, "[\n");
  for (UINT y = 0; y < h; ++y)
    for (UINT x = 0; x < w; ++x) {
      HALF* p = &px[(y * w + x) * 4];
      fprintf(j, "  {\"x\": %u, \"y\": %u, \"rgb\": [%.9g, %.9g, %.9g]}%s\n", x, y, XMConvertHalfToFloat(p[0]),
              XMConvertHalfToFloat(p[1]), XMConvertHalfToFloat(p[2]), (y == h - 1 && x == w - 1) ? "" : ",");
    }
  fprintf(j, "]\n");
  fclose(j);
  wprintf(L"wrote %ls\n", argv[1]);
  return 0;
}
