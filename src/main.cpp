#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/config/lua/LuaBindings.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/config/values/types/Vec2Value.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/SharedDefs.hpp>

#include <cairo/cairo.h>
#include <dlfcn.h>
#include <pango/pangocairo.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Desktop::View;

inline HANDLE g_pHandle = nullptr;

// Force emission of the inline client-hash getter so Hyprland can dlsym it for
// ABI verification on plugin load.
APICALL EXPORT const char* hyprtabbed_get_client_hash() {
    return __hyprland_api_get_client_hash();
}

namespace cv = Config::Values;

static SP<cv::CFloatValue>  g_cvBarWidth;
static SP<cv::CStringValue> g_cvSide;        // "left" | "right"
static SP<cv::CColorValue>  g_cvBgActive;
static SP<cv::CColorValue>  g_cvBgInactive;
static SP<cv::CColorValue>  g_cvFgActive;
static SP<cv::CColorValue>  g_cvFgInactive;
static SP<cv::CStringValue> g_cvFont;        // pango font description, e.g. "monospace 10"
static SP<cv::CFloatValue>  g_cvRounding;    // bar corner radius
static SP<cv::CFloatValue>  g_cvTuck;        // px the bar slides UNDER the window (hides the seam)
static SP<cv::CFloatValue>  g_cvCloseSize;   // px size of the per-tab close (×) glyph; 0 disables
static SP<cv::CFloatValue>  g_cvCloseThickness; // stroke weight of the × as a ratio of its size
static SP<cv::CFloatValue>  g_cvCloseMargin;    // inner margin of the × as a ratio of its size
static SP<cv::CStringValue> g_cvClosePos;    // "top" | "bottom" — close button anchor in the tab
static SP<cv::CVec2Value>   g_cvCloseOffset; // px offset {x across bar, y inward from anchor}
static SP<cv::CColorValue>  g_cvSelectColor;     // animated "snake" border color in selection mode
static SP<cv::CFloatValue>  g_cvSelectThickness; // px
static SP<cv::CFloatValue>  g_cvSelectDim;       // 0..1 dim of non-selected windows during selection

// Selection mode: pick arbitrary windows, then group them into one hy3 tab group.
static bool                  g_selecting = false;
static std::vector<CWindow*> g_selected;

static bool isSelected(CWindow* w) {
    return std::find(g_selected.begin(), g_selected.end(), w) != g_selected.end();
}

static bool sideIsRight() {
    return g_cvSide && g_cvSide->value() == "right";
}

static bool closeAtBottom() {
    return g_cvClosePos && g_cvClosePos->value() == "bottom";
}

static void cairoSetU32(cairo_t* cr, uint64_t argb) {
    const double a = ((argb >> 24) & 0xff) / 255.0;
    const double r = ((argb >> 16) & 0xff) / 255.0;
    const double g = ((argb >> 8) & 0xff) / 255.0;
    const double b = (argb & 0xff) / 255.0;
    cairo_set_source_rgba(cr, r, g, b, a);
}

// One rendered tab cell (background + 90-degree title), cached per window.
struct SLabel {
    std::string         key;
    SP<Render::ITexture> tex;
};
static std::unordered_map<CWindow*, SLabel> g_labels;

// Render a single tab cell to a texture: filled background + title rotated 90
// degrees so it runs along the vertical bar.
static SP<Render::ITexture> renderLabel(const std::string& text, bool active, int w, int h, bool right) {
    if (w <= 0 || h <= 0)
        return nullptr;

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t*         cr      = cairo_create(surface);

    // Transparent background — the bar background (with rounding) is drawn
    // separately as rects so we can round only the outer corners.

    // Rotate the context so a horizontal layout runs along the bar's length.
    // left bar  -> text reads bottom-to-top, right bar -> top-to-bottom.
    cairo_save(cr);
    if (right) {
        cairo_translate(cr, w, 0);
        cairo_rotate(cr, M_PI / 2.0);
    } else {
        cairo_translate(cr, 0, h);
        cairo_rotate(cr, -M_PI / 2.0);
    }

    PangoLayout*          layout = pango_cairo_create_layout(cr);
    PangoFontDescription* desc   = pango_font_description_from_string(g_cvFont->value().c_str());
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);

    pango_layout_set_text(layout, text.c_str(), -1);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_width(layout, (h - 8) * PANGO_SCALE); // length axis = cell height
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER); // center along the bar's length

    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);

    // Center across the bar thickness (which is `w`, the layout's height axis).
    cairo_move_to(cr, 4.0, (w - th) / 2.0);
    cairoSetU32(cr, static_cast<uint64_t>((active ? g_cvFgActive : g_cvFgInactive)->value()));
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    cairo_restore(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    auto tex = g_pHyprRenderer->createTexture(surface);
    cairo_surface_destroy(surface);
    return tex;
}

