/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include "test_health_app_includes.h"

#include <pbl/util/units.h>

#include <apps/system/health/hr_chart.h>

// Setup and Teardown
////////////////////////////////////

static GContext s_ctx;
static FrameBuffer s_fb;
static bool s_clock_24h;
static uint8_t s_minute_bpm[PBL_MIN_PER_DAY];

GContext *graphics_context_get_current_context(void) {
  return &s_ctx;
}

bool clock_is_24h_style(void) {
  return s_clock_24h;
}

void test_health_hr_chart__initialize(void) {
  framebuffer_init(&s_fb, &(GSize){DISP_COLS, DISP_ROWS});
  framebuffer_clear(&s_fb);
  graphics_context_init(&s_ctx, &s_fb, GContextInitializationMode_App);
  s_app_state_get_graphics_context = &s_ctx;

  fake_spi_flash_init(0 /* offset */, 0x1000000 /* length */);
  pfs_init(false /* run filesystem check */);
  pfs_format(true /* write erase headers */);
  load_resource_fixture_in_flash(RESOURCES_FIXTURE_PATH, SYSTEM_RESOURCES_FIXTURE_NAME,
                                 false /* is_next */);
  resource_init();

  s_clock_24h = false;
  memset(s_minute_bpm, 0, sizeof(s_minute_bpm));
}

void test_health_hr_chart__cleanup(void) {
}

// Helpers
//////////////////////

#define MINUTE(hour, min) (((hour) * PBL_MIN_PER_HOUR) + (min))

// Small deterministic wobble, 0 to 6
static int prv_noise(int minute) {
  return (minute * 37) % 7;
}

// One background reading every 10 minutes between start and end, around base_bpm
static void prv_fill_background(int start, int end, int base_bpm, int skip_every) {
  for (int m = start; m < end; m += 10) {
    if (skip_every && ((m / 10) % skip_every) == 0) {
      continue;
    }
    s_minute_bpm[m + 3] = base_bpm + prv_noise(m);
  }
}

// A reading every minute, ramping from start_bpm to end_bpm
static void prv_fill_dense(int start, int end, int start_bpm, int end_bpm) {
  for (int m = start; m < end; m++) {
    s_minute_bpm[m] =
        start_bpm + ((end_bpm - start_bpm) * (m - start)) / (end - start) + prv_noise(m) / 2;
  }
}

// Asleep until 6:45, a shower gap, a workout at 9:00 and stairs at 12:10; it is now 14:30
static uint16_t prv_fill_typical_day(void) {
  prv_fill_background(MINUTE(0, 0), MINUTE(6, 45), 52, 7);
  prv_fill_background(MINUTE(6, 45), MINUTE(7, 30), 64, 0);
  prv_fill_background(MINUTE(8, 0), MINUTE(9, 0), 74, 0);
  // A poor reading during the commute
  s_minute_bpm[MINUTE(8, 33)] = 118;
  prv_fill_dense(MINUTE(9, 0), MINUTE(9, 15), 95, 145);
  prv_fill_dense(MINUTE(9, 15), MINUTE(9, 40), 140, 150);
  prv_fill_dense(MINUTE(9, 40), MINUTE(9, 50), 135, 100);
  prv_fill_background(MINUTE(9, 50), MINUTE(14, 30), 70, 0);
  prv_fill_dense(MINUTE(12, 10), MINUTE(12, 15), 104, 112);
  return MINUTE(14, 30);
}

static GRect prv_chart_bounds(void) {
#if PBL_ROUND
  const int16_t height = (DISP_ROWS * 2) / 5;
  return GRect(DISP_COLS / 8, (DISP_ROWS - height) / 2, DISP_COLS - (DISP_COLS / 4), height);
#else
  // Below where the card's heading goes, as tall as on the card
  const int16_t padding = 5 + (((DISP_ROWS - LEGACY_2X_DISP_ROWS) / 2) / 10);
  const int16_t top = padding + 35 + padding;
  return GRect(padding, top, DISP_COLS - (2 * padding), (DISP_ROWS * 4) / 9);
#endif
}

static void prv_render(uint16_t num_minutes) {
  graphics_context_set_fill_color(&s_ctx, HEALTH_HR_CHART_BG_COLOR);
  graphics_fill_rect(&s_ctx, &GRect(0, 0, DISP_COLS, DISP_ROWS));
  const GRect bounds = prv_chart_bounds();
  HealthHrChart chart;
  health_hr_chart_init(&chart, &(HealthHrChartData){
                                 .minute_bpm = s_minute_bpm,
                                 .num_minutes = num_minutes,
                               });
  health_hr_chart_draw(&s_ctx, &bounds, &chart);
  health_hr_chart_deinit(&chart);
}

