/*
 * 电脉卫士 F6：视觉火情感知实现
 * 取流 -> YCbCr 判据 -> 时域闪烁 -> 得分 -> 供 on_fault 做告警升级
 */
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/i2c_master.h"
#include "vision.h"

/* 只借 BSP 的这一个符号，不引 bsp/esp32_p4_function_ev_board.h 整个头文件——
 * 该头会连带引入 display.h -> esp_lcd_types.h，而 esp_lcd 在 BSP 组件里是
 * PRIV_REQUIRES，外部组件包含即编译失败。这里直接声明原型，避开依赖泥潭。 */
extern i2c_master_bus_handle_t bsp_i2c_get_handle(void);

static const char *TAG = "vision";

/* ---------- 取流参数 ----------
 * 1280x720 UYVY：SC2336 已验证 30fps 稳定输出（capture_stream 实测）。
 * 两个缓冲共 3.6MB，走 PSRAM（板载 32MB，空闲约 28MB，占用可忽略）。 */
#define VIS_WIDTH        1280
#define VIS_HEIGHT       720
#define VIS_BUF_COUNT    2
#define VIS_FMT          V4L2_PIX_FMT_UYVY

/* 抽样步长：UYVY 每 4 字节含 2 像素，按 8 像素/8 行抽样
 * -> 160x90 = 14400 采样点，两遍统计约 2.9 万次整数运算，实测 <2ms。
 * 火焰是大面积连通区域，抽样不会漏检；抽样反而抑制了单像素噪点误报。 */
/* [FIX-CPU 09-19] 抽样步长由 8 放宽到 16，且只分析每 2 帧中的 1 帧。
 * 原值 8 的实测代价远超设计估算：板上帧率只有 6~10fps，即每帧 100~170ms，
 * 而我按 SRAM 速度估的是 2ms。差距来自 **PSRAM 的跨行访问**——
 * 隔 8 行取一行意味着每次跳 20480 字节，几乎每个采样点都是 cache miss。
 * 后果：vision 任务（core 1）把同核的 acq 采集任务饿死，
 * 触发 task_wdt「acq 未按时喂狗」并被 MON 判定为链路卡死 -> 整机自动复位。
 * 步长 16 使采样点由 14400 降到 3600（1/4），叠加隔帧分析（1/2），
 * 再加下方的单遍统计（1/2），合计约 1/16 的计算量。 */
#define VIS_STEP_X       16
#define VIS_STEP_Y       16
#define VIS_ANALYZE_EVERY 2

/* ---------- YCbCr 火焰判据阈值 ----------
 * TAU：|Cb-Cr| 下限。火焰是强暖色，Cb（蓝色度）低而 Cr（红色度）高，
 * 两者差值显著；白炽灯、白墙等中性色 Cb≈Cr≈128，差值接近 0，由此被排除。 */
#define VIS_TAU          40

/* ---------- 烟雾判据 ----------
 * [FIX-SMOKE 09-19] 原判据只有「低饱和 + 中等亮度」，实测**完全不可用**：
 * 无烟房间静置时占比恒为 25%（墙面/桌面/机箱外壳全部命中）——那是在数灰色像素。
 * 烟与灰色静物的本质区别**不在颜色，在运动**：烟是扩散的、边界持续变化的，
 * 而灰墙是死的。因此现判据要求同时满足三条：
 *   ① 低饱和（Cb/Cr 接近中性）
 *   ② 亮度落在中段（排除死黑与过曝）
 *   ③ **相对上一帧有显著亮度变化**（核心，新增）
 * 再叠加帧级的全局变化排除与时域增长趋势。 */
#define VIS_SMOKE_CHROMA 14      /* |Cb-128| 与 |Cr-128| 上限 */
#define VIS_SMOKE_Y_LO   50
#define VIS_SMOKE_Y_HI   210
#define VIS_SMOKE_DELTA  6       /* |Y - Y_prev| 下限，低于此视为静止背景 */

/* 全局变化排除阈值：变化点占比超过此值，判定为整帧事件
 * （自动曝光调整、开关灯、镜头前有人走过），本帧丢弃不参与烟雾判定。
 * 不做这条的话，AE 一动就是"到处都在动" -> 满屏误报。 */
