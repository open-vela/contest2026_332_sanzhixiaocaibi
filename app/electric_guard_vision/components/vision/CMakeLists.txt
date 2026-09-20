# 电脉卫士 F6：视觉火情感知（SC2336 MIPI-CSI + YCbCr 判据 + 时域闪烁）
idf_component_register(
    SRCS vision.c
    INCLUDE_DIRS "."
    REQUIRES app_core esp_video esp_timer driver
)
