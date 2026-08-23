#include <assert.h>
#include <string.h>

#include "woodfish_model.h"

// 敲击后功德/连击按预期推进
static void test_knock_increments(void) {
    woodfish_model_t m;
    woodfish_model_init(&m);
    assert(m.merit == 0 && m.session_merit == 0 && m.combo == 0);

    woodfish_model_knock(&m, 100);
    assert(m.merit == 1 && m.session_merit == 1 && m.combo == 1);

    woodfish_model_knock(&m, 900);
    assert(m.merit == 2 && m.combo == 2);
    assert(m.best_combo == 2);
}

// 间隔超过窗口时连击归零重计
static void test_combo_resets_after_window(void) {
    woodfish_model_t m;
    woodfish_model_init(&m);

    woodfish_model_knock(&m, 0);
    woodfish_model_knock(&m, WOODFISH_COMBO_WINDOW_MS);       // 恰好压线:仍算连击
    assert(m.combo == 2);

    woodfish_model_knock(&m, WOODFISH_COMBO_WINDOW_MS * 2 + 1);
    assert(m.combo == 1);
    assert(m.best_combo == 2);
}

// 毫秒时钟回绕(如长时间运行后)时连击窗口仍正确
static void test_combo_wraps_around(void) {
    woodfish_model_t m;
    woodfish_model_init(&m);

    m.merit = 10;                          // 模拟已有功德,确保进入连击分支
    woodfish_model_knock(&m, 0xFFFFFFF0u);
    assert(m.combo == 1);
    woodfish_model_knock(&m, 60);          // 跨过 0 点
    assert(m.combo == 2);
}

// 段位表边界
static void test_rank_boundaries(void) {
    assert(strcmp(woodfish_model_rank(0)->name, "Novice") == 0);
    assert(strcmp(woodfish_model_rank(99)->name, "Novice") == 0);
    assert(strcmp(woodfish_model_rank(100)->name, "Pilgrim") == 0);
    assert(strcmp(woodfish_model_rank(25000)->name, "Bodhisattva") == 0);
    assert(strcmp(woodfish_model_rank(UINT32_MAX)->name, "Enlightened") == 0);

    assert(strcmp(woodfish_model_next_rank(0)->name, "Pilgrim") == 0);
    assert(strcmp(woodfish_model_next_rank(99999)->name, "Enlightened") == 0);
    assert(woodfish_model_next_rank(100000) == NULL);
    assert(woodfish_model_next_rank(UINT32_MAX) == NULL);
}

// 进度条千分比
static void test_rank_progress(void) {
    assert(woodfish_model_rank_progress(0) == 0);
    assert(woodfish_model_rank_progress(50) == 500);
    assert(woodfish_model_rank_progress(100) == 0);            // 刚升档,重新起算
    assert(woodfish_model_rank_progress(300) == 500);
    assert(woodfish_model_rank_progress(100000) == 1000);      // 满档
    assert(woodfish_model_rank_progress(UINT32_MAX) == 1000);
}

// 溢出保护
static void test_overflow_saturates(void) {
    woodfish_model_t m;
    woodfish_model_init(&m);
    m.merit = UINT32_MAX;
    m.session_merit = UINT32_MAX;
    woodfish_model_knock(&m, 1000);
    assert(m.merit == UINT32_MAX);
    assert(m.session_merit == UINT32_MAX);
    assert(m.combo == 1);
}

int main(void) {
    test_knock_increments();
    test_combo_resets_after_window();
    test_combo_wraps_around();
    test_rank_boundaries();
    test_rank_progress();
    test_overflow_saturates();
    return 0;
}