static SP<Render::ITexture> labelFor(CWindow* key, const std::string& title, bool active, int w, int h, bool right) {
    // Key includes font + colors so a config change refreshes cached textures.
    const auto bg = static_cast<uint64_t>((active ? g_cvBgActive : g_cvBgInactive)->value());
    const auto fg = static_cast<uint64_t>((active ? g_cvFgActive : g_cvFgInactive)->value());
    const std::string k = title + "|" + (active ? "1" : "0") + "|" + std::to_string(w) + "x"
        + std::to_string(h) + (right ? "r" : "l") + "|" + g_cvFont->value() + "|"
        + std::to_string(bg) + "|" + std::to_string(fg);
    auto& e = g_labels[key];
    if (e.key != k || !e.tex) {
        e.tex = renderLabel(title, active, w, h, right);
        e.key = k;
    }
    return e.tex;
}

// Bake the bar background as a single texture: a rect with only the OUTER
// (left, or right) corners rounded. One layer => correct alpha, no seams.
static SP<Render::ITexture> renderBarBg(int w, int h, uint64_t color, int r, bool right,
                                        bool roundTop, bool roundBottom) {
    if (w <= 0 || h <= 0)
        return nullptr;
    // Only the corners on one side are rounded, so the radius may use the full
    // width (not half); cap by half-height so top/bottom corners don't overlap.
    const double R = std::min({(double) r, (double) w, h * 0.5});

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t*         cr      = cairo_create(surface);

    cairo_new_sub_path(cr);
    if (right) {
        // Outer (rounded) corners on the RIGHT edge; inner (left) stays square.
        cairo_move_to(cr, 0, 0);
        if (roundTop) {
            cairo_line_to(cr, w - R, 0);
            cairo_arc(cr, w - R, R, R, -M_PI / 2.0, 0);
        } else {
            cairo_line_to(cr, w, 0);
        }
        if (roundBottom) {
            cairo_line_to(cr, w, h - R);
            cairo_arc(cr, w - R, h - R, R, 0, M_PI / 2.0);
        } else {
            cairo_line_to(cr, w, h);
        }
        cairo_line_to(cr, 0, h);
    } else {
        // Outer (rounded) corners on the LEFT edge; inner (right) stays square.
        if (roundTop) {
            cairo_arc(cr, R, R, R, M_PI, 3.0 * M_PI / 2.0);
        } else {
            cairo_move_to(cr, 0, 0);
        }
        cairo_line_to(cr, w, 0);
        cairo_line_to(cr, w, h);
        if (roundBottom) {
            cairo_line_to(cr, R, h);
            cairo_arc(cr, R, h - R, R, M_PI / 2.0, M_PI);
        } else {
            cairo_line_to(cr, 0, h);
        }
    }
    cairo_close_path(cr);

    cairoSetU32(cr, color);
    cairo_fill(cr);
    cairo_surface_flush(surface);

    auto tex = g_pHyprRenderer->createTexture(surface);
    cairo_surface_destroy(surface);
    cairo_destroy(cr);
    return tex;
}

// Keyed by shape params (not per-window) so the title bar and the rounding
// strip — same window, different params — don't evict each other.
static std::unordered_map<std::string, SP<Render::ITexture>> g_bgCache;

static SP<Render::ITexture> barBgFor(uint64_t color, int w, int h, int r, bool right,
                                     bool roundTop, bool roundBottom) {
    const std::string k = std::to_string(color) + "|" + std::to_string(w) + "x" + std::to_string(h)
        + "|" + std::to_string(r) + (right ? "r" : "l") + (roundTop ? "T" : "") + (roundBottom ? "B" : "");
    auto it = g_bgCache.find(k);
    if (it != g_bgCache.end() && it->second)
        return it->second;
    auto tex = renderBarBg(w, h, color, r, right, roundTop, roundBottom);
    g_bgCache[k] = tex;
    return tex;
}

// Resolve hy3's exported tab-group accessor via dlsym (same pattern hy3 uses
// for hyprsplit). Re-tries while null so it picks up hy3 whenever it loads.
using Hy3GetTabSiblingsFn = size_t (*)(CWindow*, CWindow**, size_t, size_t*);
static Hy3GetTabSiblingsFn getHy3TabSiblings() {
    static Hy3GetTabSiblingsFn fn = nullptr;
    if (!fn && g_pPluginSystem) {
        for (auto& p : g_pPluginSystem->getAllPlugins()) {
            if (p->m_name == "hy3") {
                fn = reinterpret_cast<Hy3GetTabSiblingsFn>(dlsym(p->m_handle, "hy3GetTabSiblings"));
                break;
            }
        }
    }
    return fn;
}

