// Volume and media keys, and the popup that shows what they did.
//
// Candela takes these keys before Windows sees them (keyboard.cpp), so Windows'
// own flyout never appears. Volume is set through Core Audio. The volume card
// also shows when the volume changes any other way (a headset's buttons or
// wheel, a USB volume knob, Windows' own slider, another app): Core Audio
// tells Candela about every change to the output's volume, and only Candela's
// own are left out, since a key press already showed them. Play/pause,
// next and previous, and the song's title and artist, go through Windows'
// media-session API (the one the lock screen uses), called on a worker thread
// so a slow media app can't stall anything. If that API isn't available, the
// key is handed back to Windows unchanged.
//
// While nothing is asked of it, the worker also looks at the current song
// once a second (Popups -> New song popup): when a new one starts by itself,
// the song card shows without a key being pressed.
#include "media_card.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <shcore.h>
#include <propsys.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace media {

using namespace gfx;

namespace {

// ---- the popup ------------------------------------------------------------------------------
constexpr float kW = 380, kH = 124, kMargin = 22;
constexpr float kValueY0 = 40, kValueY1 = 86;
constexpr float kArtX = 18, kArtY = 44, kArt = 64;  // album cover square
constexpr float kTextX = 96;                         // text column when there's a cover

class Popup {
public:
    void Init() { surf_.Create(L"CandelaMedia", Proc, this, WS_EX_TRANSPARENT); }

    void Hide() {
        t_open_ = -1;
        if (surf_.hwnd) KillTimer(surf_.hwnd, 1);
        surf_.Hide();
    }

    // A key was pressed (or new song info arrived). First time opens the
    // panel; after that only what changed scrambles into the new text.
    void Show(const Content& c, double hold_ms, bool reposition) {
        if (!surf_.hwnd) return;
        double now = NowMs();
        const auto& st = glitch::CurrentStyle();
        Phase ph = PhaseAt(now);
        bool up = ph == kOpen || ph == kChange || ph == kHold || (ph == kClose && now - close_at_ < st.break_ms);
        if (!up) {
            t_open_ = now;
            t_change_ = -1e9;
            old_ = Content();
            reposition = true;
        } else if (ph != kOpen && !c.SameLook(cur_)) {
            ArtPtr shown_art = cur_.art;
            old_ = Visible(now);
            old_.art = shown_art;
            t_change_ = now;
        }
        cur_ = c;
        seed_ = (uint32_t)(now * 104729.0) ^ 0x5EEDu;
        double anim_end = std::max(t_open_ + glitch::OpenMs(st), t_change_ + st.change_ms);
        close_at_ = std::max({now + hold_ms, anim_end, up ? close_at_ : 0.0});
        if (reposition || placed_ != core::MediaCorner(g_cfg)) Place();  // a new corner moves it straight there
        SetTimer(surf_.hwnd, 1, 10, nullptr);
        drawn_ = false;
        Render(now);
    }

    // Song info arrived for a popup that is already up. The same song just
    // moving on updates quietly; a new song or cover scrambles in.
    void Update(const Content& c, double hold_ms) {
        double now = NowMs();
        Phase ph = PhaseAt(now);
        if (ph == kNone || ph == kClose) return;
        if (c.SameLook(cur_)) {
            cur_ = c;  // just the position
            if (surf_.hwnd) SetTimer(surf_.hwnd, 1, 10, nullptr);  // drawn on the next frame, not at the next scheduled one
            return;
        }
        Show(c, std::max(hold_ms, close_at_ - now), false);
    }

    bool Owns(HWND h) const { return h && h == surf_.hwnd; }
    void Tick() { Render(NowMs()); }
    bool Save(const std::wstring& png) { return surf_.Visible() && surf_.SavePng(png); }
    void Destroy() {
        DropBitmaps();
        surf_.Destroy();
    }

private:
    enum Phase { kNone, kOpen, kChange, kHold, kClose };

    Phase PhaseAt(double now) const {
        if (t_open_ < 0) return kNone;
        const auto& st = glitch::CurrentStyle();
        if (now >= close_at_) return now < close_at_ + glitch::CloseMs(st) ? kClose : kNone;
        if (now < t_change_ + st.change_ms) return kChange;
        if (now < t_open_ + glitch::OpenMs(st)) return kOpen;
        return kHold;
    }

    // What the text shows at `now`, before glitches.
    Content Visible(double now) const {
        const auto& st = glitch::CurrentStyle();
        Content c = cur_;
        if (!st.enabled) return c;
        Phase ph = PhaseAt(now);
        if (ph == kOpen) {
            double t = now - t_open_;
            glitch::Rng rng = glitch::RngAt(seed_, t, 1);
            c.header = glitch::Decode(cur_.header, glitch::TypeProgress(st, t, 0.0, 0.5), rng);
            c.note = glitch::Decode(cur_.note, glitch::TypeProgress(st, t, 0.35, 0.65), rng, nullptr, true);
            c.value = glitch::Decode(cur_.value, glitch::TypeProgress(st, t, 0.2, 0.8), rng);
            c.sub = glitch::Decode(cur_.sub, glitch::TypeProgress(st, t, 0.4, 0.6), rng);
            if (c.bar >= 0) c.bar = (float)(cur_.bar * glitch::Steps(glitch::TypeProgress(st, t, 0.2, 0.8), 6));
        } else if (ph == kChange) {
            double p = (now - t_change_) / st.change_ms;
            glitch::Rng rng = glitch::RngAt(seed_, now - t_change_, 2);
            c.header = glitch::Morph(old_.header, cur_.header, p * 1.4, rng);
            c.note = glitch::Morph(old_.note, cur_.note, p * 1.2, rng, false, true);
            c.value = glitch::Morph(old_.value, cur_.value, p, rng, true);
            c.sub = glitch::Morph(old_.sub, cur_.sub, p * 1.2, rng);
            if (c.bar >= 0 && old_.bar >= 0)
                c.bar = (float)(old_.bar + (cur_.bar - old_.bar) * glitch::Steps(p / 0.6, 4));
        } else if (ph == kClose) {
            double t = now - close_at_;
            glitch::Rng rng = glitch::RngAt(seed_, t, 3);
            double amount = 0.15 + 0.85 * glitch::Clamp01(t / std::max(1.0, st.break_ms));
            c.header = glitch::Corrupt(c.header, amount, rng);
            c.note = glitch::Corrupt(c.note, amount, rng);
            c.value = glitch::Corrupt(c.value, amount, rng);
            c.sub = glitch::Corrupt(c.sub, amount, rng);
        }
        return c;
    }

    // How much of the cover shows (0..1), revealed top to bottom in steps.
    double ArtReveal(double now) const {
        const auto& st = glitch::CurrentStyle();
        if (!st.enabled) return 1;
        Phase ph = PhaseAt(now);
        if (ph == kOpen) return glitch::Steps(glitch::TypeProgress(st, now - t_open_, 0.1, 0.6), 8);
        if (ph == kChange && old_.art != cur_.art) return glitch::Steps((now - t_change_) / st.change_ms, 8);
        return 1;
    }

    bool Typing(double now) const {
        Phase ph = PhaseAt(now);
        return glitch::CurrentStyle().enabled && (ph == kOpen || ph == kChange);
    }

