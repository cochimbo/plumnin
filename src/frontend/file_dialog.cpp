#include "file_dialog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <system_error>

#include "../core/i18n.h"
#include "../core/system.h"
#include "imgui.h"
#include "theme.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <SDL_syswm.h>
#endif

namespace plumnin {

namespace fs = std::filesystem;

namespace {

// A .png, .jpg or .jpeg, whatever the case of its extension.
bool isImageFile(const fs::path& p) {
    std::string ext = p.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg";
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(n - 1);
    return w;
}

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.resize(n - 1);
    return s;
}
#endif

}  // namespace

DialogResult openFileDialog(SDL_Window* window, const std::string& startFolder, Pick pick, std::string* chosen) {
#ifdef _WIN32
    HWND owner = nullptr;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info)) owner = info.info.win.window;

    wchar_t file[MAX_PATH * 4] = L"";
    const std::wstring folder = widen(startFolder);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    // Name and patterns of each filter, each ended by a 0, and one more 0 at the end.
    std::wstring filter;
    auto addFilter = [&](const std::string& name, const wchar_t* patterns) {
        filter += widen(name) + L'\0' + patterns + L'\0';
    };
    if (pick == Pick::Game)
        addFilter(std::string(tr("Juegos")) + " (*.gb, *.gbc, *.nes, *.sfc, *.smc, *.zip)",
                  L"*.gb;*.gbc;*.cgb;*.nes;*.sfc;*.smc;*.zip");
    if (pick == Pick::Image) addFilter(std::string(tr("Imágenes")) + " (*.png, *.jpg, *.jpeg)", L"*.png;*.jpg;*.jpeg");
    addFilter(tr("Todos los archivos"), L"*.*");
    filter += L'\0';
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    const std::wstring title =
        widen(pick == Pick::Game ? tr("Abrir juego") : pick == Pick::Image ? tr("Elegir imagen") : tr("Elegir archivo"));
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return DialogResult::Cancelled;
    *chosen = narrow(file);
    return DialogResult::Chosen;
#else
    (void)window;
    (void)startFolder;
    (void)pick;
    (void)chosen;
    return DialogResult::Unavailable;
#endif
}

void openInSystem(const std::string& path) {
    // file:///C:/... on Windows; a Linux path already starts with its own slash (file:///home/...).
    SDL_OpenURL(((path.empty() || path[0] != '/' ? "file:///" : "file://") + path).c_str());
}

void FileBrowser::open(const std::string& folder, Pick pick) {
    std::error_code ec;
    fs::path start = folder.empty() ? fs::current_path(ec) : fs::u8path(folder);
    if (!fs::is_directory(start, ec)) start = fs::current_path(ec);
    pick_ = pick;
    list(start.u8string());
    open_ = true;
    justOpened_ = true;
}

void FileBrowser::list(const std::string& folder) {
    folder_ = folder;
    drives_ = false;
    entries_.clear();
    selected_ = 0;
    scroll_ = true;
    std::error_code ec;
    for (fs::directory_iterator it(fs::u8path(folder), fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const bool dir = it->is_directory(ec);
        const bool wanted = pick_ == Pick::AnyFile || (pick_ == Pick::Game && isGameFile(it->path().u8string())) ||
                            (pick_ == Pick::Image && isImageFile(it->path()));
        if (dir || wanted) entries_.push_back({it->path().filename().u8string(), dir});
    }
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        return a.folder != b.folder ? a.folder : a.name < b.name;
    });
}

void FileBrowser::up() {
    const fs::path here = fs::u8path(folder_);
    const fs::path parent = here.parent_path();
    if (!parent.empty() && parent != here) {
        const std::string from = here.filename().u8string();
        list(parent.u8string());
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].name == from) selected_ = static_cast<int>(i);
        return;
    }
#ifdef _WIN32
    // Above a drive's root: the list of drives.
    entries_.clear();
    const DWORD mask = GetLogicalDrives();
    for (int d = 0; d < 26; ++d)
        if (mask & (1u << d)) entries_.push_back({std::string(1, static_cast<char>('A' + d)) + ":\\", true});
    drives_ = true;
    folder_.clear();
    selected_ = 0;
    scroll_ = true;
#endif
}

