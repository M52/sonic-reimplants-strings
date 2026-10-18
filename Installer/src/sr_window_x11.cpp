#include "imgui.h"
#include "sr_window.h"

#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/keysym.h>
#include <poll.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

struct SrWindow {
    Display* display;
    Window window;
    Visual* visual;
    int depth;
    GC gc;
    int width, height;
    Atom wm_delete;
    XIM im;
    XIC ic;
    bool shm;
    XShmSegmentInfo shm_info;
    XImage* image;
    int image_w, image_h;
    bool closed;
    float scale;
};

static bool g_x11_error;
static int x11_catch_error(Display*, XErrorEvent*) {
    g_x11_error = true;
    return 0;
}

static void image_free(SrWindow* w) {
    if (!w->image) return;
    if (w->shm) {
        XShmDetach(w->display, &w->shm_info);
        XSync(w->display, False);
        shmdt(w->shm_info.shmaddr);
        w->image->data = nullptr;
    }
    XDestroyImage(w->image);
    w->image = nullptr;
}

static bool image_make(SrWindow* w, int width, int height) {
    image_free(w);
    w->image_w = width;
    w->image_h = height;
    if (w->shm) {
        w->shm_info = {};
        XImage* image = XShmCreateImage(w->display, w->visual, (unsigned)w->depth, ZPixmap, nullptr, &w->shm_info,
                                        (unsigned)width, (unsigned)height);
        int id = image ? shmget(IPC_PRIVATE, (size_t)image->bytes_per_line * (size_t)image->height, IPC_CREAT | 0600) : -1;
        void* addr = id >= 0 ? shmat(id, nullptr, 0) : (void*)-1;
        bool attached = false;
        if (addr != (void*)-1) {
            w->shm_info.shmid = id;
            w->shm_info.shmaddr = image->data = (char*)addr;
            w->shm_info.readOnly = False;
            XSync(w->display, False);
            g_x11_error = false;
            XErrorHandler previous = XSetErrorHandler(x11_catch_error);
            XShmAttach(w->display, &w->shm_info);
            XSync(w->display, False);
            XSetErrorHandler(previous);
            attached = !g_x11_error;
        }
        if (id >= 0) shmctl(id, IPC_RMID, nullptr);
        if (attached && image->bits_per_pixel == 32) {
            w->image = image;
            return true;
        }
        if (attached) XShmDetach(w->display, &w->shm_info);
        if (addr != (void*)-1) shmdt(addr);
        if (image) {
            image->data = nullptr;
            XDestroyImage(image);
        }
        w->shm = false;
    }
    char* data = (char*)malloc((size_t)width * (size_t)height * 4);
    if (!data) return false;
    w->image = XCreateImage(w->display, w->visual, (unsigned)w->depth, ZPixmap, 0, data, (unsigned)width,
                            (unsigned)height, 32, 0);
    if (!w->image || w->image->bits_per_pixel != 32) {
        if (w->image) XDestroyImage(w->image);
        else free(data);
        w->image = nullptr;
        return false;
    }
    return true;
}

static float system_scale(Display* d) {
    float scale = 1.0f;
    if (char* resources = XResourceManagerString(d)) {
        XrmInitialize();
        XrmDatabase db = XrmGetStringDatabase(resources);
        char* type = nullptr;
        XrmValue value{};
        if (db && XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value) && value.addr) {
            double dpi = atof(value.addr);
            if (dpi > 0.0) scale = (float)(dpi / 96.0);
        }
        if (db) XrmDestroyDatabase(db);
    }
    scale = roundf(scale * 4.0f) / 4.0f;
    return scale < 1.0f ? 1.0f : scale > 3.0f ? 3.0f : scale;
}