    void Place() {
        placed_ = core::MediaCorner(g_cfg);
        HMONITOR mon = ForegroundMonitor();
        MONITORINFO mi = MonitorInfoOf(mon);
        s_ = DpiScaleForMonitor(mon);
        int w = (int)(std::round(kW * s_) + 2 * std::round(kMargin * s_));
        int h = (int)(std::round(kH * s_) + 2 * std::round(kMargin * s_));
        surf_.Resize(w, h);
        const RECT& r = mi.rcWork;
        int gap = (int)std::round(24 * s_ - kMargin * s_);  // 24 px from the screen's edge to the panel, like the warmth popup
        const std::wstring& pos = placed_;
        bool top = pos.rfind(L"top", 0) == 0;
        bool left = pos.find(L"left") != std::wstring::npos, center = pos.find(L"center") != std::wstring::npos;
        x_ = center ? (r.left + r.right - w) / 2 : left ? r.left + gap : r.right - w - gap;
        y_ = top ? r.top + gap : r.bottom - h - gap;
    }

    // Cover bitmaps, made once per cover (not every frame).
    ID2D1Bitmap* Bitmap(ID2D1RenderTarget* rt, const ArtPtr& art) {
        if (!art || art->px.empty()) return nullptr;
        if (bmp_gen_ != surf_.generation) DropBitmaps();
        bmp_gen_ = surf_.generation;
        for (auto& e : bitmaps_)
            if (e.first == art.get()) return e.second;
        ID2D1Bitmap* b = nullptr;
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        if (FAILED(rt->CreateBitmap(D2D1::SizeU(art->w, art->h), art->px.data(), art->w * 4, &props, &b))) return nullptr;
        if (bitmaps_.size() >= 3) {  // keep the last few only
            bitmaps_.front().second->Release();
            bitmaps_.erase(bitmaps_.begin());
        }
        bitmaps_.push_back({art.get(), b});
        keep_.push_back(art);
        if (keep_.size() > 3) keep_.erase(keep_.begin());
        return b;
    }
    void DropBitmaps() {
        for (auto& e : bitmaps_) e.second->Release();
        bitmaps_.clear();
        keep_.clear();
    }

    void DrawCover(ID2D1RenderTarget* rt, const Content& c, double now) {
        auto P = [&](float v) { return std::round(v * s_); };
        float x0 = P(kArtX), y0 = P(kArtY), x1 = P(kArtX + kArt), y1 = P(kArtY + kArt);
        Fill(rt, surf_, x0, y0, x1, y1, C(g_pal.paper_dark));
        const auto hq = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
        double reveal = ArtReveal(now);
        // Old cover underneath, new one decoding in from the top.
        if (reveal < 1 && PhaseAt(now) == kChange)
            if (ID2D1Bitmap* old = Bitmap(rt, old_.art)) rt->DrawBitmap(old, D2D1::RectF(x0, y0, x1, y1), 1.f, hq);
        if (ID2D1Bitmap* b = Bitmap(rt, c.art)) {
            float cut = std::round(y0 + (y1 - y0) * (float)reveal);
            if (cut > y0) {
                D2D1_SIZE_F bs = b->GetSize();
                rt->DrawBitmap(b, D2D1::RectF(x0, y0, x1, cut), 1.f, hq,
                               D2D1::RectF(0, 0, bs.width, bs.height * (cut - y0) / (y1 - y0)));
            }
            if (reveal < 1) Fill(rt, surf_, x0, cut, x1, cut + std::max(1.f, P(2)), C(g_pal.ink));  // the scan line
        } else {
            // No cover: a quiet placeholder in the same frame.
            float m = P(kArt / 2 - 5);
            Fill(rt, surf_, x0 + m, y0 + m, x1 - m, y1 - m, C(g_pal.ink_soft));
        }
        Frame(rt, surf_, x0, y0, x1, y1, std::max(1.f, P(1)), C(g_pal.ink));
    }

    void Draw(ID2D1RenderTarget* rt, const Content& c, double now) {
        auto P = [&](float v) { return std::round(v * s_); };
        const float W = P(kW), px = std::max(1.f, P(1));
        rt->Clear(C(g_pal.paper));
        Fill(rt, surf_, P(18), P(17), P(24), P(23), C(g_pal.ink));
        Text(rt, surf_, c.header, D2D1::RectF(P(32), P(8), P(220), P(32)), {P(13), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(3)},
             C(g_pal.ink));
        Text(rt, surf_, c.note, D2D1::RectF(P(170), P(8), W - P(18), P(32)), {P(11), DWRITE_FONT_WEIGHT_NORMAL, P(1.5f), kRight},
             C(g_pal.ink_soft));
        Fill(rt, surf_, P(18), P(34), W - P(18), P(34) + px, C(g_pal.ink));

        if (c.media) {
            // Song card: cover, title, artist, and where the song is.
            DrawCover(rt, c, now);
            float tx = P(kTextX);
            TextStyle title{P(18), DWRITE_FONT_WEIGHT_SEMI_BOLD, 0};
            Text(rt, surf_, c.value, D2D1::RectF(tx, P(44), W - P(18), P(68)), title, C(g_pal.ink));
            if (Typing(now)) {
                float tw = std::min(TextWidth(c.value, title), W - P(18) - tx - P(14));
                Fill(rt, surf_, tx + tw + P(3), P(49), tx + tw + P(10), P(63), C(g_pal.ink));
            }
            Text(rt, surf_, c.sub, D2D1::RectF(tx, P(68), W - P(18), P(88)), {P(13), DWRITE_FONT_WEIGHT_NORMAL, P(0.5f)},
                 C(g_pal.ink_soft));
            if (c.end > 0) {
                double at = c.PosAt(now);
                std::wstring times = Clock(at) + L" / " + Clock(c.end);
                TextStyle small{P(11), DWRITE_FONT_WEIGHT_NORMAL, P(1), kRight};
                float timew = TextWidth(times, small) + P(4);
                Text(rt, surf_, times, D2D1::RectF(W - P(18) - timew - P(4), P(94), W - P(18), P(110)), small,
                     C(g_pal.ink_soft));
                float x0 = tx, x1 = W - P(18) - timew - P(14), y0 = P(99), y1 = P(105);
                Fill(rt, surf_, x0, y0, x1, y1, C(g_pal.paper_dark));
                float fx = x0 + (x1 - x0) * (float)std::clamp(at / c.end, 0.0, 1.0);
                Fill(rt, surf_, x0, y0, fx, y1, C(g_pal.ink));
                Fill(rt, surf_, fx, y0 - P(3), fx + px * 2, y1 + P(3), C(g_pal.ink));  // playhead
            }
            return;
        }

        // Volume card: the level, big, and a bar with ticks.
        TextStyle big{P(28), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(1)};
        Text(rt, surf_, c.value, D2D1::RectF(P(18), P(kValueY0), W - P(18), P(kValueY1)), big, C(g_pal.ink));
        if (Typing(now)) {
            float tw = std::min(TextWidth(c.value, big), W - P(50));
            float cy = (P(kValueY0) + P(kValueY1)) / 2, half = std::round(big.size * 0.38f);
            Fill(rt, surf_, P(18) + tw + P(3), cy - half, P(18) + tw + P(3) + std::round(big.size * 0.4f), cy + half,
                 C(g_pal.ink));
        }
        if (c.bar >= 0) {
            float x0 = P(18), x1 = W - P(18), y0 = P(92), y1 = P(102);
            Frame(rt, surf_, x0, y0, x1, y1, px, C(g_pal.ink));
            float fill = x0 + P(2) + (x1 - x0 - P(4)) * std::clamp(c.bar, 0.f, 1.f);
            Fill(rt, surf_, x0 + P(2), y0 + P(2), fill, y1 - P(2), C(c.dim_bar ? g_pal.paper_dark : g_pal.ink));
            for (int i = 1; i < 10; i++) {  // tick every 10%
                float tx = std::round(x0 + (x1 - x0) * i / 10.f);
                Fill(rt, surf_, tx, y1 + P(2), tx + px, y1 + P(5), C(g_pal.ink_soft));
            }
        }
    }

