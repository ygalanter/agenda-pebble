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
static bool s_leading_zero = true;

static GFont s_font_14;

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

static void draw_moon(GContext *ctx, int cx, int cy) {
    graphics_context_set_fill_color(ctx, GColorPastelYellow);
    graphics_fill_circle(ctx, GPoint(cx, cy), 7);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, GPoint(cx + 4, cy - 3), 6);
}

static void draw_sun(GContext *ctx, int cx, int cy) {
    graphics_context_set_stroke_color(ctx, GColorChromeYellow);
    graphics_context_set_stroke_width(ctx, 1);
    for (int i = 0; i < 8; i++) {
        int32_t angle = TRIG_MAX_ANGLE * i / 8;
        GPoint s = GPoint(
            cx + 8 * sin_lookup(angle) / TRIG_MAX_RATIO,
            cy - 8 * cos_lookup(angle) / TRIG_MAX_RATIO);
        GPoint e = GPoint(
            cx + 10 * sin_lookup(angle) / TRIG_MAX_RATIO,
            cy - 10 * cos_lookup(angle) / TRIG_MAX_RATIO);
        graphics_draw_line(ctx, s, e);
    }
    graphics_context_set_fill_color(ctx, GColorYellow);
    graphics_fill_circle(ctx, GPoint(cx, cy), 6);
}

static void draw_cloud(GContext *ctx, int cx, int cy, GColor color) {
    graphics_context_set_fill_color(ctx, color);
    graphics_fill_circle(ctx, GPoint(cx - 4, cy - 3), 5);
    graphics_fill_circle(ctx, GPoint(cx + 4, cy - 4), 4);
    graphics_fill_rect(ctx, GRect(cx - 9, cy, 18, 5), 2, GCornersAll);
}

static void draw_icon(GContext *ctx, int cx, int cy, WeatherIcon icon, bool night) {
    switch (icon) {
        case ICON_CLEAR:
            if (night) draw_moon(ctx, cx, cy);
            else draw_sun(ctx, cx, cy);
            break;
        case ICON_FEW_CLOUDS:
            if (night) { draw_moon(ctx, cx - 3, cy - 4); }
            else { draw_sun(ctx, cx - 3, cy - 4); }
            draw_cloud(ctx, cx + 2, cy + 2, GColorLightGray);
            break;
        case ICON_CLOUDY:
            draw_cloud(ctx, cx, cy, GColorLightGray);
            break;
        case ICON_FOG:
            graphics_context_set_stroke_color(ctx, GColorLightGray);
            graphics_context_set_stroke_width(ctx, 2);
            for (int i = 0; i < 4; i++) {
                int y = cy - 5 + i * 4;
                graphics_draw_line(ctx, GPoint(cx - 8, y), GPoint(cx + 8, y));
            }
            break;
        case ICON_RAIN:
            draw_cloud(ctx, cx, cy - 4, GColorLightGray);
            graphics_context_set_stroke_color(ctx, GColorPictonBlue);
            graphics_context_set_stroke_width(ctx, 1);
            graphics_draw_line(ctx, GPoint(cx - 5, cy + 4), GPoint(cx - 6, cy + 8));
            graphics_draw_line(ctx, GPoint(cx, cy + 5), GPoint(cx - 1, cy + 9));
            graphics_draw_line(ctx, GPoint(cx + 5, cy + 4), GPoint(cx + 4, cy + 8));
            break;
        case ICON_SNOW:
            draw_cloud(ctx, cx, cy - 4, GColorLightGray);
            graphics_context_set_fill_color(ctx, GColorWhite);
            graphics_fill_circle(ctx, GPoint(cx - 5, cy + 5), 2);
            graphics_fill_circle(ctx, GPoint(cx + 1, cy + 7), 2);
            graphics_fill_circle(ctx, GPoint(cx + 6, cy + 5), 2);
            break;
        case ICON_THUNDERSTORM:
            draw_cloud(ctx, cx, cy - 4, GColorDarkGray);
            graphics_context_set_stroke_color(ctx, GColorYellow);
            graphics_context_set_stroke_width(ctx, 2);
            graphics_draw_line(ctx, GPoint(cx + 1, cy + 2), GPoint(cx - 2, cy + 5));
            graphics_draw_line(ctx, GPoint(cx - 2, cy + 5), GPoint(cx + 2, cy + 5));
            graphics_draw_line(ctx, GPoint(cx + 2, cy + 5), GPoint(cx - 1, cy + 9));
            break;
        case ICON_UNKNOWN:
            graphics_context_set_stroke_color(ctx, GColorDarkGray);
            graphics_context_set_stroke_width(ctx, 1);
            graphics_draw_line(ctx, GPoint(cx - 5, cy - 5), GPoint(cx + 5, cy + 5));
            graphics_draw_line(ctx, GPoint(cx + 5, cy - 5), GPoint(cx - 5, cy + 5));
            break;
    }
}

