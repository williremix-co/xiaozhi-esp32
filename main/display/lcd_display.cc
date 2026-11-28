#include "lcd_display.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

// --- Ojos robóticos animados en LVGL para pantalla circular 240x240 ---
static lv_obj_t* s_left_eye = nullptr;
static lv_obj_t* s_right_eye = nullptr;
static lv_obj_t* s_left_lid = nullptr;
static lv_obj_t* s_right_lid = nullptr;
static lv_timer_t* s_eye_blink_timer = nullptr;
static int s_eye_base_h = 92;
static int s_eye_base_w = 58;
static int s_eye_offset_y = 0;

static void ApplyEyeShape(int w, int h, int radius, int offset_x, int offset_y, lv_color_t color) {
    if (s_left_eye == nullptr || s_right_eye == nullptr) return;
    s_eye_base_w = w;
    s_eye_base_h = h;
    s_eye_offset_y = offset_y;

    lv_obj_set_size(s_left_eye, w, h);
    lv_obj_set_size(s_right_eye, w, h);
    lv_obj_set_style_radius(s_left_eye, radius, 0);
    lv_obj_set_style_radius(s_right_eye, radius, 0);
    lv_obj_set_style_bg_color(s_left_eye, color, 0);
    lv_obj_set_style_bg_color(s_right_eye, color, 0);

    lv_obj_align(s_left_eye, LV_ALIGN_CENTER, -42 + offset_x, offset_y);
    lv_obj_align(s_right_eye, LV_ALIGN_CENTER, 42 + offset_x, offset_y);

    if (s_left_lid != nullptr && s_right_lid != nullptr) {
        lv_obj_set_size(s_left_lid, w + 8, h / 2 + 6);
        lv_obj_set_size(s_right_lid, w + 8, h / 2 + 6);
        lv_obj_add_flag(s_left_lid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_right_lid, LV_OBJ_FLAG_HIDDEN);
    }
}

static void EyeBlinkTimerCallback(lv_timer_t* timer) {
    if (s_left_eye == nullptr || s_right_eye == nullptr) return;
    static int phase = 0;
    phase = (phase + 1) % 16;

    if (phase == 0) {
        // Cerrar ojos (parpadeo)
        lv_obj_set_height(s_left_eye, 8);
        lv_obj_set_height(s_right_eye, 8);
    } else if (phase == 1) {
        // Abrir ojos de nuevo
        lv_obj_set_height(s_left_eye, s_eye_base_h);
        lv_obj_set_height(s_right_eye, s_eye_base_h);
    } else if (phase == 6) {
        // Mirar ligeramente a la izquierda
        lv_obj_align(s_left_eye, LV_ALIGN_CENTER, -48, s_eye_offset_y);
        lv_obj_align(s_right_eye, LV_ALIGN_CENTER, 36, s_eye_offset_y);
    } else if (phase == 10) {
        // Mirar ligeramente a la derecha
        lv_obj_align(s_left_eye, LV_ALIGN_CENTER, -36, s_eye_offset_y);
        lv_obj_align(s_right_eye, LV_ALIGN_CENTER, 48, s_eye_offset_y);
    } else if (phase == 13) {
        // Volver al centro
        lv_obj_align(s_left_eye, LV_ALIGN_CENTER, -42, s_eye_offset_y);
        lv_obj_align(s_right_eye, LV_ALIGN_CENTER, 42, s_eye_offset_y);
    }
}

static void UpdateRoboEyesEmotion(const char* emotion) {
    if (s_left_eye == nullptr || s_right_eye == nullptr || emotion == nullptr) return;

    lv_color_t cyan = lv_color_hex(0x00E5FF);
    lv_color_t warm = lv_color_hex(0x00FF99);
    lv_color_t alert = lv_color_hex(0xFF5252);
    lv_color_t calm = lv_color_hex(0x64B5F6);

    if (strstr(emotion, "happy") != nullptr || strstr(emotion, "laugh") != nullptr ||
        strstr(emotion, "love") != nullptr || strstr(emotion, "wink") != nullptr) {
        // Ojos felices arqueados (párpado inferior tapa la mitad)
        ApplyEyeShape(62, 76, 30, 0, -4, warm);
        if (s_left_lid != nullptr && s_right_lid != nullptr) {
            lv_obj_remove_flag(s_left_lid, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_right_lid, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(s_left_lid, LV_ALIGN_CENTER, -42, 22);
            lv_obj_align(s_right_lid, LV_ALIGN_CENTER, 42, 22);
        }
    } else if (strstr(emotion, "angry") != nullptr) {
        // Ojos enojados entrecerrados en rojo
        ApplyEyeShape(64, 54, 16, 0, 4, alert);
        if (s_left_lid != nullptr && s_right_lid != nullptr) {
            lv_obj_remove_flag(s_left_lid, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_right_lid, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(s_left_lid, LV_ALIGN_CENTER, -42, -20);
            lv_obj_align(s_right_lid, LV_ALIGN_CENTER, 42, -20);
        }
    } else if (strstr(emotion, "sad") != nullptr || strstr(emotion, "cry") != nullptr ||
               strstr(emotion, "sleep") != nullptr) {
        // Ojos tristes o con sueño
        ApplyEyeShape(60, 42, 20, 0, 10, calm);
    } else if (strstr(emotion, "surprised") != nullptr || strstr(emotion, "shock") != nullptr ||
               strstr(emotion, "thinking") != nullptr) {
        // Ojos bien abiertos de sorpresa / pensando
        ApplyEyeShape(66, 104, 33, 0, -2, cyan);
    } else {
        // Neutral / escuchando / hablando: ojos grandes expresivos
        ApplyEyeShape(58, 92, 29, 0, 0, cyan);
    }
}

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme (fondo oscuro para resaltar los ojos en pantalla circular)
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0x000000));
    light_theme->set_text_color(lv_color_hex(0xFFFFFF));
    light_theme->set_chat_background_color(lv_color_hex(0x000000));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    light_theme->set_system_bubble_color(lv_color_hex(0x000000));
    light_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    light_theme->set_border_color(lv_color_hex(0xFFFFFF));
    light_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "dark");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw black
    std::vector<uint16_t> buffer(width_, 0x0000);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
