#include "pch.h"
#include "resource.h"
#include "IconResources.h"

#include <algorithm>
#include <array>

#include <gdiplus.h>
#include "OwSvgIcons.h"

#include <map>
#include <string_view>
#include <tuple>

#include <d3d11.h>
#include <d2d1_3.h>
#include <shlwapi.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shlwapi.lib")

////////////////////////////////////////////////////////////////////////////////

namespace
{
    std::unique_ptr<Gdiplus::Bitmap> LoadPngFromResource(UINT resourceId) {
        AFX_MANAGE_STATE(AfxGetStaticModuleState())

            HINSTANCE hResInst = AfxGetResourceHandle();
        if (hResInst == nullptr)
            return nullptr;

        HRSRC hResInfo = ::FindResource(hResInst, MAKEINTRESOURCE(resourceId), _T("PNG"));
        if (hResInfo == nullptr)
            return nullptr;

        const DWORD resourceSize = ::SizeofResource(hResInst, hResInfo);
        if (resourceSize == 0)
            return nullptr;

        HGLOBAL hResData = ::LoadResource(hResInst, hResInfo);
        if (hResData == nullptr)
            return nullptr;

        const void* pResData = ::LockResource(hResData);
        if (pResData == nullptr)
            return nullptr;

        HGLOBAL hBuffer = ::GlobalAlloc(GMEM_MOVEABLE, resourceSize);
        if (hBuffer == nullptr)
            return nullptr;

        void* pBuffer = ::GlobalLock(hBuffer);
        if (pBuffer == nullptr)
        {
            ::GlobalFree(hBuffer);
            return nullptr;
        }

        ::CopyMemory(pBuffer, pResData, resourceSize);
        ::GlobalUnlock(hBuffer);

        IStream* pStream = nullptr;
        const HRESULT hr = ::CreateStreamOnHGlobal(hBuffer, TRUE, &pStream);
        if (FAILED(hr) || pStream == nullptr)
        {
            ::GlobalFree(hBuffer);
            return nullptr;
        }

        std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromStream(pStream, FALSE));
        pStream->Release();

