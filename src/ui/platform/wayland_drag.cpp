// src/ui/platform/wayland_drag.cpp — Wayland drag source ATTEMPT, see outgoing_drag.hpp and
// docs/dnd.md. SDL 3.4 owns the one wl_seat / wl_pointer / wl_data_device it needs for itself
// and exposes only the wl_display and wl_surface. A data-device drag needs the serial of the
// button press that started it, so this binds a SECOND wl_seat, wl_pointer and wl_data_device
// on a private event queue of SDL's wl_display: the compositor sends button events to every
// pointer object of the client, and the serial of that press is valid for wl_data_device
// start_drag on the same client. Whether Mutter / Muffin / Weston accept this is the open
// question the roadmap asks to test (docs/dnd.md lists what was and was not verified); when any
// step fails the drag is "not available" and the badge points at Ctrl+C / Ctrl+V.
#include <SDL3/SDL.h>

#include "platform/outgoing_drag.hpp"

#ifdef NAVIGATOR_UI_HAVE_WAYLAND
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>
#include <wayland-client.h>

#include <navigator/transfer.hpp>

namespace navigator::ui {
namespace {

class WaylandDrag final : public OutgoingDrag {
public:
    WaylandDrag(wl_display* display, wl_surface* surface) : display_(display), surface_(surface) {}
    ~WaylandDrag() override {
        end("destroyed");
        if (device_ != nullptr) {
            wl_data_device_destroy(device_);
        }
        if (pointer_ != nullptr) {
            wl_pointer_destroy(pointer_);
        }
        if (manager_ != nullptr) {
            wl_data_device_manager_destroy(manager_);
        }
        if (seat_ != nullptr) {
            wl_seat_destroy(seat_);
        }
        if (registry_ != nullptr) {
            wl_registry_destroy(registry_);
        }
        if (wrapper_ != nullptr) {
            wl_proxy_wrapper_destroy(wrapper_);
        }
        if (queue_ != nullptr) {
            wl_event_queue_destroy(queue_);
        }
    }

    [[nodiscard]] bool init() {
        queue_ = wl_display_create_queue(display_);
        if (queue_ == nullptr) {
            return false;
        }
        wrapper_ = static_cast<wl_display*>(wl_proxy_create_wrapper(display_));
        if (wrapper_ == nullptr) {
            return false;
        }
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper_), queue_);
        registry_ = wl_display_get_registry(wrapper_);
        static const wl_registry_listener rl{&WaylandDrag::on_global,
                                             &WaylandDrag::on_global_remove};
        wl_registry_add_listener(registry_, &rl, this);
        if (wl_display_roundtrip_queue(display_, queue_) < 0 || seat_ == nullptr ||
            manager_ == nullptr) {
            return false;
        }
        static const wl_seat_listener sl{&WaylandDrag::on_caps, &WaylandDrag::on_name};
        wl_seat_add_listener(seat_, &sl, this);
        if (wl_display_roundtrip_queue(display_, queue_) < 0 || pointer_ == nullptr) {
            return false;
        }
        device_ = wl_data_device_manager_get_data_device(manager_, seat_);
        return device_ != nullptr;
    }

    [[nodiscard]] OutgoingBackend backend() const noexcept override {
        return OutgoingBackend::Wayland;
    }

