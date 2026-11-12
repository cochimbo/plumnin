#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "imgui_internal.h"  // the windows' title bars, ShadeVertsLinearColorGradientKeepAlpha
#include "chakra_petch_bold_italic_ttf.h"
#include "chakra_petch_regular_ttf.h"

namespace theme {
namespace {

// The palette: the console's case, its red "16-BIT" and the chrome of its lettering.
constexpr ImU32 kCase = IM_COL32(0x1D, 0x1D, 0x20, 0xFF);
constexpr ImU32 kRib = IM_COL32(0x12, 0x12, 0x14, 0xFF);
constexpr ImU32 kRed = IM_COL32(0xD7, 0x26, 0x1E, 0xFF);
constexpr ImU32 kRedLit = IM_COL32(0xF0, 0x45, 0x3C, 0xFF);
constexpr ImU32 kRedDim = IM_COL32(0x8A, 0x1E, 0x19, 0xFF);
constexpr ImU32 kChromeTop = IM_COL32(0xF6, 0xF7, 0xF9, 0xFF);
constexpr ImU32 kChromeBottom = IM_COL32(0xA4, 0xAB, 0xB3, 0xFF);
constexpr ImU32 kChrome = IM_COL32(0xC8, 0xCD, 0xD3, 0xFF);

Fonts gFonts;

ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }

ImFont* addFont(const unsigned char* data, size_t size, float px, const ImWchar* ranges) {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // a static array
    cfg.OversampleH = 2;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), static_cast<int>(size), px, &cfg,
                                                      ranges);
}

// Text in the title font with the chrome's light-to-grey shading, top to bottom.
void chromeText(ImDrawList* d, ImVec2 pos, float size, const char* begin, const char* end, float alpha = 1.0f) {
    const int first = d->VtxBuffer.Size;
    d->AddText(gFonts.title, size, pos, IM_COL32(255, 255, 255, static_cast<int>(alpha * 255)), begin, end);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(d, first, d->VtxBuffer.Size, pos, ImVec2(pos.x, pos.y + size), kChromeTop,
                                                  kChromeBottom);
}

// One of the pad's buttons: round for a letter, a pill for L1, Start...
float padButton(ImDrawList* d, ImVec2 pos, float line, const char* label, size_t len) {
    const float h = std::floor(line * 1.15f);
    const float r = h * 0.5f;
    const float textSize = std::floor(line * 0.8f);
    const ImVec2 t = gFonts.title->CalcTextSizeA(textSize, 1e9f, 0.0f, label, label + len);
    const float w = len == 1 ? h : std::floor(t.x + r * 1.6f);
    const ImVec2 a(pos.x, pos.y + (line - h) * 0.5f), b(a.x + w, a.y + h);
    d->AddRectFilled(ImVec2(a.x, a.y + 2), ImVec2(b.x, b.y + 2), IM_COL32(0, 0, 0, 200), r);
    d->AddRectFilled(a, b, IM_COL32(0x18, 0x18, 0x1B, 0xFF), r);
    // The light on the top of the plastic.
    d->AddRectFilledMultiColor(ImVec2(a.x + r * 0.5f, a.y + 1), ImVec2(b.x - r * 0.5f, a.y + h * 0.5f), IM_COL32(0x55, 0x55, 0x5E, 0xFF),
                               IM_COL32(0x55, 0x55, 0x5E, 0xFF), IM_COL32(0x18, 0x18, 0x1B, 0), IM_COL32(0x18, 0x18, 0x1B, 0));
    d->AddText(gFonts.title, textSize, ImVec2(a.x + (w - t.x) * 0.5f, a.y + (h - t.y) * 0.5f), kChrome, label, label + len);
    return w;
}

bool isPadButton(const char* s, size_t len) {
    static const char* const kNames[] = {"A", "B", "C", "X", "Y", "Z", "L", "R", "L1", "R1", "L2", "R2", "Start", "Select", "Mode"};
    for (const char* n : kNames)
        if (std::strlen(n) == len && std::strncmp(n, s, len) == 0) return true;
    return false;
}

}  // namespace

const Fonts& fonts() { return gFonts; }

void addBodyFont(float size, const ImWchar* ranges) {
    gFonts.body = addFont(kChakraPetchRegular, sizeof(kChakraPetchRegular), size, ranges);
}