SrWindow* sr_window_open(const char* title, int width, int height) {
    Display* d = XOpenDisplay(nullptr);
    if (!d) return nullptr;
    int screen = DefaultScreen(d);
    Visual* visual = DefaultVisual(d, screen);
    int depth = DefaultDepth(d, screen);
    if (visual->c_class != TrueColor || (depth != 24 && depth != 32) || visual->red_mask != 0xff0000 ||
        visual->green_mask != 0xff00 || visual->blue_mask != 0xff) {
        XCloseDisplay(d);
        return nullptr;
    }
    SrWindow* w = new SrWindow{};
    w->display = d;
    w->visual = visual;
    w->depth = depth;
    w->scale = system_scale(d);
    w->width = (int)(width * w->scale);
    w->height = (int)(height * w->scale);
    XSetWindowAttributes attr{};
    attr.background_pixel = 0x131414;
    attr.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | EnterWindowMask | LeaveWindowMask | FocusChangeMask | StructureNotifyMask;
    w->window = XCreateWindow(d, RootWindow(d, screen), 0, 0, (unsigned)w->width, (unsigned)w->height, 0, depth,
                              InputOutput, visual, CWBackPixel | CWEventMask, &attr);
    XStoreName(d, w->window, title);
    Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
    XChangeProperty(d, w->window, XInternAtom(d, "_NET_WM_NAME", False), utf8, 8, PropModeReplace,
                    (const unsigned char*)title, (int)strlen(title));
    XClassHint* hint = XAllocClassHint();
    if (hint) {
        hint->res_name = (char*)"sr_install";
        hint->res_class = (char*)"SonicReimplants";
        XSetClassHint(d, w->window, hint);
        XFree(hint);
    }
    XSizeHints* size = XAllocSizeHints();
    if (size) {
        size->flags = PMinSize;
        size->min_width = (int)(620 * w->scale);
        size->min_height = (int)(480 * w->scale);
        XSetWMNormalHints(d, w->window, size);
        XFree(size);
    }
    w->wm_delete = XInternAtom(d, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(d, w->window, &w->wm_delete, 1);
    w->gc = XCreateGC(d, w->window, 0, nullptr);
    XMapWindow(d, w->window);

    XkbSetDetectableAutoRepeat(d, True, nullptr);
    w->im = XOpenIM(d, nullptr, nullptr, nullptr);
    if (w->im)
        w->ic = XCreateIC(w->im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, w->window,
                          XNFocusWindow, w->window, nullptr);
    int major = 0, minor = 0;
    Bool pixmaps = False;
    w->shm = XShmQueryVersion(d, &major, &minor, &pixmaps) != 0;
    if (!image_make(w, w->width, w->height)) {
        sr_window_close(w);
        return nullptr;
    }
    XFlush(d);
    return w;
}

void sr_window_close(SrWindow* w) {
    if (!w) return;
    image_free(w);
    if (w->ic) XDestroyIC(w->ic);
    if (w->im) XCloseIM(w->im);
    if (w->gc) XFreeGC(w->display, w->gc);
    if (w->window) XDestroyWindow(w->display, w->window);
    XCloseDisplay(w->display);
    delete w;
}

float sr_window_scale(SrWindow* w) { return w->scale; }

static ImGuiKey x11_key(KeySym k) {
    if (k >= XK_a && k <= XK_z) return (ImGuiKey)(ImGuiKey_A + (int)(k - XK_a));
    if (k >= XK_A && k <= XK_Z) return (ImGuiKey)(ImGuiKey_A + (int)(k - XK_A));
    if (k >= XK_0 && k <= XK_9) return (ImGuiKey)(ImGuiKey_0 + (int)(k - XK_0));
    switch (k) {
        case XK_Return: return ImGuiKey_Enter;
        case XK_KP_Enter: return ImGuiKey_KeypadEnter;
        case XK_Tab: case XK_ISO_Left_Tab: return ImGuiKey_Tab;
        case XK_space: return ImGuiKey_Space;
        case XK_BackSpace: return ImGuiKey_Backspace;
        case XK_Escape: return ImGuiKey_Escape;
        case XK_Home: return ImGuiKey_Home;
        case XK_End: return ImGuiKey_End;
        case XK_Prior: return ImGuiKey_PageUp;
        case XK_Next: return ImGuiKey_PageDown;
        case XK_Delete: return ImGuiKey_Delete;
        case XK_Left: return ImGuiKey_LeftArrow;
        case XK_Right: return ImGuiKey_RightArrow;
        case XK_Up: return ImGuiKey_UpArrow;
        case XK_Down: return ImGuiKey_DownArrow;
        case XK_Shift_L: return ImGuiKey_LeftShift;
        case XK_Shift_R: return ImGuiKey_RightShift;
        case XK_Control_L: return ImGuiKey_LeftCtrl;
        case XK_Control_R: return ImGuiKey_RightCtrl;
        case XK_Alt_L: return ImGuiKey_LeftAlt;
        case XK_Alt_R: return ImGuiKey_RightAlt;
        default: return ImGuiKey_None;
    }
}

static void x11_modifiers(ImGuiIO& io, unsigned state, KeySym key, bool down) {
    bool shift = (state & ShiftMask) != 0, ctrl = (state & ControlMask) != 0, alt = (state & Mod1Mask) != 0;
    if (key == XK_Shift_L || key == XK_Shift_R) shift = down;
    if (key == XK_Control_L || key == XK_Control_R) ctrl = down;
    if (key == XK_Alt_L || key == XK_Alt_R) alt = down;
    io.AddKeyEvent(ImGuiMod_Shift, shift);
    io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
    io.AddKeyEvent(ImGuiMod_Alt, alt);
}

static void x11_text(SrWindow* w, ImGuiIO& io, XKeyEvent* key) {
    char raw[64];
    KeySym sym = 0;
    int n;
    if (w->ic) {
        Status status = 0;
        n = Xutf8LookupString(w->ic, key, raw, (int)sizeof(raw) - 1, &sym, &status);
        if (status == XBufferOverflow) n = 0;
    } else {
        n = XLookupString(key, raw, (int)sizeof(raw) - 1, &sym, nullptr);
    }
    if (n <= 0) return;
    char text[128];
    int len = 0;
    for (int i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)raw[i];
        if (c < 0x20 || c == 0x7f) continue;
        if (c >= 0x80 && !w->ic) {
            text[len++] = (char)(0xc0 | (c >> 6));
            text[len++] = (char)(0x80 | (c & 0x3f));
        } else {
            text[len++] = (char)c;
        }
    }
    text[len] = '\0';
    if (len) io.AddInputCharactersUTF8(text);
}