// hy3 export that groups the given windows into one tab group (resolved lazily).
using Hy3GroupWindowsFn = void (*)(CWindow**, size_t);
static Hy3GroupWindowsFn getHy3GroupWindows() {
    static Hy3GroupWindowsFn fn = nullptr;
    if (!fn && g_pPluginSystem) {
        for (auto& p : g_pPluginSystem->getAllPlugins())
            if (p->m_name == "hy3") {
                fn = reinterpret_cast<Hy3GroupWindowsFn>(dlsym(p->m_handle, "hy3GroupWindows"));
                break;
            }
    }
    return fn;
}

// Resolve the tabs for a window: hy3 group (via export), else native group, else
// the window alone. Returns count and sets curIdx to the focused tab. Returns 0
// for a non-visible group member (the visible one owns the bar).
static int tabMembers(const PHLWINDOW& w, std::vector<CWindow*>& members, size_t& curIdx) {
    members.clear();
    curIdx = 0;
    if (!w) return 0;

    if (auto fn = getHy3TabSiblings()) {
        CWindow* buf[64];
        size_t   cur = 0;
        size_t   cnt = fn(w.get(), buf, 64, &cur);
        if (cnt > 1) {
            for (size_t i = 0; i < cnt; ++i) members.push_back(buf[i]);
            curIdx = cur < (size_t) members.size() ? cur : 0;
            return (int) members.size();
        }
    }

    if (w->m_group && w->m_group->size() > 1) {
        if (w->m_group->current() != w) return 0;
        auto curw = w->m_group->current();
        for (auto& m : w->m_group->windows())
            if (auto mw = m.lock()) {
                if (mw == curw) curIdx = members.size();
                members.push_back(mw.get());
            }
        return (int) members.size();
    }

    members.push_back(w.get());
    return 1;
}

// Close-button strip height (px) reserved for the title layout on the anchored
// side; 0 if disabled or the cell is too short to also fit a title.
static double closeBtnH(double cellH) {
    const double cs = g_cvCloseSize->value();
    if (cs <= 0.0) return 0.0;
    const double h = cs * 1.5; // glyph + padding
    return (cellH > h * 2.0) ? h : 0.0;
}

// Box of the close (×) glyph for a cell: anchored top/bottom + a px offset.
// Returns a 0-size box when there's no close button.
static CBox closeBox(double barX, double cellY, double barW, double cellH) {
    if (closeBtnH(cellH) <= 0.0) return CBox{0, 0, 0, 0};
    const double sz  = std::min((double) g_cvCloseSize->value(), barW);
    const auto   off = g_cvCloseOffset->value();
    const double x   = barX + (barW - sz) / 2.0 + off.x;
    const double y   = closeAtBottom() ? (cellY + cellH - sz - off.y) : (cellY + off.y);
    return CBox{x, y, sz, sz};
}

// Cached "×" glyph drawn with cairo strokes (font-independent).
static std::unordered_map<std::string, SP<Render::ITexture>> g_closeCache;
static SP<Render::ITexture> closeGlyph(int size, uint64_t color) {
    if (size <= 0) return nullptr;
    // Stroke weight & margin come from config; fold them into the cache key so a
    // live config change refreshes the cached glyph instead of serving a stale one.
    const double thick = std::clamp((double) g_cvCloseThickness->value(), 0.02, 0.5);
    const double marg  = std::clamp((double) g_cvCloseMargin->value(), 0.0, 0.45);
    const std::string k = std::to_string(size) + "|" + std::to_string(color) + "|"
        + std::to_string(thick) + "|" + std::to_string(marg);
    if (auto it = g_closeCache.find(k); it != g_closeCache.end() && it->second) return it->second;

    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
    cairo_t*         cr      = cairo_create(surface);
    cairoSetU32(cr, color);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, std::max(1.0, size * thick)); // stroke weight (boldness)
    const double m = size * marg;                          // margin => X size within the box
    cairo_move_to(cr, m, m);
    cairo_line_to(cr, size - m, size - m);
    cairo_move_to(cr, size - m, m);
    cairo_line_to(cr, m, size - m);
    cairo_stroke(cr);
    cairo_surface_flush(surface);

    auto tex = g_pHyprRenderer->createTexture(surface);
    cairo_surface_destroy(surface);
    cairo_destroy(cr);
    g_closeCache[k] = tex;
    return tex;
}

