#include "XSurface.hpp"
#include "XWayland.hpp"
#include "../protocols/XWaylandShell.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../helpers/time/Time.hpp"
#include "../helpers/env/Env.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <ranges>

#ifndef NO_XWAYLAND

struct SFreeDeleter {
    void operator()(void* ptr) const {
        std::free(ptr); // NOLINT(cppcoreguidelines-no-malloc)
    }
};

template <typename T>
using XCBReplyPtr = std::unique_ptr<T, SFreeDeleter>;

static constexpr std::chrono::milliseconds SYNC_REQUEST_TIMEOUT = std::chrono::milliseconds(1000);

CXWaylandSurface::CXWaylandSurface(uint32_t xID_, CBox geometry_, bool OR) : m_xID(xID_), m_geometry(geometry_), m_overrideRedirect(OR) {
    m_allowCommitsSupported = !Env::envEnabled("HYPRLAND_NO_XWAYLAND_ALLOW_COMMITS");

    xcb_res_query_client_ids_cookie_t client_id_cookie = {0};
    if (g_pXWayland->m_wm->m_xres) {
        xcb_res_client_id_spec_t spec = {.client = m_xID, .mask = XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
        client_id_cookie              = xcb_res_query_client_ids(g_pXWayland->m_wm->getConnection(), 1, &spec);
    }

    uint32_t values[1];
    values[0] = XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(g_pXWayland->m_wm->getConnection(), m_xID, XCB_CW_EVENT_MASK, values);

    if (g_pXWayland->m_wm->m_xres) {
        xcb_res_query_client_ids_reply_t* reply = xcb_res_query_client_ids_reply(g_pXWayland->m_wm->getConnection(), client_id_cookie, nullptr);
        if (!reply)
            return;

        uint32_t*                          ppid = nullptr;
        xcb_res_client_id_value_iterator_t iter = xcb_res_query_client_ids_ids_iterator(reply);
        while (iter.rem > 0) {
            if (iter.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID && xcb_res_client_id_value_value_length(iter.data) > 0) {
                ppid = xcb_res_client_id_value_value(iter.data);
                break;
            }
            xcb_res_client_id_value_next(&iter);
        }
        if (!ppid) {
            free(reply); // NOLINT(cppcoreguidelines-no-malloc)
            return;
        }
        m_pid = *ppid;
        free(reply); // NOLINT(cppcoreguidelines-no-malloc)
    }

    // FIXME: this is a race, we need to listen to props changed
    recheckSupportedProps();

    m_events.resourceChange.listenStatic([this] { ensureListeners(); });
}

CXWaylandSurface::~CXWaylandSurface() {
    destroySync();
}

void CXWaylandSurface::recheckSupportedProps() {
    m_supportedProps.clear();

    auto  listCookie = xcb_list_properties(g_pXWayland->m_wm->getConnection(), m_xID);
    auto* listReply  = xcb_list_properties_reply(g_pXWayland->m_wm->getConnection(), listCookie, nullptr);
    auto  getCookie  = xcb_get_property(g_pXWayland->m_wm->getConnection(), 0, m_xID, HYPRATOMS["WM_PROTOCOLS"], XCB_ATOM_ATOM, 0, 32);
    auto* getReply   = xcb_get_property_reply(g_pXWayland->m_wm->getConnection(), getCookie, nullptr);

    if (listReply) {
        const auto* atoms = xcb_list_properties_atoms(listReply);
        auto        len   = xcb_list_properties_atoms_length(listReply);

        for (auto i = 0; i < len; ++i) {
            m_supportedProps[atoms[i]] = true;
        }

        free(listReply);
    }

    if (getReply) {
        const auto* protocols = sc<xcb_atom_t*>(xcb_get_property_value(getReply));
        const auto  len       = xcb_get_property_value_length(getReply) / sizeof(xcb_atom_t);

        for (auto i = 0u; i < len; ++i) {
            m_supportedProps[protocols[i]] = true;
        }

        free(getReply);
    }
}

void CXWaylandSurface::ensureListeners() {
    bool connected = !!m_listeners.destroySurface;

    if (connected && !m_surface) {
        m_listeners.destroySurface.reset();
        m_listeners.commitSurface.reset();
    } else if (!connected && m_surface) {
        m_listeners.destroySurface = m_surface->m_events.destroy.listen([this] {
            if (m_mapped)
                unmap();

            destroySync();

            m_surface.reset();
            m_listeners.destroySurface.reset();
            m_listeners.commitSurface.reset();
            m_events.resourceChange.emit();
        });

        m_listeners.commitSurface = m_surface->m_events.commit.listen([this] {
            if (m_surface->m_current.texture && !m_mapped) {
                map();
                return;
            }

            if (!m_surface->m_current.texture && m_mapped) {
                unmap();
                return;
            }

            // the client acked and produced the new buffer: release the sync hold
            if (m_sync.acked && m_surface->m_current.updated.bits.buffer)
                finishSync();

            m_events.commit.emit();
        });
    }

    if (m_resource) {
        m_listeners.destroyResource = m_resource->events.destroy.listen([this] {
            unmap();
            destroySync();
            m_surface.reset();
            m_events.resourceChange.emit();
        });
    }
}

void CXWaylandSurface::map() {
    if (m_mapped)
        return;

    ASSERT(m_surface);

    g_pXWayland->m_wm->m_mappedSurfaces.emplace_back(m_self);
    g_pXWayland->m_wm->m_mappedSurfacesStacking.emplace_back(m_self);

    m_mapped = true;
    m_surface->map();

    LOG(Log::DEBUG, "XWayland surface {:x} mapping", rc<uintptr_t>(this));

    m_events.map.emit();

    g_pXWayland->m_wm->updateClientList();
}

void CXWaylandSurface::unmap() {
    if (!m_mapped)
        return;

    ASSERT(m_surface);

    std::erase(g_pXWayland->m_wm->m_mappedSurfaces, m_self);
    std::erase(g_pXWayland->m_wm->m_mappedSurfacesStacking, m_self);

    m_mapped = false;
    m_events.unmap.emit();
    m_surface->unmap();

    LOG(Log::DEBUG, "XWayland surface {:x} unmapping", rc<uintptr_t>(this));

    g_pXWayland->m_wm->updateClientList();

    // windows may remap; drop the sync state so detection can set it up again
    destroySync();
}

void CXWaylandSurface::considerMap() {
    if (m_mapped)
        return;

    if (!m_surface) {
        LOG(Log::DEBUG, "XWayland surface: considerMap, nope, no surface");
        return;
    }

    if (m_surface->m_current.texture) {
        LOG(Log::DEBUG, "XWayland surface: considerMap, sure, we have a buffer");
        map();
        return;
    }

    LOG(Log::DEBUG, "XWayland surface: considerMap, nope, we don't have a buffer");
}

bool CXWaylandSurface::wantsFocus() {
    if (m_atoms.empty())
        return true;

    const std::array<uint32_t, 10> search = {
        HYPRATOMS["_NET_WM_WINDOW_TYPE_COMBO"],   HYPRATOMS["_NET_WM_WINDOW_TYPE_DND"],          HYPRATOMS["_NET_WM_WINDOW_TYPE_DROPDOWN_MENU"],
        HYPRATOMS["_NET_WM_WINDOW_TYPE_MENU"],    HYPRATOMS["_NET_WM_WINDOW_TYPE_NOTIFICATION"], HYPRATOMS["_NET_WM_WINDOW_TYPE_POPUP_MENU"],
        HYPRATOMS["_NET_WM_WINDOW_TYPE_SPLASH"],  HYPRATOMS["_NET_WM_WINDOW_TYPE_DESKTOP"],      HYPRATOMS["_NET_WM_WINDOW_TYPE_TOOLTIP"],
        HYPRATOMS["_NET_WM_WINDOW_TYPE_UTILITY"],
    };

    for (auto const& searched : search) {
        for (auto const& a : m_atoms) {
            if (a == searched)
                return false;
        }
    }

    return true;
}

void CXWaylandSurface::configure(const CBox& box) {
    Vector2D oldSize = m_geometry.size();

    m_geometry = box;

    if (m_surface)
        m_surface->m_pending.updated.bits.xwayland = true;

    // gate the resize on the client's repaint: _XWAYLAND_ALLOW_COMMITS=0 and the sync
    // request must both go out before the ConfigureNotify below. Pure moves never sync.
    const bool SIZECHANGED = box.width != oldSize.x || box.height != oldSize.y;
    if (m_mapped && m_surface && SIZECHANGED && m_sync.enabled && !m_sync.pending)
        sendSyncRequest();

    uint32_t mask     = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH;
    uint32_t values[] = {box.x, box.y, box.width, box.height, 0};
    xcb_configure_window(g_pXWayland->m_wm->getConnection(), m_xID, mask, values);

    if (box.width == oldSize.x && box.height == oldSize.y) {
        // ICCCM requires a synthetic event when window size is not changed
        xcb_configure_notify_event_t e;
        e.response_type     = XCB_CONFIGURE_NOTIFY;
        e.event             = m_xID;
        e.window            = m_xID;
        e.x                 = box.x;
        e.y                 = box.y;
        e.width             = box.width;
        e.height            = box.height;
        e.border_width      = 0;
        e.above_sibling     = XCB_NONE;
        e.override_redirect = m_overrideRedirect;
        xcb_send_event(g_pXWayland->m_wm->getConnection(), false, m_xID, XCB_EVENT_MASK_STRUCTURE_NOTIFY, rc<const char*>(&e));
    }

    g_pXWayland->m_wm->updateClientList();

    xcb_flush(g_pXWayland->m_wm->getConnection());
}

bool CXWaylandSurface::waitingForSync() const {
    return m_sync.enabled && (m_sync.pending || m_sync.acked);
}

void CXWaylandSurface::updateSyncSupport() {
    if (m_overrideRedirect)
        return;

    const auto         CONNECTION = g_pXWayland->m_wm->getConnection();
    const bool         SYNCEXT    = g_pXWayland->m_wm->m_sync && g_pXWayland->m_wm->m_sync->present;

    xcb_sync_counter_t counter = XCB_NONE;
    if (SYNCEXT) {
        const auto                            COOKIE = xcb_get_property(CONNECTION, 0, m_xID, HYPRATOMS["_NET_WM_SYNC_REQUEST_COUNTER"], XCB_ATOM_CARDINAL, 0, 1);
        XCBReplyPtr<xcb_get_property_reply_t> REPLY(xcb_get_property_reply(CONNECTION, COOKIE, nullptr));
        if (REPLY && REPLY->type == XCB_ATOM_CARDINAL && REPLY->format == 32 && xcb_get_property_value_length(REPLY.get()) >= sc<int>(sizeof(xcb_sync_counter_t)))
            counter = *sc<const xcb_sync_counter_t*>(xcb_get_property_value(REPLY.get()));
    }

    // m_protocols is authoritative: m_supportedProps races with the WM_PROTOCOLS read
    const bool PROTOCOL = std::ranges::find(m_protocols, HYPRATOMS["_NET_WM_SYNC_REQUEST"]) != m_protocols.end();
    if (!SYNCEXT || !PROTOCOL || counter == XCB_NONE) {
        if (m_sync.enabled || m_sync.pending || m_sync.acked || m_sync.alarm != XCB_NONE)
            destroySync();
        return;
    }

    if (m_sync.enabled && m_sync.counter == counter)
        return; // already set up for this counter

    // (re)initialize from scratch so the alarm test value matches a fresh serial sequence
    if (m_sync.enabled || m_sync.pending || m_sync.acked || m_sync.alarm != XCB_NONE || m_sync.timeout)
        destroySync();

    m_sync.counter = counter;
    m_sync.enabled = true;
    m_sync.value   = {0, 0};
    xcb_sync_set_counter(CONNECTION, counter, m_sync.value);

    if (m_sync.alarm == XCB_NONE) {
        const uint32_t MASK     = XCB_SYNC_CA_COUNTER | XCB_SYNC_CA_VALUE_TYPE | XCB_SYNC_CA_TEST_TYPE | XCB_SYNC_CA_EVENTS;
        const uint32_t VALUES[] = {
            m_sync.counter,
            XCB_SYNC_VALUETYPE_RELATIVE,
            XCB_SYNC_TESTTYPE_POSITIVE_TRANSITION,
            1,
        };
        m_sync.alarm                            = xcb_generate_id(CONNECTION);
        const auto                       COOKIE = xcb_sync_create_alarm_checked(CONNECTION, m_sync.alarm, MASK, VALUES);
        XCBReplyPtr<xcb_generic_error_t> ERROR(xcb_request_check(CONNECTION, COOKIE));
        if (ERROR) {
            LOG(Log::ERR, "[xwm] Failed to create the XSync alarm for window {}", m_xID);
            m_sync.alarm   = XCB_NONE;
            m_sync.enabled = false;
            return;
        }

        xcb_sync_change_alarm_value_list_t VALUE = {};
        VALUE.value.hi                           = 0;
        VALUE.value.lo                           = 1;
        VALUE.delta.hi                           = 0;
        VALUE.delta.lo                           = 1;
        xcb_sync_change_alarm_aux(CONNECTION, m_sync.alarm, XCB_SYNC_CA_DELTA | XCB_SYNC_CA_VALUE, &VALUE);
    }
}

void CXWaylandSurface::sendSyncRequest() {
    if (!m_sync.enabled || m_sync.pending)
        return; // do NOT, NEVER send a sync request when there's one on the stack

    // a new request supersedes an ack whose buffer commit has not landed yet
    m_sync.acked = false;

    if (!m_sync.timeout) {
        const auto SELF = m_self;
        m_sync.timeout  = makeShared<CEventLoopTimer>(
            std::nullopt,
            [SELF](SP<CEventLoopTimer>, void*) {
                if (const auto SURFACE = SELF.lock())
                    SURFACE->ackSyncTimeout();
            },
            nullptr);
        g_pEventLoopManager->addTimer(m_sync.timeout);
    }
    m_sync.timeout->updateTimeout(SYNC_REQUEST_TIMEOUT);

    // increment before the notify so that the ack equals the value we are expecting
    const uint32_t oldLo = m_sync.value.lo;
    m_sync.value.lo++;
    if (oldLo > m_sync.value.lo)
        m_sync.value.hi++;

    // must go out before the ClientMessage AND before the ConfigureNotify below
    setAllowCommits(false);

    xcb_client_message_data_t msg = {};
    msg.data32[0]                 = HYPRATOMS["_NET_WM_SYNC_REQUEST"];
    msg.data32[1]                 = Time::millis(Time::steadyNow());
    msg.data32[2]                 = m_sync.value.lo;
    msg.data32[3]                 = sc<uint32_t>(m_sync.value.hi);

    g_pXWayland->m_wm->sendWMMessage(m_self.lock(), &msg, XCB_EVENT_MASK_NO_EVENT);

    m_sync.pending = true;
}

void CXWaylandSurface::ackSync() {
    // a sync request can be ack'ed after the timeout. If that happens, just re-enable
    // XSync back and do nothing more.
    m_sync.pending = false;
    if (!m_sync.enabled) {
        m_sync.enabled = true;
        return;
    }

    m_sync.acked = true;
    // the timer stays armed until the new buffer commits (finishSync stops it)
    setAllowCommits(true);
}

void CXWaylandSurface::ackSyncTimeout() {
    // If a sync request times out, disable XSync temporarily until the client comes back to its senses.
    m_sync.enabled = false;
    m_sync.pending = false;
    m_sync.acked   = false;
    setAllowCommits(true);
    finishSync(); // stops the timer and emits syncFinished
}

void CXWaylandSurface::finishSync() {
    if (m_sync.timeout)
        m_sync.timeout->updateTimeout(std::nullopt);

    m_sync.acked = false;
    m_events.syncFinished.emit();
}

void CXWaylandSurface::destroySync() {
    const bool HOLDING = m_sync.pending || m_sync.acked;
    const bool CANX    = g_pXWayland && g_pXWayland->m_wm;

    if (m_sync.timeout) {
        m_sync.timeout->cancel();
        m_sync.timeout.reset();
    }

    // only write when we actually blocked commits, avoids noise on plain teardown
    if (m_sync.pending && CANX)
        setAllowCommits(true);

    if (m_sync.alarm != XCB_NONE) {
        if (CANX) {
            xcb_sync_destroy_alarm(g_pXWayland->m_wm->getConnection(), m_sync.alarm);
            xcb_flush(g_pXWayland->m_wm->getConnection());
        }
        m_sync.alarm = XCB_NONE;
    }

    m_sync.counter = XCB_NONE;
    m_sync.value   = {0, 0};
    m_sync.enabled = false;
    m_sync.pending = false;
    m_sync.acked   = false;

    if (HOLDING)
        m_events.syncFinished.emit();
}

void CXWaylandSurface::setAllowCommits(bool allow) {
    if (!m_allowCommitsSupported)
        return;

    uint32_t value = allow;
    xcb_change_property(g_pXWayland->m_wm->getConnection(), XCB_PROP_MODE_REPLACE, m_xID, HYPRATOMS["_XWAYLAND_ALLOW_COMMITS"], XCB_ATOM_CARDINAL, 32, 1, &value);
    // fail-open and teardown run outside CXWM::onEvent (timer callback / destructor), so the
    // queued write must be flushed here or XWayland keeps commits blocked.
    xcb_flush(g_pXWayland->m_wm->getConnection());
}

void CXWaylandSurface::activate(bool activate) {
    if (m_overrideRedirect && !activate)
        return;
    setWithdrawn(false);
    g_pXWayland->m_wm->activateSurface(m_self.lock(), activate);
}

void CXWaylandSurface::setFullscreen(bool fs) {
    m_fullscreen = fs;
    g_pXWayland->m_wm->sendState(m_self.lock());
}

void CXWaylandSurface::setMinimized(bool mz) {
    m_minimized = mz;
    g_pXWayland->m_wm->sendState(m_self.lock());
}

void CXWaylandSurface::restackToTop() {
    uint32_t values[1] = {XCB_STACK_MODE_ABOVE};

    xcb_configure_window(g_pXWayland->m_wm->getConnection(), m_xID, XCB_CONFIG_WINDOW_STACK_MODE, values);

    auto& stack = g_pXWayland->m_wm->m_mappedSurfacesStacking;
    auto  it    = std::ranges::find(stack, m_self);

    if (it != stack.end())
        std::rotate(it, it + 1, stack.end());

    g_pXWayland->m_wm->updateClientList();

    xcb_flush(g_pXWayland->m_wm->getConnection());
}

void CXWaylandSurface::close() {

    // Recheck the supported props, check if we maybe have WM_DELETE_WINDOW.
    recheckSupportedProps();

    if (m_supportedProps[HYPRATOMS["WM_DELETE_WINDOW"]]) {
        xcb_client_message_data_t msg = {};
        msg.data32[0]                 = HYPRATOMS["WM_DELETE_WINDOW"];
        msg.data32[1]                 = XCB_CURRENT_TIME;
        g_pXWayland->m_wm->sendWMMessage(m_self.lock(), &msg, XCB_EVENT_MASK_NO_EVENT);
    } else {
        xcb_kill_client(g_pXWayland->m_wm->getConnection(), m_self->m_xID);
        xcb_flush(g_pXWayland->m_wm->getConnection());
    }
}

void CXWaylandSurface::setWithdrawn(bool withdrawn_) {
    m_withdrawn                 = withdrawn_;
    std::vector<uint32_t> props = {XCB_ICCCM_WM_STATE_NORMAL, XCB_WINDOW_NONE};

    if (m_withdrawn)
        props[0] = XCB_ICCCM_WM_STATE_WITHDRAWN;
    else if (m_minimized)
        props[0] = XCB_ICCCM_WM_STATE_ICONIC;
    else
        props[0] = XCB_ICCCM_WM_STATE_NORMAL;

    xcb_change_property(g_pXWayland->m_wm->getConnection(), XCB_PROP_MODE_REPLACE, m_xID, HYPRATOMS["WM_STATE"], HYPRATOMS["WM_STATE"], 32, props.size(), props.data());
}

void CXWaylandSurface::ping() {
    bool supportsPing = std::ranges::find(m_protocols, HYPRATOMS["_NET_WM_PING"]) != m_protocols.end();

    if (!supportsPing) {
        LOG(Log::TRACE, "CXWaylandSurface: XID {} does not support ping, just sending an instant reply", m_xID);
        m_events.pong.emit();
        return;
    }

    xcb_client_message_data_t msg = {};
    msg.data32[0]                 = HYPRATOMS["_NET_WM_PING"];
    msg.data32[1]                 = Time::millis(Time::steadyNow());
    msg.data32[2]                 = m_xID;

    m_lastPingSeq = msg.data32[1];

    g_pXWayland->m_wm->sendWMMessage(m_self.lock(), &msg, XCB_EVENT_MASK_PROPERTY_CHANGE);
}

#else

CXWaylandSurface::CXWaylandSurface(uint32_t xID_, CBox geometry_, bool OR) : m_xID(xID_), m_geometry(geometry_), m_overrideRedirect(OR) {
    ;
}

CXWaylandSurface::~CXWaylandSurface() {
    ;
}

void CXWaylandSurface::ensureListeners() {
    ;
}

void CXWaylandSurface::map() {
    ;
}

void CXWaylandSurface::unmap() {
    ;
}

bool CXWaylandSurface::wantsFocus() {
    return false;
}

bool CXWaylandSurface::waitingForSync() const {
    return false;
}

void CXWaylandSurface::updateSyncSupport() {
    ;
}

void CXWaylandSurface::sendSyncRequest() {
    ;
}

void CXWaylandSurface::ackSync() {
    ;
}

void CXWaylandSurface::ackSyncTimeout() {
    ;
}

void CXWaylandSurface::finishSync() {
    ;
}

void CXWaylandSurface::destroySync() {
    ;
}

void CXWaylandSurface::setAllowCommits(bool allow) {
    ;
}

void CXWaylandSurface::configure(const CBox& box) {
    ;
}

void CXWaylandSurface::activate(bool activate) {
    ;
}

void CXWaylandSurface::setFullscreen(bool fs) {
    ;
}

void CXWaylandSurface::setMinimized(bool mz) {
    ;
}

void CXWaylandSurface::restackToTop() {
    ;
}

void CXWaylandSurface::close() {
    ;
}

void CXWaylandSurface::considerMap() {
    ;
}

void CXWaylandSurface::setWithdrawn(bool withdrawn) {
    ;
}

void CXWaylandSurface::ping() {
    ;
}

#endif
