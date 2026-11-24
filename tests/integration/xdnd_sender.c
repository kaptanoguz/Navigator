/* tests/integration/xdnd_sender.c (owner: qa): a minimal XDND *source* (protocol 5) for the
 * incoming-drop smoke test. It owns XdndSelection, offers text/uri-list for the given files,
 * sends XdndEnter / XdndPosition / XdndDrop to a window and serves the SelectionRequest. Exit 0
 * when the target answered XdndStatus (accepted) and XdndFinished; 1 otherwise.
 * Usage: xdnd_sender <window-id> <x> <y> <file>...   (x, y: root coordinates of the drop point)
 * Only talks to the X server named by $DISPLAY (the test runs it on a private Xvfb). */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        return 2;
    }
    Display* d = XOpenDisplay(NULL);
    if (d == NULL) {
        return 2;
    }
    Window target = (Window)strtoul(argv[1], NULL, 0);
    int x = atoi(argv[2]);
    int y = atoi(argv[3]);
    char list[8192] = "";
    for (int i = 4; i < argc; ++i) {
        strcat(list, "file://");
        strcat(list, argv[i]);
        strcat(list, "\r\n");
    }
    Window me = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 10, 10, 0, 0, 0);
    Atom sel = XInternAtom(d, "XdndSelection", False), uri = XInternAtom(d, "text/uri-list", False);
    Atom enter = XInternAtom(d, "XdndEnter", False), pos = XInternAtom(d, "XdndPosition", False);
    Atom status = XInternAtom(d, "XdndStatus", False), drop = XInternAtom(d, "XdndDrop", False);
    Atom finished = XInternAtom(d, "XdndFinished", False);
    Atom copy = XInternAtom(d, "XdndActionCopy", False);
    XSetSelectionOwner(d, sel, me, CurrentTime);
    XSelectInput(d, me, PropertyChangeMask);

    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.display = d;
    e.xclient.window = target;
    e.xclient.format = 32;
    e.xclient.message_type = enter;
    e.xclient.data.l[0] = (long)me;
    e.xclient.data.l[1] = 5L << 24;
    e.xclient.data.l[2] = (long)uri;
    XSendEvent(d, target, False, NoEventMask, &e);
    e.xclient.message_type = pos;
    e.xclient.data.l[1] = 0;
    e.xclient.data.l[2] = ((long)x << 16) | (long)y;
    e.xclient.data.l[3] = CurrentTime;
    e.xclient.data.l[4] = (long)copy;
    XSendEvent(d, target, False, NoEventMask, &e);
    XFlush(d);

    int accepted = 0, dropped = 0, done = 0;
    double deadline = now() + 8.0;
    while (now() < deadline && !done) {
        while (XPending(d)) {
            XNextEvent(d, &e);
            if (e.type == ClientMessage && e.xclient.message_type == status) {
                accepted = (e.xclient.data.l[1] & 1) != 0;
                if (accepted && !dropped) {
                    memset(&e, 0, sizeof e);
                    e.xclient.type = ClientMessage;
                    e.xclient.display = d;
                    e.xclient.window = target;
                    e.xclient.format = 32;
                    e.xclient.message_type = drop;
                    e.xclient.data.l[0] = (long)me;
                    e.xclient.data.l[2] = CurrentTime;
                    XSendEvent(d, target, False, NoEventMask, &e);
                    XFlush(d);
                    dropped = 1;
                }
            } else if (e.type == ClientMessage && e.xclient.message_type == finished) {
                done = 1;
            } else if (e.type == SelectionRequest) {
                XSelectionRequestEvent* r = &e.xselectionrequest;
                XSelectionEvent n;
                memset(&n, 0, sizeof n);
                n.type = SelectionNotify;
                n.display = d;
                n.requestor = r->requestor;
                n.selection = r->selection;
                n.target = r->target;
                n.time = r->time;
                n.property = None;
                if (r->target == uri) {
                    XChangeProperty(d, r->requestor, r->property, uri, 8, PropModeReplace,
                                    (unsigned char*)list, (int)strlen(list));
                    n.property = r->property;
                }
                XSendEvent(d, r->requestor, False, NoEventMask, (XEvent*)&n);
                XFlush(d);
                if (dropped) {
                    deadline = now() + 1.5; /* give the target time to send Finished */
                }
            }
        }
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "accepted=%d dropped=%d finished=%d\n", accepted, dropped, done);
    return accepted && dropped ? 0 : 1;
}
