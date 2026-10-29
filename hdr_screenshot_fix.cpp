// hdr_screenshot_fix: makes HDR screenshots display correctly in every viewer.
//
// JPEG XR input (scRGB, as written by the NVIDIA app and Game Bar) is converted to a
// 16-bit Rec. 2100 PQ PNG. PNG input that carries cICP 9/16/0/1 is rewritten with the
// PQ ICC profile from rec2100pq_icc.h. By default the output has no cICP chunk, because
// some viewers (Discord) mis-render PNGs that carry one; --keep-cicp keeps or adds it.
//
// Only the Windows SDK is used (WIC for image decoding and PNG encoding). CRC-32,
// Adler-32 and zlib (stored blocks) are implemented here.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "rec2100pq_icc.h"

using Microsoft::WRL::ComPtr;
using Bytes = std::vector<uint8_t>;

namespace {

// ---------------------------------------------------------------------------
// Byte helpers
// ---------------------------------------------------------------------------

void putBe32(Bytes& b, uint32_t v) {
  b.push_back(uint8_t(v >> 24));
  b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}

uint32_t getBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// ---------------------------------------------------------------------------
// CRC-32 (PNG), Adler-32 and zlib with stored deflate blocks
// ---------------------------------------------------------------------------

uint32_t crcTable[256];

void initCrc() {
  for (uint32_t n = 0; n < 256; ++n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTable[n] = c;
  }
}

uint32_t crc32Update(uint32_t crc, const uint8_t* p, size_t n) {
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = crcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

uint32_t adler32(const Bytes& d) {
  uint32_t a = 1, b = 0;
  size_t i = 0;
  while (i < d.size()) {
    size_t n = std::min<size_t>(d.size() - i, 5552);
    for (size_t k = 0; k < n; ++k) {
      a += d[i + k];
      b += a;
    }
    a %= 65521;
    b %= 65521;
    i += n;
  }
  return (b << 16) | a;
}

Bytes zlibStored(const Bytes& d) {
  Bytes z;
  z.push_back(0x78);  // CM 8 (deflate), CINFO 7 (32K window).
  z.push_back(0x01);  // FLEVEL 0, FCHECK so that the header is a multiple of 31.
  size_t i = 0;
  do {
    size_t n = std::min<size_t>(d.size() - i, 65535);
    bool last = i + n == d.size();
    z.push_back(last ? 1 : 0);  // BFINAL, BTYPE 00 (stored).
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), d.begin() + i, d.begin() + i + n);
    i += n;
  } while (i < d.size());
  putBe32(z, adler32(d));
  return z;
}

// ---------------------------------------------------------------------------
// Colour maths
// ---------------------------------------------------------------------------

// SMPTE ST 2084 constants.
const double kPqM1 = 2610.0 / 16384.0;
const double kPqM2 = 2523.0 / 4096.0 * 128.0;
const double kPqC1 = 3424.0 / 4096.0;
const double kPqC2 = 2413.0 / 4096.0 * 32.0;
const double kPqC3 = 2392.0 / 4096.0 * 32.0;

// Inverse EOTF: linear Y in [0, 1] (1 = 10000 cd/m2) to the non-linear signal in [0, 1].
double pqInverseEotf(double y) {
  double p = std::pow(std::max(y, 0.0), kPqM1);
  return std::pow((kPqC1 + kPqC2 * p) / (1.0 + kPqC3 * p), kPqM2);
}

// Rec. 709 to Rec. 2020 primaries for linear light (ITU-R BT.2087).
const float k709To2020[3][3] = {
    {0.627403895934699f, 0.329283038377884f, 0.043313065687417f},
    {0.069097289358232f, 0.919540395075459f, 0.011362315566309f},
    {0.016391438875150f, 0.088013307877226f, 0.895595253247624f}};

// ---------------------------------------------------------------------------
// PNG chunks
// ---------------------------------------------------------------------------

const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

struct Chunk {
  char type[5] = {};
  Bytes data;
  bool is(const char* t) const { return std::memcmp(type, t, 4) == 0; }
};

