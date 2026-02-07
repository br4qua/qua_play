// qua-hotkeys.c - media key daemon
#include <X11/Xlib.h>
#include <X11/XF86keysym.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <unistd.h>

static void send_cmd(const char *cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strcpy(addr.sun_path, "/tmp/qua-socket.sock");
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0)
        write(fd, cmd, strlen(cmd) + 1);
    close(fd);
}

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    Window root = DefaultRootWindow(dpy);

    KeyCode next = XKeysymToKeycode(dpy, XF86XK_AudioNext);
    KeyCode prev = XKeysymToKeycode(dpy, XF86XK_AudioPrev);
    KeyCode play = XKeysymToKeycode(dpy, XF86XK_AudioPlay);
    KeyCode stop = XKeysymToKeycode(dpy, XF86XK_AudioStop);

    XGrabKey(dpy, next, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, prev, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, play, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, stop, AnyModifier, root, True, GrabModeAsync, GrabModeAsync);

    XEvent ev;
    while (1) {
        XNextEvent(dpy, &ev);
        if (ev.type == KeyPress) {
            KeyCode kc = ev.xkey.keycode;
            if (kc == next) send_cmd("play-next");
            else if (kc == prev) send_cmd("play-prev");
            else if (kc == play) send_cmd("play");
            else if (kc == stop) send_cmd("stop");
        }
    }
}