#if CONFIG_LV_USE_BUILTIN_MALLOC
        const size_t extra_total = 2560 * 1024;
        const size_t chunk_size = LV_MEM_SIZE;
        size_t added = 0;
        while (added < extra_total) {
            size_t this_chunk = extra_total - added;
            if (this_chunk > chunk_size) {
                this_chunk = chunk_size;
            }
            void* pool = heap_caps_malloc(this_chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pool == nullptr || lv_mem_add_pool(pool, this_chunk) == nullptr) {
                if (pool != nullptr) {
                    heap_caps_free(pool);
                }
                ESP_LOGE(TAG, "Failed to add %uKB LVGL PSRAM pool (added %uKB)",
                         (unsigned)(this_chunk / 1024), (unsigned)(added / 1024));
                break;
            }
            added += this_chunk;
        }
        if (added == extra_total) {
            ESP_LOGI(TAG, "Added %uKB LVGL pool in PSRAM", (unsigned)(added / 1024));
        }
#endif
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);

    if (s_eye_blink_timer != nullptr) {
        lv_timer_del(s_eye_blink_timer);
        s_eye_blink_timer = nullptr;
    }
    s_left_eye = nullptr;
    s_right_eye = nullptr;
    s_left_lid = nullptr;
    s_right_lid = nullptr;

    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

void LcdDisplay::ShowEmotion() {
    if (emoji_label_ != nullptr) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    /* Fondo negro puro para la pantalla circular GC9A01 */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_black(), 0);

    /* Contenedor central de ojos grandes */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_set_scrollbar_mode(emoji_box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    /* Ojo izquierdo grande */
    s_left_eye = lv_obj_create(emoji_box_);
    lv_obj_set_size(s_left_eye, 58, 92);
    lv_obj_set_style_radius(s_left_eye, 29, 0);
    lv_obj_set_style_bg_color(s_left_eye, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(s_left_eye, 0, 0);
    lv_obj_set_scrollbar_mode(s_left_eye, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(s_left_eye, LV_ALIGN_CENTER, -42, 0);

    /* Ojo derecho grande */
    s_right_eye = lv_obj_create(emoji_box_);
    lv_obj_set_size(s_right_eye, 58, 92);
    lv_obj_set_style_radius(s_right_eye, 29, 0);
    lv_obj_set_style_bg_color(s_right_eye, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(s_right_eye, 0, 0);
    lv_obj_set_scrollbar_mode(s_right_eye, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(s_right_eye, LV_ALIGN_CENTER, 42, 0);

    /* Párpados para expresiones (feliz / enojado) */
    s_left_lid = lv_obj_create(emoji_box_);
    lv_obj_set_size(s_left_lid, 66, 52);
    lv_obj_set_style_radius(s_left_lid, 24, 0);
    lv_obj_set_style_bg_color(s_left_lid, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_left_lid, 0, 0);
    lv_obj_add_flag(s_left_lid, LV_OBJ_FLAG_HIDDEN);

    s_right_lid = lv_obj_create(emoji_box_);
    lv_obj_set_size(s_right_lid, 66, 52);
    lv_obj_set_style_radius(s_right_lid, 24, 0);
    lv_obj_set_style_bg_color(s_right_lid, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_right_lid, 0, 0);
    lv_obj_add_flag(s_right_lid, LV_OBJ_FLAG_HIDDEN);

    /* Temporizador de parpadeo y movimiento natural cada 250 ms */
    s_eye_blink_timer = lv_timer_create(EyeBlinkTimerCallback, 250, nullptr);

    /* Objetos ocultos de compatibilidad */
    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_label_set_text(emoji_label_, "");
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Preview image */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Barra superior discreta */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, 14, 0);
    lv_obj_set_style_pad_left(top_bar_, 36, 0);
    lv_obj_set_style_pad_right(top_bar_, 36, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lv_color_hex(0x00E5FF), 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lv_color_hex(0x00E5FF), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_hex(0x00E5FF), 0);

    /* Status bar */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, 14, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.65);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_white(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.65);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x80D8FF), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Barra inferior oculta para que los ojos luzcan limpios en el círculo */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + 8);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.7);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.8, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 12, 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || content == nullptr) {
        return;
    }
    lv_label_set_text(chat_message_label_, content);
    // Mantenemos bottom_bar_ oculto para que los ojos grandes ocupen el círculo limpio
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

bool LcdDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);
    UpdateRoboEyesEmotion(emotion);
    return true;
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
