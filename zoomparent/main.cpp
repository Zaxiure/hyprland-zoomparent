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
    CFunctionHook*            g_shouldRenderHook  = nullptr;
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

    using shouldRenderFn = bool (*)(void*, PHLWINDOW, PHLMONITOR, PHLWORKSPACE, uint8_t);

    // Like maximize: don't draw tiled windows a zoomed window fully covers.
    // Uses the current (animated) boxes, so a window stays visible until the
    // zoom animation has actually covered it.
    bool coveredByZoom(const PHLWINDOW& w) {
        if (g_zoomed.empty() || !w || w->isFloating())
            return false;

        const auto BOX = w->getWindowMainSurfaceBox();

        for (const auto& ref : g_zoomed) {
            const auto ZOOMED = ref.lock();
            if (!ZOOMED || ZOOMED == w || ZOOMED->isFloating() || !ZOOMED->mapped() || ZOOMED->isHidden())
                continue;
            if (ZOOMED->m_workspace != w->m_workspace)
                continue;

            const auto  Z   = ZOOMED->getWindowMainSurfaceBox();
            constexpr double EPS = 2.0;
            if (BOX.x >= Z.x - EPS && BOX.y >= Z.y - EPS && BOX.x + BOX.w <= Z.x + Z.w + EPS && BOX.y + BOX.h <= Z.y + Z.h + EPS)
                return true;
        }

        return false;
    }

    bool hkShouldRenderWindowInScene(void* thisptr, PHLWINDOW window, PHLMONITOR monitor, PHLWORKSPACE workspace, uint8_t mode) {
        if (coveredByZoom(window))
            return false;
        return ((shouldRenderFn)g_shouldRenderHook->m_original)(thisptr, window, monitor, workspace, mode);
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
    const auto SHOULDRENDER = findOne("shouldRenderWindowInScene", "IHyprRenderer::shouldRenderWindowInScene");
    if (!RECALC || !LAYOUTMSG || !WINDOWAT || !SHOULDRENDER) {
        HyprlandAPI::addNotification(handle, "[zoomparent] dwindle functions not found", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[zoomparent] functions not found");
    }

    g_recalcHook    = HyprlandAPI::createFunctionHook(handle, RECALC, (void*)&hkRecalcSizePosRecursive);
    g_layoutMsgHook = HyprlandAPI::createFunctionHook(handle, LAYOUTMSG, (void*)&hkLayoutMsg);
    g_windowAtHook  = HyprlandAPI::createFunctionHook(handle, WINDOWAT, (void*)&hkWindowAtInternal);
    g_shouldRenderHook = HyprlandAPI::createFunctionHook(handle, SHOULDRENDER, (void*)&hkShouldRenderWindowInScene);

    if (!g_recalcHook || !g_layoutMsgHook || !g_windowAtHook || !g_shouldRenderHook || !g_recalcHook->hook() || !g_layoutMsgHook->hook() || !g_windowAtHook->hook() ||
        !g_shouldRenderHook->hook()) {
        HyprlandAPI::addNotification(handle, "[zoomparent] failed to hook dwindle", CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
        throw std::runtime_error("[zoomparent] hook failed");
    }

    return {"zoomparent", "yabai-style zoom-parent for the dwindle layout", "zaxiure", "0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_zoomed.clear();
    // Hooks are removed by Hyprland when the plugin unloads.
}
