#include "library.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "../core/i18n.h"
#include "../render/gl.h"
#include "library_names.h"
#include "screenscraper_data.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace plumnin {

namespace fs = std::filesystem;

namespace {

constexpr int kCoverHeight = 280;  // covers are shrunk to this many pixels tall
constexpr int kSleeveHeight = 720;  // but a whole sleeve (back, spine and front) to this, for the 3D boxes
constexpr int kMaxDepth = 3;       // subfolders scanned below the library folder
// What decides how an image in the cache was shrunk: a change to decode() changes it, and
// the images cached before are not used (and are deleted by the next prune).
constexpr uint32_t kCacheVariant = 1u << 28 | static_cast<uint32_t>(kSleeveHeight) << 14 | static_cast<uint32_t>(kCoverHeight);

// Each console's subfolder of the covers folder.
constexpr const char* kCoverSubfolders[] = {"GameBoy", "GameBoyColor", "NES", "SNES"};
static_assert(std::size(kCoverSubfolders) == kSystemCount, "one subfolder per console");

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool readAll(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

bool isImage(const std::vector<uint8_t>& d) {
    static const uint8_t png[] = {0x89, 'P', 'N', 'G'};
    return (d.size() > 8 && std::equal(png, png + 4, d.begin())) || (d.size() > 3 && d[0] == 0xFF && d[1] == 0xD8);
}

// Decodes a PNG or JPEG and shrinks it to kCoverHeight lines (kSleeveHeight if it is much
// wider than tall, a whole sleeve) with a box filter, as 0xAABBGGRR in memory order R, G, B, A.
bool decode(const std::vector<uint8_t>& data, std::vector<uint32_t>& pixels, int* width, int* height) {
    int w = 0, h = 0, n = 0;
    stbi_uc* rgba = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &w, &h, &n, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        return false;
    }
    const int oh = std::min(h, w >= h * 6 / 5 ? kSleeveHeight : kCoverHeight);
    const int ow = std::max(1, static_cast<int>(static_cast<long long>(w) * oh / h));
    pixels.assign(static_cast<size_t>(ow) * oh, 0);
    for (int y = 0; y < oh; ++y) {
        const int y0 = y * h / oh, y1 = std::max(y0 + 1, (y + 1) * h / oh);
        for (int x = 0; x < ow; ++x) {
            const int x0 = x * w / ow, x1 = std::max(x0 + 1, (x + 1) * w / ow);
            uint32_t sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                    for (int c = 0; c < 4; ++c) sum[c] += rgba[(static_cast<size_t>(sy) * w + sx) * 4 + c];
            const uint32_t count = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
            uint32_t p = 0;
            for (int c = 0; c < 4; ++c) p |= (sum[c] / count) << (8 * c);
            pixels[static_cast<size_t>(y) * ow + x] = p;
        }
    }
    stbi_image_free(rgba);
    *width = ow;
    *height = oh;
    return true;
}

}  // namespace

Library::Library() {
    // A sleeve's PNG takes some 25 ms to decode: several at once, but leaving a core free
    // for the game and the interface.
    const unsigned cores = std::thread::hardware_concurrency();
    const unsigned n = std::clamp(cores > 1 ? cores - 1 : 1u, 1u, 4u);
    for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { work(); });
}

Library::~Library() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
}