        if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
            bitmap.reset();
        }

        return bitmap;
    }

    struct SubSheetMeta
    {
        int cell_size{ 0 };
        int offset_x{ 0 };
        int offset_y{ 0 };
    };

    using SubShteetMetaList = std::vector<SubSheetMeta>;

    // Rasterizes SVG documents with Direct2D (ID2D1DeviceContext5, Windows 10 1703+) into premultiplied 32 bit bitmaps.
    class SvgRasterizer
    {
    public:
        static SvgRasterizer& Instance() {
            static SvgRasterizer instance;
            return instance;
        }

        // Returns a top-down 32 bit DIB section (premultiplied alpha) of size x size filled with the SVG drawn
        // in the given colour (the "color" of the root element, i.e. what "currentColor" means in the SVG).
        // The caller owns the bitmap. Returns nullptr if the system cannot render SVG.
        HBITMAP Render(std::string_view svg, int size, COLORREF color) {
            using Microsoft::WRL::ComPtr;

            if (size <= 0 || !Init()) {
                return nullptr;
            }

            ComPtr<IStream> stream;
            stream.Attach(::SHCreateMemStream(reinterpret_cast<const BYTE*>(svg.data()),
                                              static_cast<UINT>(svg.size())));
            if (!stream) {
                return nullptr;
            }

            ComPtr<ID2D1SvgDocument> doc;
            if (FAILED(context5_->CreateSvgDocument(
                    stream.Get(), D2D1::SizeF(static_cast<float>(size), static_cast<float>(size)), &doc))) {
                return nullptr;
            }

            ComPtr<ID2D1SvgElement> root;
            doc->GetRoot(&root);
            if (root) {
                wchar_t color_text[16]{};
                swprintf_s(color_text, L"#%02x%02x%02x", GetRValue(color), GetGValue(color), GetBValue(color));
                root->SetAttributeValue(L"color", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, color_text);

                // scale the drawing (viewBox) to the requested size
                wchar_t size_text[16]{};
                swprintf_s(size_text, L"%d", size);
                root->SetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, size_text);
                root->SetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_STRING_TYPE_SVG, size_text);
            }

            ComPtr<ID2D1Bitmap1> target;
            if (FAILED(context_->CreateBitmap(
                    D2D1::SizeU(size, size), nullptr, 0,
                    D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                    &target))) {
                return nullptr;
            }

            context_->SetTarget(target.Get());
            context_->BeginDraw();
            context_->Clear(D2D1::ColorF(0, 0.0f));
            context5_->DrawSvgDocument(doc.Get());
            const bool drawn = SUCCEEDED(context_->EndDraw());
            context_->SetTarget(nullptr);
            if (!drawn) {
                return nullptr;
            }

            // copy the result into a CPU readable bitmap
            ComPtr<ID2D1Bitmap1> staging;
            if (FAILED(context_->CreateBitmap(
                    D2D1::SizeU(size, size), nullptr, 0,
                    D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ,
                                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                    &staging)) ||
                FAILED(staging->CopyFromBitmap(nullptr, target.Get(), nullptr))) {
                return nullptr;
            }

            D2D1_MAPPED_RECT mapped{};
            if (FAILED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped))) {
                return nullptr;
            }

            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = size;
            bmi.bmiHeader.biHeight = -size;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            std::uint8_t *dst_bits = nullptr;
            HBITMAP bitmap = ::CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS,
                                                reinterpret_cast<void**>(&dst_bits), nullptr, 0);
            if (bitmap != nullptr && dst_bits != nullptr) {
                for (int y = 0; y < size; ++y) {
                    std::memcpy(dst_bits + static_cast<std::size_t>(y) * size * 4,
                                mapped.bits + static_cast<std::size_t>(y) * mapped.pitch,
                                static_cast<std::size_t>(size) * 4);
                }
            }

            staging->Unmap();
            return bitmap;
        }

    private:
        bool Init() {
            using Microsoft::WRL::ComPtr;

            if (init_tried_) {
                return context5_ != nullptr;
            }
            init_tried_ = true;

            // Direct3D device only to get a DXGI device for Direct2D, hardware first and WARP as the fallback
            ComPtr<ID3D11Device> d3d_device;
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, &d3d_device, nullptr, nullptr);
            if (FAILED(hr)) {
                hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION, &d3d_device, nullptr, nullptr);
            }
            if (FAILED(hr)) {
                return false;
            }

            ComPtr<IDXGIDevice> dxgi_device;
            ComPtr<ID2D1Factory1> factory;
            ComPtr<ID2D1Device> d2d_device;
            if (FAILED(d3d_device.As(&dxgi_device)) ||
                FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, &factory)) ||
                FAILED(factory->CreateDevice(dxgi_device.Get(), &d2d_device)) ||
                FAILED(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_)) ||
                FAILED(context_.As(&context5_))) {
                context_.Reset();
                context5_.Reset();
                return false;
            }

            return true;
        }

        bool init_tried_{ false };
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> context_;
        Microsoft::WRL::ComPtr<ID2D1DeviceContext5> context5_;
    };

    // Monochrome SVG icons (stroke="currentColor"): drawn black on a light background and white on a dark one.
    // Rendered bitmaps are cached by icon index, size and colour.
    class SvgIconSheet : public IconSheet
    {
    public:
        template <std::size_t N>
        SvgIconSheet(const std::array<std::string_view, N> &svgs, const int cols, const int rows)
            : svgs_(svgs.begin(), svgs.end()), cols_(cols), rows_(rows) {}

        ~SvgIconSheet() override {
            for (const auto& [key, bitmap] : cache_) {
                if (bitmap != nullptr) {
                    DeleteObject(bitmap);
                }
            }
        }

        bool Draw(CDC &dc, const CRect &dest_rect, int index) const override {
            return DrawIcon(dc, dest_rect, index, false);
        }

        bool Draw(CDC &dc, const CRect &dest_rect, int index, bool dark_background) const override {
            return DrawIcon(dc, dest_rect, index, dark_background);
        }

        bool Draw(CDC &dc, const CRect &dest_rect, int row, int col) const override {
            if (row < 0 || row >= rows_ || col < 0 || col >= cols_) {
                return false;
            }
            return DrawIcon(dc, dest_rect, row * cols_ + col, false);
        }

        int GetCols() const override {
            return cols_;
        }

        int GetRows() const override {
            return rows_;
        }

        int GetMaxCount() const override {
            return static_cast<int>(svgs_.size());
        }

    private:
        bool DrawIcon(CDC &dc, const CRect &dest_rect, int index, bool dark_background) const {
            if (index < 0 || index >= GetMaxCount() || dest_rect.Width() <= 0 || dest_rect.Height() <= 0) {
                return false;
            }

            const int size = std::min(dest_rect.Width(), dest_rect.Height());
            const auto key = std::make_tuple(index, size, dark_background);

            auto itr = cache_.find(key);
            if (itr == cache_.end()) {
                const COLORREF color = dark_background ? RGB(255, 255, 255) : RGB(0, 0, 0);
                itr = cache_.emplace(key, SvgRasterizer::Instance().Render(svgs_[index], size, color)).first;
            }

            const HBITMAP bitmap = itr->second;
            if (bitmap == nullptr) {
                return false;
            }

            const HDC hdc = ::CreateCompatibleDC(dc.GetSafeHdc());
            if (!hdc) {
                return false;
            }

            const HGDIOBJ old_obj = ::SelectObject(hdc, bitmap);

            constexpr BLENDFUNCTION blend {
                .BlendOp = AC_SRC_OVER,
                .BlendFlags = 0,
                .SourceConstantAlpha = 255,
                .AlphaFormat = AC_SRC_ALPHA
            };

            // the icon is centred in the destination rectangle
            const int x = dest_rect.left + (dest_rect.Width() - size) / 2;
            const int y = dest_rect.top + (dest_rect.Height() - size) / 2;
            const BOOL ok = ::AlphaBlend(dc.GetSafeHdc(), x, y, size, size, hdc, 0, 0, size, size, blend);

            ::SelectObject(hdc, old_obj);
            ::DeleteDC(hdc);

            return ok != FALSE;
        }

        std::vector<std::string_view> svgs_;
        int cols_{ 0 };
        int rows_{ 0 };
        mutable std::map<std::tuple<int, int, bool>, HBITMAP> cache_;
    };
    class CompositedIconSheet : public IconSheet
    {
    public:
        ~CompositedIconSheet() override {
            DeleteObject(hbitmap_);
        }

        bool Load(const UINT resource_id, const int cols, const int rows, const int margin,
                  SubShteetMetaList &&sub_meta_list) {
            if (hbitmap_ != nullptr) {
                return true;
            }

            if (cols <= 0 || rows <= 0 || margin < 0 || sub_meta_list.empty()) {
                TRACE(L"invalid cols/rows/margin/sub_meta_list\n");
                return false;
            }
            
            const auto gdiplus_bitmap = LoadPngFromResource(resource_id);
            if (gdiplus_bitmap == nullptr) {
                TRACE(L"failed to load png resources (id: %u)", resource_id);
                return false;
            }

            const auto image_w = static_cast<int>(gdiplus_bitmap->GetWidth());
            const auto image_h = static_cast<int>(gdiplus_bitmap->GetHeight());

            {
                int img_max_x = 0;
                int img_max_y = 0;
                for (const auto [cell_size, offset_x, offset_y] : sub_meta_list) {
                    auto max_x = offset_x + margin + (cell_size + margin) * cols;
                    auto max_y = offset_y + margin + (cell_size + margin) * rows;

                    img_max_x = std::max(img_max_x, max_x);
                    img_max_y = std::max(img_max_y, max_y);
                }

                if (img_max_x > image_w || img_max_y > image_h) {
                    TRACE(L"sub-sheets (max x*y: %d*%d) exceed the size of the image (%d*%d)",
                          img_max_x, img_max_y, image_w, image_h);
                    return false;
                }
            }

            if (gdiplus_bitmap->GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hbitmap_) != Gdiplus::Ok) {
                TRACE(L"failed to get hbitmap from gdiplus bitmap");
                hbitmap_ = nullptr;
                return false;
            }

            cols_ = cols;
            rows_ = rows;
            margin_ = margin;
            sub_sheets_ = std::move(sub_meta_list);

            return true;
        }

        bool Draw(CDC &dc, const CRect &dest_rect, int index) const override {
            if (hbitmap_ == nullptr || index < 0 || index >= GetMaxCount())
                return false;

            const int row = index / cols_;
            const int col = index % cols_;

            return Draw(dc, dest_rect, row, col);
        }

        bool Draw(CDC &dc, const CRect &dest_rect, int row, int col) const override {
            if (hbitmap_ == nullptr || sub_sheets_.empty()) {
                return false;
            }

            if (dest_rect.Width() <= 0 || dest_rect.Height() <= 0)
                return false;

            if (row < 0 || row >= rows_ || col < 0 || col >= cols_)
                return false;

            const int target_size = std::min(dest_rect.Width(), dest_rect.Height());
            auto itr = std::ranges::upper_bound(
                sub_sheets_,
                target_size,
                std::less<>{},
                &SubSheetMeta::cell_size
            );

            const auto [cell_size, offset_x, offset_y] = 
                itr == sub_sheets_.begin() ? sub_sheets_.front() : *(--itr);

            const int src_x = offset_x + margin_ + col * (cell_size + margin_);
            const int src_y = offset_y + margin_ + row * (cell_size + margin_);

            const HDC hdc = ::CreateCompatibleDC(dc.GetSafeHdc());
            if (!hdc)
                return false;

            const HGDIOBJ oldObj = ::SelectObject(hdc, hbitmap_);

            constexpr BLENDFUNCTION blend {
                .BlendOp = AC_SRC_OVER,
                .BlendFlags = 0,
                .SourceConstantAlpha = 255,
                .AlphaFormat = AC_SRC_ALPHA
            };

            const BOOL ok = ::AlphaBlend(
                dc.GetSafeHdc(),
                dest_rect.left,
                dest_rect.top,
                dest_rect.Width(),
                dest_rect.Height(),
                hdc,
                src_x,
                src_y,
                cell_size,
                cell_size,
                blend
            );

            ::SelectObject(hdc, oldObj);
            ::DeleteDC(hdc);

            return ok != FALSE;
        }

        int GetCols() const override {
            return cols_;
        }

        int GetRows() const override {
            return rows_;
        }

        int GetMaxCount() const override {
            return cols_ * rows_;
        }

    private:
        SubShteetMetaList sub_sheets_;
        HBITMAP hbitmap_{ nullptr };
        int cols_{ 0 };
        int rows_{ 0 };
        int margin_{ 0 };
    };
}

