#include "jpeg_decoder.hpp"

#ifdef _WIN32

#include <wincodec.h>
#include <objbase.h>

#include <stdexcept>
#include <string>

namespace bag2mkv {
namespace {

void check(HRESULT hr, const char *what) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string("JPEG decoder: ") + what + " failed (HRESULT 0x" +
                                 std::to_string(static_cast<unsigned long>(hr)) + ")");
    }
}

}  // namespace

JpegDecoder::JpegDecoder() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        com_initialized_ = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        check(hr, "CoInitializeEx");
    }
    IWICImagingFactory *factory = nullptr;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
          "CoCreateInstance(WICImagingFactory)");
    factory_ = factory;
}

JpegDecoder::~JpegDecoder() {
    if (factory_ != nullptr) {
        static_cast<IWICImagingFactory *>(factory_)->Release();
        factory_ = nullptr;
    }
    if (com_initialized_) {
        CoUninitialize();
    }
}

void JpegDecoder::decode(const uint8_t *jpeg, size_t size, std::vector<uint8_t> &bgra, uint32_t &width,
                         uint32_t &height) {
    if (jpeg == nullptr || size == 0) {
        throw std::runtime_error("JPEG decoder: empty input");
    }
    auto *factory = static_cast<IWICImagingFactory *>(factory_);
    IWICStream *stream = nullptr;
    IWICBitmapDecoder *decoder = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *converter = nullptr;

    try {
        check(factory->CreateStream(&stream), "CreateStream");
        check(stream->InitializeFromMemory(const_cast<BYTE *>(jpeg), static_cast<DWORD>(size)),
              "InitializeFromMemory");
        check(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder),
              "CreateDecoderFromStream");
        check(decoder->GetFrame(0, &frame), "GetFrame");
        UINT w = 0;
        UINT h = 0;
        check(frame->GetSize(&w, &h), "GetSize");
        if (w == 0 || h == 0 || w > 16384 || h > 16384) {
            throw std::runtime_error("JPEG decoder: invalid dimensions");
        }
        check(factory->CreateFormatConverter(&converter), "CreateFormatConverter");
        check(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeCustom),
              "InitializeFormatConverter");
        width = w;
        height = h;
        bgra.resize(static_cast<size_t>(w) * h * 4);
        check(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(bgra.size()), bgra.data()), "CopyPixels");
    } catch (...) {
        if (converter) converter->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (stream) stream->Release();
        throw;
    }
    converter->Release();
    frame->Release();
    decoder->Release();
    stream->Release();
}

}  // namespace bag2mkv

#else

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace bag2mkv {

JpegDecoder::JpegDecoder() = default;
JpegDecoder::~JpegDecoder() = default;

void JpegDecoder::decode(const uint8_t *jpeg, size_t size, std::vector<uint8_t> &bgra, uint32_t &width,
                         uint32_t &height) {
    if (jpeg == nullptr || size == 0) {
        throw std::runtime_error("JPEG decoder: empty input");
    }
    if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("JPEG decoder: input is too large");
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char *rgb = stbi_load_from_memory(jpeg, static_cast<int>(size), &w, &h, &channels, 3);
    if (rgb == nullptr) {
        throw std::runtime_error(std::string("JPEG decoder: ") + stbi_failure_reason());
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        stbi_image_free(rgb);
        throw std::runtime_error("JPEG decoder: invalid dimensions");
    }
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    bgra.resize(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        bgra[i * 4 + 0] = rgb[i * 3 + 2];
        bgra[i * 4 + 1] = rgb[i * 3 + 1];
        bgra[i * 4 + 2] = rgb[i * 3 + 0];
        bgra[i * 4 + 3] = 255;
    }
    stbi_image_free(rgb);
}

}  // namespace bag2mkv

#endif
