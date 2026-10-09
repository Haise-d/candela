// The switcher's state: the window list, the selection and the phase.
#pragma once
#include "render.h"

namespace switcher {
namespace detail {

struct Item {
    HWND hwnd;
    std::wstring title, exe;
    HICON icon;
    ID2D1Bitmap* bmp = nullptr;  // the row's icon, as drawn
    int bmp_gen = -1;
    bool icon_asked = false;     // the icon is asked for only when its row is first drawn
    bool closing = false;        // asked to close (× or Delete); it leaves the list once it's gone
};

enum Phase { kHidden, kPending, kOpen, kClosing };

extern std::vector<Item> g_items;
extern int g_sel, g_prev_sel;
extern Phase g_phase;
extern double g_t_shown, g_t_sel, g_t_close;
extern uint32_t g_seed;
extern HWND g_dim;

void Select(int idx, double now);     // moves the selection (wraps around)
std::wstring AppName(const Item& it); // "chrome", "App" for Store apps; as the exe has it
void HideAll();                       // everything away, now

}  // namespace detail

}  // namespace switcher