    void Render(double now) {
        const auto& st = glitch::CurrentStyle();
        Phase ph = PhaseAt(now);
        if (ph == kNone) {
            KillTimer(surf_.hwnd, 1);
            surf_.Hide();
            t_open_ = -1;
            return;
        }
        glitch::Fx fx;
        if (ph == kOpen) {
            fx = glitch::OpenFx(st, now - t_open_, seed_, kW, kH);
        } else if (ph == kChange) {
            double p = (now - t_change_) / st.change_ms;
            glitch::Rng rng = glitch::RngAt(seed_, now - t_change_, 5);
            bool full = g_cfg.animation == L"full";
            fx.tears = glitch::Tears(rng, st, 1 - p, full ? 0 : kValueY0, full ? kH : kValueY1);
            fx.blocks = glitch::Blocks(rng, st, 0.5 * (1 - p), kW, kH);
            fx.alpha = glitch::Flicker(rng, st, 0.5 * (1 - p));
        } else if (ph == kClose) {
            fx = glitch::CloseFx(st, now - close_at_, seed_, kW, kH);
        }
        Content vis = Visible(now);
        bool typing = Typing(now);
        // While it just sits there, only redraw when something visible moved:
        // the progress bar moves on a quarter second at a time.
        int progress_tick = vis.media && vis.end > 0 ? (int)(vis.PosAt(now) * 4) : 0;
        if (ph == kHold && drawn_ && vis.SameLook(last_) && fx == last_fx_ && typing == last_typing_ &&
            progress_tick == last_tick_ && surf_.Visible()) {
            Schedule(now, ph, vis);
            return;
        }
        last_ = vis;
        last_fx_ = fx;
        last_typing_ = typing;
        last_tick_ = progress_tick;
        drawn_ = true;
        float cw = std::round(kW * s_), ch = std::round(kH * s_), m = std::round(kMargin * s_);
        ID2D1RenderTarget* rt = surf_.Begin();
        if (!rt) return;
        if (auto* scratch = surf_.Scratch((int)cw, (int)ch)) {
            scratch->BeginDraw();
            Draw(scratch, vis, now);
            scratch->EndDraw();
            ID2D1Bitmap* bmp = nullptr;
            if (SUCCEEDED(scratch->GetBitmap(&bmp))) {
                Compose(rt, surf_, bmp, m, m, cw, ch, fx, s_);
                bmp->Release();
            }
        }
        surf_.End(x_, y_, fx.alpha);
        Schedule(now, ph, vis);
    }

    // The next frame: every 10 ms while something moves; while it just sits
    // there, only when the progress bar moves on or when it starts closing.
    void Schedule(double now, Phase ph, const Content& vis) {
        double next = 10;
        if (ph == kHold) {
            next = close_at_ - now;
            if (vis.media && vis.end > 0 && vis.playing) {
                double pos = vis.PosAt(now), to_tick = (std::floor(pos * 4) + 1) / 4 - pos;  // quarter seconds
                next = std::min(next, to_tick * 1000 + 1);
            }
        }
        SetTimer(surf_.hwnd, 1, (UINT)std::clamp(next, 10.0, 1000.0), nullptr);
    }

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = (Popup*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (msg == WM_TIMER && self) {
            self->Render(NowMs());
            return 0;
        }
        if (msg == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(h, msg, wp, lp);
    }

    Surface surf_;
    Content cur_, old_, last_;
    glitch::Fx last_fx_;
    bool last_typing_ = false, drawn_ = false;
    int last_tick_ = -1;
    double t_open_ = -1, t_change_ = -1e9, close_at_ = 0;
    uint32_t seed_ = 1;
    float s_ = 1;
    int x_ = 0, y_ = 0;
    std::wstring placed_;  // the corner it was placed in
    std::vector<std::pair<const Art*, ID2D1Bitmap*>> bitmaps_;
    std::vector<ArtPtr> keep_;  // keeps the covers alive as long as their bitmaps
    int bmp_gen_ = -1;
};

Popup g_popup;

// g_cfg belongs to the main thread (a reload replaces it whole), so the worker
// reads the one setting it needs from here instead.
std::atomic<bool> g_want_art{true};

// What the card on screen was opened for, so the worker's late updates (a
// cover arriving, say) only change a card that is about the same thing: a
// volume press, or a new song that wasn't shown, is left alone.
enum Owner { kOwnerNone, kOwnerVolume, kOwnerKeys, kOwnerSong };
Owner g_owner = kOwnerNone;

// ---- volume (Core Audio) -------------------------------------------------------------------------
IMMDeviceEnumerator* g_devices = nullptr;
// PKEY_Device_FriendlyName ("Speakers (Realtek Audio)").
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
std::wstring g_device_id, g_device_name;

IAudioEndpointVolume* Endpoint(std::wstring* name) {
    if (!g_devices &&
        FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void**)&g_devices)))
        return nullptr;
    IMMDevice* dev = nullptr;
    if (FAILED(g_devices->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) return nullptr;
    IAudioEndpointVolume* vol = nullptr;
    dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol);
    LPWSTR id = nullptr;
    if (SUCCEEDED(dev->GetId(&id)) && id) {
        if (g_device_id != id) {  // new default device: read its friendly name once
            g_device_id = id;
            g_device_name.clear();
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) g_device_name = v.pwszVal;
                PropVariantClear(&v);
                props->Release();
            }
        }
        CoTaskMemFree(id);
    }
    if (name) *name = g_device_name;
    dev->Release();
    return vol;
}