////////////////////////////////////////////////////////////////////////////////

/**********************************************************************************************************************/
// IconSheetManager

IconManager& IconManager::Instance() {
    static IconManager instance;

    return instance;
}

const IconSheet* IconManager::GetIconSheet(IconResType type) {
    if (icon_sheets_.contains(type)) {
        return icon_sheets_[type].get();
    } else {
        return nullptr;
    }
}

HICON IconManager::GetIcon(UINT icon_id) {
    if (!icons_.contains(icon_id)) {
        AFX_MANAGE_STATE(AfxGetStaticModuleState())

        const auto hicon = static_cast<HICON>(
            LoadImageW(AfxGetInstanceHandle(), MAKEINTRESOURCE(icon_id), IMAGE_ICON,
                       16, 16, 0));

        icons_[icon_id] = hicon;
    }

    return icons_[icon_id];
}

void IconManager::LoadIconResources() {
    AFX_MANAGE_STATE(AfxGetStaticModuleState())

    // start gdi+ temporarily
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;

    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
        return;

    constexpr std::array<SubSheetMeta, 9> SSM_C8R2M4{ {
        {.cell_size = 16, .offset_x = 616, .offset_y = 300},
        {.cell_size = 20, .offset_x = 420, .offset_y = 300},
        {.cell_size = 24, .offset_x = 548, .offset_y = 76},
        {.cell_size = 28, .offset_x = 484, .offset_y = 232},
        {.cell_size = 32, .offset_x = 548, .offset_y = 0},
        {.cell_size = 40, .offset_x = 484, .offset_y = 140},
        {.cell_size = 48, .offset_x = 0,   .offset_y = 264},
        {.cell_size = 56, .offset_x = 0,   .offset_y = 140},
        {.cell_size = 64, .offset_x = 0,   .offset_y = 0},
    } };

    constexpr std::array<SubSheetMeta, 9> SSM_C8R7M4{ {
        {.cell_size = 16, .offset_x = 616, .offset_y = 1020},
        {.cell_size = 20, .offset_x = 420, .offset_y = 1020},
        {.cell_size = 24, .offset_x = 548, .offset_y = 256},
        {.cell_size = 28, .offset_x = 484, .offset_y = 792},
        {.cell_size = 32, .offset_x = 548, .offset_y = 0},
        {.cell_size = 40, .offset_x = 484, .offset_y = 480},
        {.cell_size = 48, .offset_x = 0,   .offset_y = 904},
        {.cell_size = 56, .offset_x = 0,   .offset_y = 480},
        {.cell_size = 64, .offset_x = 0,   .offset_y = 0},
    } };

    constexpr std::array<SubSheetMeta, 9> SSM_C8R8M4{ {
        {.cell_size = 16, .offset_x = 1164, .offset_y = 616},
        {.cell_size = 20, .offset_x = 1164, .offset_y = 420},
        {.cell_size = 24, .offset_x = 292,  .offset_y = 548},
        {.cell_size = 28, .offset_x = 904,  .offset_y = 484},
        {.cell_size = 32, .offset_x = 0,    .offset_y = 548},
        {.cell_size = 40, .offset_x = 548,  .offset_y = 484},
        {.cell_size = 48, .offset_x = 1032, .offset_y = 0},
        {.cell_size = 56, .offset_x = 548,  .offset_y = 0},
        {.cell_size = 64, .offset_x = 0,    .offset_y = 0},
    } };

    // weather icons: wcc
    {
        const auto icons = std::make_shared<CompositedIconSheet>();
        icons->Load(IDB_PNG_MS_WCC, 8, 7, 4,
                    SubShteetMetaList{ SSM_C8R7M4.begin(), SSM_C8R7M4.end() });
        icon_sheets_[IconResType::WccBlue] = icons;
        icon_sheets_[IconResType::WccWhite] = icons;
    }

    // weather icons: qweather fill & hollow
    {
        const auto icons_fill = std::make_shared<CompositedIconSheet>();
        icons_fill->Load(IDB_PNG_MS_QW_FILL, 8, 8, 4,
                         SubShteetMetaList{ SSM_C8R8M4.begin(), SSM_C8R8M4.end() });
        icon_sheets_[IconResType::QWeatherFill] = icons_fill;

        const auto icons_hollow = std::make_shared<CompositedIconSheet>();
        icons_hollow->Load(IDB_PNG_MS_QW_HOLLOW, 8, 8, 4,
                           SubShteetMetaList{ SSM_C8R8M4.begin(), SSM_C8R8M4.end() });
        icon_sheets_[IconResType::QWeatherHollow] = icons_hollow;
    }

    // weather icons: open weather
    {
        // monochrome SVG icons (black on a light taskbar, white on a dark one), 8 columns x 2 rows like the old sprite
        icon_sheets_[IconResType::OpenWeather] = std::make_shared<SvgIconSheet>(ow_svg::kIcons, 8, 2);
    }

    // loading frame icons
    {
        const auto icons_loading = std::make_shared<CompositedIconSheet>();
        icons_loading->Load(IDB_PNG_MS_LOADING, 8, 2, 4,
                            SubShteetMetaList{ SSM_C8R2M4.begin(), SSM_C8R2M4.end() });
        icon_sheets_[IconResType::Loading] = icons_loading;
    }

    // shutdown gdi+
    Gdiplus::GdiplusShutdown(token);
}
