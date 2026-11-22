#include "manual.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "../core/i18n.h"
#include "../render/gl.h"
#include "library.h"

namespace plumnin {

namespace fs = std::filesystem;

namespace {

// How tall a page is drawn, in pixels: enough to read its small print up close.
constexpr int kPageHeight = 1600;

// PDFium's C functions used here, loaded from its library (they take the platform's
// default calling convention on 64 bits).
struct Pdfium {
    using Doc = void*;
    using Pg = void*;
    using Bmp = void*;
    void (*InitLibrary)() = nullptr;
    Doc (*LoadMemDocument)(const void*, int, const char*) = nullptr;
    void (*CloseDocument)(Doc) = nullptr;
    int (*GetPageCount)(Doc) = nullptr;
    unsigned long (*GetLastError)() = nullptr;
    Pg (*LoadPage)(Doc, int) = nullptr;
    void (*ClosePage)(Pg) = nullptr;
    float (*GetPageWidthF)(Pg) = nullptr;
    float (*GetPageHeightF)(Pg) = nullptr;
    Bmp (*BitmapCreate)(int, int, int) = nullptr;
    int (*BitmapFillRect)(Bmp, int, int, int, int, unsigned long) = nullptr;
    void (*RenderPageBitmap)(Bmp, Pg, int, int, int, int, int, int) = nullptr;
    void* (*BitmapGetBuffer)(Bmp) = nullptr;
    int (*BitmapGetStride)(Bmp) = nullptr;
    void (*BitmapDestroy)(Bmp) = nullptr;
    bool ok = false;
};

// Loaded once, the first time it is asked for, and kept until plumnin ends.
Pdfium& pdfium() {
    static Pdfium p = [] {
        Pdfium f;
#ifdef _WIN32
        const char* names[] = {"pdfium.dll"};
#elif defined(__APPLE__)
        const char* names[] = {"libpdfium.dylib"};
#else
        const char* names[] = {"libpdfium.so"};
#endif
        void* lib = nullptr;
        for (const char* name : names) {
            // Next to plumnin first, then wherever the system looks.
            if (char* base = SDL_GetBasePath()) {
                lib = SDL_LoadObject((std::string(base) + name).c_str());
                SDL_free(base);
            }
            if (!lib) lib = SDL_LoadObject(name);
            if (lib) break;
        }
        if (!lib) return f;
        auto get = [lib](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(SDL_LoadFunction(lib, name));
            return fn != nullptr;
        };
        f.ok = get(f.InitLibrary, "FPDF_InitLibrary") && get(f.LoadMemDocument, "FPDF_LoadMemDocument") &&
               get(f.CloseDocument, "FPDF_CloseDocument") && get(f.GetPageCount, "FPDF_GetPageCount") &&
               get(f.GetLastError, "FPDF_GetLastError") && get(f.LoadPage, "FPDF_LoadPage") &&
               get(f.ClosePage, "FPDF_ClosePage") && get(f.GetPageWidthF, "FPDF_GetPageWidthF") &&
               get(f.GetPageHeightF, "FPDF_GetPageHeightF") && get(f.BitmapCreate, "FPDFBitmap_Create") &&
               get(f.BitmapFillRect, "FPDFBitmap_FillRect") && get(f.RenderPageBitmap, "FPDF_RenderPageBitmap") &&
               get(f.BitmapGetBuffer, "FPDFBitmap_GetBuffer") && get(f.BitmapGetStride, "FPDFBitmap_GetStride") &&
               get(f.BitmapDestroy, "FPDFBitmap_Destroy");
        if (f.ok) f.InitLibrary();
        return f;
    }();
    return p;
}

}  // namespace

bool ManualReader::available() { return pdfium().ok; }

ManualReader::ManualReader() : worker_([this] { work(); }) {}

ManualReader::~ManualReader() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

void ManualReader::open(const std::string& path) {
    if (path == path_) return;
    clearTextures();
    path_ = path;
    pages_ = path.empty() ? 0 : -1;
    error_.clear();
    first_ = 0, last_ = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++serial_;
        toOpen_ = path;
        openPending_ = true;
        toDraw_.clear();
        drawn_.clear();
    }
    wake_.notify_all();
}

void ManualReader::want(int first, int last) {
    if (pages_ > 0) {
        first = std::max(first, 0);
        last = std::min(last, pages_ - 1);
    }
    if (first == first_ && last == last_) return;
    first_ = first, last_ = last;
    // What is far from them goes; the nearest first.
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->first < first - 2 || it->first > last + 2) {
            if (it->second.texture) gl::DeleteTextures(1, &it->second.texture);
            it = textures_.erase(it);
        } else ++it;
    }
    if (pages_ <= 0) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        toDraw_.clear();
        for (int i = first; i <= last; ++i)
            if (!textures_.count(i) || !textures_[i].texture) toDraw_.push_back(i);
    }
    wake_.notify_all();
}

