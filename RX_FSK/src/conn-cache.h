#ifndef conn_cache_h
#define conn_cache_h

#include "Sonde.h"

// One buffered frame: exactly the fields the uploaders read, plus bookkeeping.
struct CachedFrame {
    uint32_t seq;       // monotonic sequence number of this frame
    uint32_t rxtime;    // wall-clock seconds when received (age cap + SondeHub time_received)
    SondeType type;
    float freq;
    int32_t afc;
    int rssi;
    SondeData d;
};

// Shared in-RAM ring of recent decoded frames. Drop-oldest on overflow.
// Not a Conn; a passive store that connectors replay from via a per-connector cursor.
class FrameCache {
public:
    // Allocate the ring for `size` frames (<=0 disables). Idempotent (no-op if already begun).
    void begin(int size);
    bool enabled() { return buf != nullptr && capacity > 0; }

    // Snapshot s into the ring; returns the assigned seq. No-op returning headSeq() if disabled.
    uint32_t push(SondeInfo *s);

    uint32_t headSeq() { return head; }   // seq the next push will assign (== total pushes)
    uint32_t oldestSeq();                 // lowest seq still resident in the ring

    // Reconstruct frame `seq` into *out; false if evicted, not yet pushed, or disabled.
    bool get(uint32_t seq, SondeInfo *out);

private:
    CachedFrame *buf = nullptr;
    int capacity = 0;
    uint32_t head = 0;                    // next seq to assign
};

extern FrameCache frameCache;
#endif