// Clustering
//////////////////////

void test_health_hr_chart__cluster_empty(void) {
  HealthHrChartCluster clusters[4];
  cl_assert_equal_i(health_hr_chart_cluster(nullptr, 0, clusters, 4), 0);
}

void test_health_hr_chart__cluster_single(void) {
  uint8_t bpm[] = {72};
  HealthHrChartCluster clusters[4];
  cl_assert_equal_i(health_hr_chart_cluster(bpm, 1, clusters, 4), 1);
  cl_assert_equal_i(clusters[0].min_bpm, 72);
  cl_assert_equal_i(clusters[0].max_bpm, 72);
}

void test_health_hr_chart__cluster_tight(void) {
  uint8_t bpm[] = {66, 61, 63};
  HealthHrChartCluster clusters[4];
  cl_assert_equal_i(health_hr_chart_cluster(bpm, 3, clusters, 4), 1);
  cl_assert_equal_i(clusters[0].min_bpm, 61);
  cl_assert_equal_i(clusters[0].max_bpm, 66);
}

void test_health_hr_chart__cluster_outlier(void) {
  uint8_t bpm[] = {64, 95, 62};
  HealthHrChartCluster clusters[4];
  cl_assert_equal_i(health_hr_chart_cluster(bpm, 3, clusters, 4), 2);
  cl_assert_equal_i(clusters[0].min_bpm, 62);
  cl_assert_equal_i(clusters[0].max_bpm, 64);
  cl_assert_equal_i(clusters[1].min_bpm, 95);
  cl_assert_equal_i(clusters[1].max_bpm, 95);
}

void test_health_hr_chart__cluster_gap_at_threshold(void) {
  uint8_t bpm[] = {60, 60 + HEALTH_HR_CHART_CLUSTER_GAP_BPM};
  HealthHrChartCluster clusters[4];
  cl_assert_equal_i(health_hr_chart_cluster(bpm, 2, clusters, 4), 1);
}

void test_health_hr_chart__cluster_two_ranges(void) {
  uint8_t bpm[] = {104, 70, 106, 74, 72};
  HealthHrChartCluster clusters[4];
  cl_assert_equal_i(health_hr_chart_cluster(bpm, 5, clusters, 4), 2);
  cl_assert_equal_i(clusters[0].min_bpm, 70);
  cl_assert_equal_i(clusters[0].max_bpm, 74);
  cl_assert_equal_i(clusters[1].min_bpm, 104);
  cl_assert_equal_i(clusters[1].max_bpm, 106);
}

void test_health_hr_chart__cluster_dense_ramp(void) {
  // A workout ramp is one range however wide, as long as readings are close together
  uint8_t bpm[30];
  for (int i = 0; i < 30; i++) {
    bpm[i] = 150 - (i * 2);
  }
  HealthHrChartCluster clusters[4];
  cl_assert_equal_i(health_hr_chart_cluster(bpm, 30, clusters, 4), 1);
  cl_assert_equal_i(clusters[0].min_bpm, 92);
  cl_assert_equal_i(clusters[0].max_bpm, 150);
}

void test_health_hr_chart__cluster_max_clusters(void) {
  uint8_t bpm[] = {50, 70, 90, 110};
  HealthHrChartCluster clusters[2];
  cl_assert_equal_i(health_hr_chart_cluster(bpm, 4, clusters, 2), 2);
  cl_assert_equal_i(clusters[1].min_bpm, 70);
}

// Rendering
//////////////////////

void test_health_hr_chart__render_no_data(void) {
  prv_render(MINUTE(14, 30));
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_health_hr_chart__render_typical_day(void) {
  prv_render(prv_fill_typical_day());
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_health_hr_chart__render_typical_day_24h(void) {
  s_clock_24h = true;
  prv_render(prv_fill_typical_day());
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_health_hr_chart__render_flat_day(void) {
  prv_fill_background(MINUTE(0, 0), MINUTE(10, 0), 60, 0);
  prv_render(MINUTE(10, 0));
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}

void test_health_hr_chart__render_just_after_midnight(void) {
  // A few readings within a couple of BPM of each other
  s_minute_bpm[MINUTE(0, 3)] = 72;
  s_minute_bpm[MINUTE(0, 13)] = 74;
  s_minute_bpm[MINUTE(0, 23)] = 73;
  prv_render(MINUTE(0, 25));
  cl_check(gbitmap_pbi_eq(&s_ctx.dest_bitmap, TEST_PBI_FILE));
}
