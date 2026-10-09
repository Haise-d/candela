// The settings window, and where windows go.
#pragma once
#include "options.h"
#include "render.h"

namespace ui {

class Window {
public:
    virtual ~Window() = default;
    virtual void Open(const POINT* center) = 0;  // center: where to put it (nullptr: the screen with the pointer)
    virtual void ShowSection(const std::wstring& key) { (void)key; }  // before Open: start on that section
    virtual void Close(bool animate) = 0;
    virtual bool IsOpen() const = 0;
    virtual bool Owns(HWND h) const = 0;
    virtual POINT Center() const = 0;
    virtual void Refresh() = 0;
    virtual void DebugDump(const std::wstring& dir) = 0;
    virtual void Shutdown() = 0;
};

Window& Main();

// Where a window of w x h (unscaled, plus `margin` of shadow around it) goes:
// centred on `center` or on the screen with the pointer, scaled for that
// screen's DPI and shrunk if the screen is too small.
struct Placement {
    int x, y, w, h;
    float s;
};
Placement Place(float w, float h, float margin, const POINT* center);

// Gives a window the Candela icon (taskbar, Alt+Tab).
void SetIcons(HWND h);

}  // namespace ui