void GiveBackToWindows(UINT vk) {
    INPUT in[2] = {};
    for (auto& i : in) {
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = (WORD)vk;
        i.ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
        i.ki.dwExtraInfo = CANDELA_INJECTED;
    }
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

std::wstring Upper(std::wstring s) {
    CharUpperBuffW(s.data(), (DWORD)s.size());
    return s;
}

// The popup's words, in the current language.
std::wstring Say(const wchar_t* en, const wchar_t* ja) { return Tr(en, ja); }
std::wstring Name(std::wstring s) { return Upper(s); }

// Shows c in the popup. If the warmth popup uses the same corner, it goes away.
void ShowCard(const Content& c, double hold_ms) {
    if (core::MediaCorner(g_cfg) == g_cfg.popup_position) popup::Hide();
    g_popup.Show(c, hold_ms, false);
}
void UpdateCard(const Content& c, double hold_ms) { g_popup.Update(c, hold_ms); }

// Marks the volume changes Candela makes itself, so the change notification
// below can tell them from everyone else's. {5c4d3a1e-...}: any fixed GUID.
const GUID kOwnChange = {0x5c4d3a1e, 0x9b7f, 0x4c1a, {0x8e, 0x2d, 0x41, 0x43, 0x41, 0x4e, 0x44, 0x01}};

void ShowVolume(int pct, bool muted, const std::wstring& name) {
    Content c;
    c.header = Say(L"AUDIO", L"音量");
    c.note = Upper(name);
    c.value = muted ? Say(L"MUTED", L"ミュート") : std::to_wstring(pct) + L"%";
    c.bar = pct / 100.f;
    c.dim_bar = muted;
    ShowCard(c, 1300);
    g_owner = kOwnerVolume;
}

void Volume(UINT vk) {
    std::wstring name;
    IAudioEndpointVolume* vol = Endpoint(&name);
    if (!vol) {
        static bool logged = false;
        if (!logged) Log(L"No audio output device found; volume keys go to Windows");
        logged = true;
        if (vk) GiveBackToWindows(vk);
        return;
    }
    float level = 0;
    BOOL muted = FALSE;
    vol->GetMasterVolumeLevelScalar(&level);
    vol->GetMute(&muted);
    int step = g_cfg.volume_step, pct = (int)std::lround(level * 100);
    if (vk == 0) {
        // Only showing it (the settings' Preview): nothing changes.
    } else if (vk == VK_VOLUME_MUTE) {
        muted = !muted;
        vol->SetMute(muted, &kOwnChange);
    } else {
        // Snap to the step grid like Windows does, then move one step.
        if (vk == VK_VOLUME_UP) pct = std::min(100, (pct / step) * step + step);
        else pct = std::max(0, ((pct + step - 1) / step) * step - step);
        vol->SetMasterVolumeLevelScalar(pct / 100.f, &kOwnChange);
        if (muted) {
            muted = FALSE;  // changing the volume unmutes, as in Windows
            vol->SetMute(FALSE, &kOwnChange);
        }
    }
    vol->Release();
    if (vk && !g_cfg.popup_in_games && GameInFront()) return;
    ShowVolume(pct, muted != FALSE, name);
}

// ---- volume changes from anywhere else --------------------------------------------------------
// Core Audio calls these on a thread of its own. They only note what changed
// and wake the main thread (once, however many changes pile up meanwhile);
// the main thread draws the card and re-watches when the output device changes.
std::atomic<float> g_seen_level{0};
std::atomic<bool> g_seen_muted{false}, g_change_posted{false};

class VolumeWatcher : public IAudioEndpointVolumeCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == __uuidof(IAudioEndpointVolumeCallback)) {
            *out = static_cast<IAudioEndpointVolumeCallback*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }  // lives as long as Candela
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnNotify(AUDIO_VOLUME_NOTIFICATION_DATA* n) override {
        if (!n || n->guidEventContext == kOwnChange) return S_OK;
        g_seen_level = n->fMasterVolume;
        g_seen_muted = n->bMuted != FALSE;
        if (!g_change_posted.exchange(true)) PostMessageW(g_main, WM_CANDELA_VOLUME, kVolumeChanged, 0);
        return S_OK;
    }
} g_volume_watcher;