Chunk makeChunk(const char* type, Bytes data) {
  Chunk c;
  std::memcpy(c.type, type, 4);
  c.data = std::move(data);
  return c;
}

bool parsePng(const Bytes& buf, std::vector<Chunk>& out, std::wstring& err) {
  if (buf.size() < 8 || std::memcmp(buf.data(), kPngSig, 8) != 0) {
    err = L"not a PNG file";
    return false;
  }
  size_t pos = 8;
  bool sawEnd = false;
  while (pos < buf.size()) {
    if (sawEnd) {
      err = L"data after IEND";
      return false;
    }
    if (buf.size() - pos < 12) {
      err = L"truncated chunk";
      return false;
    }
    uint32_t len = getBe32(&buf[pos]);
    if (len > 0x7FFFFFFFu || buf.size() - pos - 12 < len) {
      err = L"truncated or oversized chunk";
      return false;
    }
    Chunk c;
    std::memcpy(c.type, &buf[pos + 4], 4);
    for (int i = 0; i < 4; ++i) {
      char ch = c.type[i];
      if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))) {
        err = L"invalid chunk type";
        return false;
      }
    }
    uint32_t want = getBe32(&buf[pos + 8 + len]);
    uint32_t got = crc32Update(0, &buf[pos + 4], size_t(len) + 4);
    if (want != got) {
      wchar_t msg[96];
      swprintf_s(msg, L"CRC mismatch in %hs chunk at offset %zu", c.type, pos);
      err = msg;
      return false;
    }
    c.data.assign(buf.begin() + pos + 8, buf.begin() + pos + 8 + len);
    if (out.empty() && (!c.is("IHDR") || len != 13)) {
      err = L"first chunk is not a valid IHDR";
      return false;
    }
    if (c.is("IEND")) sawEnd = true;
    out.push_back(std::move(c));
    pos += 12 + size_t(len);
  }
  if (!sawEnd) {
    err = L"missing IEND";
    return false;
  }
  return true;
}

Bytes serializePng(const std::vector<Chunk>& chunks) {
  Bytes out(kPngSig, kPngSig + 8);
  for (const Chunk& c : chunks) {
    putBe32(out, uint32_t(c.data.size()));
    size_t typeAt = out.size();
    out.insert(out.end(), c.type, c.type + 4);
    out.insert(out.end(), c.data.begin(), c.data.end());
    putBe32(out, crc32Update(0, &out[typeAt], c.data.size() + 4));
  }
  return out;
}

Chunk iccpChunk(const Bytes& profile) {
  Bytes d;
  const char* name = "RGB_D65_202_Rel_PeQ";  // The profile's own desc.
  d.insert(d.end(), name, name + std::strlen(name));
  d.push_back(0);  // Name terminator.
  d.push_back(0);  // Compression method 0 (zlib deflate).
  Bytes z = zlibStored(profile);
  d.insert(d.end(), z.begin(), z.end());
  return makeChunk("iCCP", std::move(d));
}

Chunk cicpChunk() {
  return makeChunk("cICP", Bytes{9, 16, 0, 1});
}

bool isColourChunk(const Chunk& c) {
  return c.is("iCCP") || c.is("sRGB") || c.is("gAMA") || c.is("cHRM");
}

const size_t kIdatChunkMax = 65536;

bool hasIdatLargerThan(const std::vector<Chunk>& chunks, size_t limit) {
  for (const Chunk& c : chunks)
    if (c.is("IDAT") && c.data.size() > limit) return true;
  return false;
}

