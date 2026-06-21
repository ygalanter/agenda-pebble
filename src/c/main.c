#include <pebble.h>

#define NUM_FORECAST_DAYS 7

typedef enum {
    ICON_CLEAR,
    ICON_FEW_CLOUDS,
    ICON_CLOUDY,
    ICON_FOG,
    ICON_RAIN,
    ICON_SNOW,
    ICON_THUNDERSTORM,
    ICON_UNKNOWN
} WeatherIcon;

static Window *s_main_window;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static TextLayer *s_cal_title_layer;
static TextLayer *s_cal_time_layer;
static Layer *s_canvas_layer;

// Size-driven layout. The >=200 (emery) branch reproduces the original pixel
// positions/fonts exactly; the 144x168 branch compresses to fit the smaller
// screen. Filled once in window_load, read by canvas_update_proc.
typedef struct {
    GFont time_font;
    GFont date_font;
    GFont cal_title_font;
    GFont cal_time_font;
    int time_y, time_h;
    int date_y, date_h;
    int cal_title_y, cal_title_h;
    int cal_time_y, cal_time_h;
    int cal_margin;
    int sep1_y, sep2_y;
    int forecast_top;
    int day_y, day_h;
    int icon_cy;
    int temp_y, temp_h;
    int loading_y;
} Layout;

static Layout s_layout;

static int s_forecast_temps[NUM_FORECAST_DAYS];
static WeatherIcon s_forecast_icons[NUM_FORECAST_DAYS];
static bool s_has_forecast = false;

static char s_cal_title_buf[64];
static char s_cal_time_buf[48];

static char s_time_buf[8];
static char s_date_buf[32];

static int s_battery_level = 100;
static bool s_battery_charging = false;
static bool s_is_day = true;
static bool s_leading_zero = false;

static GFont s_font_14;

// Weather-icon scale (percent). 100 = original size on emery; the 144x168
// platforms shrink the icons so each gets a few px of padding in its column.
static int s_icon_scale = 100;
#define ISC(x) ((x) * s_icon_scale / 100)

static void tick_handler(struct tm *tick_time, TimeUnits units_changed);

static WeatherIcon wmo_to_icon(int code) {
    if (code <= 1) return ICON_CLEAR;
    if (code == 2) return ICON_FEW_CLOUDS;
    if (code == 3) return ICON_CLOUDY;
    if (code <= 48) return ICON_FOG;
    if (code <= 67) return ICON_RAIN;
    if (code <= 77) return ICON_SNOW;
    if (code <= 82) return ICON_RAIN;
    if (code <= 86) return ICON_SNOW;
    if (code >= 95) return ICON_THUNDERSTORM;
    return ICON_UNKNOWN;
}

// All icon primitives draw relative to the (cx, cy) anchor with every offset
// and radius passed through ISC(), so the whole glyph scales uniformly about
// its anchor (lets the 144x168 builds shrink icons for column padding).

static void draw_moon(GContext *ctx, int cx, int cy) {
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorPastelYellow, GColorWhite));
    graphics_fill_circle(ctx, GPoint(cx, cy), ISC(7));
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, GPoint(cx + ISC(4), cy - ISC(3)), ISC(6));
}

static void draw_sun(GContext *ctx, int cx, int cy) {
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite));
    graphics_context_set_stroke_width(ctx, 1);
    for (int i = 0; i < 8; i++) {
        int32_t angle = TRIG_MAX_ANGLE * i / 8;
        GPoint s = GPoint(
            cx + ISC(8) * sin_lookup(angle) / TRIG_MAX_RATIO,
            cy - ISC(8) * cos_lookup(angle) / TRIG_MAX_RATIO);
        GPoint e = GPoint(
            cx + ISC(10) * sin_lookup(angle) / TRIG_MAX_RATIO,
            cy - ISC(10) * cos_lookup(angle) / TRIG_MAX_RATIO);
        graphics_draw_line(ctx, s, e);
    }
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorYellow, GColorWhite));
    graphics_fill_circle(ctx, GPoint(cx, cy), ISC(6));
}