class DeviceWatcher : public IMMNotificationClient {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == IID_IUnknown || iid == __uuidof(IMMNotificationClient)) {
            *out = static_cast<IMMNotificationClient*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        // The output the volume keys use (Endpoint(): eRender, eMultimedia) is another one now.
        if (flow == eRender && role == eMultimedia) PostMessageW(g_main, WM_CANDELA_VOLUME, kDeviceChanged, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
} g_device_watcher;

IAudioEndpointVolume* g_watched = nullptr;  // the output whose volume changes are watched (main thread)
bool g_device_watch = false;

void Unwatch() {
    if (g_watched) {
        g_watched->UnregisterControlChangeNotify(&g_volume_watcher);
        g_watched->Release();
        g_watched = nullptr;
    }
}

// Watches the default output's volume (again, after it changed).
void Watch() {
    Unwatch();
    std::wstring name;
    g_watched = Endpoint(&name);  // also makes g_devices the first time, even with no output yet
    // Told when the default output changes (headphones plugged in, a headset connecting), to watch that one instead.
    if (!g_device_watch && g_devices) g_device_watch = SUCCEEDED(g_devices->RegisterEndpointNotificationCallback(&g_device_watcher));
    if (g_watched && FAILED(g_watched->RegisterControlChangeNotify(&g_volume_watcher))) {
        Log(L"Can't watch the volume of %ls; only volume keys show the popup", name.c_str());
        g_watched->Release();
        g_watched = nullptr;
    }
}

// ---- media session (WinRT, called by hand) ---------------------------------------------------------
// MinGW has no headers for Windows.Media.Control, so the few calls Candela needs
// are made through the interfaces' method tables. Layouts and IDs come from
// the Windows SDK metadata (via Microsoft's windows-rs bindings).
using HSTR = void*;
using RoInitialize_t = HRESULT(WINAPI*)(int);
using RoGetActivationFactory_t = HRESULT(WINAPI*)(HSTR, REFIID, void**);
using WindowsCreateString_t = HRESULT(WINAPI*)(LPCWSTR, UINT32, HSTR*);
using WindowsDeleteString_t = HRESULT(WINAPI*)(HSTR);
using WindowsGetStringRawBuffer_t = PCWSTR(WINAPI*)(HSTR, UINT32*);

struct WinRt {
    RoInitialize_t RoInitialize = nullptr;
    RoGetActivationFactory_t RoGetActivationFactory = nullptr;
    WindowsCreateString_t WindowsCreateString = nullptr;
    WindowsDeleteString_t WindowsDeleteString = nullptr;
    WindowsGetStringRawBuffer_t WindowsGetStringRawBuffer = nullptr;
    template <class T> static T Get(HMODULE m, const char* name) {
        return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    }
    bool Load() {
        HMODULE m = LoadLibraryW(L"combase.dll");
        if (!m) return false;
        RoInitialize = Get<RoInitialize_t>(m, "RoInitialize");
        RoGetActivationFactory = Get<RoGetActivationFactory_t>(m, "RoGetActivationFactory");
        WindowsCreateString = Get<WindowsCreateString_t>(m, "WindowsCreateString");
        WindowsDeleteString = Get<WindowsDeleteString_t>(m, "WindowsDeleteString");
        WindowsGetStringRawBuffer = Get<WindowsGetStringRawBuffer_t>(m, "WindowsGetStringRawBuffer");
        return RoInitialize && RoGetActivationFactory && WindowsCreateString && WindowsDeleteString &&
               WindowsGetStringRawBuffer;
    }
} g_rt;

// IGlobalSystemMediaTransportControlsSessionManagerStatics
const GUID IID_ManagerStatics = {0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}};
const GUID IID_AsyncInfo = {0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

// Method slots (IInspectable takes 0-5).
enum {
    kStatics_RequestAsync = 6,
    kAsyncOp_GetResults = 8,
    kAsyncInfo_Status = 7,
    kManager_GetCurrentSession = 6,
    kSession_SourceAppUserModelId = 6,
    kSession_TryGetMediaPropertiesAsync = 7,
    kSession_GetPlaybackInfo = 9,
    kSession_TryStopAsync = 12,
    kSession_TrySkipNextAsync = 16,
    kSession_TrySkipPreviousAsync = 17,
    kSession_TryTogglePlayPauseAsync = 20,
    kSession_GetTimelineProperties = 8,
    kProps_Title = 6,
    kProps_Artist = 9,
    kProps_Thumbnail = 15,
    kPlayback_PlaybackStatus = 7,
    kTimeline_EndTime = 7,
    kTimeline_Position = 10,
    kTimeline_LastUpdatedTime = 11,
    kStreamRef_OpenReadAsync = 6,
};
enum { kStatusPlaying = 4, kStatusPaused = 5 };

template <class F> F Method(void* obj, int slot) { return (F)((*(void***)obj)[slot]); }
using GetObj_t = HRESULT(STDMETHODCALLTYPE*)(void*, void**);
using GetStr_t = HRESULT(STDMETHODCALLTYPE*)(void*, HSTR*);
using GetI32_t = HRESULT(STDMETHODCALLTYPE*)(void*, int32_t*);
using GetI64_t = HRESULT(STDMETHODCALLTYPE*)(void*, int64_t*);  // TimeSpan, DateTime: 100 ns ticks

void Release(void*& p) {
    if (p) ((IUnknown*)p)->Release();
    p = nullptr;
}

std::wstring TakeString(HSTR h) {
    if (!h) return L"";
    UINT32 len = 0;
    PCWSTR raw = g_rt.WindowsGetStringRawBuffer(h, &len);
    std::wstring s(raw ? raw : L"", raw ? len : 0);
    g_rt.WindowsDeleteString(h);
    return s;
}

// Waits for a WinRT async call (we're on a worker thread, so blocking is fine).
bool Wait(void* op, int timeout_ms) {
    IUnknown* info = nullptr;
    if (!op || FAILED(((IUnknown*)op)->QueryInterface(IID_AsyncInfo, (void**)&info))) return false;
    bool ok = false;
    // Timed by the clock: Sleep(5) really sleeps ~15 ms unless the timer
    // resolution was raised, so counting sleeps would stretch every timeout.
    double start = NowMs();
    for (;;) {
        int32_t status = 0;
        if (FAILED(Method<GetI32_t>(info, kAsyncInfo_Status)(info, &status))) break;
        if (status == 1) { ok = true; break; }   // Completed
        if (status != 0) break;                  // Canceled or Error
        if (NowMs() - start >= timeout_ms) break;
        Sleep(5);
    }
    info->Release();
    return ok;
}

// Calls a method that starts an async operation and waits for it, ignoring
// its result. For the Try...Async actions, whose result is a plain bool, not
// an object: reading it as an object and releasing it crashes.
void AsyncRun(void* obj, int slot, int timeout_ms) {
    void* op = nullptr;
    if (SUCCEEDED(Method<GetObj_t>(obj, slot)(obj, &op)) && op) Wait(op, timeout_ms);
    Release(op);
}

// Calls a method that starts an async operation and returns its result object.
// Only for operations whose result IS an object.
void* AsyncResult(void* obj, int slot, int timeout_ms) {
    void* op = nullptr;
    void* result = nullptr;
    if (SUCCEEDED(Method<GetObj_t>(obj, slot)(obj, &op)) && op && Wait(op, timeout_ms))
        Method<GetObj_t>(op, kAsyncOp_GetResults)(op, &result);
    Release(op);
    return result;
}

struct Info {
    bool session = false;
    std::wstring title, artist, app;
    std::wstring aumid;  // the app's ID with Windows' media controls ("Spotify.exe", "Chrome", a Store app's ID...)
    int status = 0;
    int cmd = 0;
    bool fresh = false;  // a new song started by itself: open the card (later updates only change it)
    ArtPtr art;
    double pos = 0, end = 0, stamp = 0;  // seconds; stamp = NowMs() when pos was true
    // The song moving on (pos) isn't a change worth telling the popup about.
    bool operator==(const Info& o) const {
        return session == o.session && title == o.title && artist == o.artist && app == o.app && status == o.status &&
               art == o.art && end == o.end;
    }
};

// Decodes a cover image stream into a square (centre-cropped) of `size` px.
ArtPtr DecodeArt(IStream* stream, IWICImagingFactory* wic, UINT size) {
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapClipper* clip = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* conv = nullptr;
    ArtPtr result;
    UINT w = 0, h = 0;
    if (SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h &&
        SUCCEEDED(wic->CreateBitmapClipper(&clip))) {
        UINT side = std::min(w, h);
        WICRect r = {(INT)((w - side) / 2), (INT)((h - side) / 2), (INT)side, (INT)side};
        auto art = std::make_shared<Art>();
        art->w = art->h = (int)size;
        art->px.resize((size_t)size * size * 4);
        if (SUCCEEDED(clip->Initialize(frame, &r)) && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) &&
            SUCCEEDED(scaler->Initialize(clip, size, size, WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                       WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(conv->CopyPixels(nullptr, size * 4, (UINT)art->px.size(), art->px.data())))
            result = art;
    }
    if (conv) conv->Release();
    if (scaler) scaler->Release();
    if (clip) clip->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return result;
}

std::wstring AppFromAumid(std::wstring id) {
    size_t bang = id.find(L'!');
    if (bang != std::wstring::npos) id = id.substr(bang + 1);
    if (id.size() > 4 && !_wcsicmp(id.c_str() + id.size() - 4, L".exe")) id.resize(id.size() - 4);
    size_t dot = id.find_last_of(L'.');
    if (dot != std::wstring::npos && dot + 1 < id.size()) id = id.substr(dot + 1);
    bool hexish = id.size() >= 12 && std::all_of(id.begin(), id.end(), [](wchar_t c) { return iswxdigit(c); });
    return hexish ? L"" : id;  // some browsers use a hash here
}

class Worker {
public:
    enum Cmd { kToggle, kNext, kPrev, kStop, kNewSong };

    bool Start() {
        if (!g_rt.Load()) return false;
        thread_ = std::thread([this] { Run(); });
        return true;
    }
    void Post(Cmd c, UINT vk) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.push_back({c, vk});
        }
        cv_.notify_one();
    }
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            quit_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) thread_.join();
    }
    // Whether to look for new songs (from the main thread, when settings change).
    void Watch(bool on) {
        {
            std::lock_guard<std::mutex> lock(mu_);  // so a worker about to wait can't miss it
            if (watch_ == on) return;
            watch_ = on;
        }
        cv_.notify_one();
    }

