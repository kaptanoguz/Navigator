// src/ui/platform/xdnd_source.cpp — X11 XDND drag source (protocol version 5), see
// outgoing_drag.hpp. Uses only Xlib on the Display and Window SDL already owns; SDL's X11 event
// hook delivers the ClientMessages (XdndStatus, XdndFinished) and SelectionRequests that the
// protocol needs and keeps them away from SDL itself.
#include <SDL3/SDL.h>

#include "platform/outgoing_drag.hpp"

#ifdef NAVIGATOR_UI_HAVE_X11
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <navigator/transfer.hpp>

// Xlib last: its macros (None, ...) must not reach navigator headers.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#undef None
#undef Bool
#undef Status

namespace navigator::ui {
namespace {

constexpr unsigned long kNone = 0;
constexpr long kXdndVersion = 5;
constexpr double kFinishTimeoutMs = 5000.0;

class XdndSource final : public OutgoingDrag {
public:
    XdndSource(Display* dpy, Window win) : dpy_(dpy), win_(win) {
        const auto atom = [&](const char* n) { return XInternAtom(dpy_, n, False); };
        aware_ = atom("XdndAware");
        enter_ = atom("XdndEnter");
        position_ = atom("XdndPosition");
        status_ = atom("XdndStatus");
        leave_ = atom("XdndLeave");
        drop_ = atom("XdndDrop");
        finished_ = atom("XdndFinished");
        selection_ = atom("XdndSelection");
        action_copy_ = atom("XdndActionCopy");
        uri_list_ = atom("text/uri-list");
        gnome_ = atom("x-special/gnome-copied-files");
        text_plain_ = atom("text/plain");
        utf8_ = atom("UTF8_STRING");
        targets_ = atom("TARGETS");
        SDL_SetX11EventHook(&XdndSource::hook, this);
    }
    ~XdndSource() override {
        SDL_SetX11EventHook(nullptr, nullptr);
        if (active_) {
            XSetSelectionOwner(dpy_, selection_, kNone, CurrentTime);
        }
    }

    [[nodiscard]] OutgoingBackend backend() const noexcept override { return OutgoingBackend::X11; }