static void draw_cloud(GContext *ctx, int cx, int cy, GColor color) {
#if defined(PBL_BW)
    // On B&W the gray cloud collapses to black and vanishes on the black
    // background; draw it white so the icon reads.
    color = GColorWhite;
#endif
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_circle(ctx, GPoint(cx - ISC(4), cy - ISC(3)), ISC(5));
    graphics_fill_circle(ctx, GPoint(cx + ISC(4), cy - ISC(4)), ISC(4));
    graphics_fill_rect(ctx, GRect(cx - ISC(9), cy, ISC(18), ISC(5)), 2, GCornersAll);
}

static void draw_icon(GContext *ctx, int cx, int cy, WeatherIcon icon, bool night) {
    // Vertical center of each icon's bounding box, relative to its anchor, in
    // unscaled units. Shifting the anchor by -ISC(offset) puts every icon's
    // middle on the same cy line, so the row stays aligned on all platforms.
    static const int center_off[ICON_UNKNOWN + 1] = {
        0,   // ICON_CLEAR (sun/moon already centered)
        -3,  // ICON_FEW_CLOUDS (sun/moon above + cloud below)
        -2,  // ICON_CLOUDY
        1,   // ICON_FOG
        -2,  // ICON_RAIN
        -2,  // ICON_SNOW
        -2,  // ICON_THUNDERSTORM
        0    // ICON_UNKNOWN
    };
    int acy = cy - ISC(center_off[icon]);

    switch (icon) {
        case ICON_CLEAR:
            if (night) draw_moon(ctx, cx, acy);
            else draw_sun(ctx, cx, acy);
            break;
        case ICON_FEW_CLOUDS:
            if (night) { draw_moon(ctx, cx - ISC(3), acy - ISC(4)); }
            else { draw_sun(ctx, cx - ISC(3), acy - ISC(4)); }
            draw_cloud(ctx, cx + ISC(2), acy + ISC(2), GColorLightGray);
            break;
        case ICON_CLOUDY:
            draw_cloud(ctx, cx, acy, GColorLightGray);
            break;
        case ICON_FOG:
            graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
            graphics_context_set_stroke_width(ctx, 2);
            for (int i = 0; i < 4; i++) {
                int y = acy - ISC(5) + ISC(i * 4);
                graphics_draw_line(ctx, GPoint(cx - ISC(8), y), GPoint(cx + ISC(8), y));
            }
            break;
        case ICON_RAIN:
            draw_cloud(ctx, cx, acy - ISC(4), GColorLightGray);
            graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorPictonBlue, GColorWhite));
            graphics_context_set_stroke_width(ctx, 1);
            graphics_draw_line(ctx, GPoint(cx - ISC(5), acy + ISC(4)), GPoint(cx - ISC(6), acy + ISC(8)));
            graphics_draw_line(ctx, GPoint(cx, acy + ISC(5)), GPoint(cx - ISC(1), acy + ISC(9)));
            graphics_draw_line(ctx, GPoint(cx + ISC(5), acy + ISC(4)), GPoint(cx + ISC(4), acy + ISC(8)));
            break;
        case ICON_SNOW:
            draw_cloud(ctx, cx, acy - ISC(4), GColorLightGray);
            graphics_context_set_fill_color(ctx, GColorWhite);
            graphics_fill_circle(ctx, GPoint(cx - ISC(5), acy + ISC(5)), ISC(2));
            graphics_fill_circle(ctx, GPoint(cx + ISC(1), acy + ISC(7)), ISC(2));
            graphics_fill_circle(ctx, GPoint(cx + ISC(6), acy + ISC(5)), ISC(2));
            break;
        case ICON_THUNDERSTORM:
            draw_cloud(ctx, cx, acy - ISC(4), GColorDarkGray);
            graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorYellow, GColorWhite));
            graphics_context_set_stroke_width(ctx, 2);
            graphics_draw_line(ctx, GPoint(cx + ISC(1), acy + ISC(2)), GPoint(cx - ISC(2), acy + ISC(5)));
            graphics_draw_line(ctx, GPoint(cx - ISC(2), acy + ISC(5)), GPoint(cx + ISC(2), acy + ISC(5)));
            graphics_draw_line(ctx, GPoint(cx + ISC(2), acy + ISC(5)), GPoint(cx - ISC(1), acy + ISC(9)));
            break;
        case ICON_UNKNOWN:
            graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
            graphics_context_set_stroke_width(ctx, 1);
            graphics_draw_line(ctx, GPoint(cx - ISC(5), acy - ISC(5)), GPoint(cx + ISC(5), acy + ISC(5)));
            graphics_draw_line(ctx, GPoint(cx + ISC(5), acy - ISC(5)), GPoint(cx - ISC(5), acy + ISC(5)));
            break;
    }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int w = bounds.size.w;

    int bar_w = w * s_battery_level / 100;