void Library::scan(const std::vector<std::string>& folders) {
    clearTextures();
    games_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        pending_ -= static_cast<int>(jobs_.size());
        jobs_.clear();
        results_.clear();
        usedKeys_.clear();
    }
    pruned_ = false;
    folders_ = folders;
    scanned_ = true;
    std::vector<fs::path> files;
    std::error_code ec;
    for (const std::string& folder : folders) {
        const fs::path root = fs::u8path(folder);
        if (folder.empty() || !fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it.depth() >= kMaxDepth) it.disable_recursion_pending();
            if (!it->is_regular_file(ec) || !isGameFile(it->path().u8string())) continue;
            // Boot ROM dumps ("[BIOS] Game Boy (World).gb") are not games.
            if (lower(it->path().filename().u8string()).find("bios") != std::string::npos) continue;
            files.push_back(it->path());
        }
        ec.clear();
    }
    std::vector<std::string> seen;  // the same folder listed twice, or one inside another
    for (const fs::path& p : files) {
        const std::string canonical = lower(fs::weakly_canonical(p, ec).u8string());
        if (std::find(seen.begin(), seen.end(), canonical) != seen.end()) continue;
        seen.push_back(canonical);
        Game g;
        g.path = p.u8string();
        g.stem = p.stem().u8string();
        g.title = gameTitle(g.stem);
        g.tags = gameTags(g.stem);
        g.key = searchKey(g.title + " " + g.tags + " " + g.stem);
        g.system = detectSystem(g.path);
        games_.push_back(std::move(g));
    }
    // In title order, with "The Lion King" among the L's.
    auto order = [](const Game& g) {
        std::string k = searchKey(g.title);
        return k.compare(0, 4, "the ") == 0 ? k.substr(4) : k;
    };
    std::sort(games_.begin(), games_.end(), [&](const Game& a, const Game& b) {
        const std::string ka = order(a), kb = order(b);
        if (ka != kb) return ka < kb;
        if (a.system != b.system) return a.system < b.system;
        return a.stem < b.stem;
    });
    for (size_t i = 0; i < games_.size(); ++i) {
        games_[i].cover = Cover::Pending;
        queue({i, generation_, games_[i].path, games_[i].stem, games_[i].system});
    }
}

bool Library::decodeImage(const std::vector<uint8_t>& data, std::vector<uint32_t>& pixels, int* width, int* height) {
    return isImage(data) && decode(data, pixels, width, height);
}

unsigned int Library::uploadTexture(unsigned int texture, const std::vector<uint32_t>& pixels, int width, int height) {
    if (!texture) gl::GenTextures(1, &texture);
    gl::BindTexture(gl::TEXTURE_2D, texture);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE);
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA8, width, height, 0, gl::RGBA, gl::UNSIGNED_BYTE, pixels.data());
    return texture;
}

std::string Library::systemCovers(ConsoleSystem s) const {
    const char* sub = kCoverSubfolders[static_cast<int>(s)];
    if (coversFolder_.empty() || !*sub) return coversFolder_;
    return (fs::u8path(coversFolder_) / fs::u8path(sub)).u8string() + static_cast<char>(fs::path::preferred_separator);
}

void Library::prioritize(const std::vector<size_t>& games) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (jobs_.empty()) return;
    // The last one first, each put at the front, so that the first ends up first.
    for (auto g = games.rbegin(); g != games.rend(); ++g) {
        const auto it = std::find_if(jobs_.begin(), jobs_.end(), [&](const Job& j) { return j.index == *g; });
        if (it == jobs_.end() || it == jobs_.begin()) continue;
        Job job = std::move(*it);
        jobs_.erase(it);
        jobs_.push_front(std::move(job));
    }
}

void Library::queue(Job job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        jobs_.push_back(std::move(job));
        ++pending_;
    }
    wake_.notify_one();
}

bool Library::labelFromFile(size_t game, const std::string& file, std::string* error) {
    std::vector<uint8_t> image;
    if (!readAll(fs::u8path(file), image)) {
        *error = trf("No se pudo leer %s", file.c_str());
        return false;
    }
    if (!isImage(image)) {
        *error = trf("%s no es una imagen PNG o JPEG", fs::u8path(file).filename().u8string().c_str());
        return false;
    }
    return saveCover(game, image, error, true);
}