bool FileBrowser::handleEvent(const SDL_Event& e) {
    if (!open_ || ImGui::GetIO().WantTextInput) return false;
    if (e.type != SDL_KEYDOWN && e.type != SDL_CONTROLLERBUTTONDOWN) {
        return e.type == SDL_KEYUP || e.type == SDL_CONTROLLERBUTTONUP || e.type == SDL_TEXTINPUT;
    }
    enum { None, Up, Down, PageUp, PageDown, Open, Back, Close } action = None;
    if (e.type == SDL_KEYDOWN) {
        switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_UP: action = Up; break;
            case SDL_SCANCODE_DOWN: action = Down; break;
            case SDL_SCANCODE_PAGEUP: action = PageUp; break;
            case SDL_SCANCODE_PAGEDOWN: action = PageDown; break;
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: action = Open; break;
            case SDL_SCANCODE_BACKSPACE: action = Back; break;
            case SDL_SCANCODE_ESCAPE: action = Close; break;
            default: break;
        }
    } else {
        switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP: action = Up; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: action = Down; break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: action = PageUp; break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: action = PageDown; break;
            case SDL_CONTROLLER_BUTTON_A: action = Open; break;
            case SDL_CONTROLLER_BUTTON_B: action = Back; break;
            case SDL_CONTROLLER_BUTTON_START: case SDL_CONTROLLER_BUTTON_BACK: action = Close; break;
            default: break;
        }
    }
    const int count = static_cast<int>(entries_.size());
    switch (action) {
        case Up: selected_ = std::max(0, selected_ - 1); scroll_ = true; break;
        case Down: selected_ = std::min(std::max(0, count - 1), selected_ + 1); scroll_ = true; break;
        case PageUp: selected_ = std::max(0, selected_ - 10); scroll_ = true; break;
        case PageDown: selected_ = std::min(std::max(0, count - 1), selected_ + 10); scroll_ = true; break;
        case Open: activate_ = true; break;
        case Back: up(); break;
        case Close: open_ = false; break;
        case None: break;
    }
    return true;
}

bool FileBrowser::draw(std::string* chosen) {
    const bool pickFolder = pick_ == Pick::Folder;
    // The same id in every language, so a language change does not lose the popup.
    const std::string title =
        std::string(pickFolder              ? tr("Elegir carpeta")
                    : pick_ == Pick::Game  ? tr("Abrir juego")
                    : pick_ == Pick::Image ? tr("Elegir imagen")
                                           : tr("Elegir archivo")) +
        "###explorador";
    if (justOpened_) {
        ImGui::OpenPopup(title.c_str());
        justOpened_ = false;
    }
    bool picked = false;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(theme::px(640.0f), vp->Size.x - 40), std::min(theme::px(480.0f), vp->Size.y - 40)),
                             ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(title.c_str(), &open_)) {
        ImGui::TextWrapped("%s", drives_ ? tr("Unidades") : folder_.c_str());
        if (ImGui::Button(tr("Subir"))) up();
        if (pickFolder && !drives_) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Usar esta carpeta"))) {
                *chosen = folder_;
                picked = true;
            }
        }
        ImGui::SameLine();
        theme::hint(pickFolder ? tr("A / Intro: entrar   B / Retroceso: subir   Esc: cerrar")
                               : tr("A / Intro: abrir   B / Retroceso: subir   Esc: cerrar"));
        ImGui::Separator();
        std::string next;
        auto choose = [&](const Entry& e) {
            const std::string path = drives_ ? e.name : (fs::u8path(folder_) / fs::u8path(e.name)).u8string();
            if (e.folder) {
                next = path;
            } else {
                *chosen = path;
                picked = true;
            }
        };
        if (ImGui::BeginChild("archivos", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()))) {
            for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
                const Entry& e = entries_[static_cast<size_t>(i)];
                const std::string label = e.folder ? "[" + e.name + "]" : e.name;
                ImGui::PushID(i);
                if (ImGui::Selectable(label.c_str(), i == selected_, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selected_ = i;
                    // A folder opens with one click, like before; files too.
                    choose(e);
                }
                if (i == selected_ && scroll_) {
                    ImGui::SetScrollHereY(0.5f);
                    scroll_ = false;
                }
                ImGui::PopID();
            }
            if (entries_.empty())
                ImGui::TextDisabled("%s", pickFolder ? tr("No hay carpetas dentro de esta.") : tr("No hay ROMs en esta carpeta."));
        }
        ImGui::EndChild();
        if (activate_) {
            activate_ = false;
            if (selected_ >= 0 && selected_ < static_cast<int>(entries_.size()) && next.empty() && !picked)
                choose(entries_[static_cast<size_t>(selected_)]);
        }
        if (!next.empty()) list(next);
        if (ImGui::Button(tr("Cancelar"))) open_ = false;
        if (picked) open_ = false;
        if (!open_) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    } else {
        open_ = false;
    }
    return picked;
}

}  // namespace plumnin