    [[nodiscard]] bool begin(const std::vector<Path>& paths) override {
        if (paths.empty() || active_) {
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
        text_data_.clear();
        for (const Path& p : paths) {
            text_data_ += p.string();
            text_data_ += '\n';
        }
        XSetSelectionOwner(dpy_, selection_, win_, CurrentTime);
        if (XGetSelectionOwner(dpy_, selection_) != win_) {
            state_ = "refused: could not own XdndSelection";
            return false;
        }
        active_ = true;
        dropped_ = false;
        target_ = kNone;
        accepted_ = false;
        status_wait_ = false;
        state_ = "started";
        return true;
    }

    bool motion() override {
        if (!active_ || dropped_) {
            return accepted_;
        }
        Window root_ret = kNone;
        Window child_ret = kNone;
        int rx = 0;
        int ry = 0;
        int wx = 0;
        int wy = 0;
        unsigned int mask = 0;
        const Window root = DefaultRootWindow(dpy_);
        if (!XQueryPointer(dpy_, root, &root_ret, &child_ret, &rx, &ry, &wx, &wy, &mask)) {
            return accepted_;
        }
        const Window target = find_target(root, rx, ry);
        if (target != target_) {
            leave_target();
            target_ = target;
            accepted_ = false;
            status_wait_ = false;
            if (target_ != kNone) {
                send_enter();
            }
        }
        if (target_ != kNone && (!status_wait_ || position_moved(rx, ry))) {
            send_position(rx, ry);
        }
        return accepted_;
    }

    void finish(bool drop) override {
        if (!active_ || dropped_) {
            return;
        }
        if (drop && target_ != kNone && accepted_) {
            send_client(target_, drop_,
                        {static_cast<long>(win_), 0, static_cast<long>(CurrentTime), 0, 0});
            dropped_ = true;
            dropped_at_ms_ = -1.0; // set by the first poll()
            state_ = "dropped";
            return;
        }
        leave_target();
        end("cancelled");
    }

    bool poll(double now_ms) override {
        if (!active_) {
            return false;
        }
        if (dropped_) {
            if (dropped_at_ms_ < 0.0) {
                dropped_at_ms_ = now_ms;
            }
            if (finished_flag_ || now_ms - dropped_at_ms_ > kFinishTimeoutMs) {
                end(finished_flag_ ? "finished" : "timeout");
            }
        }
        return active_;
    }

    [[nodiscard]] bool active() const noexcept override { return active_; }
    [[nodiscard]] std::string state() const override { return state_; }

private:
    static bool hook(void* userdata, XEvent* ev) {
        return static_cast<XdndSource*>(userdata)->handle(ev);
    }

    // Returns false to keep the event away from SDL.
    bool handle(XEvent* ev) {
        if (!active_) {
            return true;
        }
        switch (ev->type) {
        case ClientMessage:
            if (ev->xclient.message_type == status_ && ev->xclient.window == win_) {
                const auto source = static_cast<Window>(ev->xclient.data.l[0]);
                if (source == target_) {
                    accepted_ = (ev->xclient.data.l[1] & 1L) != 0;
                    status_wait_ = false;
                    if (accepted_) {
                        state_ = "accepted";
                    }
                }
                return false;
            }
            if (ev->xclient.message_type == finished_ && ev->xclient.window == win_) {
                finished_flag_ = true;
                accepted_ = (ev->xclient.data.l[1] & 1L) != 0 || accepted_;
                return false;
            }
            return true;
        case SelectionRequest:
            if (ev->xselectionrequest.selection == selection_) {
                answer(ev->xselectionrequest);
                return false;
            }
            return true;
        default: return true;
        }
    }

    void answer(const XSelectionRequestEvent& rq) {
        XEvent reply{};
        reply.xselection.type = SelectionNotify;
        reply.xselection.display = rq.display;
        reply.xselection.requestor = rq.requestor;
        reply.xselection.selection = rq.selection;
        reply.xselection.target = rq.target;
        reply.xselection.time = rq.time;
        reply.xselection.property = kNone;
        const Atom prop = rq.property != kNone ? rq.property : rq.target;
        const auto put_text = [&](const std::string& data, Atom type) {
            XChangeProperty(dpy_, rq.requestor, prop, type, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(data.data()),
                            static_cast<int>(data.size()));
            reply.xselection.property = prop;
        };
        if (rq.target == targets_) {
            std::vector<Atom> t{targets_, uri_list_, gnome_, text_plain_, utf8_, XA_STRING};
            XChangeProperty(dpy_, rq.requestor, prop, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(t.data()),
                            static_cast<int>(t.size()));
            reply.xselection.property = prop;
        } else if (rq.target == uri_list_) {
            put_text(uri_data_, uri_list_);
        } else if (rq.target == gnome_ && !gnome_data_.empty()) {
            put_text(gnome_data_, gnome_);
        } else if (rq.target == text_plain_ || rq.target == utf8_ || rq.target == XA_STRING) {
            put_text(text_data_, rq.target);
        }
        XSendEvent(dpy_, rq.requestor, False, NoEventMask, &reply);
        XFlush(dpy_);
    }

    // The deepest window under (x, y), then up to the nearest XdndAware one; kNone when it is
    // our own window or none is aware.
    Window find_target(Window root, int x, int y) {
        Window w = root;
        for (int depth = 0; depth < 32; ++depth) {
            int dx = 0;
            int dy = 0;
            Window child = kNone;
            if (!XTranslateCoordinates(dpy_, root, w, x, y, &dx, &dy, &child) || child == kNone) {
                break;
            }
            w = child;
        }
        for (Window cur = w; cur != kNone && cur != root;) {
            if (cur == win_) {
                return kNone; // still over Navigator itself
            }
            Atom type = kNone;
            int fmt = 0;
            unsigned long n = 0;
            unsigned long left = 0;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(dpy_, cur, aware_, 0, 1, False, XA_ATOM, &type, &fmt, &n, &left,
                                   &data) == Success &&
                data != nullptr) {
                const bool aware = type == XA_ATOM && n > 0;
                if (aware) {
                    target_version_ =
                        std::min(kXdndVersion, static_cast<long>(*reinterpret_cast<Atom*>(data)));
                }
                XFree(data);
                if (aware) {
                    return cur;
                }
            }
            Window r2 = kNone;
            Window parent = kNone;
            Window* kids = nullptr;
            unsigned int nk = 0;
            if (!XQueryTree(dpy_, cur, &r2, &parent, &kids, &nk)) {
                break;
            }
            if (kids != nullptr) {
                XFree(kids);
            }
            cur = parent;
        }
        return kNone;
    }

