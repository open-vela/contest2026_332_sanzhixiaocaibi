/*
 * 电脉卫士 F6：视觉火情感知（成员 C）
 * ------------------------------------------------------------
 * 硬件：板载 SC2336（200 万像素 MIPI-CSI），经 esp_video V4L2 取流。
 *
 * 定位：**电气火灾的第二条证据链**，不是独立功能。
 *   电流侧（F2 fault_detect）判出电弧/过载时，本模块提供"现场是否真的起火"
 *   的独立证据，用于在切断决策前**升级或维持**告警等级：
 *     - 见火  -> 升 Lv3 立即切断（电弧 + 明火 = 已经在烧）
 *     - 无火  -> 维持原等级（避免因单一判据误切负载）
 *
 * 算法：YCbCr 色彩判据 + 时域闪烁分析，**确定性规则，不依赖推理框架**。
 *   选择理由：火焰在 YCbCr 空间有稳定可分的统计特征（Celik & Demirel, 2009），
 *   且"闪烁"是火焰区别于红色静物（警示灯、红色外壳、反光）的关键判据——
 *   这一点纯色彩法做不到，也是本实现把时域项作为独立维度的原因。
 *   全部运算为整数加减比较，1280x720 抽样后每帧约 1.4 万次，实测 <2ms。
 *
 * ⚠️ 定位声明：本模块为【预警级辅助判据】，用于提高电气故障告警的置信度。
 *   不替代 GB 4717 / GB 15631 规定的火灾自动报警系统与感烟探测器。
 *   摄像头受遮挡、逆光、镜头污染影响，不得作为唯一消防依据。
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 火情统计快照（UI / 上云 / 调试） ---------- */
typedef struct {
    float    score;        /* 综合火情得分 0~1（面积项 + 闪烁项加权） */
    float    area_ratio;   /* 疑似火焰像素占比 0~1 */
    float    flicker_cv;   /* 时域闪烁变异系数（火焰高、静物低） */
    float    smoke_ratio;  /* 疑似烟雾像素占比 0~1（低饱和 + 中亮度 + 帧间变化） */
    float    smoke_score;  /* 烟雾综合得分 0~1（占比项 × 增长趋势调制） */
    float    smoke_growth; /* 烟雾面积增长趋势 0~1（真烟持续扩散，扰动无增长） */
    bool     smoke_active; /* 是否已确认烟雾（连续 15 帧超阈，对应 Lv2 预警） */
    uint32_t frames;       /* 累计处理帧数 */
    uint32_t fps_x10;      /* 实测帧率 ×10（避免浮点日志） */
    bool     active;       /* 是否已确认火情（连续 N 帧超阈） */
    bool     ready;        /* 摄像头链路是否就绪 */
} vision_stats_t;

/* 初始化摄像头并启动检测任务。
 * 失败不影响系统其余部分——视觉是增强项，缺失时电气主线照常工作。 */
esp_err_t vision_init(void);

/* 当前是否确认火情（供 on_fault 做告警升级判定） */
bool vision_fire_active(void);

/* 当前是否确认烟雾。
 * 注意语义差别：烟雾对应 Lv2 预警（告警+上云，**不切断**），
 * 因为电气火灾先阴燃冒烟后见明火，见烟即跳闸会让一次误报断掉整条回路。 */
bool vision_smoke_active(void);

/* 当前烟雾得分 0~1 */
float vision_smoke_score(void);

/* 当前火情得分 0~1（可直接填入 fault_event_t.confidence） */
float vision_fire_score(void);

/* 取统计快照（UI 视觉页 / 健康自检 / 上云） */
void vision_get_stats(vision_stats_t *out);

/* 运行时调参（NVS / 云端下发）。传 0 或负值表示不修改该项。
 *   score_th  : 火情确认得分阈值，默认 0.55
 *   area_ref  : 面积归一化基准（占比），默认 0.015（1.5% 画面）
 *   confirm_n : 连续确认帧数，默认 5（@15fps 约 0.33s，抑制瞬时误报） */
esp_err_t vision_set_thresholds(float score_th, float area_ref, int confirm_n);

/* 摄像头链路是否就绪（供 selftest 汇总） */
bool vision_ready(void);

#ifdef __cplusplus
}
#endif