ManualReader::Page ManualReader::page(int index) const {
    auto it = textures_.find(index);
    return it == textures_.end() ? Page{} : it->second;
}

void ManualReader::update() {
    std::deque<Drawn> drawn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Drawn& d : drawn_)
            if (d.serial == serial_) drawn.push_back(std::move(d));
        drawn_.clear();
    }
    for (Drawn& d : drawn) {
        if (d.index < 0) {
            pages_ = d.count;
            error_ = d.error;
            // Now that it is known how many there are, the ones wanted can be drawn.
            const int first = first_, last = last_;
            first_ = 0, last_ = -2;
            want(first, last);
            continue;
        }
        if (d.index < first_ - 2 || d.index > last_ + 2 || d.pixels.empty()) continue;
        Page& p = textures_[d.index];
        p.texture = Library::uploadTexture(p.texture, d.pixels, d.width, d.height);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR_MIPMAP_LINEAR);
        gl::GenerateMipmap(gl::TEXTURE_2D);
        p.width = d.width, p.height = d.height;
    }
}

void ManualReader::clearTextures() {
    for (auto& t : textures_)
        if (t.second.texture) gl::DeleteTextures(1, &t.second.texture);
    textures_.clear();
}

void ManualReader::work() {
    Pdfium* pdf = nullptr;
    void* doc = nullptr;
    std::vector<char> data;  // PDFium reads the document from it while it is open
    for (;;) {
        unsigned serial;
        bool opening;
        std::string path;
        int index = -1;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || openPending_ || !toDraw_.empty(); });
            if (stop_) break;
            serial = serial_;
            opening = openPending_;
            if (opening) {
                path = toOpen_;
                openPending_ = false;
            } else {
                index = toDraw_.front();
                toDraw_.pop_front();
            }
        }
        Drawn d;
        d.serial = serial;
        if (opening) {
            if (doc) pdf->CloseDocument(doc);
            doc = nullptr;
            data.clear();
            if (path.empty()) continue;
            if (!pdf) pdf = &pdfium();
            if (!pdf->ok) {
                d.error = tr("Falta pdfium.dll junto a plumnin: sin ella no se pueden leer los manuales.");
            } else {
                std::ifstream in(fs::u8path(path), std::ios::binary);
                data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
                if (data.empty()) d.error = trf("No se pudo leer %s", path.c_str());
                else if (!(doc = pdf->LoadMemDocument(data.data(), static_cast<int>(data.size()), nullptr)))
                    d.error = tr("El manual no es un PDF que se pueda abrir.");
                else d.count = pdf->GetPageCount(doc);
                if (doc && d.count <= 0) d.error = tr("El manual no tiene páginas.");
            }
        } else {
            if (!doc) continue;
            void* page = pdf->LoadPage(doc, index);
            if (!page) continue;
            const float pw = pdf->GetPageWidthF(page), ph = pdf->GetPageHeightF(page);
            if (pw > 0 && ph > 0) {
                d.index = index;
                d.height = kPageHeight;
                d.width = std::clamp(static_cast<int>(std::lround(kPageHeight * pw / ph)), 16, kPageHeight * 3);
                if (void* bmp = pdf->BitmapCreate(d.width, d.height, 0)) {
                    pdf->BitmapFillRect(bmp, 0, 0, d.width, d.height, 0xFFFFFFFFul);
                    // Annotations too (0x01), and text made smooth for the screen (0x02).
                    pdf->RenderPageBitmap(bmp, page, 0, 0, d.width, d.height, 0, 0x01 | 0x02);
                    const auto* src = static_cast<const uint8_t*>(pdf->BitmapGetBuffer(bmp));
                    const int stride = pdf->BitmapGetStride(bmp);
                    d.pixels.resize(static_cast<size_t>(d.width) * static_cast<size_t>(d.height));
                    // BGRx to RGBA, as uploadTexture takes it.
                    for (int y = 0; y < d.height; ++y) {
                        const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(stride);
                        uint32_t* out = &d.pixels[static_cast<size_t>(y) * static_cast<size_t>(d.width)];
                        for (int x = 0; x < d.width; ++x)
                            out[x] = static_cast<uint32_t>(row[x * 4 + 2]) | static_cast<uint32_t>(row[x * 4 + 1]) << 8 |
                                     static_cast<uint32_t>(row[x * 4]) << 16 | 0xFF000000u;
                    }
                    pdf->BitmapDestroy(bmp);
                }
            }
            pdf->ClosePage(page);
            if (d.index < 0) continue;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (serial == serial_) drawn_.push_back(std::move(d));
    }
    if (doc) pdf->CloseDocument(doc);
}

}  // namespace plumnin