// Rewrites all IDAT chunks as one run of chunks of at most kIdatChunkMax bytes, at the
// position of the first IDAT. The concatenated zlib stream is unchanged.
void rechunkIdat(std::vector<Chunk>& chunks) {
  Bytes stream;
  std::vector<Chunk> result;
  size_t firstIdat = SIZE_MAX;
  for (Chunk& c : chunks) {
    if (c.is("IDAT")) {
      if (firstIdat == SIZE_MAX) firstIdat = result.size();
      stream.insert(stream.end(), c.data.begin(), c.data.end());
    } else {
      result.push_back(std::move(c));
    }
  }
  if (firstIdat == SIZE_MAX) {
    chunks = std::move(result);
    return;
  }
  std::vector<Chunk> idats;
  for (size_t i = 0; i < stream.size(); i += kIdatChunkMax) {
    size_t n = std::min(kIdatChunkMax, stream.size() - i);
    idats.push_back(makeChunk("IDAT", Bytes(stream.begin() + i, stream.begin() + i + n)));
  }
  result.insert(result.begin() + firstIdat, std::make_move_iterator(idats.begin()),
                std::make_move_iterator(idats.end()));
  chunks = std::move(result);
}

Bytes be32Payload(std::initializer_list<uint32_t> values) {
  Bytes b;
  for (uint32_t v : values) putBe32(b, v);
  return b;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

std::wstring winError(DWORD code) {
  wchar_t* msg = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
  std::wstring s = msg ? msg : L"unknown error";
  if (msg) LocalFree(msg);
  while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
  return s;
}

std::wstring hrError(const wchar_t* what, HRESULT hr) {
  wchar_t buf[64];
  swprintf_s(buf, L" failed (HRESULT 0x%08X)", unsigned(hr));
  return std::wstring(what) + buf;
}

bool readFile(const std::wstring& path, Bytes& out, std::wstring& err) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    err = L"cannot open: " + winError(GetLastError());
    return false;
  }
  LARGE_INTEGER size;
  if (!GetFileSizeEx(h, &size) || size.QuadPart > 0x7FFFFFFF) {
    CloseHandle(h);
    err = L"cannot read file size, or file larger than 2 GB";
    return false;
  }
  out.resize(size_t(size.QuadPart));
  size_t done = 0;
  while (done < out.size()) {
    DWORD got = 0;
    DWORD want = DWORD(std::min<size_t>(out.size() - done, 1u << 30));
    if (!ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) {
      err = L"read failed: " + winError(GetLastError());
      CloseHandle(h);
      return false;
    }
    done += got;
  }
  CloseHandle(h);
  return true;
}

// Creates a new file and writes data. Fails with ERROR_FILE_EXISTS rather than
// overwriting anything.
DWORD writeNewFile(const std::wstring& path, const Bytes& data) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return GetLastError();
  size_t done = 0;
  while (done < data.size()) {
    DWORD put = 0;
    DWORD want = DWORD(std::min<size_t>(data.size() - done, 1u << 30));
    if (!WriteFile(h, data.data() + done, want, &put, nullptr)) {
      DWORD e = GetLastError();
      CloseHandle(h);
      DeleteFileW(path.c_str());  // Only the partial file this call created.
      return e;
    }
    done += put;
  }
  if (!CloseHandle(h)) return GetLastError();
  return ERROR_SUCCESS;
}

// Writes data to the first free name in `candidates` followed by numbered variants of
// the last candidate. Never overwrites an existing file.
bool writeUnique(const std::wstring& base, const std::vector<std::wstring>& suffixes, const Bytes& data,
                 std::wstring& written, std::wstring& err) {
  std::vector<std::wstring> names;
  for (const auto& s : suffixes) names.push_back(base + s);
  const std::wstring& last = suffixes.back();  // e.g. ".hdr.png" -> ".hdr.2.png" ...
  std::wstring stem = last.substr(0, last.size() - 4);
  for (int i = 2; i <= 99; ++i) names.push_back(base + stem + L"." + std::to_wstring(i) + L".png");
  for (const auto& n : names) {
    DWORD e = writeNewFile(n, data);
    if (e == ERROR_SUCCESS) {
      written = n;
      return true;
    }
    if (e != ERROR_FILE_EXISTS && e != ERROR_ALREADY_EXISTS) {
      err = L"cannot write " + n + L": " + winError(e);
      return false;
    }
  }
  err = L"no free output name (tried up to " + names.back() + L")";
  return false;
}