#define VIS_GLOBAL_CHANGE 0.50f

/* 烟雾面积归一化基准、确认/解除帧数。
 * 烟比火慢得多，确认帧数取得更长；且**烟只报 Lv2 预警不切断**——
 * 电气火灾是先阴燃冒烟后见明火，见烟即跳闸会让一次误报断掉整条回路。 */
#define VIS_SMOKE_REF     0.08f
#define VIS_SMOKE_CONF_N  15
#define VIS_SMOKE_CLEAR_N 30
#define VIS_SMOKE_TH      0.55f
#define VIS_SMOKE_FLOOR   0.010f

/* 抽样点上限：step=16 时 (1280/16)x(720/16)=80x45=3600，留余量 */
#define VIS_MAX_POINTS   4096

/* 时域窗口：@~15fps 覆盖约 2s。火焰闪烁 7~12Hz，窗口需跨越多个周期。 */
#define VIS_HIST_N       32

/* 闪烁变异系数归一化基准：实测明火 CV 通常 0.2~0.6，红色静物 <0.05 */
#define VIS_CV_REF       0.25f

/* 面积下限：低于此占比直接判无火 */
#define VIS_AREA_FLOOR   0.002f

/* 闪烁项的调制范围：score = 面积项 × (BASE + SPAN × 闪烁项)
 * [FIX-FALSEFIRE 09-19] 原实现是"面积项×0.6 + 闪烁项×0.4"的**相加**式，
 * 开机即误报：实测启动第 ~2.1s 面积仅 0.49%（远低于 1.5% 基准，面积项 0.33），
 * 但传感器 AE/AWB 尚在收敛、画面剧烈跳变使 CV 冲到 0.99（闪烁项封顶 1.0），
 * 0.6×0.33 + 0.4×1.0 = 0.59 > 0.55 阈值 -> 判定火情 -> Lv3 切断互锁。
 * 真实设备上这等于"每次上电就跳闸"。
 * 改为**相乘**后，闪烁只能调制已有的面积证据、不能独立贡献分数：
 * 同样数据 0.33 × (0.6+0.4×1.0) = 0.33 < 0.55，正确判为无火；
 * 而真实明火（面积≥1.5% 且 CV 高）仍可得满分。 */
#define VIS_BASE         0.6f
#define VIS_SPAN         0.4f

/* 预热帧数：传感器 AE/AWB 收敛期间的画面不参与判定。
 * @30fps 约 1.5s，实测足够 ISP 稳定（启动误报发生在 t=2.1s，即第 ~10 帧）。 */
#define VIS_WARMUP_FRAMES 45

/* 计算闪烁 CV 所需的最小样本数。样本太少时方差估计不可靠——
 * 启动误报时 hist 里只有 4~5 个剧烈跳变的样本就算出了 CV=0.99。 */
#define VIS_CV_MIN_N     16

/* 解除火情所需的连续低分帧数，以及迟滞带下沿比例（见 FIX-FLAP 注释） */
#define VIS_CLEAR_N      15
#define VIS_CLEAR_RATIO  0.75f

typedef struct {
    int      fd;
    uint8_t *buf[VIS_BUF_COUNT];
    uint32_t buf_len[VIS_BUF_COUNT];

    float    hist[VIS_HIST_N];   /* 面积占比历史，用于闪烁分析 */
    int      hist_idx;
    int      hist_cnt;

    int      confirm_run;        /* 连续超阈帧数 */
    int      clear_run;          /* 连续低于迟滞下沿的帧数（解除用） */

    /* ---- 烟雾状态 ---- */
    uint8_t  prev_y[VIS_MAX_POINTS]; /* 上一帧抽样点亮度，用于帧间差分 */
    int      prev_n;                 /* 上一帧有效点数，0 表示尚无参考帧 */
    float    smoke_hist[VIS_HIST_N]; /* 烟雾占比历史，用于增长趋势 */
    int      smoke_idx;
    int      smoke_cnt;
    int      smoke_conf_run;
    int      smoke_clear_run;
    bool     smoke_active;

    /* 可调参数 */
    float    score_th;
    float    area_ref;
    int      confirm_n;

    /* 输出快照（任务写，读者拷贝，单字段原子性够用——非关键控制路径） */
    vision_stats_t st;
} vision_ctx_t;