bool Library::coverFromFile(size_t game, const std::string& file, std::string* error) {
    std::vector<uint8_t> image;
    if (!readAll(fs::u8path(file), image)) {
        *error = trf("No se pudo leer %s", file.c_str());
        return false;
    }
    int w = 0, h = 0, n = 0;
    if (!isImage(image) || !stbi_info_from_memory(image.data(), static_cast<int>(image.size()), &w, &h, &n)) {
        *error = trf("%s no es una imagen PNG o JPEG", fs::u8path(file).filename().u8string().c_str());
        return false;
    }
    return saveCover(game, image, error);
}

void Library::useDownloaded(const std::string& path, const std::vector<uint8_t>& cover, const std::vector<uint8_t>& label,
                            bool replace) {
    for (size_t i = 0; i < games_.size(); ++i) {
        const Game& g = games_[i];
        if (g.path != path) continue;
        std::string error;
        int w = 0, h = 0, n = 0;
        const bool sleeve = !cover.empty() && stbi_info_from_memory(cover.data(), static_cast<int>(cover.size()), &w, &h, &n) &&
                            w >= h * 6 / 5;
        const bool hasSleeve = g.cover == Cover::Ready && g.width >= g.height * 6 / 5;
        const bool hasLabel = g.label != 0;
        if (!cover.empty() && isImage(cover) &&
            (replace || g.cover == Cover::Missing || g.cover == Cover::Unknown || (sleeve && !hasSleeve)))
            saveCover(i, cover, &error);
        if (!label.empty() && isImage(label) && (replace || !hasLabel)) saveCover(i, label, &error, true);
        return;
    }
}

bool Library::saveCover(size_t game, const std::vector<uint8_t>& image, std::string* error, bool label) {
    if (game >= games_.size()) return false;
    Game& g = games_[game];
    std::string target = systemCovers(g.system);
    if (!target.empty() && label) target = (fs::u8path(target) / "etiquetas").u8string() + static_cast<char>(fs::path::preferred_separator);
    if (target.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::u8path(target), ec);
    // The .png is looked for before the .jpg: the old one, of either kind, must go.
    for (const char* ext : {".png", ".jpg", ".jpeg"}) fs::remove(fs::u8path(target + g.stem + ext), ec);
    const bool png = image.size() > 4 && image[0] == 0x89 && image[1] == 'P';
    {
        std::ofstream f(fs::u8path(target + g.stem + (png ? ".png" : ".jpg")), std::ios::binary);
        f.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        if (!f) {
            *error = trf("No se pudo guardar la carátula en %s", target.c_str());
            return false;
        }
    }
    g.cover = Cover::Pending;
    queue({game, generation_, g.path, g.stem, g.system});
    return true;
}

void Library::update(int budgetMs) {
    // Each upload is quick, but dozens of sleeves at once would stutter the game.
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = Clock::now();
    for (int n = 0; n == 0 || Clock::now() - start < std::chrono::milliseconds(budgetMs); ++n) {
        Result r;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (results_.empty()) {
                // Every cover of this scan is in: what else is in the cache is of no use.
                if (!pruned_ && pending_ == 0 && scanned_) {
                    pruned_ = true;
                    std::vector<uint64_t> keep = usedKeys_;
                    std::sort(keep.begin(), keep.end());
                    cache_.prune(keep);
                }
                return;
            }
            r = std::move(results_.front());
            results_.pop_front();
        }
        if (r.generation != generation_ || r.index >= games_.size()) continue;
        Game& g = games_[r.index];
        if (!r.found) {
            g.cover = Cover::Missing;
            continue;
        }
        if (!r.label.empty()) g.label = uploadTexture(g.label, r.label, r.labelWidth, r.labelHeight);
        g.labelWidth = r.labelWidth;
        g.labelHeight = r.labelHeight;
        g.texture = uploadTexture(g.texture, r.pixels, r.width, r.height);
        g.width = r.width;
        g.height = r.height;
        g.cover = Cover::Ready;
    }
}

void Library::clearTextures() {
    for (Game& g : games_) {
        if (g.label) {
            gl::DeleteTextures(1, &g.label);
            g.label = 0;
        }
        if (g.texture) {
            gl::DeleteTextures(1, &g.texture);
            g.texture = 0;
            if (g.cover == Cover::Ready) g.cover = Cover::Unknown;
        }
    }
}

