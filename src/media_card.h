// What the volume / media popup shows.
#pragma once
#include <algorithm>
#include <memory>

#include "render.h"

namespace media {

// An album cover, decoded on the media worker: premultiplied BGRA pixels.
struct Art {
    int w = 0, h = 0;
    std::vector<BYTE> px;
};
using ArtPtr = std::shared_ptr<const Art>;

struct Content {
    std::wstring header, note, value, sub;
    float bar = -1;        // 0..1, or < 0 for none (volume)
    bool dim_bar = false;  // muted: the bar shows the level greyed out
    bool media = false;    // a song card (cover, title, artist, progress)
    ArtPtr art;
    // Where the song is: seconds, measured at `stamp` (NowMs). end <= 0: unknown.
    double pos = 0, end = 0, stamp = 0;
    bool playing = false;
    // Same words and picture: anything else (the song moving on) never
    // counts as a change, or the card would scramble constantly.
    bool SameLook(const Content& o) const {
        return header == o.header && note == o.note && value == o.value && sub == o.sub && bar == o.bar &&
               dim_bar == o.dim_bar && media == o.media && art == o.art;
    }
    double PosAt(double now) const {
        double p = pos + (playing ? (now - stamp) / 1000.0 : 0);
        return end > 0 ? std::clamp(p, 0.0, end) : p;
    }
};

std::wstring Clock(double seconds);  // "3:07", "1:02:03"


}  // namespace media
