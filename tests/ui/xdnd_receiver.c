// Minimal XDND receiver (owner: ui) for tests/ui/xdnd_smoke.sh: a plain X11 window that is
// XdndAware, accepts a drop of text/uri-list, converts the XdndSelection, writes the received
// bytes to argv[1] and sends XdndFinished. It stands in for GNOME Files / Nemo at the protocol
// level only (docs/dnd.md). Usage: navigator_xdnd_receiver OUTFILE X Y W H
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 6)
        return 2;
    const char* out = argv[1];
    Display* d = XOpenDisplay(NULL);
    if (!d)
        return 3;
    int s = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, s), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                                   atoi(argv[5]), 0, 0, 0x336699);
    XStoreName(d, w, "xdnd-receiver");
    Atom aware = XInternAtom(d, "XdndAware", False), enter = XInternAtom(d, "XdndEnter", False),
         position = XInternAtom(d, "XdndPosition", False),
         status = XInternAtom(d, "XdndStatus", False), leave = XInternAtom(d, "XdndLeave", False),
         drop = XInternAtom(d, "XdndDrop", False), finished = XInternAtom(d, "XdndFinished", False),
         sel = XInternAtom(d, "XdndSelection", False),
         copy = XInternAtom(d, "XdndActionCopy", False),
         uri = XInternAtom(d, "text/uri-list", False), prop = XInternAtom(d, "NAV_RECV", False);
    long ver = 5;
    XChangeProperty(d, w, aware, XA_ATOM, 32, PropModeReplace, (unsigned char*)&ver, 1);
    XMapWindow(d, w);
    XFlush(d);
    Window src = 0;
    int entered = 0;
    FILE* log = fopen("/dev/stderr", "w");
    for (;;) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == ClientMessage) {
            Atom t = e.xclient.message_type;
            if (t == enter) {
                src = (Window)e.xclient.data.l[0];
                entered = 1;
                fprintf(log, "recv: enter from %lx types %ld %ld %ld\n", src, e.xclient.data.l[2],
                        e.xclient.data.l[3], e.xclient.data.l[4]);
            } else if (t == position) {
                XEvent r;
                memset(&r, 0, sizeof r);
                r.xclient.type = ClientMessage;
                r.xclient.display = d;
                r.xclient.window = src;
                r.xclient.message_type = status;
                r.xclient.format = 32;
                r.xclient.data.l[0] = (long)w;
                r.xclient.data.l[1] = 1;
                r.xclient.data.l[4] = (long)copy;
                XSendEvent(d, src, False, NoEventMask, &r);
                XFlush(d);
                fprintf(log, "recv: position %ld\n", e.xclient.data.l[2]);
            } else if (t == leave) {
                fprintf(log, "recv: leave\n");
                entered = 0;
            } else if (t == drop) {
                fprintf(log, "recv: drop\n");
                XConvertSelection(d, sel, uri, prop, w, CurrentTime);
                XFlush(d);
            }
        } else if (e.type == SelectionNotify) {
            if (e.xselection.property == None) {
                fprintf(log, "recv: selection refused\n");
            } else {
                Atom type;
                int fmt;
                unsigned long n, left;
                unsigned char* data = NULL;
                XGetWindowProperty(d, w, prop, 0, 1 << 20, True, AnyPropertyType, &type, &fmt, &n,
                                   &left, &data);
                FILE* f = fopen(out, "w");
                if (f && data) {
                    fwrite(data, 1, n * (fmt / 8), f);
                    fclose(f);
                }
                fprintf(log, "recv: wrote %lu bytes\n", n);
                if (data)
                    XFree(data);
            }
            XEvent r;
            memset(&r, 0, sizeof r);
            r.xclient.type = ClientMessage;
            r.xclient.display = d;
            r.xclient.window = src;
            r.xclient.message_type = finished;
            r.xclient.format = 32;
            r.xclient.data.l[0] = (long)w;
            r.xclient.data.l[1] = 1;
            r.xclient.data.l[2] = (long)copy;
            XSendEvent(d, src, False, NoEventMask, &r);
            XFlush(d);
            usleep(300000);
            return 0;
        }
    }
}