private:
    void Run() {
        g_rt.RoInitialize(1);  // multithreaded
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                         (void**)&wic_);
        void* manager = nullptr;
        double manager_failed_at = -1e18;
        for (;;) {
            std::pair<Cmd, UINT> job;
            bool have_job = false;
            {
                std::unique_lock<std::mutex> lock(mu_);
                auto ready = [this] { return quit_ || !queue_.empty(); };
                if (watch_) cv_.wait_for(lock, std::chrono::milliseconds(kWatchMs), ready);
                else cv_.wait(lock, [this] { return quit_ || !queue_.empty() || watch_; });
                if (quit_) break;
                if (!queue_.empty()) {
                    job = queue_.front();
                    queue_.pop_front();
                    have_job = true;
                }
            }
            if (!have_job && !watch_) {
                songs_.clear();  // looking again later starts from what's playing then, not from stale songs
                look_have_ = false;
                continue;
            }
            // Asking again for the session manager every second would be wasted
            // work (and fill the log) on a PC without it: once a minute is enough.
            if (!manager && (have_job || NowMs() - manager_failed_at > 60000)) {
                manager = GetManager(!have_job && manager_failed_at > -1e18);
                if (!manager) manager_failed_at = NowMs();
            }
            if (!manager) {
                if (have_job) GiveBackToWindows(job.second);  // no media API here: let Windows handle the key
                continue;
            }
            if (have_job) Do(manager, job.first);
            else Look(manager);
            if (manager_dead_) {
                // The media service behind it went away (it restarted, say). Without
                // this, every key would say NO MEDIA until Candela restarts.
                Log(L"Media session manager disconnected; asking for a new one");
                manager_dead_ = false;
                Release(manager);
                manager_failed_at = NowMs();  // the once-a-second looks wait a minute; a key asks again at once
                if (have_job) {
                    manager = GetManager();
                    if (manager) Do(manager, job.first);  // the key that found it gone still counts
                    if (manager_dead_) {
                        manager_dead_ = false;
                        Release(manager);
                    }
                    if (!manager) GiveBackToWindows(job.second);
                }
            }
        }
        Release(manager);
        if (wic_) wic_->Release();
        wic_ = nullptr;
    }

    // The calls' errors that mean the object is gone for good (its service
    // restarted), not just "nothing playing".
    static bool Disconnected(HRESULT hr) {
        return hr == RPC_E_DISCONNECTED || hr == RPC_E_SERVER_DIED || hr == RPC_E_SERVER_DIED_DNE ||
               hr == CO_E_OBJNOTCONNECTED || hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ||
               hr == HRESULT_FROM_WIN32(RPC_S_CALL_FAILED);
    }

    // A song's identity: which app, and what's playing in it.
    static std::wstring Key(const Info& info) { return info.app + L"\n" + info.title + L"\n" + info.artist; }

    // Once a second while nothing else is asked: has a new song started by itself?
    //
    // Asking for the title and cover is the costly part of a look. So most
    // looks only read the cheap state (app, playing or paused, length,
    // position), and the title is asked for when that changed: another app,
    // play/pause, a different length, the position jumping back (a new song
    // starts at 0), or, while playing, every kPropsBackstopMs regardless.
    void Look(void* manager) {
        Info cheap = Read(manager, false);
        if (!cheap.session) {
            look_have_ = false;
            return;
        }
        if (look_have_ && cheap.aumid == look_last_.aumid && cheap.status == look_last_.status &&
            cheap.end == look_last_.end && !(look_last_.status == kStatusPlaying &&
                                              cheap.pos + 3 < look_last_.pos + (cheap.stamp - look_last_.stamp) / 1000.0) &&
            !(cheap.status == kStatusPlaying && NowMs() - look_props_at_ >= kPropsBackstopMs))
            return;
        Info now = Read(manager);
        look_last_ = now;
        look_have_ = true;
        look_props_at_ = NowMs();
        if (!now.session || now.title.empty()) return;  // nothing playing, or between songs: keep the last one
        std::wstring key = Key(now);
        auto it = songs_.find(now.aumid);
        bool known = it != songs_.end();
        if (known && it->second == key) return;
        songs_[now.aumid] = key;
        // An app seen for the first time only teaches what it's on. Pausing,
        // seeking, and going back to an app that's still on the same song
        // (Spotify after a YouTube video) never count as a new song. Which
        // apps' new songs show is the main thread's to decide (song_apps).
        if (!known || now.status != kStatusPlaying) return;
        Info shown = now;
        shown.fresh = true;
        Send(shown, kNewSong);
        Follow(manager, now, kNewSong);  // the cover and the artist often arrive a moment after the title
    }

    void* GetManager(bool quiet = false) {
        HSTR name = nullptr;
        const wchar_t* cls = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
        if (FAILED(g_rt.WindowsCreateString(cls, (UINT32)wcslen(cls), &name))) return nullptr;
        void* statics = nullptr;
        HRESULT hr = g_rt.RoGetActivationFactory(name, IID_ManagerStatics, &statics);
        g_rt.WindowsDeleteString(name);
        if (FAILED(hr) || !statics) {
            if (!quiet) Log(L"Media session API unavailable (0x%08lx)", (unsigned long)hr);
            return nullptr;
        }
        void* manager = AsyncResult(statics, kStatics_RequestAsync, 2000);
        Release(statics);
        if (!manager && !quiet) Log(L"Media session manager request failed");
        return manager;
    }

    Info Read(void* manager, bool with_props = true) {
        Info info;
        void* session = nullptr;
        HRESULT hr = Method<GetObj_t>(manager, kManager_GetCurrentSession)(manager, &session);
        if (Disconnected(hr)) manager_dead_ = true;
        if (FAILED(hr) || !session) return info;
        info.session = true;
        HSTR h = nullptr;
        if (SUCCEEDED(Method<GetStr_t>(session, kSession_SourceAppUserModelId)(session, &h))) {
            info.aumid = TakeString(h);
            info.app = AppFromAumid(info.aumid);
        }
        void* playback = nullptr;
        if (SUCCEEDED(Method<GetObj_t>(session, kSession_GetPlaybackInfo)(session, &playback)) && playback) {
            int32_t status = 0;
            if (SUCCEEDED(Method<GetI32_t>(playback, kPlayback_PlaybackStatus)(playback, &status))) info.status = status;
            Release(playback);
        }
        void* timeline = nullptr;
        if (SUCCEEDED(Method<GetObj_t>(session, kSession_GetTimelineProperties)(session, &timeline)) && timeline) {
            int64_t end = 0, pos = 0, updated = 0;
            Method<GetI64_t>(timeline, kTimeline_EndTime)(timeline, &end);
            Method<GetI64_t>(timeline, kTimeline_Position)(timeline, &pos);
            Method<GetI64_t>(timeline, kTimeline_LastUpdatedTime)(timeline, &updated);
            info.end = end / 1e7;
            info.pos = pos / 1e7;
            // Apps report the position now and then; carry it forward to now.
            FILETIME ft;
            GetSystemTimePreciseAsFileTime(&ft);
            int64_t now = ((int64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
            if (info.status == kStatusPlaying && updated > 0 && now > updated)
                info.pos += std::min((now - updated) / 1e7, 24 * 3600.0);
            info.stamp = NowMs();
            Release(timeline);
        }
        if (!with_props) {
            Release(session);
            return info;
        }
        if (void* props = AsyncResult(session, kSession_TryGetMediaPropertiesAsync, 400)) {
            h = nullptr;
            if (SUCCEEDED(Method<GetStr_t>(props, kProps_Title)(props, &h))) info.title = TakeString(h);
            h = nullptr;
            if (SUCCEEDED(Method<GetStr_t>(props, kProps_Artist)(props, &h))) info.artist = TakeString(h);
            info.art = Cover(props, info);
            Release(props);
        }
        Release(session);
        return info;
    }

    // The song's cover, fetched once per song (it can arrive a moment after
    // the title, so a song without one yet is asked again next time).
    //
    // Apps often share the cover a second or two after the title, so a song
    // without one is asked again, at most every kArtRetryMs, for kArtWaitMs
    // after it started; after that it's taken to have none. Keyed by app and
    // title only: the artist arriving a moment later keeps the cover.
    ArtPtr Cover(void* props, const Info& info) {
        std::wstring key = info.app + L"\n" + info.title;
        double now = NowMs();
        if (key != art_key_) {
            art_.reset();
            art_key_ = key;
            art_since_ = now;
            art_asked_ = -1e18;
        }
        if (art_) return art_;
        if (!wic_ || !g_want_art || !CoverPending(now) || now - art_asked_ < kArtRetryMs) return nullptr;
        art_asked_ = now;
        void* ref = nullptr;
        if (FAILED(Method<GetObj_t>(props, kProps_Thumbnail)(props, &ref)) || !ref) return nullptr;
        void* stream = AsyncResult(ref, kStreamRef_OpenReadAsync, 800);
        Release(ref);
        if (!stream) return nullptr;
        IStream* is = nullptr;
        if (SUCCEEDED(CreateStreamOverRandomAccessStream((IUnknown*)stream, IID_IStream, (void**)&is)) && is) {
            art_ = DecodeArt(is, wic_, 160);
            is->Release();
        }
        Release(stream);
        return art_;
    }

    // The current song has no cover yet, but one may still come.
    bool CoverPending(double now) const { return !art_ && g_want_art && now - art_since_ < kArtWaitMs; }

    void Send(const Info& info, Cmd cmd) {
        auto* copy = new Info(info);
        copy->cmd = cmd;
        if (!PostMessageW(g_main, WM_CANDELA_MEDIA_INFO, 0, (LPARAM)copy)) delete copy;
    }

    void Do(void* manager, Cmd cmd) {
        void* session = nullptr;
        if (Disconnected(Method<GetObj_t>(manager, kManager_GetCurrentSession)(manager, &session))) {
            manager_dead_ = true;
            return;  // the caller gets a new manager and tries again
        }
        if (session) {
            int slot = cmd == kToggle ? kSession_TryTogglePlayPauseAsync
                       : cmd == kNext ? kSession_TrySkipNextAsync
                       : cmd == kPrev ? kSession_TrySkipPreviousAsync
                                      : kSession_TryStopAsync;
            AsyncRun(session, slot, 600);
            Release(session);
        }
        // The app updates its state a moment later: watch for ~1.5 s and pass
        // every change on, so the popup's text scrambles into the new song.
        Info last = Read(manager);
        Send(last, cmd);
        Follow(manager, last, cmd);
    }

    // Watches for ~1.5 s after something happened and passes every change on.
    // While the cover hasn't come yet it keeps watching, a little slower, for
    // up to kFollowMaxMs, so a late cover still makes it onto the card.
    void Follow(void* manager, Info last, Cmd cmd) {
        if (last.session && !last.title.empty()) songs_[last.aumid] = Key(last);
        double start = NowMs();
        for (;;) {
            double waited = NowMs() - start;
            bool waiting_for_cover = last.session && !last.title.empty() && CoverPending(NowMs());
            if (waited >= (waiting_for_cover ? kFollowMaxMs : 1500)) return;
            Sleep(waited < 1500 ? 120 : 250);
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (quit_ || !queue_.empty()) return;  // a newer key press takes over
            }
            if (manager_dead_) return;  // Run() gets a new one
            Info now = Read(manager);
            if (!(now == last)) {
                last = now;
                Send(now, cmd);
            }
            // What the song settles on is what the next look compares with.
            if (now.session && !now.title.empty()) songs_[now.aumid] = Key(now);
        }
    }

    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::pair<Cmd, UINT>> queue_;
    bool quit_ = false;
    std::atomic<bool> watch_{false};
    bool manager_dead_ = false;  // a call found the session manager disconnected (worker thread only)
    Info look_last_;  // what the last full look found (worker thread only)
    bool look_have_ = false;
    double look_props_at_ = 0;
    std::map<std::wstring, std::wstring> songs_;  // each app's last song seen, by its ID (worker thread only)
    IWICImagingFactory* wic_ = nullptr;
    std::wstring art_key_;
    ArtPtr art_;
    double art_since_ = 0, art_asked_ = -1e18;  // when this song was first seen, and its cover last asked for
    static constexpr int kWatchMs = 1000;
    static constexpr double kPropsBackstopMs = 4000;
    static constexpr double kArtRetryMs = 250, kArtWaitMs = 8000, kFollowMaxMs = 6000;
};