std::wstring stripExtension(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return path;
  return path.substr(0, dot);
}

std::wstring fileName(const std::wstring& path) {
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

const Bytes g_profile(kRec2100PqIcc, kRec2100PqIcc + sizeof(kRec2100PqIcc));
bool g_keepCicp = false;

// PNG with cICP 9/16/0/1: put the ICC profile after IHDR in place of any iCCP or sRGB,
// drop cICP unless --keep-cicp, and keep every other chunk byte for byte.
bool convertPng(const std::wstring& in, const Bytes& file, std::wstring& out, std::wstring& err, bool& skipped) {
  std::vector<Chunk> chunks;
  if (!parsePng(file, chunks, err)) return false;

  const Chunk* cicp = nullptr;
  for (const Chunk& c : chunks)
    if (c.is("cICP")) cicp = &c;
  if (!cicp) {
    err = L"no cICP chunk (SDR PNG, left untouched)";
    skipped = true;
    return false;
  }
  if (cicp->data.size() != 4 || cicp->data[0] != 9 || cicp->data[1] != 16 || cicp->data[2] != 0 ||
      cicp->data[3] != 1) {
    wchar_t msg[128];
    if (cicp->data.size() == 4)
      swprintf_s(msg, L"cICP is %u/%u/%u/%u, not 9/16/0/1 (BT.2020 PQ full range)", cicp->data[0],
                 cicp->data[1], cicp->data[2], cicp->data[3]);
    else
      swprintf_s(msg, L"malformed cICP chunk");
    err = msg;
    skipped = true;
    return false;
  }

  std::vector<Chunk> result;
  result.reserve(chunks.size() + 1);
  for (Chunk& c : chunks) {
    // gAMA and cHRM stay: ICC-aware decoders ignore them when iCCP is present.
    if (c.is("iCCP") || c.is("sRGB") || (c.is("cICP") && !g_keepCicp)) continue;
    result.push_back(std::move(c));
    if (result.size() == 1) result.push_back(iccpChunk(g_profile));  // Right after IHDR.
  }
  // Pixel data is left alone unless a single IDAT is large enough to trip some decoders.
  if (hasIdatLargerThan(result, 1u << 20)) rechunkIdat(result);
  Bytes png = serializePng(result);
  return writeUnique(stripExtension(in), {L".icc.png"}, png, out, err);
}

bool isFloatHdrFormat(const GUID& f) {
  const GUID* known[] = {&GUID_WICPixelFormat64bppRGBAHalf,  &GUID_WICPixelFormat64bppRGBHalf,
                         &GUID_WICPixelFormat48bppRGBHalf,   &GUID_WICPixelFormat128bppRGBAFloat,
                         &GUID_WICPixelFormat128bppRGBFloat, &GUID_WICPixelFormat96bppRGBFloat,
                         &GUID_WICPixelFormat64bppRGBAFixedPoint, &GUID_WICPixelFormat64bppRGBFixedPoint,
                         &GUID_WICPixelFormat48bppRGBFixedPoint,  &GUID_WICPixelFormat128bppRGBAFixedPoint,
                         &GUID_WICPixelFormat128bppRGBFixedPoint, &GUID_WICPixelFormat32bppRGBE};
  for (const GUID* g : known)
    if (IsEqualGUID(f, *g)) return true;
  return false;
}

// JPEG XR in scRGB: convert to a 16-bit BT.2020 PQ PNG with the ICC profile and cLLi
// (plus cICP with --keep-cicp).
bool convertJxr(IWICImagingFactory* factory, const std::wstring& in, std::wstring& out, std::wstring& err,
                std::wstring& note) {
  HRESULT hr;
  ComPtr<IWICBitmapDecoder> decoder;
  hr = factory->CreateDecoderFromFilename(in.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                          &decoder);
  if (FAILED(hr)) return err = hrError(L"WIC decode", hr), false;
  ComPtr<IWICBitmapFrameDecode> frame;
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) return err = hrError(L"WIC GetFrame", hr), false;

  UINT w = 0, h = 0;
  frame->GetSize(&w, &h);
  if (w == 0 || h == 0) return err = L"empty image", false;
  if (uint64_t(w) * 16 > 0xFFFFFFFFu) return err = L"image too wide", false;

  WICPixelFormatGUID srcFormat;
  frame->GetPixelFormat(&srcFormat);
  if (!isFloatHdrFormat(srcFormat))
    note = L"warning: source is not a float/half pixel format, so it holds no HDR range";

  ComPtr<IWICFormatConverter> conv;
  hr = factory->CreateFormatConverter(&conv);
  if (FAILED(hr)) return err = hrError(L"CreateFormatConverter", hr), false;
  hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat128bppRGBAFloat, WICBitmapDitherTypeNone, nullptr, 0.0,
                        WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) return err = hrError(L"conversion to 128bppRGBAFloat", hr), false;

  // Decode in strips, convert to BT.2020 PQ, quantise to 16 bits. Content light level per
  // CTA-861.3, in nits of the encoded (clamped BT.2020) signal: MaxCLL is the largest
  // max(R,G,B) of any pixel, MaxFALL the frame average of max(R,G,B).
  std::vector<uint16_t> pixels(size_t(w) * h * 3);
  double maxCll = 0.0, sumMax = 0.0;
  const UINT kStrip = 64;
  std::vector<float> strip(size_t(w) * 4 * kStrip);
  for (UINT y0 = 0; y0 < h; y0 += kStrip) {
    UINT rows = std::min(kStrip, h - y0);
    WICRect rc = {0, INT(y0), INT(w), INT(rows)};
    hr = conv->CopyPixels(&rc, w * 16, w * 16 * rows, reinterpret_cast<BYTE*>(strip.data()));
    if (FAILED(hr)) return err = hrError(L"CopyPixels", hr), false;
    for (size_t i = 0; i < size_t(w) * rows; ++i) {
      const float* s = &strip[i * 4];
      float rgb[3];
      for (int c = 0; c < 3; ++c) rgb[c] = std::isfinite(s[c]) ? s[c] : 0.0f;
      uint16_t* d = &pixels[(size_t(y0) * w + i) * 3];
      double pixelMax = 0.0;
      for (int c = 0; c < 3; ++c) {
        float v = k709To2020[c][0] * rgb[0] + k709To2020[c][1] * rgb[1] + k709To2020[c][2] * rgb[2];
        double nits = std::min(std::max(double(v), 0.0) * 80.0, 10000.0);
        pixelMax = std::max(pixelMax, nits);
        d[c] = uint16_t(std::lround(pqInverseEotf(nits / 10000.0) * 65535.0));
      }
      maxCll = std::max(maxCll, pixelMax);
      sumMax += pixelMax;
    }
  }
  const double maxFall = sumMax / (double(w) * double(h));
  // Units of 0.0001 cd/m2.
  const Bytes clli =
      be32Payload({uint32_t(std::lround(maxCll * 10000.0)), uint32_t(std::lround(maxFall * 10000.0))});
  wchar_t lightNote[96];
  swprintf_s(lightNote, L"MaxCLL %.1f nits, MaxFALL %.1f nits", maxCll, maxFall);
  note = note.empty() ? lightNote : note + L"; " + lightNote;

  // Encode a 48bppRGB PNG into memory.
  ComPtr<IStream> stream;
  hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
  if (FAILED(hr)) return err = hrError(L"CreateStreamOnHGlobal", hr), false;
  ComPtr<IWICBitmapEncoder> encoder;
  hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  if (FAILED(hr)) return err = hrError(L"create PNG encoder", hr), false;
  hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
  if (FAILED(hr)) return err = hrError(L"PNG encoder Initialize", hr), false;
  ComPtr<IWICBitmapFrameEncode> outFrame;
  ComPtr<IPropertyBag2> props;
  hr = encoder->CreateNewFrame(&outFrame, &props);
  if (FAILED(hr)) return err = hrError(L"CreateNewFrame", hr), false;
  hr = outFrame->Initialize(props.Get());
  if (FAILED(hr)) return err = hrError(L"frame Initialize", hr), false;
  hr = outFrame->SetSize(w, h);
  if (FAILED(hr)) return err = hrError(L"SetSize", hr), false;
  WICPixelFormatGUID fmt = GUID_WICPixelFormat48bppRGB;
  hr = outFrame->SetPixelFormat(&fmt);
  if (FAILED(hr) || !IsEqualGUID(fmt, GUID_WICPixelFormat48bppRGB))
    return err = L"PNG encoder does not accept 48bppRGB", false;
  const UINT stride = w * 6;
  for (UINT y0 = 0; y0 < h; y0 += kStrip) {
    UINT rows = std::min(kStrip, h - y0);
    hr = outFrame->WritePixels(rows, stride, stride * rows,
                               reinterpret_cast<BYTE*>(&pixels[size_t(y0) * w * 3]));
    if (FAILED(hr)) return err = hrError(L"WritePixels", hr), false;
  }
  hr = outFrame->Commit();
  if (FAILED(hr)) return err = hrError(L"frame Commit", hr), false;
  hr = encoder->Commit();
  if (FAILED(hr)) return err = hrError(L"encoder Commit", hr), false;

  STATSTG st = {};
  hr = stream->Stat(&st, STATFLAG_NONAME);
  if (FAILED(hr)) return err = hrError(L"stream Stat", hr), false;
  HGLOBAL hg = nullptr;
  hr = GetHGlobalFromStream(stream.Get(), &hg);
  if (FAILED(hr)) return err = hrError(L"GetHGlobalFromStream", hr), false;
  Bytes encoded(size_t(st.cbSize.QuadPart));
  void* mem = GlobalLock(hg);
  if (!mem) return err = L"GlobalLock failed", false;
  std::memcpy(encoded.data(), mem, encoded.size());
  GlobalUnlock(hg);

  // Replace WIC's ancillary colour chunks. After IHDR: iCCP, cICP (only with --keep-cicp),
  // cLLi, sBIT (16 bits), cHRM (BT.2020 primaries, D65 white). No gAMA: the data is PQ, not
  // a power curve.
  std::vector<Chunk> chunks;
  std::wstring perr;
  if (!parsePng(encoded, chunks, perr)) return err = L"WIC produced an invalid PNG: " + perr, false;
  std::vector<Chunk> result;
  for (Chunk& c : chunks) {
    if (isColourChunk(c) || c.is("cICP") || c.is("cLLi") || c.is("sBIT")) continue;
    result.push_back(std::move(c));
    if (result.size() == 1) {
      result.push_back(iccpChunk(g_profile));
      if (g_keepCicp) result.push_back(cicpChunk());
      result.push_back(makeChunk("cLLi", clli));
      result.push_back(makeChunk("sBIT", Bytes{16, 16, 16}));
      result.push_back(
          makeChunk("cHRM", be32Payload({31270, 32900, 70800, 29200, 17000, 79700, 13100, 4600})));
    }
  }
  rechunkIdat(result);
  Bytes png = serializePng(result);
  return writeUnique(stripExtension(in), {L".png", L".hdr.png"}, png, out, err);
}