static vision_ctx_t s_v = {
    .fd        = -1,
    .score_th  = 0.55f,
    .area_ref  = 0.015f,
    .confirm_n = 5,
};

/* ------------------------------------------------------------------
 * 单帧分析：两遍扫描
 *   第一遍求 Y/Cb/Cr 均值（自适应，适应不同环境光）
 *   第二遍按 Celik 规则计数
 * ------------------------------------------------------------------ */
static void vision_analyze_frame(const uint8_t *p, uint32_t len,
                                 float *out_area, float *out_smoke,
                                 bool *out_global_change)
{
    const uint32_t row_bytes = VIS_WIDTH * 2;          /* UYVY: 2 字节/像素 */
    const uint32_t grp_step  = (VIS_STEP_X / 2) * 4;   /* 每组 4 字节 = 2 像素 */

    /* [FIX-CPU 09-19] 改为**单遍**统计：本帧的判据用**上一帧**的均值。
     * 原实现两遍扫描（先求均值、再计数），意味着同一片 PSRAM 数据要跨行读两次，
     * 缺失代价翻倍。视频相邻帧的全局色度均值变化极小（30fps 下尤其如此），
     * 用上一帧均值判本帧完全够用，代价减半。
     * 首帧用中性值 128 起步，预热期（45 帧）内本就不出分，不影响判定。 */
    static uint32_t m_y = 128, m_cb = 128, m_cr = 128;

    uint32_t n = 0;
    uint32_t sum_y = 0, sum_cb = 0, sum_cr = 0;
    uint32_t fire = 0, smoke = 0;
    uint32_t changed = 0;              /* 相对上一帧有显著亮度变化的点数 */
    const bool have_prev = (s_v.prev_n > 0);

    for (uint32_t row = 0; row < VIS_HEIGHT; row += VIS_STEP_Y) {
        uint32_t off = row * row_bytes;
        if (off + row_bytes > len) {
            break;
        }
        const uint8_t *q = p + off;
        for (uint32_t b = 0; b + 3 < row_bytes && n < VIS_MAX_POINTS; b += grp_step) {
            const int cb = q[b + 0];   /* U = Cb */
            const int y  = q[b + 1];   /* Y0     */
            const int cr = q[b + 2];   /* V = Cr */

            /* 帧间差分：与上一帧同一抽样位置比较亮度 */
            int dy = 0;
            if (have_prev && n < (uint32_t)s_v.prev_n) {
                const int py = s_v.prev_y[n];
                dy = y > py ? y - py : py - y;
                if (dy >= VIS_SMOKE_DELTA) {
                    changed++;
                }
            }
            s_v.prev_y[n] = (uint8_t)y;   /* 就地更新为本帧，供下一帧使用 */

            sum_cb += cb;
            sum_y  += y;
            sum_cr += cr;
            n++;

            /* 火焰四条规则全部成立才计数 */
            if (y >= cb &&                                   /* R1 亮度高于蓝色度 */
                cr >= cb &&                                  /* R2 偏红 */
                (cr - cb) >= VIS_TAU &&                      /* R3 暖色足够强 */
                y > (int)m_y && cb < (int)m_cb && cr > (int)m_cr) {  /* R4 相对本帧突出 */
                fire++;
            } else {
                /* 烟雾三条同时成立：低饱和 + 亮度中段 + **相对上一帧在变化**。
                 * 第三条是与旧版的本质差别——没有它就是在数灰色像素。 */
                const int dcb = cb > 128 ? cb - 128 : 128 - cb;
                const int dcr = cr > 128 ? cr - 128 : 128 - cr;
                if (dcb < VIS_SMOKE_CHROMA && dcr < VIS_SMOKE_CHROMA &&
                    y > VIS_SMOKE_Y_LO && y < VIS_SMOKE_Y_HI &&
                    have_prev && dy >= VIS_SMOKE_DELTA) {
                    smoke++;
                }
            }
        }
    }

    s_v.prev_n = (int)n;

    if (n == 0) {
        *out_area = 0.0f;
        *out_smoke = 0.0f;
        *out_global_change = false;
        return;
    }

    /* 全局变化事件：变化点过半 -> 自动曝光调整/开关灯/镜头前有人经过。
     * 此时"运动"这一维度失去区分力，本帧的烟雾结论不可信，交由调用方丢弃。 */
    *out_global_change = ((float)changed / (float)n) > VIS_GLOBAL_CHANGE;

    /* 用本帧统计量更新均值，供下一帧判据使用 */
    m_y  = sum_y  / n;
    m_cb = sum_cb / n;
    m_cr = sum_cr / n;

    *out_area  = (float)fire  / (float)n;
    *out_smoke = (float)smoke / (float)n;
}