// Point at perimeter offset `d` (px, clockwise from top-left) of a box.
static Vector2D perimeterPoint(const CBox& b, double d) {
    const double P = 2.0 * (b.w + b.h);
    d = std::fmod(d, P);
    if (d < 0) d += P;
    if (d < b.w) return {b.x + d, b.y};
    d -= b.w;
    if (d < b.h) return {b.x + b.w, b.y + d};
    d -= b.h;
    if (d < b.w) return {b.x + b.w - d, b.y + b.h};
    d -= b.h;
    return {b.x, b.y + b.h - d};
}

// Dim full border + a bright segment travelling around the perimeter (Nokia-snake
// style), for windows picked in selection mode.
static void drawSnakeBorder(const CBox& box, float a) {
    if (box.w <= 0 || box.h <= 0) return;
    const double   t   = std::max(1.0, (double) g_cvSelectThickness->value());
    const uint64_t col = static_cast<uint64_t>(g_cvSelectColor->value());

    const auto rect = [&](double x, double y, double w, double h, const CHyprColor& c) {
        if (w <= 0 || h <= 0) return;
        CRectPassElement::SRectData d;
        d.box   = CBox{x, y, w, h};
        d.color = c;
        d.round = 0;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(d));
    };

    // Dim base border so the whole selection is always visible.
    const CHyprColor dim = CHyprColor{col}.modifyA(0.25f * a);
    rect(box.x, box.y, box.w, t, dim);
    rect(box.x, box.y + box.h - t, box.w, t, dim);
    rect(box.x, box.y, t, box.h, dim);
    rect(box.x + box.w - t, box.y, t, box.h, dim);

    // Bright travelling segment.
    static const auto epoch = std::chrono::steady_clock::now();
    const double ms   = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - epoch).count();
    const double P    = 2.0 * (box.w + box.h);
    const double head = std::fmod(ms / 1000.0 * 450.0, P); // 450 px/s
    const double seg  = P * 0.22;
    const CHyprColor bright = CHyprColor{col}.modifyA(a);
    for (double d = 0; d < seg; d += t * 0.6) {
        const Vector2D pt = perimeterPoint(box, head + d);
        rect(pt.x - t / 2.0, pt.y - t / 2.0, t, t, bright);
    }
}

class CTabbedDecoration : public IHyprWindowDecoration {
  public:
    CTabbedDecoration(PHLWINDOW window) : IHyprWindowDecoration(window), m_window(window) {}
    virtual ~CTabbedDecoration() = default;

    // True when this window is part of a real (>1) native group.
    bool grouped() const {
        auto w = m_window.lock();
        return w && w->m_group && w->m_group->size() > 1;
    }

    // Every tiled window gets a vertical titlebar (like somewm's per-client bar).
    bool shouldDecorate() const {
        auto w = m_window.lock();
        return w && !w->m_isFloating;
    }

    virtual SDecorationPositioningInfo getPositioningInfo() override {
        SDecorationPositioningInfo info;
        info.policy   = DECORATION_POSITION_ABSOLUTE;
        info.priority = 100;

        if (shouldDecorate()) {
            const double barW = g_cvBarWidth->value();
            info.reserved       = true;
            info.desiredExtents = SBoxExtents{};
            if (sideIsRight()) {
                info.edges                        = DECORATION_EDGE_RIGHT;
                info.desiredExtents.bottomRight.x = barW;
            } else {
                info.edges                    = DECORATION_EDGE_LEFT;
                info.desiredExtents.topLeft.x = barW;
            }
        } else {
            info.edges          = 0;
            info.desiredExtents = SBoxExtents{};
            info.reserved       = false;
        }
        return info;
    }

    virtual void onPositioningReply(const SDecorationPositioningReply&) override {}

