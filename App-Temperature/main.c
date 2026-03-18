#include <stdio.h>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include <FreeRTOS.h>
#include <task.h>

#define USER_LED_PIN 25
#define USER_BUTTON_PIN 24
#define WS2812_PIN 23

#define IS_RGBW false
#define TEMP_SAMPLE_COUNT 32

// TODO: Make easily configurable (OLED Display with an encoder planned)
#define TEMP_CAL_OFFSET_C (-11.6f)

// 0 = absolute, 1 = adaptive
#define DEFAULT_MODE 1
#define ABSOLUTE_TEMP_MIN_C 20.0f
#define ABSOLUTE_TEMP_MAX_C 35.0f
#define ADAPTIVE_MIN_SPAN_C 2.0f
#define ADAPTIVE_RELAX_FACTOR 0.02f
#define HEAT_COLOR_MAX 80.0f

typedef struct {
  float last_temp_c;
  float observed_min_c;
  float observed_max_c;
  float cal_offset_c;
  uint8_t mode;
  bool new_sample;
} app_state_t;

static app_state_t g_state = {.last_temp_c = 0.0f,
                              .observed_min_c = 1000.0f,
                              .observed_max_c = -1000.0f,
                              .cal_offset_c = TEMP_CAL_OFFSET_C,
                              .mode = DEFAULT_MODE,
                              .new_sample = false};

static PIO g_pio = pio0;
static uint g_sm = 0;
static uint g_ws2812_offset = 0;

static const uint16_t ws2812_program_instructions[] = {0x6221, 0x1123, 0x1400,
                                                       0xa442};

static const struct pio_program ws2812_program = {
    .instructions = ws2812_program_instructions,
    .length = 4,
    .origin = -1,
};

static inline pio_sm_config ws2812_program_get_default_config(uint offset) {
  pio_sm_config c = pio_get_default_sm_config();
  sm_config_set_wrap(&c, offset + 0, offset + 3);
  sm_config_set_sideset(&c, 1, false, false);
  return c;
}

static inline void ws2812_program_init(PIO pio, uint sm, uint offset, uint pin,
                                       float freq, bool rgbw) {
  pio_sm_config c = ws2812_program_get_default_config(offset);

  sm_config_set_sideset_pins(&c, pin);
  sm_config_set_out_shift(&c, false, true, rgbw ? 32 : 24);
  sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);

  float div = (float)clock_get_hz(clk_sys) / (freq * 10.0f);
  sm_config_set_clkdiv(&c, div);

  pio_gpio_init(pio, pin);
  pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);

  pio_sm_init(pio, sm, offset, &c);
  pio_sm_set_enabled(pio, sm, true);
}

static inline uint32_t urgb_u32(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint32_t)r << 8) | ((uint32_t)g << 16) | (uint32_t)b;
}

static inline void put_pixel(PIO pio, uint sm, uint32_t pixel_grb) {
  pio_sm_put_blocking(pio, sm, pixel_grb << 8u);
}

static float clampf(float x, float lo, float hi) {
  if (x < lo)
    return lo;
  if (x > hi)
    return hi;
  return x;
}

static const char *mode_name(uint8_t mode) {
  switch (mode) {
  case 0:
    return "absolute";
  case 1:
    return "adaptive";
  default:
    return "unknown";
  }
}

static float temp_sensor_read_raw_c(void) {
  uint32_t sum = 0;
  adc_select_input(4);

  for (int i = 0; i < TEMP_SAMPLE_COUNT; i++) {
    sum += adc_read();
  }

  float raw_avg = (float)sum / (float)TEMP_SAMPLE_COUNT;
  const float conversion_factor = 3.3f / (1 << 12);
  float voltage = raw_avg * conversion_factor;

  return 27.0f - (voltage - 0.706f) / 0.001721f;
}

static float temp_sensor_read_c(float cal_offset_c) {
  return temp_sensor_read_raw_c() + cal_offset_c;
}

static void update_adaptive_range_locked(float t) {
  if (t < g_state.observed_min_c) {
    g_state.observed_min_c = t;
  } else {
    g_state.observed_min_c +=
        ADAPTIVE_RELAX_FACTOR * (t - g_state.observed_min_c);
  }

  if (t > g_state.observed_max_c) {
    g_state.observed_max_c = t;
  } else {
    g_state.observed_max_c +=
        ADAPTIVE_RELAX_FACTOR * (t - g_state.observed_max_c);
  }

  float center = 0.5f * (g_state.observed_min_c + g_state.observed_max_c);
  float span = g_state.observed_max_c - g_state.observed_min_c;

  if (span < ADAPTIVE_MIN_SPAN_C) {
    g_state.observed_min_c = center - 0.5f * ADAPTIVE_MIN_SPAN_C;
    g_state.observed_max_c = center + 0.5f * ADAPTIVE_MIN_SPAN_C;
  }
}

static float normalize_absolute_temp(float t) {
  return clampf((t - ABSOLUTE_TEMP_MIN_C) /
                    (ABSOLUTE_TEMP_MAX_C - ABSOLUTE_TEMP_MIN_C),
                0.0f, 1.0f);
}

static float normalize_adaptive_temp_from_range(float t, float tmin,
                                                float tmax) {
  float span = tmax - tmin;
  if (span < 0.1f)
    span = 0.1f;
  return clampf((t - tmin) / span, 0.0f, 1.0f);
}

