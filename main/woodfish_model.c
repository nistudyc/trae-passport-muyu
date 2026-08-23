#include "woodfish_model.h"

#include <stddef.h>

// 段位表按 threshold 升序排列;woodfish_model_rank 用线性扫描(表很短)。
static const woodfish_rank_t RANKS[] = {
    { "Novice",            0 },
    { "Pilgrim",         100 },
    { "Temple Guest",    500 },
    { "Little Monk",    1500 },
    { "Chant Adept",    4000 },
    { "Devotee",       10000 },
    { "Bodhisattva",   25000 },
    { "Enlightened",  100000 },
};
#define RANK_COUNT (sizeof(RANKS) / sizeof(RANKS[0]))

void woodfish_model_init(woodfish_model_t *m) {
    m->merit = 0;
    m->session_merit = 0;
    m->combo = 0;
    m->best_combo = 0;
    m->last_knock_ms = 0;
    m->muted = false;
    m->auto_mode = false;
}

void woodfish_model_knock(woodfish_model_t *m, uint32_t now_ms) {
    // 无符号减法天然处理毫秒时钟回绕;首次敲击(last=0)两个分支结果一致。
    if (m->merit > 0 && (uint32_t)(now_ms - m->last_knock_ms) <= WOODFISH_COMBO_WINDOW_MS) {
        m->combo++;
    } else {
        m->combo = 1;
    }
    if (m->combo > m->best_combo) m->best_combo = m->combo;
    m->last_knock_ms = now_ms;
    if (m->merit < UINT32_MAX) m->merit++;
    if (m->session_merit < UINT32_MAX) m->session_merit++;
}

const woodfish_rank_t *woodfish_model_rank(uint32_t merit) {
    const woodfish_rank_t *rank = &RANKS[0];
    for (size_t i = 0; i < RANK_COUNT; i++) {
        if (merit >= RANKS[i].threshold) rank = &RANKS[i];
    }
    return rank;
}

const woodfish_rank_t *woodfish_model_next_rank(uint32_t merit) {
    for (size_t i = 0; i < RANK_COUNT; i++) {
        if (merit < RANKS[i].threshold) return &RANKS[i];
    }
    return NULL;
}

uint32_t woodfish_model_rank_progress(uint32_t merit) {
    const woodfish_rank_t *cur = woodfish_model_rank(merit);
    const woodfish_rank_t *next = woodfish_model_next_rank(merit);
    if (!next) return 1000;
    uint32_t span = next->threshold - cur->threshold;
    uint32_t done = merit - cur->threshold;
    return done * 1000 / span;
}
