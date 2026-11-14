// The player's own event table (coop_events.h), game 1.1.1: RSDB/EventSetting.Product.<n>.rstbl.byml.zs of the game's
// RomFS (the highest <n>), zstd with the game's dictionary (Pack/ZsDic.pack.zs, member zs.zsdic). Its rows are BYML
// dictionaries; a row's __RowId is the event's name, its EventSettingComponentList the kind of event (an NPC one: a
// talk, a "near" line, a chase, an each-frame line) and its EventFileType whether it is a pre-rendered movie.
//
// Read once, on a thread of its own (some 2400 rows: well under a second), the first time anything asks; the lists are
// published with the state (release, acquire) and never change after.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "coop_events.h"
#include "coop_ext_wire.h"   // COOP_CUT_EVENT_MAX: the longest event name a CUTSCENE carries
#include "fs.h"
#include "host.h"
#include "platform/platform.h"

#define H_MOVIE 0x5044cc65u   // MurmurHash3 of "Movie": the EventFileType of a pre-rendered movie

static CoopStory *stories;    // story cutscenes, sorted by name
static int nstories;
static uint32_t *talk_flags;  // HavePlayedEvent.<event> of the NPC talk events, sorted
static int ntalk;
static volatile int32_t ev_state;   // 0 not asked, 1 being read, 2 read

// MurmurHash3 (32 bits, seed 0): the game names its flags by it.
static uint32_t murmur3(const uint8_t *p, size_t len) {
    uint32_t h = 0;
    size_t n = len & ~(size_t)3;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t k;
        memcpy(&k, p + i, 4);
        k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u;
        h ^= k, h = (h << 13) | (h >> 19), h = h * 5 + 0xe6546b64u;
    }
    uint32_t k = 0;
    switch (len & 3) {
    case 3: k ^= (uint32_t)p[n + 2] << 16;   // fall through
    case 2: k ^= (uint32_t)p[n + 1] << 8;    // fall through
    case 1:
        k ^= p[n];
        k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u;
        h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}
static uint32_t hash_of(const char *s) { return murmur3((const uint8_t *)s, strlen(s)); }

static uint32_t u24(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
static uint32_t u32le(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

// The member of a SARC archive by name: its offset and size; 0 when it is not there or the archive does not hold.
static int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size) {
    if (n < 0x20 || memcmp(p, "SARC", 4)) return 0;
    uint16_t head, count;
    uint32_t data;
    memcpy(&head, p + 4, 2);
    memcpy(&data, p + 12, 4);
    if ((size_t)head + 12 > n || memcmp(p + head, "SFAT", 4)) return 0;
    memcpy(&count, p + head + 6, 2);
    size_t names = (size_t)head + 12 + (size_t)count * 16 + 8, len = strlen(name);
    for (unsigned i = 0; i < count; i++) {
        uint32_t attr, lo, hi;
        size_t e = (size_t)head + 12 + (size_t)i * 16;
        if (e + 16 > n) return 0;
        memcpy(&attr, p + e + 4, 4), memcpy(&lo, p + e + 8, 4), memcpy(&hi, p + e + 12, 4);
        size_t s = names + (size_t)(attr & 0xffffff) * 4;
        if (s + len + 1 > n || memcmp(p + s, name, len + 1)) continue;
        if (lo > hi || (size_t)data + hi > n) return 0;
        *at = data + lo, *size = hi - lo;
        return 1;
    }
    return 0;
}

// The i-th text of a BYML text table (node 0xc2: its count, then the texts' offsets from the node).
static const char *byml_text(const uint8_t *p, size_t n, uint32_t table, uint32_t i) {
    if (!table || (size_t)table + 4 > n || p[table] != 0xc2 || i >= u24(p + table + 1) || (size_t)table + 8 + 4 * (size_t)i > n)
        return NULL;
    uint32_t off = u32le(p + table + 4 + 4 * (size_t)i);
    if ((size_t)table + off >= n || !memchr(p + table + off, 0, n - table - off)) return NULL;
    return (const char *)p + table + off;
}

static int key_index(const uint8_t *p, size_t n, uint32_t keys, const char *want) {
    uint32_t count = keys + 4 <= n && p[keys] == 0xc2 ? u24(p + keys + 1) : 0;
    for (uint32_t i = 0; i < count; i++) {
        const char *k = byml_text(p, n, keys, i);
        if (k && !strcmp(k, want)) return (int)i;
    }
    return -1;
}

static int by_story_name(const void *a, const void *b) {
    return strcmp(((const CoopStory *)a)->name, ((const CoopStory *)b)->name);
}
static int by_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

// The rows: a BYML array (node 0xc0) of dictionaries (0xc1: entries of a key's number in 24 bits, the value's kind, the
// value), texts (0xa0) by their number in the text table.
static void events_parse(const uint8_t *p, size_t n, int *rows_out, int *movies_out) {
    if (n < 16 || memcmp(p, "YB", 2)) return;
    uint32_t keys = u32le(p + 4), strs = u32le(p + 8), root = u32le(p + 12);
    if ((size_t)root + 4 > n || p[root] != 0xc0) return;
    int k_row = key_index(p, n, keys, "__RowId"), k_list = key_index(p, n, keys, "EventSettingComponentList"),
        k_type = key_index(p, n, keys, "EventFileType");
    if (k_row < 0 || k_list < 0) return;
    uint32_t count = u24(p + root + 1), vals = root + 4 + ((count + 3) & ~3u);
    if ((size_t)vals + 4ull * count > n) return;
    stories = calloc(count, sizeof *stories);
    talk_flags = calloc(count, sizeof *talk_flags);
    if (!stories || !talk_flags) return;
    int movies = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (p[root + 4 + i] != 0xc1) continue;
        uint32_t d = u32le(p + vals + 4 * i);
        if ((size_t)d + 4 > n || p[d] != 0xc1) continue;
        uint32_t m = u24(p + d + 1);
        const char *row = NULL, *list = "";
        uint32_t type = 0;
        for (uint32_t k = 0; k < m && (size_t)d + 12 + 8 * (size_t)k <= n; k++) {
            const uint8_t *e = p + d + 4 + 8 * (size_t)k;
            int key = (int)u24(e);
            uint32_t v = u32le(e + 4);
            if (e[3] == 0xa0 && key == k_row) row = byml_text(p, n, strs, v);
            else if (e[3] == 0xa0 && key == k_list) list = byml_text(p, n, strs, v);
            else if ((e[3] == 0xd3 || e[3] == 0xd1) && key == k_type) type = v;
            if (!list) list = "";
        }
        if (!row || !row[0] || strlen(row) > COOP_CUT_EVENT_MAX) continue;
        (*rows_out)++;
        const char *base = strrchr(list, '/');
        base = base ? base + 1 : list;
        if (!strncmp(base, "NPC", 3)) {   // an NPC talk, "near" line, chase or each-frame line
            char flag[128];
            snprintf(flag, sizeof flag, "HavePlayedEvent.%s", row);
            talk_flags[ntalk++] = hash_of(flag);
            continue;
        }
        if (strncmp(row, "Dm_", 3) || !strncmp(row, "Dm_SY_", 6) || !strncmp(row, "Dm_Title_", 9)) continue;
        char *copy = malloc(strlen(row) + 1);
        if (!copy) continue;
        strcpy(copy, row);
        CoopStory *s = &stories[nstories++];
        s->name = copy, s->movie = type == H_MOVIE, s->boss = strstr(row, "_BossIn") || strstr(row, "_BossOut");
        movies += s->movie;
    }
    *movies_out = movies;
    qsort(stories, (size_t)nstories, sizeof *stories, by_story_name);
    qsort(talk_flags, (size_t)ntalk, sizeof *talk_flags, by_u32);
}