    virtual void draw(PHLMONITOR pMonitor, float const& a) override {
        auto w = m_window.lock();
        if (!w || !pMonitor || !shouldDecorate())
            return;

        std::vector<CWindow*> members;
        size_t                curIdx = 0;
        const int             n = tabMembers(w, members, curIdx);
        if (n < 1)
            return;

        const double barW  = g_cvBarWidth->value();
        const bool   right = sideIsRight();

        // Follow workspace slide/change animations like hy3's own bar does.
        Vector2D cpos = w->position(IGeometric::GEOMETRIC_CURRENT) - pMonitor->m_position;
        if (w->m_workspace)
            cpos += w->m_workspace->m_renderOffset->value();
        const Vector2D csz = w->size(IGeometric::GEOMETRIC_CURRENT);
        const double   barX  = right ? (cpos.x + csz.x) : (cpos.x - barW);
        const double   cellH = csz.y / n;

        // Label textures are keyed by (int) cell size; keying them off the ANIMATED
        // size churns the cache every frame (recreating a GL texture mid-pass →
        // flicker, plus ellipsize toggling). Size the texture off the settled GOAL
        // size and let the GPU scale it onto the animated box instead.
        const double cellHGoal = w->size(IGeometric::GEOMETRIC_GOAL).y / n;

        const int barRad = static_cast<int>(g_cvRounding->value());

        const auto addTex = [&](SP<Render::ITexture> tex, double x, double y, double ww, double hh) {
            if (!tex) return;
            CTexPassElement::SRenderData d;
            d.tex   = tex;
            d.box   = CBox{x, y, ww, hh};
            d.a     = a;
            d.round = 0;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
        };

        for (int i = 0; i < n; ++i) {
            auto* mw = members[i];
            if (!mw)
                continue;

            // Grouped: highlight the current tab. Single: highlight when focused.
            const bool active = (n > 1) ? (static_cast<size_t>(i) == curIdx)
                                        : Desktop::focusState()->isWindowActive(w);
            const std::string title = mw->m_title.empty() ? std::string("…") : mw->m_title;

            const double cellY = cpos.y + i * cellH;
            const auto   bgU32 = static_cast<uint64_t>((active ? g_cvBgActive : g_cvBgInactive)->value());

            // Bar background as a baked texture with the OUTER corners rounded
            // (rect-rounding on the pass element wasn't rendering). Widened by
            // `tuck` toward the window so that edge slides UNDER the window (drawn
            // below it) → the window hides the seam. Only the stack's top (first
            // cell) and bottom (last cell) outer corners round; interior seams
            // stay square. Baked at the settled GOAL height to avoid per-frame
            // texture churn, then scaled onto the animated box.
            const double tuck = g_cvTuck->value();
            const double bgX  = right ? (barX - tuck) : barX;
            const double bgW  = barW + tuck;
            addTex(barBgFor(bgU32, static_cast<int>(bgW), static_cast<int>(cellHGoal),
                            barRad, right, i == 0, i == n - 1),
                   bgX, cellY, bgW, cellH);

            const double closeH = closeBtnH(cellH);
            const auto   fgU32  = static_cast<uint64_t>((active ? g_cvFgActive : g_cvFgInactive)->value());
            const bool   cBot   = closeAtBottom();

            // Close button (×), anchored top/bottom + px offset.
            if (closeH > 0.0) {
                const CBox cb = closeBox(barX, cellY, barW, cellH);
                addTex(closeGlyph(static_cast<int>(cb.w), fgU32), cb.x, cb.y, cb.w, cb.h);
            }

            // Title in the remaining cell space (opposite the anchored close strip).
            const double titleH = cellH - closeH;
            const double titleY = cBot ? cellY : (cellY + closeH);
            // Texture rendered at the stable goal height (no per-frame churn),
            // drawn onto the animated box.
            const double titleHTex = cellHGoal - closeBtnH(cellHGoal);
            addTex(labelFor(mw, title, active, static_cast<int>(barW), static_cast<int>(titleHTex), right),
                   barX, titleY, barW, titleH);
        }

        // Selection-mode feedback: animated snake border around the whole tile.
        if (g_selecting && isSelected(w.get())) {
            const double tileX = right ? cpos.x : barX;
            drawSnakeBorder(CBox{tileX, cpos.y, barW + csz.x, csz.y}, a);
            g_pHyprRenderer->damageMonitor(pMonitor); // keep the animation ticking
        }
    }

    // Clicks are handled by a global mouse listener (more reliable than deco
    // input, especially for a DECORATION_LAYER_UNDER bar).
    virtual eDecorationType  getDecorationType() override { return DECORATION_CUSTOM; }
    virtual eDecorationLayer getDecorationLayer() override { return DECORATION_LAYER_UNDER; }
    virtual uint64_t         getDecorationFlags() override { return 0; }
    virtual std::string      getDisplayName() override { return "Tabbed Bar"; }
    virtual void             updateWindow(PHLWINDOW) override {}

    virtual void damageEntire() override {
        auto w = m_window.lock();
        if (w && w->m_monitor)
            g_pHyprRenderer->damageMonitor(w->m_monitor.lock());
    }

  private:
    PHLWINDOWREF m_window;
};

// Overlay decoration that dims a window while selection mode is active and the
// window isn't selected — makes the picked windows stand out.
class CSelectionDimDeco : public IHyprWindowDecoration {
  public:
    CSelectionDimDeco(PHLWINDOW window) : IHyprWindowDecoration(window), m_window(window) {}
    virtual ~CSelectionDimDeco() = default;