#if defined(PBL_COLOR)
    GColor bar_color = s_battery_charging ? GColorGreen :
                       (s_battery_level <= 20 ? GColorRed :
                        s_battery_level <= 40 ? GColorChromeYellow : GColorGreen);
#else
    // The thin level-tinted bar reduces to black on B&W and disappears on the
    // black background; draw it white (level is still encoded by width).
    GColor bar_color = GColorWhite;
#endif
    graphics_context_set_fill_color(ctx, bar_color);
    graphics_fill_rect(ctx, GRect(0, 0, bar_w, 3), 0, GCornerNone);

    // DarkGray separators map to black and vanish on B&W; draw them white there.
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(10, s_layout.sep1_y), GPoint(w - 10, s_layout.sep1_y));
    graphics_draw_line(ctx, GPoint(10, s_layout.sep2_y), GPoint(w - 10, s_layout.sep2_y));

    if (!s_has_forecast) {
        graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorLightGray));
        graphics_draw_text(ctx, "Loading weather...", s_font_14,
            GRect(0, s_layout.loading_y, w, 20), GTextOverflowModeTrailingEllipsis,
            GTextAlignmentCenter, NULL);
        return;
    }

    int col_w = w / NUM_FORECAST_DAYS;
    static const char *days[] = {"Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"};
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int today_wday = t->tm_wday;

    for (int i = 0; i < NUM_FORECAST_DAYS; i++) {
        int cx = col_w / 2 + i * col_w;
        int day_idx = (today_wday + i) % 7;

        if (i == 0) {
#if defined(PBL_COLOR)
            graphics_context_set_fill_color(ctx, GColorDarkGreen);
            graphics_fill_rect(ctx, GRect(0, s_layout.forecast_top, col_w,
                bounds.size.h - s_layout.forecast_top), 0, GCornerNone);
#else
            // DarkGreen fill collapses to black on B&W; mark today with a white
            // outline instead so the white glyphs/icon inside still read.
            graphics_context_set_stroke_color(ctx, GColorWhite);
            graphics_context_set_stroke_width(ctx, 1);
            graphics_draw_rect(ctx, GRect(0, s_layout.forecast_top, col_w,
                bounds.size.h - s_layout.forecast_top));
#endif
        }

        graphics_context_set_text_color(ctx, i == 0 ? GColorWhite : GColorLightGray);
        graphics_draw_text(ctx, days[day_idx], s_font_14,
            GRect(cx - col_w / 2, s_layout.day_y, col_w, s_layout.day_h),
            GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

        draw_icon(ctx, cx, s_layout.icon_cy, s_forecast_icons[i], i == 0 && !s_is_day);

        char temp_str[8];
        snprintf(temp_str, sizeof(temp_str), "%d°", s_forecast_temps[i]);
        graphics_context_set_text_color(ctx, GColorWhite);
        graphics_draw_text(ctx, temp_str, s_font_14,
            GRect(cx - col_w / 2, s_layout.temp_y, col_w, s_layout.temp_h),
            GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    }
}

static void parse_int_csv(const char *csv, int *out, int max_count) {
    int idx = 0;
    const char *p = csv;
    while (*p && idx < max_count) {
        out[idx++] = atoi(p);
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
}

static void inbox_received_callback(DictionaryIterator *iter, void *context) {
    Tuple *t = dict_find(iter, MESSAGE_KEY_FORECAST_TEMPS);
    if (t) {
        parse_int_csv(t->value->cstring, s_forecast_temps, NUM_FORECAST_DAYS);
    }

    t = dict_find(iter, MESSAGE_KEY_FORECAST_CODES);
    if (t) {
        int codes[NUM_FORECAST_DAYS];
        parse_int_csv(t->value->cstring, codes, NUM_FORECAST_DAYS);
        for (int i = 0; i < NUM_FORECAST_DAYS; i++) {
            s_forecast_icons[i] = wmo_to_icon(codes[i]);
        }
        s_has_forecast = true;
    }

    t = dict_find(iter, MESSAGE_KEY_IS_DAY);
    if (t) {
        s_is_day = t->value->int32 != 0;
    }

    t = dict_find(iter, MESSAGE_KEY_CAL_TITLE);
    if (t) {
        strncpy(s_cal_title_buf, t->value->cstring, sizeof(s_cal_title_buf) - 1);
        s_cal_title_buf[sizeof(s_cal_title_buf) - 1] = '\0';
        text_layer_set_text(s_cal_title_layer, s_cal_title_buf);
    }

    t = dict_find(iter, MESSAGE_KEY_CAL_TIME);
    if (t) {
        strncpy(s_cal_time_buf, t->value->cstring, sizeof(s_cal_time_buf) - 1);
        s_cal_time_buf[sizeof(s_cal_time_buf) - 1] = '\0';
        text_layer_set_text(s_cal_time_layer, s_cal_time_buf);
    }

    t = dict_find(iter, MESSAGE_KEY_LEADING_ZERO);
    if (t) {
        s_leading_zero = t->value->int32 != 0;
        time_t now = time(NULL);
        tick_handler(localtime(&now), MINUTE_UNIT);
    }

    layer_mark_dirty(s_canvas_layer);
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", (int)reason);
}

static void request_data(void) {
    DictionaryIterator *out;
    AppMessageResult result = app_message_outbox_begin(&out);
    if (result == APP_MSG_OK) {
        dict_write_uint8(out, MESSAGE_KEY_REQUEST_DATA, 1);
        app_message_outbox_send();
    }
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
    strftime(s_time_buf, sizeof(s_time_buf),
             clock_is_24h_style() ? "%H:%M" : "%I:%M", tick_time);
    if (!s_leading_zero && s_time_buf[0] == '0') {
        s_time_buf[0] = ' ';
    }
    text_layer_set_text(s_time_layer, s_time_buf);

    strftime(s_date_buf, sizeof(s_date_buf), "%A, %b %d", tick_time);
    text_layer_set_text(s_date_layer, s_date_buf);

    if (tick_time->tm_min % 30 == 0) {
        request_data();
    }

    layer_mark_dirty(s_canvas_layer);
}

static void battery_handler(BatteryChargeState charge) {
    s_battery_level = charge.charge_percent;
    s_battery_charging = charge.is_charging;
    layer_mark_dirty(s_canvas_layer);
}

static void init_layout(void) {
    // Compile-time branch: LECO_60 glyphs exceed the 256B max-glyph-size on the
    // 144x168 platforms, so FONT_KEY_LECO_60_NUMBERS_AM_PM only exists on emery.
    // Selecting by PBL_DISPLAY_WIDTH keeps the unavailable constant out of the
    // small-platform builds entirely.
#if PBL_DISPLAY_WIDTH >= 200
    // emery (200x228) — original sizes.
    s_icon_scale = 100;
    s_layout.time_font      = fonts_get_system_font(FONT_KEY_LECO_60_NUMBERS_AM_PM);
    s_layout.date_font      = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    s_layout.cal_title_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    s_layout.cal_time_font  = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    s_layout.time_y = 2;        s_layout.time_h = 70;
    s_layout.date_y = 72;       s_layout.date_h = 28;
    s_layout.cal_title_y = 108; s_layout.cal_title_h = 22;
    s_layout.cal_time_y = 130;  s_layout.cal_time_h = 22;
    s_layout.cal_margin = 8;
    s_layout.sep1_y = 104;      s_layout.sep2_y = 156;
    s_layout.forecast_top = 157;
    s_layout.day_y = 160;       s_layout.day_h = 16;
    s_layout.icon_cy = 190;
    s_layout.temp_y = 206;      s_layout.temp_h = 16;
    s_layout.loading_y = 180;
#else
    // basalt/diorite/flint/aplite (144x168) — compressed to fit.
    // Shrink icons to ~80% so each gets a couple px of padding in its 20px column.
    s_icon_scale = 80;
    s_layout.time_font      = fonts_get_system_font(FONT_KEY_LECO_42_NUMBERS);
    s_layout.date_font      = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    s_layout.cal_title_font = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    s_layout.cal_time_font  = fonts_get_system_font(FONT_KEY_GOTHIC_14);
    s_layout.time_y = 2;        s_layout.time_h = 46;
    s_layout.date_y = 48;       s_layout.date_h = 20;
    s_layout.cal_title_y = 72;  s_layout.cal_title_h = 20;
    s_layout.cal_time_y = 91;   s_layout.cal_time_h = 18;
    s_layout.cal_margin = 5;
    s_layout.sep1_y = 70;       s_layout.sep2_y = 112;
    s_layout.forecast_top = 113;
    s_layout.day_y = 114;       s_layout.day_h = 14;
    s_layout.icon_cy = 140;
    s_layout.temp_y = 150;      s_layout.temp_h = 16;
    s_layout.loading_y = 128;
#endif
}

static void window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(root);
    int w = bounds.size.w;

    init_layout();

    s_font_14 = fonts_get_system_font(FONT_KEY_GOTHIC_14);

    s_canvas_layer = layer_create(bounds);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(root, s_canvas_layer);

    s_time_layer = text_layer_create(GRect(0, s_layout.time_y, w, s_layout.time_h));
    text_layer_set_background_color(s_time_layer, GColorClear);
    text_layer_set_text_color(s_time_layer, GColorWhite);
    text_layer_set_font(s_time_layer, s_layout.time_font);
    text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
    layer_add_child(root, text_layer_get_layer(s_time_layer));

    s_date_layer = text_layer_create(GRect(0, s_layout.date_y, w, s_layout.date_h));
    text_layer_set_background_color(s_date_layer, GColorClear);
    // CadetBlue collapses on B&W; use white there so the date stays visible.
    text_layer_set_text_color(s_date_layer, PBL_IF_COLOR_ELSE(GColorCadetBlue, GColorWhite));
    text_layer_set_font(s_date_layer, s_layout.date_font);
    text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
    layer_add_child(root, text_layer_get_layer(s_date_layer));

    s_cal_title_layer = text_layer_create(
        GRect(s_layout.cal_margin, s_layout.cal_title_y,
              w - 2 * s_layout.cal_margin, s_layout.cal_title_h));
    text_layer_set_background_color(s_cal_title_layer, GColorClear);
    text_layer_set_text_color(s_cal_title_layer, PBL_IF_COLOR_ELSE(GColorGreen, GColorWhite));
    text_layer_set_font(s_cal_title_layer, s_layout.cal_title_font);
    text_layer_set_text_alignment(s_cal_title_layer, GTextAlignmentLeft);
    text_layer_set_overflow_mode(s_cal_title_layer,
        GTextOverflowModeTrailingEllipsis);
    text_layer_set_text(s_cal_title_layer, "Loading calendar...");
    layer_add_child(root, text_layer_get_layer(s_cal_title_layer));

    s_cal_time_layer = text_layer_create(
        GRect(s_layout.cal_margin, s_layout.cal_time_y,
              w - 2 * s_layout.cal_margin, s_layout.cal_time_h));
    text_layer_set_background_color(s_cal_time_layer, GColorClear);
    text_layer_set_text_color(s_cal_time_layer, GColorLightGray);
    text_layer_set_font(s_cal_time_layer, s_layout.cal_time_font);
    text_layer_set_text_alignment(s_cal_time_layer, GTextAlignmentLeft);
    text_layer_set_overflow_mode(s_cal_time_layer,
        GTextOverflowModeTrailingEllipsis);
    layer_add_child(root, text_layer_get_layer(s_cal_time_layer));

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    tick_handler(t, MINUTE_UNIT);
    battery_handler(battery_state_service_peek());
}

static void window_unload(Window *window) {
    text_layer_destroy(s_time_layer);
    text_layer_destroy(s_date_layer);
    text_layer_destroy(s_cal_title_layer);
    text_layer_destroy(s_cal_time_layer);
    layer_destroy(s_canvas_layer);
}

static void init(void) {
    s_main_window = window_create();
    window_set_background_color(s_main_window, GColorBlack);
    window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = window_load,
        .unload = window_unload,
    });
    window_stack_push(s_main_window, true);

    tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
    battery_state_service_subscribe(battery_handler);

    app_message_register_inbox_received(inbox_received_callback);
    app_message_register_inbox_dropped(inbox_dropped_callback);
    app_message_open(512, 64);
}

static void deinit(void) {
    tick_timer_service_unsubscribe();
    battery_state_service_unsubscribe();
    window_destroy(s_main_window);
}

int main(void) {
    init();
    app_event_loop();
    deinit();
}