void addTitleFont(float size, const ImWchar* ranges) {
    gFonts.title = addFont(kChakraPetchBoldItalic, sizeof(kChakraPetchBoldItalic), size, ranges);
}

namespace {
float gScale = 1.0f;
}

float scale() { return gScale; }

void apply(float scale) {
    gScale = scale;
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);
    s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = 0.0f;
    s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0.0f;
    s.WindowBorderSize = s.PopupBorderSize = s.FrameBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 5);
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 12.0f;
    s.TabBarBorderSize = 1.0f;
    s.TabBarOverlineSize = 3.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None;  // no collapse arrow: the ribs go there
    s.SeparatorTextBorderSize = 1.0f;

    ImVec4* c = s.Colors;
    const ImVec4 red = rgb(0xD7, 0x26, 0x1E), redLit = rgb(0xF0, 0x45, 0x3C);
    c[ImGuiCol_Text] = rgb(0xE6, 0xE6, 0xE8);
    c[ImGuiCol_TextDisabled] = rgb(0x8B, 0x8B, 0x93);
    c[ImGuiCol_WindowBg] = rgb(0x26, 0x26, 0x2A);
    c[ImGuiCol_ChildBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = rgb(0x1D, 0x1D, 0x20, 0.98f);
    c[ImGuiCol_Border] = rgb(0x3A, 0x3A, 0x40);
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = rgb(0x15, 0x15, 0x17);
    c[ImGuiCol_FrameBgHovered] = rgb(0x22, 0x22, 0x26);
    c[ImGuiCol_FrameBgActive] = rgb(0x2E, 0x2E, 0x33);
    c[ImGuiCol_TitleBg] = rgb(0x1A, 0x1A, 0x1D);
    c[ImGuiCol_TitleBgActive] = rgb(0x24, 0x24, 0x28);
    c[ImGuiCol_TitleBgCollapsed] = rgb(0x1A, 0x1A, 0x1D, 0.9f);
    c[ImGuiCol_MenuBarBg] = rgb(0x17, 0x17, 0x1A);
    c[ImGuiCol_ScrollbarBg] = rgb(0x15, 0x15, 0x17, 0.6f);
    c[ImGuiCol_ScrollbarGrab] = rgb(0x3A, 0x3A, 0x40);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(0x4E, 0x4E, 0x56);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(0x8B, 0x8B, 0x93);
    c[ImGuiCol_CheckMark] = redLit;
    c[ImGuiCol_SliderGrab] = red;
    c[ImGuiCol_SliderGrabActive] = redLit;
    c[ImGuiCol_Button] = rgb(0x33, 0x33, 0x39);
    c[ImGuiCol_ButtonHovered] = rgb(0x44, 0x44, 0x4C);
    c[ImGuiCol_ButtonActive] = red;
    c[ImGuiCol_Header] = rgb(0x5A, 0x1E, 0x1B);
    c[ImGuiCol_HeaderHovered] = rgb(0x42, 0x24, 0x23);
    c[ImGuiCol_HeaderActive] = rgb(0x7A, 0x22, 0x1D);
    c[ImGuiCol_Separator] = rgb(0x3A, 0x3A, 0x40);
    c[ImGuiCol_SeparatorHovered] = rgb(0x8A, 0x1E, 0x19);
    c[ImGuiCol_SeparatorActive] = red;
    c[ImGuiCol_ResizeGrip] = rgb(0x3A, 0x3A, 0x40, 0.5f);
    c[ImGuiCol_ResizeGripHovered] = rgb(0x8A, 0x1E, 0x19);
    c[ImGuiCol_ResizeGripActive] = red;
    c[ImGuiCol_Tab] = rgb(0x1D, 0x1D, 0x20);
    c[ImGuiCol_TabHovered] = rgb(0x42, 0x24, 0x23);
    c[ImGuiCol_TabSelected] = rgb(0x30, 0x1C, 0x1B);
    c[ImGuiCol_TabSelectedOverline] = red;
    c[ImGuiCol_TabDimmed] = rgb(0x1D, 0x1D, 0x20);
    c[ImGuiCol_TabDimmedSelected] = rgb(0x2A, 0x22, 0x22);
    c[ImGuiCol_TabDimmedSelectedOverline] = rgb(0x8A, 0x1E, 0x19);
    c[ImGuiCol_PlotLines] = rgb(0xC8, 0xCD, 0xD3);
    c[ImGuiCol_PlotLinesHovered] = redLit;
    c[ImGuiCol_PlotHistogram] = red;
    c[ImGuiCol_PlotHistogramHovered] = redLit;
    c[ImGuiCol_TableHeaderBg] = rgb(0x1D, 0x1D, 0x20);
    c[ImGuiCol_TableBorderStrong] = rgb(0x3A, 0x3A, 0x40);
    c[ImGuiCol_TableBorderLight] = rgb(0x2E, 0x2E, 0x33);
    c[ImGuiCol_TableRowBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgb(0xFF, 0xFF, 0xFF, 0.025f);
    c[ImGuiCol_TextLink] = rgb(0xF0, 0x6A, 0x62);
    c[ImGuiCol_TextSelectedBg] = rgb(0xD7, 0x26, 0x1E, 0.35f);
    c[ImGuiCol_DragDropTarget] = rgb(0xE8, 0xC2, 0x4A);
    c[ImGuiCol_NavCursor] = red;
    c[ImGuiCol_NavWindowingHighlight] = rgb(0xFF, 0xFF, 0xFF, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = rgb(0, 0, 0, 0.5f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0, 0, 0, 0.6f);
    if (scale != 1.0f) s.ScaleAllSizes(scale);
}

// ImGui draws the title bar in Begin; this paints over it, up to the close button: the
// title in chrome italics, the case's ribs after it, and the red line under it all.
void decorateWindows() {
    ImGuiContext& g = *GImGui;
    if (!gFonts.title) return;
    const ImGuiStyle& style = g.Style;
    for (ImGuiWindow* w : g.Windows) {
        if (!w->Active || w->Hidden || (w->Flags & (ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_ChildWindow))) continue;
        const ImRect bar = w->TitleBarRect();
        const float font = w->FontWindowScale * g.FontBaseSize, size = std::floor(font * 1.1f);
        float right = bar.Max.x - w->WindowBorderSize;
        if (w->HasCloseButton) right -= style.FramePadding.x + font + style.ItemInnerSpacing.x;
        const bool focused = g.NavWindow && w->RootWindow == g.NavWindow->RootWindow;
        ImDrawList* d = w->DrawList;
        d->PushClipRect(bar.Min, bar.Max, false);
        d->AddRectFilled(ImVec2(bar.Min.x + w->WindowBorderSize, bar.Min.y + w->WindowBorderSize), ImVec2(right, bar.Max.y),
                         ImGui::GetColorU32(focused ? ImGuiCol_TitleBgActive : ImGuiCol_TitleBg));

        const char* end = ImGui::FindRenderedTextEnd(w->Name);
        const ImVec2 text = gFonts.title->CalcTextSizeA(size, 1e9f, 0.0f, w->Name, end);
        const ImVec2 at(bar.Min.x + style.FramePadding.x + 2, bar.Min.y + (bar.GetHeight() - text.y) * 0.5f);
        d->PushClipRect(bar.Min, ImVec2(right, bar.Max.y), true);
        chromeText(d, at, size, w->Name, end, focused ? 1.0f : 0.6f);
        // The ribs, from after the title to the close button.
        const float ribL = at.x + text.x + size * 0.8f, ribR = right - size * 0.4f;
        if (ribR - ribL > size) {
            for (float y = bar.Min.y + 5; y + 1 < bar.Max.y - 5; y += 3)
                d->AddRectFilled(ImVec2(ribL, y), ImVec2(ribR, y + 1), kRib);
            d->AddRectFilled(ImVec2(ribL, bar.Min.y + 4), ImVec2(ribL + 3, bar.Max.y - 4), focused ? kRed : kRedDim);
        }
        d->PopClipRect();
        if (!w->Collapsed)
            d->AddRectFilled(ImVec2(bar.Min.x, bar.Max.y - 2), ImVec2(bar.Max.x, bar.Max.y), focused ? kRed : kRedDim);
        d->PopClipRect();
    }
}

bool primaryButton(const char* label, float width) {
    const float size = ImGui::GetFontSize() * 1.1f;
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 t = (gFonts.title ? gFonts.title : ImGui::GetFont())->CalcTextSizeA(size, 1e9f, 0.0f, label, end);
    const float h = std::floor(size + ImGui::GetStyle().FramePadding.y * 2.5f);
    const float w = width < 0 ? ImGui::GetContentRegionAvail().x : width;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    const float slant = h * 0.35f;
    const ImVec2 pts[4] = {ImVec2(a.x + slant, a.y), ImVec2(a.x + w, a.y), ImVec2(a.x + w - slant, a.y + h), ImVec2(a.x, a.y + h)};
    ImDrawList* d = ImGui::GetWindowDrawList();
    d->AddConvexPolyFilled(pts, 4, held ? kRedDim : hovered ? kRedLit : kRed);
    d->AddLine(pts[0], pts[1], IM_COL32(255, 255, 255, 60));
    d->AddText(gFonts.title ? gFonts.title : ImGui::GetFont(), size, ImVec2(a.x + (w - t.x) * 0.5f, a.y + (h - t.y) * 0.5f),
               IM_COL32_WHITE, label, end);
    return pressed;
}

bool switchButton(const char* id, bool* on, const char* onLabel, const char* offLabel) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float w = std::max(ImGui::CalcTextSize(onLabel).x, ImGui::CalcTextSize(offLabel).x) + style.FramePadding.x * 2;
    ImGui::AlignTextToFramePadding();  // text after it on the line sits as next to a frame
    const ImVec2 a = ImGui::GetCursorScreenPos(), size(w, ImGui::GetFrameHeight());
    const bool pressed = ImGui::InvisibleButton(id, size);
    if (pressed) *on = !*on;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 b(a.x + size.x, a.y + size.y);
    if (*on) d->AddRectFilled(a, b, hovered ? kRedLit : kRed);
    else d->AddRectFilled(a, b, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg));
    d->AddRect(a, b, *on ? kRedDim : ImGui::GetColorU32(ImGuiCol_Border));
    const char* label = *on ? onLabel : offLabel;
    const ImVec2 t = ImGui::CalcTextSize(label);
    d->AddText(ImVec2(a.x + (size.x - t.x) * 0.5f, a.y + (size.y - t.y) * 0.5f),
               *on ? IM_COL32_WHITE : ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    return pressed;
}

void boxGrid(ImVec2 min, ImVec2 max) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    ImDrawList* d = w->DrawList;
    const float cell = 14.0f;
    d->AddRectFilled(min, max, IM_COL32(0x05, 0x05, 0x06, 0xFF));
    const ImU32 line = IM_COL32(0xFF, 0xFF, 0xFF, 0x50);
    const float x0 = min.x - std::fmod(w->Scroll.x, cell), y0 = min.y - std::fmod(w->Scroll.y, cell);
    for (float x = x0; x < max.x; x += cell)
        if (x >= min.x) d->AddRectFilled(ImVec2(x, min.y), ImVec2(x + 1, max.y), line);
    for (float y = y0; y < max.y; y += cell)
        if (y >= min.y) d->AddRectFilled(ImVec2(min.x, y), ImVec2(max.x, y + 1), line);
}