    [[nodiscard]] bool begin(const std::vector<Path>& paths) override {
        pump();
        if (paths.empty() || active_) {
            return false;
        }
        if (press_serial_ == 0 || !pointer_on_our_surface_) {
            state_ = "refused: no button press serial seen on the second pointer";
            return false;
        }
        Result<std::string> uris = transfer::encode_uri_list(paths);
        if (!uris) {
            return false;
        }
        uri_data_ = std::move(*uris);
        transfer::FileClipboard fc;
        fc.paths = paths;
        if (Result<std::string> g = transfer::encode_gnome_copied_files(fc); g) {
            gnome_data_ = std::move(*g);
        }
        source_ = wl_data_device_manager_create_data_source(manager_);
        if (source_ == nullptr) {
            return false;
        }
        static const wl_data_source_listener dl{
            &WaylandDrag::on_target,   &WaylandDrag::on_send,         &WaylandDrag::on_cancelled,
            &WaylandDrag::on_dnd_drop, &WaylandDrag::on_dnd_finished, &WaylandDrag::on_action};
        wl_data_source_add_listener(source_, &dl, this);
        wl_data_source_offer(source_, "text/uri-list");
        wl_data_source_offer(source_, "x-special/gnome-copied-files");
        wl_data_source_offer(source_, "text/plain");
        if (wl_data_source_get_version(source_) >= 3) {
            wl_data_source_set_actions(source_, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        }
        wl_data_device_start_drag(device_, source_, surface_, nullptr, press_serial_);
        wl_display_flush(display_);
        active_ = true;
        accepted_ = false;
        state_ = "started";
        return true;
    }

    bool motion() override {
        pump();
        return accepted_;
    }

    void finish(bool /*drop*/) override {
        // The compositor owns the pointer during a Wayland drag: the drop (or the cancel) is
        // reported through the data source events; nothing to send from here.
        pump();
    }

    bool poll(double now_ms) override {
        pump();
        if (active_) {
            if (started_ms_ < 0.0) {
                started_ms_ = now_ms;
            }
            if (finished_flag_ || cancelled_flag_ || now_ms - started_ms_ > 60000.0) {
                end(finished_flag_ ? "finished" : (cancelled_flag_ ? "cancelled" : "timeout"));
            }
        }
        return active_;
    }

    [[nodiscard]] bool active() const noexcept override { return active_; }
    [[nodiscard]] std::string state() const override { return state_; }

private:
    void pump() {
        if (queue_ != nullptr) {
            wl_display_dispatch_queue_pending(display_, queue_);
            wl_display_flush(display_);
        }
    }
    void end(const char* why) {
        if (source_ != nullptr) {
            wl_data_source_destroy(source_);
            source_ = nullptr;
        }
        active_ = false;
        finished_flag_ = false;
        cancelled_flag_ = false;
        started_ms_ = -1.0;
        state_ = why;
    }

    static void on_global(void* data, wl_registry* reg, uint32_t name, const char* iface,
                          uint32_t version) {
        auto* self = static_cast<WaylandDrag*>(data);
        if (std::strcmp(iface, wl_seat_interface.name) == 0 && self->seat_ == nullptr) {
            self->seat_ = static_cast<wl_seat*>(
                wl_registry_bind(reg, name, &wl_seat_interface, std::min<uint32_t>(version, 5)));
        } else if (std::strcmp(iface, wl_data_device_manager_interface.name) == 0) {
            self->manager_ = static_cast<wl_data_device_manager*>(wl_registry_bind(
                reg, name, &wl_data_device_manager_interface, std::min<uint32_t>(version, 3)));
        }
    }
    static void on_global_remove(void*, wl_registry*, uint32_t) {}
    static void on_caps(void* data, wl_seat* seat, uint32_t caps) {
        auto* self = static_cast<WaylandDrag*>(data);
        if ((caps & WL_SEAT_CAPABILITY_POINTER) != 0 && self->pointer_ == nullptr) {
            self->pointer_ = wl_seat_get_pointer(seat);
            static const wl_pointer_listener pl = [] {
                wl_pointer_listener l{};
                l.enter = &WaylandDrag::on_enter;
                l.leave = &WaylandDrag::on_leave;
                l.motion = &WaylandDrag::on_motion;
                l.button = &WaylandDrag::on_button;
                l.axis = &WaylandDrag::on_axis;
                l.frame = &WaylandDrag::on_frame;
                l.axis_source = &WaylandDrag::on_axis_source;
                l.axis_stop = &WaylandDrag::on_axis_stop;
                l.axis_discrete = &WaylandDrag::on_axis_discrete;
                return l;
            }();
            wl_pointer_add_listener(self->pointer_, &pl, self);
        }
    }
    static void on_name(void*, wl_seat*, const char*) {}
    static void on_enter(void* data, wl_pointer*, uint32_t, wl_surface* s, wl_fixed_t, wl_fixed_t) {
        auto* self = static_cast<WaylandDrag*>(data);
        self->pointer_on_our_surface_ = s == self->surface_;
    }
    static void on_leave(void* data, wl_pointer*, uint32_t, wl_surface*) {
        static_cast<WaylandDrag*>(data)->pointer_on_our_surface_ = false;
    }
    static void on_motion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
    static void on_button(void* data, wl_pointer*, uint32_t serial, uint32_t, uint32_t,
                          uint32_t state) {
        auto* self = static_cast<WaylandDrag*>(data);
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            self->press_serial_ = serial;
        }
    }
    static void on_axis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
    static void on_frame(void*, wl_pointer*) {}
    static void on_axis_source(void*, wl_pointer*, uint32_t) {}
    static void on_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
    static void on_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}

    static void on_target(void* data, wl_data_source*, const char* mime) {
        static_cast<WaylandDrag*>(data)->accepted_ = mime != nullptr;
    }
    static void on_send(void* data, wl_data_source*, const char* mime, int32_t fd) {
        auto* self = static_cast<WaylandDrag*>(data);
        const std::string_view m(mime != nullptr ? mime : "");
        const std::string& bytes =
            m == "x-special/gnome-copied-files" ? self->gnome_data_ : self->uri_data_;
        std::size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
            if (n <= 0) {
                break;
            }
            off += static_cast<std::size_t>(n);
        }
        ::close(fd);
    }
    static void on_cancelled(void* data, wl_data_source*) {
        static_cast<WaylandDrag*>(data)->cancelled_flag_ = true;
    }
    static void on_dnd_drop(void*, wl_data_source*) {}
    static void on_dnd_finished(void* data, wl_data_source*) {
        static_cast<WaylandDrag*>(data)->finished_flag_ = true;
    }
    static void on_action(void*, wl_data_source*, uint32_t) {}

    wl_display* display_ = nullptr;
    wl_surface* surface_ = nullptr;
    wl_event_queue* queue_ = nullptr;
    wl_display* wrapper_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_data_device_manager* manager_ = nullptr;
    wl_data_device* device_ = nullptr;
    wl_data_source* source_ = nullptr;
    uint32_t press_serial_ = 0;
    bool pointer_on_our_surface_ = false;
    bool active_ = false;
    bool accepted_ = false;
    bool finished_flag_ = false;
    bool cancelled_flag_ = false;
    double started_ms_ = -1.0;
    std::string uri_data_;
    std::string gnome_data_;
    std::string state_ = "idle";
};

} // namespace

std::unique_ptr<OutgoingDrag> create_wayland_outgoing_drag(SDL_Window* window) {
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    auto* display = static_cast<wl_display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
    auto* surface = static_cast<wl_surface*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
    if (display == nullptr || surface == nullptr) {
        return nullptr;
    }
    auto drag = std::make_unique<WaylandDrag>(display, surface);
    if (!drag->init()) {
        return nullptr; // the compositor did not give us what the attempt needs
    }
    return drag;
}

} // namespace navigator::ui
#else
namespace navigator::ui {
std::unique_ptr<OutgoingDrag> create_wayland_outgoing_drag(SDL_Window*) { return nullptr; }
} // namespace navigator::ui
#endif