Worker g_worker;
bool g_worker_ok = false;
Content g_last_media;  // what the popup last said about the music

// ---- which apps' new songs show (song_apps, plus Spotify always) -----------------------------------------
// Windows names the app behind a media session by an ID, not by its .exe:
// "Spotify.exe", a full path, "Chrome", "MSEdge", a Store app's
// "Publisher.App_hash!App", or a hash Firefox makes up. So an .exe matches
// when the ID names it, when the app's name in the ID is the .exe's name,
// or when one of the .exe's windows carries that ID.
const wchar_t* const kAlwaysSongApp = L"spotify.exe";

std::wstring Stem(const std::wstring& exe) {
    return exe.size() > 4 && !_wcsicmp(exe.c_str() + exe.size() - 4, L".exe") ? exe.substr(0, exe.size() - 4) : exe;
}

bool IdNamesExe(const std::wstring& aumid, const std::wstring& exe) {
    size_t slash = aumid.find_last_of(L"\\/");
    std::wstring tail = slash == std::wstring::npos ? aumid : aumid.substr(slash + 1);
    if (!_wcsicmp(tail.c_str(), exe.c_str())) return true;
    std::wstring app = AppFromAumid(aumid);
    return !app.empty() && !_wcsicmp(app.c_str(), Stem(exe).c_str());
}

// PKEY_AppUserModel_ID
const PROPERTYKEY kAppUserModelId = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

// The .exe of a window that carries this app ID ("" if none is open).
std::wstring ExeWithWindowId(const std::wstring& aumid) {
    struct Search {
        const std::wstring* id;
        std::wstring exe;
    } search{&aumid, L""};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* s = (Search*)lp;
            if (!IsWindowVisible(h)) return TRUE;
            IPropertyStore* props = nullptr;
            if (FAILED(SHGetPropertyStoreForWindow(h, IID_IPropertyStore, (void**)&props)) || !props) return TRUE;
            PROPVARIANT v;
            PropVariantInit(&v);
            bool same = SUCCEEDED(props->GetValue(kAppUserModelId, &v)) && v.vt == VT_LPWSTR && v.pwszVal &&
                        !_wcsicmp(v.pwszVal, s->id->c_str());
            PropVariantClear(&v);
            props->Release();
            if (!same) return TRUE;
            s->exe = ProcessExeName(h);
            return FALSE;
        },
        (LPARAM)&search);
    return search.exe;
}

bool SongAppAllowed(const std::wstring& aumid) {
    if (aumid.empty()) return false;
    std::vector<std::wstring> apps = core::SplitList(g_cfg.song_apps);
    apps.insert(apps.begin(), kAlwaysSongApp);
    for (auto& exe : apps)
        if (IdNamesExe(aumid, exe)) return true;
    // Only asked when the ID alone didn't tell, and not again for 30 s: it goes
    // through every window, and a song change would otherwise do that each time.
    static std::map<std::wstring, std::pair<std::wstring, double>> owners;
    double now = NowMs();
    auto cached = owners.find(aumid);
    if (cached == owners.end() || now - cached->second.second > 30000) {
        if (owners.size() > 32) owners.clear();
        cached = owners.insert_or_assign(aumid, std::make_pair(ExeWithWindowId(aumid), now)).first;
    }
    std::wstring owner = cached->second.first;
    for (auto& exe : apps)
        if (!owner.empty() && !_wcsicmp(owner.c_str(), exe.c_str())) return true;
    return false;
}

}  // namespace

