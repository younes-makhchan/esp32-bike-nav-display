#include "rootmaker_display.h"
#include <esp_heap_caps.h>

static SemaphoreHandle_t s_lvgl_mutex = NULL;
static lv_display_t *s_disp = NULL;
static lv_indev_t *s_indev = NULL;

#define DRAW_BUF_LINES 40
static lv_color16_t *s_draw_buf1 = NULL;
static lv_color16_t *s_draw_buf2 = NULL;

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    if (board.rootmaker.lcd) {
        board.rootmaker.lcd->pushImageDMA(area->x1, area->y1, w, h, (const lgfx::rgb565_t *)px_map);
    }

    lv_display_flush_ready(disp);
}

static void touchpad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x = 0, y = 0;
    if (board.rootmaker.lcd && board.rootmaker.lcd->getTouch(&x, &y)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_task(void *pvParam)
{
    while (1) {
        if (rootmaker_display_lock(100)) {
            uint32_t time_till_next = lv_timer_handler();
            rootmaker_display_unlock();
            if (time_till_next < 5) time_till_next = 5;
            if (time_till_next > 30) time_till_next = 30;
            vTaskDelay(pdMS_TO_TICKS(time_till_next));
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

static uint32_t my_tick_cb(void)
{
    return millis();
}

void rootmaker_display_init(void)
{
    if (board.rootmaker.lcd) {
        board.rootmaker.lcd->setSwapBytes(true);
    }

    if (s_lvgl_mutex == NULL) {
        s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    }

    lv_init();
    lv_tick_set_cb(my_tick_cb);

    size_t buf_size = 240 * DRAW_BUF_LINES * sizeof(lv_color16_t);
    s_draw_buf1 = (lv_color16_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_draw_buf1) s_draw_buf1 = (lv_color16_t *)malloc(buf_size);

    s_draw_buf2 = (lv_color16_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_draw_buf2) s_draw_buf2 = (lv_color16_t *)malloc(buf_size);

    s_disp = lv_display_create(240, 240);
    lv_display_set_buffers(s_disp, s_draw_buf1, s_draw_buf2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, disp_flush_cb);

    s_indev = lv_indev_create();
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, touchpad_read_cb);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl_task", 8192, NULL, 5, NULL, 1);
}

bool rootmaker_display_lock(uint32_t timeout_ms)
{
    if (!s_lvgl_mutex) return false;
    return (xSemaphoreTakeRecursive(s_lvgl_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
}

void rootmaker_display_unlock(void)
{
    if (s_lvgl_mutex) {
        xSemaphoreGiveRecursive(s_lvgl_mutex);
    }
}

