// zoomparent: yabai-style "--toggle zoom-parent" for Hyprland's dwindle layout.
//
// Adds a dwindle layout message, used as hl.dsp.layout("zoomparent"), that
// toggles the focused tiled window between its own tile and the area of its
// parent split. The window's sibling(s) keep their tiles underneath; the
// zoomed window is raised so it covers them.
//
// Built against Hyprland 0.57.2. It hooks internal functions, so it must be
// rebuilt (make) after every Hyprland update.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/layout/algorithm/tiled/dwindle/DwindleAlgorithm.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>

#include <algorithm>
#include <ranges>
#include <string_view>
#include <vector>

using namespace Layout::Tiled;

namespace {
    HANDLE                    g_handle            = nullptr;
    CFunctionHook*            g_recalcHook        = nullptr;
    CFunctionHook*            g_layoutMsgHook     = nullptr;
    CFunctionHook*            g_windowAtHook      = nullptr;
    std::vector<PHLWINDOWREF> g_zoomed;

    using recalcFn    = void (*)(SDwindleNodeData*, bool, bool, bool);
    using layoutMsgFn = Config::ErrorResult (*)(CDwindleAlgorithm*, const std::string_view&);
    using windowAtFn  = PHLWINDOW (*)(const void*, const Vector2D&, uint16_t, PHLWINDOW, PHLWORKSPACE);

    // Hyprland's hit test checks tiled windows bottom-to-top, assuming tiles
    // never overlap. A zoomed window does overlap its sibling, so without this
    // the window underneath would win (and get focused on hover). If the hit
    // is a tiled window and a zoomed window on the same workspace covers the
    // point, return the zoomed window instead.
    PHLWINDOW hkWindowAtInternal(const void* thisptr, const Vector2D& pos, uint16_t properties, PHLWINDOW ignoreWindow, PHLWORKSPACE workspace) {
        auto found = ((windowAtFn)g_windowAtHook->m_original)(thisptr, pos, properties, ignoreWindow, workspace);

        if (!found || found->isFloating() || g_zoomed.empty())
            return found;

        for (const auto& ref : g_zoomed | std::views::reverse) {
            const auto ZOOMED = ref.lock();
            if (!ZOOMED || ZOOMED == found || ZOOMED == ignoreWindow || ZOOMED->isFloating() || !ZOOMED->mapped() || ZOOMED->isHidden())
                continue;
            if (ZOOMED->m_workspace != found->m_workspace)
                continue;
            if (ZOOMED->layoutBox().containsPoint(pos))
                return ZOOMED;
        }

        return found;
    }

    // Tiled windows we faded out because a zoomed window covers them.
    std::vector<PHLWINDOWREF> g_faded;

    bool isZoomed(const PHLWINDOW& w);

    // Is this tiled window fully inside a zoomed window's (final) layout box?
    bool coveredByZoom(const PHLWINDOW& w) {
        if (g_zoomed.empty() || !w || w->isFloating() || isZoomed(w))
            return false;

        const auto BOX = w->layoutBox();

        for (const auto& ref : g_zoomed) {
            const auto ZOOMED = ref.lock();
            if (!ZOOMED || ZOOMED->isFloating() || !ZOOMED->mapped() || ZOOMED->isHidden())
                continue;
            if (ZOOMED->m_workspace != w->m_workspace)
                continue;

            const auto       Z   = ZOOMED->layoutBox();
            constexpr double EPS = 2.0;
            if (BOX.x >= Z.x - EPS && BOX.y >= Z.y - EPS && BOX.x + BOX.w <= Z.x + Z.w + EPS && BOX.y + BOX.h <= Z.y + Z.h + EPS)
                return true;
        }

        return false;
    }

    // Animate a window's "fullscreen" alpha, the same value Hyprland fades to
    // 0 for windows under a maximized one (using the fadeIn animation). The
    // renderer already skips windows whose alpha reached 0.
    void fadeTo(const PHLWINDOW& w, float target) {
        if (w->isBlockedByFullscreen()) // real fullscreen/maximize owns it
            return;
        auto& alpha = w->presentation().alpha(Desktop::View::WINDOW_ALPHA_FULLSCREEN);
        if (alpha->goal() != target)
            *alpha = target;
    }

    // Like maximize: fade out tiled windows a zoomed window fully covers, and
    // fade back in the ones that are no longer covered.
    void updateFaded() {
        std::vector<PHLWINDOWREF> nowCovered;
        for (const auto& w : Desktop::windowState()->windows()) {
            if (w->mapped() && coveredByZoom(w))
                nowCovered.emplace_back(w);
        }

        for (const auto& ref : g_faded) {
            const auto W = ref.lock();
            if (W && !std::ranges::any_of(nowCovered, [&](const PHLWINDOWREF& c) { return c.lock() == W; }))
                fadeTo(W, 1.F);
        }

        for (const auto& ref : nowCovered)
            fadeTo(ref.lock(), 0.F);

        g_faded = std::move(nowCovered);
    }