/* 时域闪烁：面积占比序列的变异系数 std/mean。
 * 火焰面积随燃烧不断起伏 -> CV 高；红色静物面积恒定 -> CV≈0。 */
static float vision_flicker_cv(void)
{
    if (s_v.hist_cnt < VIS_CV_MIN_N) {
        return 0.0f;
    }
    const int n = s_v.hist_cnt;
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        sum += s_v.hist[i];
    }
    const float mean = sum / n;
    if (mean < 1e-6f) {
        return 0.0f;
    }
    float var = 0.0f;
    for (int i = 0; i < n; i++) {
        const float d = s_v.hist[i] - mean;
        var += d * d;
    }
    return sqrtf(var / n) / mean;
}

/* 烟雾增长趋势：历史窗口后半段均值相对前半段的增幅。
 * 真烟是持续扩散、面积递增；挥手/走动造成的瞬时变化则无持续增长。
 * 返回 0~1，已归一化。 */
static float vision_smoke_growth(void)
{
    const int n = s_v.smoke_cnt;
    if (n < 8) {
        return 0.0f;
    }
    const int half = n / 2;
    float old_sum = 0.0f, new_sum = 0.0f;
    /* smoke_idx 指向下一个写入位；往回数 n 个即为时间顺序 */
    for (int k = 0; k < n; k++) {
        const int idx = (s_v.smoke_idx - n + k + VIS_HIST_N * 2) % VIS_HIST_N;
        if (k < half) {
            old_sum += s_v.smoke_hist[idx];
        } else {
            new_sum += s_v.smoke_hist[idx];
        }
    }
    const float old_mean = old_sum / half;
    const float new_mean = new_sum / (n - half);
    if (new_mean < 1e-6f) {
        return 0.0f;
    }
    const float g = (new_mean - old_mean) / new_mean;   /* 增幅比例 */
    if (g <= 0.0f) {
        return 0.0f;
    }
    return g > 1.0f ? 1.0f : g;
}