    virtual SDecorationPositioningInfo getPositioningInfo() override {
        SDecorationPositioningInfo info;
        info.policy         = DECORATION_POSITION_ABSOLUTE;
        info.priority       = 1;
        info.reserved       = false;
        info.edges          = 0;
        info.desiredExtents = SBoxExtents{};
        return info;
    }
    virtual void onPositioningReply(const SDecorationPositioningReply&) override {}

    virtual void draw(PHLMONITOR pMonitor, float const& a) override {
        auto w = m_window.lock();
        if (!w || !pMonitor || !g_selecting || w->m_isFloating || isSelected(w.get()))
            return;
        const float dim = g_cvSelectDim->value();
        if (dim <= 0.f)
            return;

        Vector2D pos = w->position(IGeometric::GEOMETRIC_CURRENT) - pMonitor->m_position;
        if (w->m_workspace)
            pos += w->m_workspace->m_renderOffset->value();
        const Vector2D sz = w->size(IGeometric::GEOMETRIC_CURRENT);

        CRectPassElement::SRectData d;
        d.box   = CBox{pos.x, pos.y, sz.x, sz.y};
        d.color = CHyprColor{0xff000000}.modifyA(dim * a);
        d.round = 0;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(d));
    }

    virtual eDecorationType  getDecorationType() override { return DECORATION_CUSTOM; }
    virtual eDecorationLayer getDecorationLayer() override { return DECORATION_LAYER_OVERLAY; }
    virtual uint64_t         getDecorationFlags() override { return DECORATION_NON_SOLID; }
    virtual std::string      getDisplayName() override { return "Selection Dim"; }
    virtual void             updateWindow(PHLWINDOW) override {}
    virtual void             damageEntire() override {}

  private:
    PHLWINDOWREF m_window;
};

static CHyprSignalListener g_listenerOpen;
static CHyprSignalListener g_listenerMouseBtn;

static void dbglog(const std::string& s); // fwd decl (definition below)

// Left-click on a tab bar: close the tab (× zone) or focus it (switch tab).
static void onMouseButton(IPointer::SButtonEvent event, Event::SCallbackInfo& info) {
    if (event.state != 1 || event.button != 272) return; // left press only
    if (!g_pCompositor || !g_pInputManager) return;

    const Vector2D pos   = g_pInputManager->getMouseCoordsInternal();
    const double   barW  = g_cvBarWidth->value();
    dbglog("[click] pos=(" + std::to_string(pos.x) + "," + std::to_string(pos.y) + ")");
    const bool     right = sideIsRight();

    for (auto& w : Desktop::windowState()->windows()) {
        if (!w || !w->m_isMapped || w->m_isFloating || w->isHidden()) continue;

        std::vector<CWindow*> members;
        size_t                curIdx = 0;
        const int             n = tabMembers(w, members, curIdx);
        if (n < 1) continue;

        const Vector2D wp   = w->position(IGeometric::GEOMETRIC_CURRENT);
        const Vector2D ws   = w->size(IGeometric::GEOMETRIC_CURRENT);
        const double   barX = right ? (wp.x + ws.x) : (wp.x - barW);
        const double   barY = wp.y;
        if (pos.x < barX || pos.x > barX + barW || pos.y < barY || pos.y > barY + ws.y) continue;

        dbglog("[click]   HIT win='" + w->m_title + "' class='" + w->m_class
               + "' wp=(" + std::to_string(wp.x) + "," + std::to_string(wp.y) + ")"
               + " ws=(" + std::to_string(ws.x) + "," + std::to_string(ws.y) + ")"
               + " barX=" + std::to_string(barX) + " barW=" + std::to_string(barW)
               + " n=" + std::to_string(n));

        const double cellH = ws.y / n;
        int          idx   = static_cast<int>((pos.y - barY) / cellH);
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;

        info.cancelled = true; // consume the click on the bar

        auto* target = members[idx];
        if (!target) return;

        const double cellY = barY + idx * cellH;
        const CBox   cb    = closeBox(barX, cellY, barW, cellH);
        const double pad   = 4.0; // a little slack to make the × easier to hit
        const bool   inClose = cb.w > 0.0 && pos.x >= cb.x - pad && pos.x <= cb.x + cb.w + pad
            && pos.y >= cb.y - pad && pos.y <= cb.y + cb.h + pad;
        if (inClose) {
            dbglog("[click]   -> CLOSE idx=" + std::to_string(idx));
            target->sendClose();
        } else if (auto self = target->m_self.lock()) {
            dbglog("[click]   -> FOCUS idx=" + std::to_string(idx) + " title='" + target->m_title + "'");
            Desktop::focusState()->fullWindowFocus(self, Desktop::FOCUS_REASON_CLICK);
        }
        return;
    }
}