static uint32_t gradient_heat_color(float x) {
  uint8_t r, g, b;

  if (x < 0.25f) {
    float k = x / 0.25f;
    r = 0;
    g = (uint8_t)(k * HEAT_COLOR_MAX);
    b = (uint8_t)HEAT_COLOR_MAX;
  } else if (x < 0.50f) {
    float k = (x - 0.25f) / 0.25f;
    r = 0;
    g = (uint8_t)HEAT_COLOR_MAX;
    b = (uint8_t)((1.0f - k) * HEAT_COLOR_MAX);
  } else if (x < 0.75f) {
    float k = (x - 0.50f) / 0.25f;
    r = (uint8_t)(k * HEAT_COLOR_MAX);
    g = (uint8_t)HEAT_COLOR_MAX;
    b = 0;
  } else {
    float k = (x - 0.75f) / 0.25f;
    r = (uint8_t)HEAT_COLOR_MAX;
    g = (uint8_t)((1.0f - k) * HEAT_COLOR_MAX);
    b = 0;
  }

  return urgb_u32(r, g, b);
}

static uint32_t temp_to_absolute_heat_rgb(float t) {
  return gradient_heat_color(normalize_absolute_temp(t));
}

static uint32_t temp_to_adaptive_heat_rgb_from_range(float t, float tmin,
                                                     float tmax) {
  return gradient_heat_color(normalize_adaptive_temp_from_range(t, tmin, tmax));
}

static void temp_task(void *param) {
  (void)param;
  TickType_t last_wake = xTaskGetTickCount();

  while (1) {
    float cal_offset_c;
    taskENTER_CRITICAL();
    cal_offset_c = g_state.cal_offset_c;
    taskEXIT_CRITICAL();

    float t = temp_sensor_read_c(cal_offset_c);

    float tmin, tmax;
    uint8_t mode;
    taskENTER_CRITICAL();
    g_state.last_temp_c = t;
    update_adaptive_range_locked(t);
    g_state.new_sample = true;

    tmin = g_state.observed_min_c;
    tmax = g_state.observed_max_c;
    mode = g_state.mode;
    cal_offset_c = g_state.cal_offset_c;
    taskEXIT_CRITICAL();

    printf("Temp: %.2f C | Offset: %.2f C | Mode: %s | Range: [%.2f .. %.2f]\n",
           t, cal_offset_c, mode_name(mode), tmin, tmax);

    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
  }
}

static void ui_task(void *param) {
  (void)param;
  bool prev_btn = true;
  TickType_t last_wake = xTaskGetTickCount();
  TickType_t last_press = 0;

  while (1) {
    bool btn = gpio_get(USER_BUTTON_PIN);
    TickType_t now = xTaskGetTickCount();

    if (prev_btn && !btn) {
      if ((now - last_press) > pdMS_TO_TICKS(200)) {
        uint8_t mode;
        taskENTER_CRITICAL();
        g_state.mode = (g_state.mode + 1) % 2;
        mode = g_state.mode;
        taskEXIT_CRITICAL();

        printf("Mode changed to %s\n", mode_name(mode));
        last_press = now;
      }
    }

    prev_btn = btn;
    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(20));
  }
}

static void led_task(void *param) {
  (void)param;

  TickType_t last_wake = xTaskGetTickCount();
  bool heartbeat = false;

  while (1) {
    float t, tmin, tmax;
    uint8_t mode;
    bool new_sample;

    taskENTER_CRITICAL();
    t = g_state.last_temp_c;
    tmin = g_state.observed_min_c;
    tmax = g_state.observed_max_c;
    mode = g_state.mode;
    new_sample = g_state.new_sample;
    if (g_state.new_sample) {
      g_state.new_sample = false;
    }
    taskEXIT_CRITICAL();

    uint32_t color;
    if (mode == 0) {
      color = temp_to_absolute_heat_rgb(t);
    } else {
      color = temp_to_adaptive_heat_rgb_from_range(t, tmin, tmax);
    }

    put_pixel(g_pio, g_sm, color);

    if (new_sample) {
      heartbeat = true;
      gpio_put(USER_LED_PIN, 1);
    } else if (heartbeat) {
      heartbeat = false;
      gpio_put(USER_LED_PIN, 0);
    }

    vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(100));
  }
}

int main(void) {
  stdio_init_all();
  sleep_ms(2000);

  gpio_init(USER_LED_PIN);
  gpio_set_dir(USER_LED_PIN, GPIO_OUT);
  gpio_put(USER_LED_PIN, 0);

  gpio_init(USER_BUTTON_PIN);
  gpio_set_dir(USER_BUTTON_PIN, GPIO_IN);
  gpio_pull_up(USER_BUTTON_PIN);

  adc_init();
  adc_set_temp_sensor_enabled(true);

  g_ws2812_offset = pio_add_program(g_pio, &ws2812_program);
  ws2812_program_init(g_pio, g_sm, g_ws2812_offset, WS2812_PIN, 800000.0f,
                      IS_RGBW);

  xTaskCreate(temp_task, "TEMP", 256, NULL, 2, NULL);
  xTaskCreate(ui_task, "UI", 256, NULL, 2, NULL);
  xTaskCreate(led_task, "LED", 512, NULL, 1, NULL);

  vTaskStartScheduler();

  while (1) {
    tight_loop_contents();
  }
}