static void vision_task(void *arg)
{
    struct v4l2_buffer buf;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    int64_t t_mark = esp_timer_get_time();
    uint32_t f_mark = 0;

    ESP_LOGI(TAG, "检测任务启动：%dx%d UYVY，抽样 %dx%d，阈值 score=%.2f area_ref=%.3f confirm=%d",
             VIS_WIDTH, VIS_HEIGHT, VIS_STEP_X, VIS_STEP_Y,
             s_v.score_th, s_v.area_ref, s_v.confirm_n);

    while (1) {
        memset(&buf, 0, sizeof(buf));
        buf.type   = type;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(s_v.fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGW(TAG, "取帧失败，200ms 后重试");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* 只分析完好帧；出错帧同样要还回队列，否则缓冲耗尽卡死 */
        s_v.st.frames++;
        /* 隔帧分析：降一半 CPU 占用；未分析的帧直接还队，保持取流管线不堵 */
        if ((buf.flags & V4L2_BUF_FLAG_DONE) && (s_v.st.frames % VIS_ANALYZE_EVERY) == 0) {
            float area = 0.0f, smoke = 0.0f;
            bool  global_change = false;
            vision_analyze_frame(s_v.buf[buf.index], buf.bytesused,
                                 &area, &smoke, &global_change);

            s_v.hist[s_v.hist_idx] = area;
            s_v.hist_idx = (s_v.hist_idx + 1) % VIS_HIST_N;
            if (s_v.hist_cnt < VIS_HIST_N) {
                s_v.hist_cnt++;
            }

            const float cv = vision_flicker_cv();

            float score = 0.0f;
            /* 预热期内只累积历史、不出分：传感器 AE/AWB 未收敛的画面不可用于判定 */
            if (s_v.st.frames >= VIS_WARMUP_FRAMES && area >= VIS_AREA_FLOOR) {
                float a = area / s_v.area_ref;
                if (a > 1.0f) {
                    a = 1.0f;
                }
                float f = cv / VIS_CV_REF;
                if (f > 1.0f) {
                    f = 1.0f;
                }
                /* 相乘：闪烁调制面积证据，不能独立成分（见上方 FIX-FALSEFIRE 注释） */
                score = a * (VIS_BASE + VIS_SPAN * f);
            }

            /* [FIX-FLAP 09-19] 确认与解除都需连续 N 帧（迟滞）。
             * 原实现不对称：确认要连续 5 帧，解除只要 1 帧掉到阈值以下。
             * 火焰本身就在闪烁，得分必然反复穿越阈值 —— 实测出现
             * 确认(22.4s)->解除(25.3s)->确认(34.3s)->解除(35.9s)->确认(37.2s)
             * 的反复跳变，每跳一次就是一次继电器切断 + 事件入库 + 四路上云，
             * 演示时表现为"疯狂跳闸刷告警"。
             * 现在解除同样要连续 VIS_CLEAR_N 帧低于阈值，且阈值下移一档形成迟滞带。 */
            const float clear_th = s_v.score_th * VIS_CLEAR_RATIO;
            if (score >= s_v.score_th) {
                if (s_v.confirm_run < s_v.confirm_n) {
                    s_v.confirm_run++;
                }
                s_v.clear_run = 0;
            } else if (score < clear_th) {
                if (s_v.clear_run < VIS_CLEAR_N) {
                    s_v.clear_run++;
                }
                if (s_v.clear_run >= VIS_CLEAR_N) {
                    s_v.confirm_run = 0;
                }
            }
            /* 处于 [clear_th, score_th) 迟滞带内：两个计数都不动，维持现状 */

            const bool was = s_v.st.active;
            const bool now = (s_v.confirm_run >= s_v.confirm_n);

            /* ---------- 烟雾判定（独立于火焰） ----------
             * 全局变化帧直接丢弃：此时"运动"维度失效，任何烟雾结论都不可信。 */
            float smoke_score = 0.0f;
            float growth = 0.0f;
            if (!global_change && s_v.st.frames >= VIS_WARMUP_FRAMES) {
                s_v.smoke_hist[s_v.smoke_idx] = smoke;
                s_v.smoke_idx = (s_v.smoke_idx + 1) % VIS_HIST_N;
                if (s_v.smoke_cnt < VIS_HIST_N) {
                    s_v.smoke_cnt++;
                }
                growth = vision_smoke_growth();
                if (smoke >= VIS_SMOKE_FLOOR) {
                    float sa = smoke / VIS_SMOKE_REF;
                    if (sa > 1.0f) {
                        sa = 1.0f;
                    }
                    /* 与火焰同构：面积为主，趋势做调制，趋势不能独立成分 */
                    smoke_score = sa * (0.7f + 0.3f * growth);
                }

                if (smoke_score >= VIS_SMOKE_TH) {
                    if (s_v.smoke_conf_run < VIS_SMOKE_CONF_N) {
                        s_v.smoke_conf_run++;
                    }
                    s_v.smoke_clear_run = 0;
                } else if (smoke_score < VIS_SMOKE_TH * VIS_CLEAR_RATIO) {
                    if (s_v.smoke_clear_run < VIS_SMOKE_CLEAR_N) {
                        s_v.smoke_clear_run++;
                    }
                    if (s_v.smoke_clear_run >= VIS_SMOKE_CLEAR_N) {
                        s_v.smoke_conf_run = 0;
                    }
                }
            }
            const bool smoke_was = s_v.smoke_active;
            const bool smoke_now = (s_v.smoke_conf_run >= VIS_SMOKE_CONF_N);
            s_v.smoke_active = smoke_now;
            if (smoke_now && !smoke_was) {
                ESP_LOGW(TAG, "【烟雾预警】得分=%.2f 占比=%.2f%% 增长=%.2f",
                         smoke_score, smoke * 100.0f, growth);
            } else if (!smoke_now && smoke_was) {
                ESP_LOGI(TAG, "烟雾解除 得分=%.2f", smoke_score);
            }

            s_v.st.score       = score;
            s_v.st.area_ratio  = area;
            s_v.st.flicker_cv  = cv;
            s_v.st.smoke_ratio = smoke;
            s_v.st.active      = now;
            s_v.st.smoke_score = smoke_score;
            s_v.st.smoke_growth = growth;
            s_v.st.smoke_active = smoke_now;

            if (now && !was) {
                ESP_LOGW(TAG, "【火情确认】score=%.2f 面积=%.2f%% 闪烁CV=%.2f",
                         score, area * 100.0f, cv);
            } else if (!now && was) {
                ESP_LOGI(TAG, "火情解除 score=%.2f", score);
            }

            /* [DEBUG-TUNE 09-19] 每 30 帧输出一次实测值，用于现场标定阈值。
             * 没有这行，一旦点火未触发就无从判断是面积不足还是闪烁不足。
             * 标定完成后可降为 VERBOSE 或删除。 */
            if ((s_v.st.frames % 30) == 0) {
                ESP_LOGI(TAG, "[调试] 火:面积%.3f%% CV%.2f 分%.2f(%d/%d) | 烟:占比%.3f%% 增长%.2f 分%.2f(%d/%d)%s | 帧%lu",
                         area * 100.0f, cv, score, s_v.confirm_run, s_v.confirm_n,
                         smoke * 100.0f, growth, smoke_score,
                         s_v.smoke_conf_run, VIS_SMOKE_CONF_N,
                         global_change ? " [全局变化丢弃]" : "",
                         (unsigned long)s_v.st.frames);
            }

            /* 每 2s 更新一次实测帧率 */
            const int64_t now_us = esp_timer_get_time();
            if (now_us - t_mark >= 2000000) {
                const uint32_t df = s_v.st.frames - f_mark;
                s_v.st.fps_x10 = (uint32_t)((df * 10.0 * 1000000.0) / (now_us - t_mark));
                t_mark = now_us;
                f_mark = s_v.st.frames;
            }
        }

        if (ioctl(s_v.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGW(TAG, "还帧失败");
        }

        /* [FIX-CPU 09-19] 显式让出 CPU。
         * 原实现只靠 DQBUF 阻塞来让出，但帧持续可用时该调用几乎不阻塞，
         * 任务近似满负荷自旋，把同核的 acq 采集任务饿到触发 task_wdt。
         * 10ms 的固定让出把分析上限压到约 50fps，远高于火情检测所需，
         * 同时给同核任务留出确定的时间片。 */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t vision_init(void)
{
    /* ---- SCCB 必须复用 BSP 的 I2C 总线，不能自建 ----
     * [FIX-I2C-SHARE 09-19] 本板 SC2336(SCCB)、GT911(触摸)、ES8311(声卡) 三者
     * **共用同一对物理引脚** SCL=GPIO8 / SDA=GPIO7（见 bsp/esp32_p4_function_ev_board.h
     * BSP_I2C_SCL/BSP_I2C_SDA）。BSP 已在 port 1 上建好这条总线。
     * 若让 esp_video 按 example 默认行为在 port 0 上"再建一条"，两个 I2C 控制器
     * 会同时驱动同一对引脚 —— 实测现象：摄像头自身初始化成功（PID 读到 0xcb3a），
     * 但随后 ES8311 的每一次 I2C 读写全部失败（I2C_If: Fail to read/write dev 30），
     * 最终 esp_codec_dev_open 崩溃、整机 panic 重启。
     * 因此这里取 BSP 句柄、置 init_sccb=false，让摄像头挂到既有总线上。 */
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "取 BSP I2C 句柄失败（BSP 尚未初始化？）——视觉停用");
        return ESP_ERR_INVALID_STATE;
    }

    esp_video_init_csi_config_t csi_cfg = {
        .sccb_config = {
            .init_sccb  = false,      /* 复用已有总线 */
            .i2c_handle = i2c_bus,
            .freq       = 100000,     /* SC2336 SCCB 100kHz，与 example 默认一致 */
        },
        .reset_pin = -1,              /* v1.5 板子无独立复位脚 */
        .pwdn_pin  = -1,              /* v1.5 板子无独立掉电脚 */
        .dont_init_ldo = false,       /* MIPI LDO 由 CSI 侧初始化（实测可与 DSI 共存） */
    };
    esp_video_init_config_t vcfg = {
        .csi = &csi_cfg,
    };

    esp_err_t ret = esp_video_init(&vcfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "摄像头初始化失败 (%s)——视觉功能停用，不影响电气主线", esp_err_to_name(ret));
        return ret;
    }

    s_v.fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (s_v.fd < 0) {
        ESP_LOGE(TAG, "打开 %s 失败", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        return ESP_FAIL;
    }

    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width       = VIS_WIDTH,
        .fmt.pix.height      = VIS_HEIGHT,
        .fmt.pix.pixelformat = VIS_FMT,
    };
    if (ioctl(s_v.fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "设置格式失败");
        goto fail;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = VIS_BUF_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_v.fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "申请缓冲失败");
        goto fail;
    }

    for (int i = 0; i < VIS_BUF_COUNT; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index  = i;
        if (ioctl(s_v.fd, VIDIOC_QUERYBUF, &b) != 0) {
            ESP_LOGE(TAG, "查询缓冲 %d 失败", i);
            goto fail;
        }
        s_v.buf[i] = (uint8_t *)mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, s_v.fd, b.m.offset);
        if (!s_v.buf[i]) {
            ESP_LOGE(TAG, "映射缓冲 %d 失败", i);
            goto fail;
        }
        s_v.buf_len[i] = b.length;
        if (ioctl(s_v.fd, VIDIOC_QBUF, &b) != 0) {
            ESP_LOGE(TAG, "入队缓冲 %d 失败", i);
            goto fail;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_v.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "启动取流失败");
        goto fail;
    }

    s_v.st.ready = true;

    /* 优先级 2：低于采集(F1)与切断(F3)，视觉不得抢占安全关键路径。
     * 栈 6KB 足够——算法全部在栈上只用少量局部变量，帧数据在 PSRAM。 */
    if (xTaskCreatePinnedToCore(vision_task, "vision", 6144, NULL, 2, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "创建检测任务失败");
        s_v.st.ready = false;
        goto fail;
    }

    ESP_LOGI(TAG, "视觉火情感知就绪（SC2336 %dx%d）", VIS_WIDTH, VIS_HEIGHT);
    return ESP_OK;