#include <fstream>
static void dbglog(const std::string& s) {
    std::ofstream f("/tmp/hyprtabbed.log", std::ios::app);
    if (f) f << s << "\n";
}

static void damageAllMonitors() {
    for (auto& m : State::monitorState()->monitors()) g_pHyprRenderer->damageMonitor(m);
}

// Group the currently-selected windows into one hy3 tab group (same workspace).
static void selectionFinalize() {
    std::vector<CWindow*> live;
    for (auto* sel : g_selected)
        for (auto& cw : Desktop::windowState()->windows())
            if (cw.get() == sel && cw->m_isMapped) { live.push_back(sel); break; }

    if (live.size() >= 2)
        if (auto fn = getHy3GroupWindows()) fn(live.data(), live.size());

    g_selected.clear();
}

static SDispatchResult dispSelectToggle(std::string) {
    if (!g_selecting) {
        g_selecting = true;
        g_selected.clear();
    } else {
        g_selecting = false;
        selectionFinalize();
    }
    dbglog(std::string("selecttoggle -> selecting=") + (g_selecting ? "1" : "0"));
    damageAllMonitors();
    return {};
}

static SDispatchResult dispSelectAdd(std::string) {
    dbglog(std::string("selectadd called, selecting=") + (g_selecting ? "1" : "0"));
    if (!g_selecting || !Desktop::focusState()) return {};
    auto w = Desktop::focusState()->window();
    if (!w) return {};
    auto* raw = w.get();
    if (auto it = std::find(g_selected.begin(), g_selected.end(), raw); it != g_selected.end())
        g_selected.erase(it); // toggle off
    else
        g_selected.push_back(raw);
    damageAllMonitors();
    return {};
}

// Lua bindings (this Hyprland routes `hyprctl dispatch` through Lua, so plugin
// dispatchers must be exposed as hl.plugin.hyprtabbed.* functions like hy3 does).
// Each returns a closure that runs the action when the keybind fires.
static int luaSelectToggle(lua_State* L) {
    lua_pushcclosure(L, [](lua_State*) -> int { dispSelectToggle(""); return 0; }, 0);
    return 1;
}
static int luaSelectAdd(lua_State* L) {
    lua_pushcclosure(L, [](lua_State*) -> int { dispSelectAdd(""); return 0; }, 0);
    return 1;
}