bool Library::findLocal(const Job& job, std::string* coverPath) const {
    std::error_code ec;
    const fs::path rom = fs::u8path(job.path);
    const fs::path dir = rom.parent_path();
    const fs::path stem = fs::u8path(job.stem);
    std::vector<fs::path> folders = {fs::u8path(systemCovers(job.system)), dir};
    for (const char* sub : {"caratulas", "carátulas", "covers", "boxart", "Named_Boxarts"}) folders.push_back(dir / fs::u8path(sub));
    for (const auto& f : folders) {
        if (f.empty()) continue;
        for (const char* ext : {".png", ".jpg", ".jpeg"}) {
            fs::path p = f / stem;
            p += ext;
            if (fs::is_regular_file(p, ec)) {
                *coverPath = p.u8string();
                return true;
            }
        }
    }
    // What ScreenScraper downloaded of it: the whole sleeve (for the 3D boxes), or the front.
    return findMedia(job, {"portada-completa", "portada"}, coverPath);
}

bool Library::findMedia(const Job& job, std::initializer_list<const char*> kinds, std::string* path) const {
    if (mediaFolder_.empty()) return false;
    std::error_code ec;
    const std::string dir = ssGameFolder(mediaFolder_, static_cast<int>(job.system), job.stem);
    for (const char* kind : kinds)
        for (const char* ext : {".png", ".jpg"}) {
            const fs::path p = fs::u8path(dir + kind + ext);
            if (fs::is_regular_file(p, ec)) {
                *path = p.u8string();
                return true;
            }
        }
    return false;
}

bool Library::findLabel(const Job& job, std::string* labelPath) const {
    std::error_code ec;
    const fs::path dir = fs::u8path(job.path).parent_path();
    const fs::path stem = fs::u8path(job.stem);
    std::vector<fs::path> folders;
    if (!systemCovers(job.system).empty()) folders.push_back(fs::u8path(systemCovers(job.system)) / "etiquetas");
    for (const char* sub : {"etiquetas", "labels", "Named_Labels"}) folders.push_back(dir / fs::u8path(sub));
    for (const auto& f : folders)
        for (const char* ext : {".png", ".jpg", ".jpeg"}) {
            fs::path p = f / stem;
            p += ext;
            if (fs::is_regular_file(p, ec)) {
                *labelPath = p.u8string();
                return true;
            }
        }
    return findMedia(job, {"etiqueta"}, labelPath);
}

bool Library::loadImage(const std::string& path, std::vector<uint32_t>& pixels, int* width, int* height, uint64_t* key) const {
    *key = cache_.folder().empty() ? 0 : TextureCache::key(path, kCacheVariant);
    if (*key && cache_.load(*key, pixels, width, height)) return true;
    std::vector<uint8_t> data;
    if (!readAll(fs::u8path(path), data) || !decode(data, pixels, width, height)) {
        *key = 0;
        return false;
    }
    if (*key) cache_.store(*key, pixels, *width, *height);
    return true;
}

void Library::work() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        Result r{job.index, job.generation, false, {}, 0, 0, {}, 0, 0};
        uint64_t coverKey = 0, labelKey = 0;
        std::string cover;
        if (findLocal(job, &cover)) r.found = loadImage(cover, r.pixels, &r.width, &r.height, &coverKey);
        std::string label;
        if (r.found && findLabel(job, &label) && !loadImage(label, r.label, &r.labelWidth, &r.labelHeight, &labelKey))
            r.label.clear(), r.labelWidth = r.labelHeight = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        --pending_;
        if (job.generation != generation_) continue;
        if (coverKey) usedKeys_.push_back(coverKey);
        if (labelKey) usedKeys_.push_back(labelKey);
        results_.push_back(std::move(r));
    }
}

}  // namespace plumnin