    void pruneDead() {
        std::erase_if(g_zoomed, [](const PHLWINDOWREF& w) { return w.expired(); });
    }

    bool isZoomed(const PHLWINDOW& w) {
        return std::ranges::any_of(g_zoomed, [&](const PHLWINDOWREF& z) { return z.lock() == w; });
    }

    // Give every zoomed leaf in this subtree its parent's box.
    void applyZoom(SDwindleNodeData* node) {
        if (!node)
            return;

        if (node->children[0]) {
            applyZoom(node->children[0].get());
            applyZoom(node->children[1].get());
            return;
        }

        if (node->pTarget.expired() || node->pParent.expired())
            return;

        const auto TARGET = node->pTarget.lock();
        const auto WINDOW = TARGET->window();
        if (WINDOW && isZoomed(WINDOW))
            TARGET->setPositionGlobal(node->pParent->box);
    }

    void hkRecalcSizePosRecursive(SDwindleNodeData* thisptr, bool force, bool horizontalOverride, bool verticalOverride) {
        ((recalcFn)g_recalcHook->m_original)(thisptr, force, horizontalOverride, verticalOverride);

        // The compiled original handles its second child with an internal loop
        // instead of a call, so leaves don't all pass through this hook. Walk
        // the subtree ourselves after the original has positioned everything.
        if (!g_zoomed.empty())
            applyZoom(thisptr);

        // Also runs once zoom ends (e.g. the zoomed window closed), so faded
        // windows come back.
        if (!g_zoomed.empty() || !g_faded.empty()) {
            pruneDead();
            updateFaded();
        }
    }

    Config::ErrorResult hkLayoutMsg(CDwindleAlgorithm* thisptr, const std::string_view& sv) {
        if (sv != "zoomparent")
            return ((layoutMsgFn)g_layoutMsgHook->m_original)(thisptr, sv);

        pruneDead();

        const auto WINDOW = Desktop::focusState()->window();
        const auto NODE   = WINDOW ? thisptr->getNodeFromWindow(WINDOW) : nullptr;
        if (!NODE)
            return Config::configError("zoomparent: no focused tiled window", Config::eConfigErrorLevel::WARNING, Config::eConfigErrorCode::INVALID_STATE);

        if (isZoomed(WINDOW))
            std::erase_if(g_zoomed, [&](const PHLWINDOWREF& z) { return z.lock() == WINDOW; });
        else {
            g_zoomed.emplace_back(WINDOW);
            Desktop::windowState()->raise(WINDOW);
        }

        thisptr->recalculate();
        updateFaded();
        return {};
    }

    void* findOne(const std::string& name, std::string_view mustContain) {
        for (const auto& fn : HyprlandAPI::findFunctionsByName(g_handle, name)) {
            if (fn.demangled.find(mustContain) != std::string::npos)
                return fn.address;
        }
        return nullptr;
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    g_handle = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();
    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(handle, "[zoomparent] built for a different Hyprland version, rebuild it", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[zoomparent] version mismatch");
    }

    const auto RECALC    = findOne("recalcSizePosRecursive", "SDwindleNodeData::recalcSizePosRecursive");
    const auto LAYOUTMSG = findOne("layoutMsg", "CDwindleAlgorithm::layoutMsg");
    const auto WINDOWAT  = findOne("windowAtInternal", "CViewHitTester::windowAtInternal");
    if (!RECALC || !LAYOUTMSG || !WINDOWAT) {
        HyprlandAPI::addNotification(handle, "[zoomparent] dwindle functions not found", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[zoomparent] functions not found");
    }

    g_recalcHook    = HyprlandAPI::createFunctionHook(handle, RECALC, (void*)&hkRecalcSizePosRecursive);
    g_layoutMsgHook = HyprlandAPI::createFunctionHook(handle, LAYOUTMSG, (void*)&hkLayoutMsg);
    g_windowAtHook  = HyprlandAPI::createFunctionHook(handle, WINDOWAT, (void*)&hkWindowAtInternal);

    if (!g_recalcHook || !g_layoutMsgHook || !g_windowAtHook || !g_recalcHook->hook() || !g_layoutMsgHook->hook() || !g_windowAtHook->hook()) {
        HyprlandAPI::addNotification(handle, "[zoomparent] failed to hook dwindle", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[zoomparent] hook failed");
    }

    return {"zoomparent", "yabai-style zoom-parent for the dwindle layout", "zaxiure", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_zoomed.clear();
    // bring back anything we faded out
    for (const auto& ref : g_faded) {
        if (const auto W = ref.lock())
            fadeTo(W, 1.F);
    }
    g_faded.clear();
    // Hooks are removed by Hyprland when the plugin unloads.
}