static void attachDecoration(PHLWINDOW window) {
    if (!window)
        return;
    HyprlandAPI::addWindowDecoration(g_pHandle, window, makeUnique<CTabbedDecoration>(window));
    // (Per-window selection dim disabled — a full-screen spotlight dim is planned instead.)
    // Reserving decorations added to already-open windows won't be positioned
    // until the window recalculates; force it so the bar shows immediately.
    if (g_pDecorationPositioner)
        g_pDecorationPositioner->forceRecalcFor(window);
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_pHandle = handle;

    g_cvBarWidth   = makeShared<cv::CFloatValue>("plugin:hyprtabbed:bar_width", "Width of the vertical tab bar in px", 28.0f);
    g_cvSide       = makeShared<cv::CStringValue>("plugin:hyprtabbed:side", "Which side the bar sits on: left or right", std::string{"left"});
    g_cvBgActive   = makeShared<cv::CColorValue>("plugin:hyprtabbed:bg_active", "Active tab background (0xAARRGGBB)", Config::INTEGER{0xff544399});
    g_cvBgInactive = makeShared<cv::CColorValue>("plugin:hyprtabbed:bg_inactive", "Inactive tab background (0xAARRGGBB)", Config::INTEGER{0xff1a142d});
    g_cvFgActive   = makeShared<cv::CColorValue>("plugin:hyprtabbed:fg_active", "Active tab text color (0xAARRGGBB)", Config::INTEGER{0xffeee4dd});
    g_cvFgInactive = makeShared<cv::CColorValue>("plugin:hyprtabbed:fg_inactive", "Inactive tab text color (0xAARRGGBB)", Config::INTEGER{0xff9a8fbf});
    g_cvFont       = makeShared<cv::CStringValue>("plugin:hyprtabbed:font", "Pango font description for tab titles", std::string{"monospace 10"});
    g_cvRounding   = makeShared<cv::CFloatValue>("plugin:hyprtabbed:rounding", "Bar corner radius in px", 10.0f);
    g_cvTuck       = makeShared<cv::CFloatValue>("plugin:hyprtabbed:tuck", "Px the bar slides under the window to hide the seam", 12.0f);
    g_cvCloseSize  = makeShared<cv::CFloatValue>("plugin:hyprtabbed:close_size", "Size in px of the per-tab close (×) glyph; 0 disables", 20.0f);
    g_cvCloseThickness = makeShared<cv::CFloatValue>("plugin:hyprtabbed:close_thickness", "Stroke weight of the × as a ratio of its size (0.02..0.5)", 0.17f);
    g_cvCloseMargin = makeShared<cv::CFloatValue>("plugin:hyprtabbed:close_margin", "Inner margin of the × as a ratio of its size (0..0.45); smaller = bigger X", 0.26f);
    g_cvClosePos   = makeShared<cv::CStringValue>("plugin:hyprtabbed:close_position", "Close button anchor in the tab: top or bottom", std::string{"top"});
    g_cvCloseOffset = makeShared<cv::CVec2Value>("plugin:hyprtabbed:close_offset", "Close button px offset {x across bar, y inward from anchor}", Config::VEC2{0.f, 0.f});
    g_cvSelectColor = makeShared<cv::CColorValue>("plugin:hyprtabbed:select_color", "Selection-mode snake border color (0xAARRGGBB)", Config::INTEGER{0xffffca8a});
    g_cvSelectThickness = makeShared<cv::CFloatValue>("plugin:hyprtabbed:select_thickness", "Selection border thickness in px", 3.0f);
    g_cvSelectDim   = makeShared<cv::CFloatValue>("plugin:hyprtabbed:select_dim", "Dim of non-selected windows in selection mode (0..1)", 0.45f);

    HyprlandAPI::addConfigValueV2(handle, g_cvBarWidth);
    HyprlandAPI::addConfigValueV2(handle, g_cvSide);
    HyprlandAPI::addConfigValueV2(handle, g_cvBgActive);
    HyprlandAPI::addConfigValueV2(handle, g_cvBgInactive);
    HyprlandAPI::addConfigValueV2(handle, g_cvFgActive);
    HyprlandAPI::addConfigValueV2(handle, g_cvFgInactive);
    HyprlandAPI::addConfigValueV2(handle, g_cvFont);
    HyprlandAPI::addConfigValueV2(handle, g_cvRounding);
    HyprlandAPI::addConfigValueV2(handle, g_cvTuck);
    HyprlandAPI::addConfigValueV2(handle, g_cvCloseSize);
    HyprlandAPI::addConfigValueV2(handle, g_cvCloseThickness);
    HyprlandAPI::addConfigValueV2(handle, g_cvCloseMargin);
    HyprlandAPI::addConfigValueV2(handle, g_cvClosePos);
    HyprlandAPI::addConfigValueV2(handle, g_cvCloseOffset);
    HyprlandAPI::addConfigValueV2(handle, g_cvSelectColor);
    HyprlandAPI::addConfigValueV2(handle, g_cvSelectThickness);
    HyprlandAPI::addConfigValueV2(handle, g_cvSelectDim);

    HyprlandAPI::addDispatcherV2(handle, "hyprtabbed:selecttoggle", dispSelectToggle);
    HyprlandAPI::addDispatcherV2(handle, "hyprtabbed:selectadd", dispSelectAdd);
    HyprlandAPI::addLuaFunction(handle, "hyprtabbed", "select_toggle", luaSelectToggle);
    HyprlandAPI::addLuaFunction(handle, "hyprtabbed", "select_add", luaSelectAdd);

    g_listenerOpen = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { attachDecoration(w); });
    g_listenerMouseBtn = Event::bus()->m_events.input.mouse.button.listen(onMouseButton);

    for (const auto& w : Desktop::windowState()->windows())
        attachDecoration(w);

    HyprlandAPI::reloadConfig();

    return {"hyprtabbed", "Vertical side tab bar for window groups with rotated titles", "whax", "0.1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listenerOpen.reset();
    g_listenerMouseBtn.reset();
    g_closeCache.clear();
    g_labels.clear();
    g_bgCache.clear();
    g_cvBarWidth.reset();
    g_cvSide.reset();
    g_cvBgActive.reset();
    g_cvBgInactive.reset();
    g_cvFgActive.reset();
    g_cvFgInactive.reset();
    g_cvFont.reset();
    g_cvRounding.reset();
    g_cvTuck.reset();
    g_cvCloseSize.reset();
    g_cvCloseThickness.reset();
    g_cvCloseMargin.reset();
    g_cvClosePos.reset();
    g_cvCloseOffset.reset();
    g_cvSelectColor.reset();
    g_cvSelectThickness.reset();
    g_cvSelectDim.reset();
    g_selected.clear();
}