fail:
    if (s_v.fd >= 0) {
        close(s_v.fd);
        s_v.fd = -1;
    }
    return ESP_FAIL;
}

bool vision_fire_active(void)
{
    return s_v.st.ready && s_v.st.active;
}

float vision_fire_score(void)
{
    return s_v.st.ready ? s_v.st.score : 0.0f;
}

bool vision_smoke_active(void)
{
    return s_v.st.ready && s_v.st.smoke_active;
}

float vision_smoke_score(void)
{
    return s_v.st.ready ? s_v.st.smoke_score : 0.0f;
}

void vision_get_stats(vision_stats_t *out)
{
    if (out) {
        *out = s_v.st;
    }
}

bool vision_ready(void)
{
    return s_v.st.ready;
}

esp_err_t vision_set_thresholds(float score_th, float area_ref, int confirm_n)
{
    if (score_th > 0.0f && score_th <= 1.0f) {
        s_v.score_th = score_th;
    }
    if (area_ref > 0.0f && area_ref <= 1.0f) {
        s_v.area_ref = area_ref;
    }
    if (confirm_n > 0 && confirm_n <= 60) {
        s_v.confirm_n = confirm_n;
    }
    ESP_LOGI(TAG, "阈值更新：score=%.2f area_ref=%.3f confirm=%d",
             s_v.score_th, s_v.area_ref, s_v.confirm_n);
    return ESP_OK;
}
