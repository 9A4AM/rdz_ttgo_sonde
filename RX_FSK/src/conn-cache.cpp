#include "conn-cache.h"
#include <stdlib.h>
#include <time.h>
#include "logger.h"

static const char *TAG = "framecache";

// Upper bound on cachesize so a mis-typed config value can't exhaust the ESP32 heap.
// At ~1 frame/s even 120 covers a 2-min gap, and MAX_REPLAY_AGE (30 min) caps how far
// back replay reaches anyway, so buffering more than a few hundred frames is pointless.
#define FRAMECACHE_MAX 600

FrameCache frameCache;

void FrameCache::begin(int size) {
    if (buf) return;                    // already allocated
    if (size <= 0) { capacity = 0; return; }
    if (size > FRAMECACHE_MAX) {
        LOG_W(TAG, "cachesize %d exceeds max %d; clamping\n", size, FRAMECACHE_MAX);
        size = FRAMECACHE_MAX;
    }
    buf = (CachedFrame *) calloc(size, sizeof(CachedFrame));
    if (!buf) {
        capacity = 0;
        LOG_E(TAG, "cache alloc failed for %d frames; feature disabled\n", size);
        return;
    }
    capacity = size;
    head = 0;
    LOG_I(TAG, "frame cache enabled: %d frames (%d bytes)\n", size, (int)(size * sizeof(CachedFrame)));
}

uint32_t FrameCache::push(SondeInfo *s) {
    if (!enabled()) return head;
    uint32_t seq = head;
    CachedFrame *e = &buf[seq % capacity];
    e->seq = seq;
    e->rxtime = s->rxtime;
    e->type = s->type;
    e->freq = s->freq;
    e->afc = s->afc;
    e->rssi = s->rssi;
    e->d = s->d;
    head++;
    return seq;
}

uint32_t FrameCache::oldestSeq() {
    if (!enabled()) return 0;
    return (head > (uint32_t)capacity) ? head - (uint32_t)capacity : 0;
}

bool FrameCache::get(uint32_t seq, SondeInfo *out) {
    if (!enabled()) return false;
    if (seq >= head) return false;             // not pushed yet
    if (seq < oldestSeq()) return false;       // evicted
    CachedFrame *e = &buf[seq % capacity];
    if (e->seq != seq) return false;           // safety: slot reused
    out->type = e->type;
    out->freq = e->freq;
    out->afc = e->afc;
    out->rssi = e->rssi;
    out->rxtime = e->rxtime;
    out->d = e->d;
    return true;
}