static void canvas_update_proc(Layer *layer, GContext *ctx) {
    GRect bounds = layer_get_bounds(layer);
    int w = bounds.size.w;

    int bar_w = w * s_battery_level / 100;
    GColor bar_color = s_battery_charging ? GColorGreen :
                       (s_battery_level <= 20 ? GColorRed :
                        s_battery_level <= 40 ? GColorChromeYellow : GColorGreen);
    graphics_context_set_fill_color(ctx, bar_color);
    graphics_fill_rect(ctx, GRect(0, 0, bar_w, 3), 0, GCornerNone);

    graphics_context_set_stroke_color(ctx, GColorDarkGray);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_line(ctx, GPoint(10, 104), GPoint(w - 10, 104));
    graphics_draw_line(ctx, GPoint(10, 156), GPoint(w - 10, 156));

    if (!s_has_forecast) {
        graphics_context_set_text_color(ctx, GColorDarkGray);
        graphics_draw_text(ctx, "Loading weather...", s_font_14,
            GRect(0, 180, w, 20), GTextOverflowModeTrailingEllipsis,
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
            graphics_context_set_fill_color(ctx, GColorDarkGreen);
            graphics_fill_rect(ctx, GRect(0, 157, col_w, bounds.size.h - 157), 0, GCornerNone);
        }

        graphics_context_set_text_color(ctx, i == 0 ? GColorWhite : GColorLightGray);
        graphics_draw_text(ctx, days[day_idx], s_font_14,
            GRect(cx - col_w / 2, 160, col_w, 16),
            GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

        draw_icon(ctx, cx, 190, s_forecast_icons[i], i == 0 && !s_is_day);

        char temp_str[8];
        snprintf(temp_str, sizeof(temp_str), "%d°", s_forecast_temps[i]);
        graphics_context_set_text_color(ctx, GColorWhite);
        graphics_draw_text(ctx, temp_str, s_font_14,
            GRect(cx - col_w / 2, 206, col_w, 16),
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

static void window_load(Window *window) {
    Layer *root = window_get_root_layer(window);
    GRect bounds = layer_get_bounds(root);
    int w = bounds.size.w;

    s_font_14 = fonts_get_system_font(FONT_KEY_GOTHIC_14);

    s_canvas_layer = layer_create(bounds);
    layer_set_update_proc(s_canvas_layer, canvas_update_proc);
    layer_add_child(root, s_canvas_layer);

    s_time_layer = text_layer_create(GRect(0, 2, w, 70));
    text_layer_set_background_color(s_time_layer, GColorClear);
    text_layer_set_text_color(s_time_layer, GColorWhite);
    text_layer_set_font(s_time_layer,
        fonts_get_system_font(FONT_KEY_LECO_60_NUMBERS_AM_PM));
    text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);
    layer_add_child(root, text_layer_get_layer(s_time_layer));

    s_date_layer = text_layer_create(GRect(0, 72, w, 28));
    text_layer_set_background_color(s_date_layer, GColorClear);
    text_layer_set_text_color(s_date_layer, GColorCadetBlue);
    text_layer_set_font(s_date_layer,
        fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
    text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
    layer_add_child(root, text_layer_get_layer(s_date_layer));

    s_cal_title_layer = text_layer_create(GRect(8, 108, w - 16, 22));
    text_layer_set_background_color(s_cal_title_layer, GColorClear);
    text_layer_set_text_color(s_cal_title_layer, GColorGreen);
    text_layer_set_font(s_cal_title_layer,
        fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
    text_layer_set_text_alignment(s_cal_title_layer, GTextAlignmentLeft);
    text_layer_set_overflow_mode(s_cal_title_layer,
        GTextOverflowModeTrailingEllipsis);
    text_layer_set_text(s_cal_title_layer, "Loading calendar...");
    layer_add_child(root, text_layer_get_layer(s_cal_title_layer));

    s_cal_time_layer = text_layer_create(GRect(8, 130, w - 16, 22));
    text_layer_set_background_color(s_cal_time_layer, GColorClear);
    text_layer_set_text_color(s_cal_time_layer, GColorLightGray);
    text_layer_set_font(s_cal_time_layer,
        fonts_get_system_font(FONT_KEY_GOTHIC_18));
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