bool isJxr(const Bytes& b) {
  return b.size() >= 4 && b[0] == 'I' && b[1] == 'I' && b[2] == 0xBC && b[3] <= 1;
}

void printUsage() {
  wprintf(
      L"hdr_screenshot_fix - make HDR screenshots display correctly in every viewer\n"
      L"\n"
      L"Usage: hdr_screenshot_fix [--no-pause] [--keep-cicp] [--dump-icc out.icc] file...\n"
      L"       (or drag files onto the exe)\n"
      L"\n"
      L"  .jxr  scRGB JPEG XR (NVIDIA app / Game Bar) -> <name>.png, a 16-bit BT.2020 PQ PNG\n"
      L"        with a PQ ICC profile and cLLi. If <name>.png exists, <name>.hdr.png is used.\n"
      L"  .png  PNG with cICP 9/16/0/1 -> <name>.icc.png with the PQ ICC profile and the cICP\n"
      L"        chunk removed; pixel data is copied unchanged. SDR PNGs are skipped.\n"
      L"\n"
      L"  --no-pause        do not wait for Enter at the end\n"
      L"  --keep-cicp       keep (PNG) or add (JXR) cICP 9/16/0/1 for viewers that use it;\n"
      L"                    some viewers, such as Discord, show such PNGs wrongly\n"
      L"  --dump-icc FILE   write the embedded ICC profile to FILE (must not exist)\n"
      L"\n"
      L"Existing files are never overwritten. The ICC profile is a libjxl-generated PQ\n"
      L"profile (desc RGB_D65_202_Rel_PeQ, copyright tag CC0), included unmodified.\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  _setmode(_fileno(stdout), _O_U8TEXT);
  initCrc();

  bool pause = true;
  std::wstring dumpIcc;
  std::vector<std::wstring> files;
  for (int i = 1; i < argc; ++i) {
    std::wstring a = argv[i];
    if (a == L"--no-pause") {
      pause = false;
    } else if (a == L"--keep-cicp") {
      g_keepCicp = true;
    } else if (a == L"--dump-icc") {
      if (i + 1 >= argc) {
        wprintf(L"--dump-icc needs a file name\n");
        return 2;
      }
      dumpIcc = argv[++i];
    } else if (a == L"--help" || a == L"-h" || a == L"/?") {
      printUsage();
      return 0;
    } else {
      files.push_back(a);
    }
  }

  int exitCode = 0;

  if (!dumpIcc.empty()) {
    DWORD e = writeNewFile(dumpIcc, g_profile);
    if (e == ERROR_SUCCESS) {
      wprintf(L"ICC profile written to %ls (%zu bytes)\n", dumpIcc.c_str(), g_profile.size());
    } else {
      wprintf(L"cannot write %ls: %ls\n", dumpIcc.c_str(), winError(e).c_str());
      exitCode = 1;
    }
  }

  if (files.empty()) {
    if (dumpIcc.empty()) {
      printUsage();
      if (pause) {
        wprintf(L"\nPress Enter to close.");
        getchar();
      }
    }
    return exitCode;
  }

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ComPtr<IWICImagingFactory> factory;
  if (SUCCEEDED(hr))
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    wprintf(L"cannot start WIC (HRESULT 0x%08X)\n", unsigned(hr));
    return 3;
  }

  int converted = 0, failed = 0;
  for (const std::wstring& in : files) {
    std::wstring out, err, note;
    bool ok = false, skipped = false;
    try {
      Bytes file;
      if (!readFile(in, file, err)) {
        ok = false;
      } else if (file.size() >= 8 && std::memcmp(file.data(), kPngSig, 8) == 0) {
        ok = convertPng(in, file, out, err, skipped);
      } else if (isJxr(file)) {
        file.clear();
        file.shrink_to_fit();
        ok = convertJxr(factory.Get(), in, out, err, note);
      } else {
        err = L"not a PNG or JPEG XR file";
      }
    } catch (const std::bad_alloc&) {
      err = L"out of memory";
    } catch (const std::exception& e) {
      wchar_t buf[256];
      swprintf_s(buf, L"error: %hs", e.what());
      err = buf;
    }
    if (ok) {
      ++converted;
      wprintf(L"OK    %ls -> %ls\n", fileName(in).c_str(), fileName(out).c_str());
      if (!note.empty()) wprintf(L"      %ls\n", note.c_str());
    } else {
      ++failed;
      wprintf(L"%ls  %ls: %ls\n", skipped ? L"SKIP" : L"FAIL", fileName(in).c_str(), err.c_str());
    }
  }

  wprintf(L"\n%d converted, %d failed\n", converted, failed);
  factory.Reset();
  CoUninitialize();

  if (pause) {
    wprintf(L"Press Enter to close.");
    getchar();
  }
  return (failed || exitCode) ? 1 : 0;
}