bool sr_window_pump(SrWindow* w, int timeout_ms) {
    if (!XPending(w->display)) {
        pollfd pfd = {ConnectionNumber(w->display), POLLIN, 0};
        poll(&pfd, 1, timeout_ms);
    }
    bool have_io = ImGui::GetCurrentContext() != nullptr;
    while (XPending(w->display)) {
        XEvent ev;
        XNextEvent(w->display, &ev);
        if (w->ic && XFilterEvent(&ev, None)) continue;
        if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == w->wm_delete) {
            w->closed = true;
            continue;
        }
        if (ev.type == ConfigureNotify) {
            w->width = ev.xconfigure.width;
            w->height = ev.xconfigure.height;
            continue;
        }
        if (!have_io) continue;
        ImGuiIO& io = ImGui::GetIO();
        switch (ev.type) {
            case MotionNotify:
                x11_modifiers(io, ev.xmotion.state, 0, false);
                io.AddMousePosEvent((float)ev.xmotion.x, (float)ev.xmotion.y);
                break;
            case EnterNotify:
                io.AddMousePosEvent((float)ev.xcrossing.x, (float)ev.xcrossing.y);
                break;
            case LeaveNotify:
                if (!(ev.xcrossing.state & (Button1Mask | Button2Mask | Button3Mask)))
                    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                break;
            case ButtonPress:
            case ButtonRelease: {
                bool down = ev.type == ButtonPress;
                x11_modifiers(io, ev.xbutton.state, 0, false);
                io.AddMousePosEvent((float)ev.xbutton.x, (float)ev.xbutton.y);
                unsigned b = ev.xbutton.button;
                if (b >= 4 && b <= 7) {
                    if (down)
                        io.AddMouseWheelEvent(b == 6 ? 1.0f : b == 7 ? -1.0f : 0.0f,
                                              b == 4 ? 1.0f : b == 5 ? -1.0f : 0.0f);
                    break;
                }
                int button = b == 1 ? 0 : b == 2 ? 2 : b == 3 ? 1 : -1;
                if (button >= 0) io.AddMouseButtonEvent(button, down);
                break;
            }
            case KeyPress:
            case KeyRelease: {
                bool down = ev.type == KeyPress;
                KeySym sym = XLookupKeysym(&ev.xkey, 0);
                x11_modifiers(io, ev.xkey.state, sym, down);
                ImGuiKey key = x11_key(sym);
                if (key != ImGuiKey_None) io.AddKeyEvent(key, down);
                if (down && !(ev.xkey.state & (ControlMask | Mod1Mask))) x11_text(w, io, &ev.xkey);
                break;
            }
            case FocusIn:
            case FocusOut:
                if (ev.xfocus.detail == NotifyPointer || ev.xfocus.detail == NotifyInferior) break;
                if (w->ic) {
                    if (ev.type == FocusIn) XSetICFocus(w->ic);
                    else XUnsetICFocus(w->ic);
                }
                io.AddFocusEvent(ev.type == FocusIn);
                break;
            default:
                break;
        }
    }
    return !w->closed;
}

void sr_window_size(SrWindow* w, int* width, int* height) {
    *width = w->width;
    *height = w->height;
}

void sr_window_present(SrWindow* w, const uint32_t* pixels, int width, int height) {
    if (!w->image || w->image_w != width || w->image_h != height) {
        if (w->shm) XSync(w->display, False);
        if (!image_make(w, width, height)) return;
    }
    for (int y = 0; y < height; ++y)
        memcpy(w->image->data + (size_t)y * (size_t)w->image->bytes_per_line, pixels + (size_t)y * (size_t)width,
               (size_t)width * 4);
    if (w->shm) {
        XShmPutImage(w->display, w->window, w->gc, w->image, 0, 0, 0, 0, (unsigned)width, (unsigned)height, False);
        XSync(w->display, False);
    } else {
        XPutImage(w->display, w->window, w->gc, w->image, 0, 0, 0, 0, (unsigned)width, (unsigned)height);
        XFlush(w->display);
    }
}

std::string sr_window_take_drop(SrWindow*) { return std::string(); }