void hint(const char* text) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float line = ImGui::GetTextLineHeight();
    const float space = ImGui::CalcTextSize(" ").x;
    const ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const char* p = text;
    while (*p) {
        const char* eol = std::strchr(p, '\n');
        if (!eol) eol = p + std::strlen(p);
        ImVec2 start = ImGui::GetCursorScreenPos();
        start.y += ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;  // after SameLine by a button
        float x = start.x;
        // Word by word: a pad button's name (A, L1, Start...) on its own, before any ':'
        // or ',', is drawn as the button.
        for (const char* s = p; s < eol;) {
            if (*s == ' ') {
                x += space;
                ++s;
                continue;
            }
            const char* e = s;
            while (e < eol && *e != ' ') ++e;
            const char* name = e;
            while (name > s && (name[-1] == ':' || name[-1] == ',' || name[-1] == ';')) --name;
            if (isPadButton(s, static_cast<size_t>(name - s))) {
                x += padButton(d, ImVec2(x, start.y), line, s, static_cast<size_t>(name - s)) + 1;
                s = name;
            }
            if (s < e) {
                d->AddText(ImVec2(x, start.y), dim, s, e);
                x += ImGui::CalcTextSize(s, e).x;
            }
            s = e;
        }
        ImGui::Dummy(ImVec2(x - start.x, line + 2));
        p = *eol ? eol + 1 : eol;
    }
}

}  // namespace theme