    void send_client(Window to, Atom type, const std::array<long, 5>& data) {
        XEvent ev{};
        ev.xclient.type = ClientMessage;
        ev.xclient.display = dpy_;
        ev.xclient.window = to;
        ev.xclient.message_type = type;
        ev.xclient.format = 32;
        for (std::size_t i = 0; i < 5; ++i) {
            ev.xclient.data.l[i] = data[i];
        }
        XSendEvent(dpy_, to, False, NoEventMask, &ev);
        XFlush(dpy_);
    }

    void send_enter() {
        // Three offered types fit in the message: uri list, gnome copied-files, plain text.
        send_client(target_, enter_,
                    {static_cast<long>(win_), target_version_ << 24, static_cast<long>(uri_list_),
                     static_cast<long>(gnome_), static_cast<long>(text_plain_)});
    }

    void send_position(int rx, int ry) {
        last_x_ = rx;
        last_y_ = ry;
        status_wait_ = true;
        send_client(target_, position_,
                    {static_cast<long>(win_), 0, (static_cast<long>(rx) << 16) | (ry & 0xFFFF),
                     static_cast<long>(CurrentTime), static_cast<long>(action_copy_)});
    }

    [[nodiscard]] bool position_moved(int rx, int ry) const {
        return rx != last_x_ || ry != last_y_;
    }

    void leave_target() {
        if (target_ != kNone) {
            send_client(target_, leave_, {static_cast<long>(win_), 0, 0, 0, 0});
        }
        target_ = kNone;
        accepted_ = false;
        status_wait_ = false;
    }

    void end(const char* why) {
        active_ = false;
        dropped_ = false;
        finished_flag_ = false;
        XSetSelectionOwner(dpy_, selection_, kNone, CurrentTime);
        state_ = why;
    }

    Display* dpy_ = nullptr;
    Window win_ = kNone;
    Atom aware_ = kNone, enter_ = kNone, position_ = kNone, status_ = kNone, leave_ = kNone,
         drop_ = kNone, finished_ = kNone, selection_ = kNone, action_copy_ = kNone,
         uri_list_ = kNone, gnome_ = kNone, text_plain_ = kNone, utf8_ = kNone, targets_ = kNone;
    bool active_ = false;
    bool dropped_ = false;
    bool finished_flag_ = false;
    bool accepted_ = false;
    bool status_wait_ = false;
    Window target_ = kNone;
    long target_version_ = kXdndVersion;
    int last_x_ = -1;
    int last_y_ = -1;
    double dropped_at_ms_ = -1.0;
    std::string uri_data_;
    std::string gnome_data_;
    std::string text_data_;
    std::string state_ = "idle";
};

} // namespace

std::unique_ptr<OutgoingDrag> create_x11_outgoing_drag(SDL_Window* window) {
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    auto* dpy = static_cast<Display*>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
    const auto win =
        static_cast<Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    if (dpy == nullptr || win == kNone) {
        return nullptr;
    }
    return std::make_unique<XdndSource>(dpy, win);
}

} // namespace navigator::ui
#else
namespace navigator::ui {
std::unique_ptr<OutgoingDrag> create_x11_outgoing_drag(SDL_Window*) { return nullptr; }
} // namespace navigator::ui
#endif