static void events_thread(void *arg) {
    (void)arg;
    uint64_t t0 = plat_now_ns(), dn = 0, zn = 0;
    const char *path = NULL;
    for (uint64_t i = 0, count = romfs_file_count(); i < count; i++) {   // RSDB/EventSetting.Product.<n>.rstbl.byml.zs
        uint64_t off, size;
        const char *pth = romfs_entry(i, &off, &size);
        size_t len = pth ? strlen(pth) : 0;
        if (pth && !strncmp(pth, "RSDB/EventSetting.Product.", 26) && len > 14 && !strcmp(pth + len - 14, ".rstbl.byml.zs") &&
            (!path || strcmp(pth, path) > 0))
            path = pth;
    }
    uint8_t *dz = romfs_load("Pack/ZsDic.pack.zs", &dn), *dics = NULL, *z = path ? romfs_load(path, &zn) : NULL, *raw = NULL;
    ZSTD_DDict *dd = NULL;
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    unsigned long long dsize = dz ? ZSTD_getFrameContentSize(dz, (size_t)dn) : 0, rn = z ? ZSTD_getFrameContentSize(z, (size_t)zn) : 0;
    size_t at = 0, size = 0;
    if (dz && dsize && dsize < (64u << 20) && (dics = malloc((size_t)dsize)) &&
        ZSTD_decompress(dics, (size_t)dsize, dz, (size_t)dn) == dsize && sarc_member(dics, (size_t)dsize, "zs.zsdic", &at, &size))
        dd = ZSTD_createDDict(dics + at, size);
    int ok = 0, rows = 0, movies = 0;
    if (z && rn && rn < (64u << 20) && (raw = malloc((size_t)rn))) {
        ok = dd && dctx && ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, z, (size_t)zn, dd) == rn;
        if (!ok) ok = ZSTD_decompress(raw, (size_t)rn, z, (size_t)zn) == rn;
        if (ok) events_parse(raw, (size_t)rn, &rows, &movies);
    }
    ZSTD_freeDCtx(dctx);
    ZSTD_freeDDict(dd);
    free(dics), free(dz), free(z), free(raw);
    if (nstories && ntalk)
        rt_log("coop: events: the event table %s: %d events, %d story cutscenes (%d movies), %d NPC talk events, in %.0f ms",
               path, rows, nstories, movies, ntalk, (double)(plat_now_ns() - t0) / 1e6);
    else
        rt_log("coop: events: the event table could not be read (%s%s): no story cutscene is shared, and talks with NPCs "
               "stay in progress sync", path ? path : "no RSDB/EventSetting file", ok ? ", no rows" : "");
    __atomic_store_n(&ev_state, 2, __ATOMIC_RELEASE);
}

int coop_events_state(void) {
    int s = __atomic_load_n(&ev_state, __ATOMIC_ACQUIRE);
    if (!s) {
        int32_t zero = 0;
        if (__atomic_compare_exchange_n(&ev_state, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (!plat_thread_spawn(events_thread, NULL, "coop events", 0)) {
                rt_log("coop: events: no thread for the event table");
                __atomic_store_n(&ev_state, 2, __ATOMIC_RELEASE);
            }
        }
        s = __atomic_load_n(&ev_state, __ATOMIC_ACQUIRE);
    }
    return s;
}

int coop_events_talk_flags(const uint32_t **out) {
    int read = coop_events_state() == 2;
    *out = read ? talk_flags : NULL;
    return read ? ntalk : 0;
}

const CoopStory *coop_events_story(const char *name) {
    if (coop_events_state() != 2 || !name || !name[0] || !stories) return NULL;
    CoopStory key = {name, 0, 0};
    return bsearch(&key, stories, (size_t)nstories, sizeof *stories, by_story_name);
}