std::wstring Clock(double seconds) {
    int t = (int)std::max(0.0, seconds);
    wchar_t buf[32];
    if (t >= 3600) swprintf(buf, 32, L"%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else swprintf(buf, 32, L"%d:%02d", t / 60, t % 60);
    return buf;
}

bool Init() {
    g_want_art = g_cfg.media_art;
    g_popup.Init();
    Watch();
    g_worker_ok = g_worker.Start();
    if (!g_worker_ok) Log(L"WinRT not available; media keys go to Windows");
    return true;
}

void OnKey(UINT vk) {
    switch (vk) {
        case VK_VOLUME_UP: case VK_VOLUME_DOWN: case VK_VOLUME_MUTE:
            Volume(vk);
            return;
    }
    if (!g_worker_ok) {
        GiveBackToWindows(vk);
        return;
    }
    Worker::Cmd cmd = vk == VK_MEDIA_NEXT_TRACK   ? Worker::kNext
                      : vk == VK_MEDIA_PREV_TRACK ? Worker::kPrev
                      : vk == VK_MEDIA_STOP       ? Worker::kStop
                                                  : Worker::kToggle;
    g_worker.Post(cmd, vk);
    if (!g_cfg.popup_in_games && GameInFront()) return;
    // Show straight away with what we knew; the real state scrambles in a
    // moment later when the app answers.
    Content c = g_last_media;
    c.media = true;
    if (c.playing) {  // freeze where the song was; the fresh position arrives in a moment
        c.pos = c.PosAt(NowMs());
        c.playing = false;
    }
    c.stamp = NowMs();
    c.header = cmd == Worker::kNext   ? Say(L"NEXT", L"次へ")
               : cmd == Worker::kPrev ? Say(L"PREVIOUS", L"前へ")
               : cmd == Worker::kStop ? Say(L"STOP", L"停止")
                                      : Say(L"PLAY / PAUSE", L"再生／一時停止");
    if (c.value.empty()) c.value = Say(L"SEARCHING", L"取得中");
    ShowCard(c, 2200);
    g_owner = kOwnerKeys;
}

// Whether a new song starting by itself shows the card (the worker looks for
// them only while this is true). Off while paused or stepping aside for an
// anti-cheat game, like the rest of the volume popup.
bool NewSongWanted() {
    return g_worker_ok && g_cfg.media_popup && g_cfg.media_new_song && !engine::Paused() &&
           engine::SteppingAsideFor().empty();
}

void OnInfo(LPARAM lp) {
    Info* info = (Info*)lp;
    if (!info) return;
    Content c;
    c.header = Say(L"MEDIA", L"メディア");
    c.media = true;
    if (!info->session) {
        c.value = Say(L"NO MEDIA", L"メディアなし");
    } else {
        c.art = info->art;
        c.pos = info->pos;
        c.end = info->end;
        c.stamp = info->stamp;
        c.playing = info->status == kStatusPlaying;
        c.note = Name(info->app);
        c.header = info->cmd == Worker::kNewSong && info->status == kStatusPlaying ? Say(L"NOW PLAYING", L"次の曲")
                   : info->status == kStatusPlaying  ? Say(L"PLAYING", L"再生中")
                   : info->status == kStatusPaused ? Say(L"PAUSED", L"一時停止")
                   : info->cmd == Worker::kStop    ? Say(L"STOPPED", L"停止")
                                                   : Say(L"MEDIA", L"メディア");
        c.value = info->title.empty() ? Say(L"UNKNOWN", L"不明") : info->title;
        c.sub = info->artist;
    }
    bool fresh = info->fresh, song = info->cmd == Worker::kNewSong;
    std::wstring aumid = info->aumid;
    delete info;
    g_last_media = c;
    if (fresh) {
        // A new song by itself: the card opens if it's from one of the apps
        // in the list, unless it was turned off meanwhile or a game is in front.
        if (NewSongWanted() && SongAppAllowed(aumid) && (g_cfg.popup_in_games || !GameInFront())) {
            ShowCard(c, 3500);
            g_owner = kOwnerSong;
        } else if (g_owner == kOwnerSong) {
            g_owner = kOwnerNone;  // its updates mustn't turn an older song card into this one
        }
        return;
    }
    if (g_owner != (song ? kOwnerSong : kOwnerKeys)) return;  // the card is about something else now
    UpdateCard(c, 2200);  // only if the popup is up (not over a game)
}

void SettingsChanged() {
    g_want_art = g_cfg.media_art;
    if (g_worker_ok) g_worker.Watch(NewSongWanted());
}

bool IsOwnWindow(HWND h) { return g_popup.Owns(h); }

void Preview() { Volume(0); }

void OnVolume(WPARAM what) {
    if (what == kDeviceChanged) {
        Watch();
        return;
    }
    g_change_posted = false;  // a change from here on wakes the main thread again
    // Shown whenever the volume keys would show it: the popup is on, Candela
    // isn't paused or stepping aside for an anti-cheat game, no game in front.
    if (!g_cfg.media_popup || engine::Paused() || !engine::SteppingAsideFor().empty()) return;
    if (!g_cfg.popup_in_games && GameInFront()) return;
    ShowVolume((int)std::lround(g_seen_level * 100), g_seen_muted, g_device_name);  // the name Watch() read
}

std::vector<std::wstring> AlwaysSongApps() { return {kAlwaysSongApp}; }

void Hide() { g_popup.Hide(); }

// Developer screenshots (--dump): fake presses without touching real volume.
void DebugShow(int kind) {
    Content c;
    if (kind == 0 || kind == 1) {
        c.header = Say(L"AUDIO", L"音量");
        c.note = Say(L"SPEAKERS (REALTEK(R) AUDIO)", L"スピーカー (REALTEK(R) AUDIO)");
        int pct = kind == 0 ? 44 : 46;
        c.value = std::to_wstring(pct) + L"%";
        c.bar = pct / 100.f;
        ShowCard(c, 1300);
        return;
    }
    // A made-up cover: diagonal bands, so the decode-in is easy to see.
    auto art = std::make_shared<Art>();
    art->w = art->h = 160;
    art->px.resize(160 * 160 * 4);
    for (int y = 0; y < 160; y++)
        for (int x = 0; x < 160; x++) {
            BYTE* p = &art->px[(y * 160 + x) * 4];
            bool band = ((x + y) / 20) % 2 == (kind == 2 ? 0 : 1);
            int v = band ? 200 : 70;
            p[0] = (BYTE)(v * (kind == 2 ? 0.6 : 1.0));
            p[1] = (BYTE)(v * 0.8);
            p[2] = (BYTE)(kind == 2 ? v : v * 0.5);
            p[3] = 255;
        }
    c.media = true;
    c.art = art;
    c.header = kind == 2 ? Say(L"PLAYING", L"再生中") : Say(L"NEXT", L"次へ");
    c.note = Name(L"spotify");
    c.value = kind == 2 ? Tr(L"Amber Hours", L"琥珀の時間") : Tr(L"Low Tide Static", L"引き潮のノイズ");
    c.sub = Tr(L"The Late Lamps", L"ザ・レイト・ランプス");
    c.end = kind == 2 ? 346 : 272;
    c.pos = kind == 2 ? 83 : 0;
    c.playing = true;
    c.stamp = NowMs();
    ShowCard(c, 2200);
}
void DebugTick() { g_popup.Tick(); }
bool DebugSave(const std::wstring& png) { return g_popup.Save(png); }

void Shutdown() {
    if (g_worker_ok) g_worker.Stop();
    g_popup.Destroy();
    Unwatch();
    if (g_devices && g_device_watch) g_devices->UnregisterEndpointNotificationCallback(&g_device_watcher);
    g_device_watch = false;
    if (g_devices) g_devices->Release();
    g_devices = nullptr;
}

}  // namespace media
