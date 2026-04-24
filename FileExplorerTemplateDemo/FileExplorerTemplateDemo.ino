#include <Arduino.h>
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>

namespace app_config {
inline int display_width() { return M5.Display.width(); }
inline int display_height() { return M5.Display.height(); }
#define SCREEN_W display_width()
#define SCREEN_H display_height()

constexpr int SD_SPI_SCK_PIN = 36;
constexpr int SD_SPI_MISO_PIN = 35;
constexpr int SD_SPI_MOSI_PIN = 37;
constexpr int SD_SPI_CS_PIN = 4;

constexpr uint32_t SD_RETRY_MS = 3000;
constexpr size_t MAX_BROWSER_ENTRIES = 64;
}  // namespace app_config

enum class AppScreen : uint8_t {
  Browser = 0,
  Details,
};

struct Rect {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;

  bool contains(int16_t px, int16_t py) const {
    return x <= px && px < x + w && y <= py && py < y + h;
  }
};

struct BrowserEntry {
  String name;
  String path;
  bool is_dir = false;
  size_t size_bytes = 0;
};

struct FileDetails {
  bool valid = false;
  String name;
  String path;
  String parent;
  String extension;
  size_t size_bytes = 0;
};

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t COLOR_BG = rgb565(6, 10, 16);
constexpr uint16_t COLOR_PANEL = rgb565(12, 22, 32);
constexpr uint16_t COLOR_PANEL_ALT = rgb565(18, 30, 42);
constexpr uint16_t COLOR_BORDER = rgb565(54, 96, 118);
constexpr uint16_t COLOR_ACCENT = rgb565(64, 230, 196);
constexpr uint16_t COLOR_ACCENT_DIM = rgb565(26, 106, 98);
constexpr uint16_t COLOR_TEXT = rgb565(226, 234, 242);
constexpr uint16_t COLOR_TEXT_DIM = rgb565(128, 148, 164);
constexpr uint16_t COLOR_WARN = rgb565(255, 114, 72);

static M5Canvas g_canvas(&M5.Display);
static AppScreen g_screen = AppScreen::Browser;
static bool g_needs_redraw = true;
static bool g_sd_ready = false;
static String g_status_message = "Booting";
static uint32_t g_last_sd_check_ms = 0;

static BrowserEntry g_browser_entries[app_config::MAX_BROWSER_ENTRIES];
static size_t g_browser_entry_count = 0;
static size_t g_browser_selected_index = 0;
static size_t g_browser_scroll_offset = 0;
static String g_browser_path = "/";

static FileDetails g_file_details;

void set_status(const String& message) {
  g_status_message = message;
  g_needs_redraw = true;
}

void close_file(File& file) {
  if (file) {
    file.close();
  }
}

String fit_text(const String& text, size_t max_len) {
  if (text.length() <= static_cast<int>(max_len)) {
    return text;
  }
  if (max_len < 4) {
    return text.substring(0, max_len);
  }
  return text.substring(0, max_len - 3) + "...";
}

String format_size(size_t size_bytes) {
  if (size_bytes < 1024) {
    return String(static_cast<unsigned long>(size_bytes)) + " B";
  }
  if (size_bytes < 1024UL * 1024UL) {
    const unsigned long whole = static_cast<unsigned long>(size_bytes / 1024UL);
    const unsigned long frac = static_cast<unsigned long>((size_bytes % 1024UL) * 10UL / 1024UL);
    return String(whole) + "." + String(frac) + " KB";
  }
  const unsigned long whole = static_cast<unsigned long>(size_bytes / (1024UL * 1024UL));
  const unsigned long frac = static_cast<unsigned long>((size_bytes % (1024UL * 1024UL)) * 10UL / (1024UL * 1024UL));
  return String(whole) + "." + String(frac) + " MB";
}

String format_extension(const String& name) {
  const int dot = name.lastIndexOf('.');
  if (dot <= 0 || dot + 1 >= name.length()) {
    return "(none)";
  }
  return name.substring(dot + 1);
}

bool is_root_path(const String& path) {
  return path.length() <= 1;
}

String parent_path(const String& path) {
  if (is_root_path(path)) {
    return "/";
  }

  const int last_slash = path.lastIndexOf('/');
  if (last_slash <= 0) {
    return "/";
  }
  return path.substring(0, last_slash);
}

String base_name(const String& path) {
  const int last_slash = path.lastIndexOf('/');
  return last_slash >= 0 ? path.substring(last_slash + 1) : path;
}

String child_path(const String& parent, const String& child_name) {
  if (is_root_path(parent)) {
    return "/" + child_name;
  }
  return parent + "/" + child_name;
}

int footer_button_bar_y() {
  return app_config::SCREEN_H - 30;
}

int status_line_y() {
  return app_config::SCREEN_H - 44;
}

int browser_detail_panel_y() {
  return app_config::SCREEN_H - 102;
}

Rect browser_button_rect(size_t index) {
  const int16_t button_y = static_cast<int16_t>(footer_button_bar_y());
  const int16_t button_h = 26;
  const int16_t button_w = static_cast<int16_t>(app_config::SCREEN_W / 4);
  return {
      static_cast<int16_t>(index * button_w),
      button_y,
      index == 3 ? static_cast<int16_t>(app_config::SCREEN_W - button_w * 3) : button_w,
      button_h,
  };
}

Rect detail_button_rect(size_t index) {
  const int16_t button_y = static_cast<int16_t>(footer_button_bar_y());
  const int16_t button_h = 26;
  const int16_t button_w = static_cast<int16_t>(app_config::SCREEN_W / 2);
  return {
      static_cast<int16_t>(index * button_w),
      button_y,
      index == 1 ? static_cast<int16_t>(app_config::SCREEN_W - button_w) : button_w,
      button_h,
  };
}

bool browser_entry_less(const BrowserEntry& lhs, const BrowserEntry& rhs) {
  if (lhs.is_dir != rhs.is_dir) {
    return lhs.is_dir && !rhs.is_dir;
  }
  return lhs.name.compareTo(rhs.name) < 0;
}

void clear_browser_entries() {
  for (size_t i = 0; i < app_config::MAX_BROWSER_ENTRIES; ++i) {
    g_browser_entries[i] = BrowserEntry{};
  }
  g_browser_entry_count = 0;
  g_browser_selected_index = 0;
  g_browser_scroll_offset = 0;
}

void insert_browser_entry(const BrowserEntry& entry) {
  if (g_browser_entry_count >= app_config::MAX_BROWSER_ENTRIES) {
    return;
  }

  size_t insert_at = g_browser_entry_count;
  const size_t sort_start = is_root_path(g_browser_path) ? 0 : 1;
  while (insert_at > sort_start && browser_entry_less(entry, g_browser_entries[insert_at - 1])) {
    g_browser_entries[insert_at] = g_browser_entries[insert_at - 1];
    --insert_at;
  }
  g_browser_entries[insert_at] = entry;
  ++g_browser_entry_count;
}

void refresh_browser_directory() {
  clear_browser_entries();

  if (!g_sd_ready) {
    return;
  }

  File dir = SD.open(g_browser_path.c_str(), FILE_READ);
  if (!dir || !dir.isDirectory()) {
    close_file(dir);
    g_browser_path = "/";
    return;
  }

  if (!is_root_path(g_browser_path)) {
    g_browser_entries[0].name = "..";
    g_browser_entries[0].path = parent_path(g_browser_path);
    g_browser_entries[0].is_dir = true;
    g_browser_entry_count = 1;
  }

  File entry = dir.openNextFile();
  while (entry && g_browser_entry_count < app_config::MAX_BROWSER_ENTRIES) {
    const String name = base_name(String(entry.name()));
    BrowserEntry browser_entry;
    browser_entry.name = name;
    browser_entry.path = child_path(g_browser_path, name);
    browser_entry.is_dir = entry.isDirectory();
    browser_entry.size_bytes = entry.size();
    insert_browser_entry(browser_entry);

    close_file(entry);
    entry = dir.openNextFile();
  }

  close_file(dir);
}

size_t browser_visible_rows() {
  const int body_y = 34;
  const int row_h = 11;
  const int max_height = browser_detail_panel_y() - body_y - 4;
  if (max_height <= row_h) {
    return 1;
  }
  return static_cast<size_t>(max_height / row_h);
}

void ensure_browser_selection_visible() {
  const size_t visible_rows = browser_visible_rows();
  if (g_browser_selected_index < g_browser_scroll_offset) {
    g_browser_scroll_offset = g_browser_selected_index;
  } else if (g_browser_selected_index >= g_browser_scroll_offset + visible_rows) {
    g_browser_scroll_offset = g_browser_selected_index - visible_rows + 1;
  }
}

bool init_sd_card() {
  SPI.begin(app_config::SD_SPI_SCK_PIN, app_config::SD_SPI_MISO_PIN, app_config::SD_SPI_MOSI_PIN, app_config::SD_SPI_CS_PIN);
  g_sd_ready = SD.begin(app_config::SD_SPI_CS_PIN, SPI, 25000000);
  g_last_sd_check_ms = millis();
  return g_sd_ready;
}

bool load_file_details(const BrowserEntry& entry) {
  g_file_details = FileDetails{};

  if (entry.is_dir) {
    set_status("Directory selected");
    return false;
  }

  File file = SD.open(entry.path.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    close_file(file);
    set_status("Failed to open file");
    return false;
  }

  g_file_details.valid = true;
  g_file_details.name = entry.name;
  g_file_details.path = entry.path;
  g_file_details.parent = parent_path(entry.path);
  g_file_details.extension = format_extension(entry.name);
  g_file_details.size_bytes = file.size();
  close_file(file);
  return true;
}

void draw_battery_status() {
  const int battery_level = M5.Power.getBatteryLevel();
  const int capped_level = battery_level < 0 ? 0 : (battery_level > 100 ? 100 : battery_level);
  g_canvas.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  g_canvas.setCursor(app_config::SCREEN_W - 48, 3);
  g_canvas.printf("%3d%%", capped_level);
}

void draw_header_status(const char* title) {
  const int title_right = 6 + g_canvas.textWidth(title) + 10;
  const int status_left = title_right + 4;
  const int status_right = app_config::SCREEN_W - 52;
  const int status_width = status_right - status_left;
  if (status_width <= 8) {
    return;
  }

  String status = g_status_message;
  while (status.length() > 4 && g_canvas.textWidth(status) > status_width) {
    status = fit_text(status, status.length() - 1);
  }

  g_canvas.setTextColor(COLOR_TEXT_DIM, COLOR_PANEL);
  const int text_x = status_left + (status_width - g_canvas.textWidth(status)) / 2;
  g_canvas.setCursor(text_x, 4);
  g_canvas.print(status);
}

void draw_header(const char* title) {
  g_canvas.fillScreen(COLOR_BG);
  g_canvas.fillRoundRect(0, 0, app_config::SCREEN_W, 18, 6, COLOR_PANEL);
  g_canvas.drawRoundRect(0, 0, app_config::SCREEN_W, 18, 6, COLOR_BORDER);
  g_canvas.setTextColor(COLOR_ACCENT, COLOR_PANEL);
  g_canvas.setCursor(6, 4);
  g_canvas.print(title);
  draw_header_status(title);
  draw_battery_status();
}

void draw_footer(const String& text) {
  g_canvas.fillRoundRect(0, status_line_y(), app_config::SCREEN_W, 14, 5, COLOR_PANEL);
  g_canvas.drawRoundRect(0, status_line_y(), app_config::SCREEN_W, 14, 5, COLOR_BORDER);
  g_canvas.setTextColor(COLOR_TEXT_DIM, COLOR_PANEL);
  g_canvas.setCursor(4, status_line_y() + 3);
  g_canvas.print(fit_text(text, app_config::SCREEN_W >= 320 ? 52 : 38));
}

void draw_touch_button(const Rect& rect, const char* label, uint16_t fill, uint16_t text) {
  g_canvas.fillRoundRect(rect.x, rect.y, rect.w, rect.h, 5, fill);
  g_canvas.drawRoundRect(rect.x, rect.y, rect.w, rect.h, 5, COLOR_BORDER);
  g_canvas.setTextColor(text, fill);
  g_canvas.setCursor(rect.x + (rect.w - g_canvas.textWidth(label)) / 2, rect.y + (rect.h - 8) / 2);
  g_canvas.print(label);
}

void draw_browser_touch_controls() {
  draw_touch_button(browser_button_rect(0), "UP", COLOR_PANEL_ALT, COLOR_TEXT);
  draw_touch_button(browser_button_rect(1), "DOWN", COLOR_PANEL_ALT, COLOR_TEXT);
  draw_touch_button(browser_button_rect(2), "OPEN", COLOR_ACCENT_DIM, COLOR_TEXT);
  draw_touch_button(browser_button_rect(3), "BACK", COLOR_PANEL_ALT, COLOR_TEXT_DIM);
}

void draw_detail_touch_controls() {
  draw_touch_button(detail_button_rect(0), "BACK", COLOR_PANEL_ALT, COLOR_TEXT);
  draw_touch_button(detail_button_rect(1), "REFRESH", COLOR_ACCENT_DIM, COLOR_TEXT);
}

void draw_browser_preview_panel() {
  const int panel_y = browser_detail_panel_y();
  const int panel_h = footer_button_bar_y() - panel_y - 16;
  g_canvas.fillRoundRect(0, panel_y, app_config::SCREEN_W, panel_h, 5, COLOR_PANEL);
  g_canvas.drawRoundRect(0, panel_y, app_config::SCREEN_W, panel_h, 5, COLOR_BORDER);

  if (g_browser_entry_count == 0 || g_browser_selected_index >= g_browser_entry_count) {
    g_canvas.setTextColor(COLOR_WARN, COLOR_PANEL);
    g_canvas.setCursor(6, panel_y + 8);
    g_canvas.print(g_sd_ready ? "No entries" : "Insert an SD card");
    return;
  }

  const BrowserEntry& entry = g_browser_entries[g_browser_selected_index];
  g_canvas.setTextColor(COLOR_TEXT, COLOR_PANEL);
  g_canvas.setCursor(6, panel_y + 6);
  g_canvas.print("Selected");
  g_canvas.setCursor(62, panel_y + 6);
  g_canvas.print(fit_text(entry.name, 28));

  g_canvas.setTextColor(COLOR_TEXT_DIM, COLOR_PANEL);
  g_canvas.setCursor(6, panel_y + 20);
  g_canvas.print("Type");
  g_canvas.setCursor(62, panel_y + 20);
  g_canvas.print(entry.is_dir ? "Directory" : "File");

  g_canvas.setCursor(6, panel_y + 34);
  g_canvas.print("Size");
  g_canvas.setCursor(62, panel_y + 34);
  g_canvas.print(entry.is_dir ? "-" : format_size(entry.size_bytes));

  g_canvas.setCursor(6, panel_y + 48);
  g_canvas.print("Path");
  g_canvas.setCursor(62, panel_y + 48);
  g_canvas.print(fit_text(entry.path, 30));
}

void draw_browser_screen() {
  draw_header("File Explorer");

  g_canvas.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  g_canvas.setCursor(4, 22);
  g_canvas.printf("SD:%s  %s", g_sd_ready ? "OK" : "NG", fit_text(g_browser_path, 24).c_str());

  const int body_y = 34;
  const int row_h = 11;
  const size_t visible_rows = browser_visible_rows();
  for (size_t row = 0; row < visible_rows; ++row) {
    const size_t index = g_browser_scroll_offset + row;
    if (index >= g_browser_entry_count) {
      continue;
    }

    const int y = body_y + static_cast<int>(row) * row_h;
    const bool selected = index == g_browser_selected_index;
    const BrowserEntry& entry = g_browser_entries[index];
    const uint16_t fill = selected ? COLOR_ACCENT_DIM : COLOR_PANEL_ALT;
    const uint16_t text = selected ? COLOR_TEXT : COLOR_TEXT_DIM;

    g_canvas.fillRoundRect(0, y - 1, app_config::SCREEN_W, 10, 3, fill);
    g_canvas.setTextColor(text, fill);
    g_canvas.setCursor(4, y + 1);

    String label = entry.name;
    if (entry.is_dir) {
      label = "[" + label + "]";
    }
    g_canvas.print(fit_text(label, app_config::SCREEN_W >= 320 ? 34 : 24));

    if (!entry.is_dir && entry.size_bytes > 0) {
      g_canvas.setCursor(app_config::SCREEN_W - 60, y + 1);
      g_canvas.print(fit_text(format_size(entry.size_bytes), 8));
    }
  }

  if (g_browser_entry_count == 0) {
    g_canvas.setTextColor(COLOR_WARN, COLOR_BG);
    g_canvas.setCursor(4, 48);
    g_canvas.print(g_sd_ready ? "No files" : "Insert an SD card");
  }

  draw_browser_preview_panel();
  draw_footer("Tap row: select/open  buttons: up/down/open/back");
  draw_browser_touch_controls();
}

void draw_detail_field(int y, const char* label, const String& value) {
  g_canvas.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  g_canvas.setCursor(6, y);
  g_canvas.print(label);
  g_canvas.setTextColor(COLOR_TEXT, COLOR_BG);
  g_canvas.setCursor(76, y);
  g_canvas.print(value);
}

void draw_details_screen() {
  draw_header("File Details");

  if (!g_file_details.valid) {
    g_canvas.setTextColor(COLOR_WARN, COLOR_BG);
    g_canvas.setCursor(6, 28);
    g_canvas.print("No file selected");
    draw_footer("Back to return");
    draw_detail_touch_controls();
    return;
  }

  g_canvas.fillRoundRect(4, 24, app_config::SCREEN_W - 8, 148, 6, COLOR_PANEL_ALT);
  g_canvas.drawRoundRect(4, 24, app_config::SCREEN_W - 8, 148, 6, COLOR_BORDER);

  draw_detail_field(32, "Name", fit_text(g_file_details.name, 28));
  draw_detail_field(50, "Ext", g_file_details.extension);
  draw_detail_field(68, "Size", format_size(g_file_details.size_bytes));
  draw_detail_field(86, "Bytes", String(static_cast<unsigned long>(g_file_details.size_bytes)));
  draw_detail_field(104, "Parent", fit_text(g_file_details.parent, 26));
  draw_detail_field(122, "Path", fit_text(g_file_details.path, 26));

  g_canvas.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  g_canvas.setCursor(6, 148);
  g_canvas.print("Tap REFRESH after swapping the SD card.");

  draw_footer("BACK: browser  REFRESH: reload selected file");
  draw_detail_touch_controls();
}

void draw_screen() {
  if (!g_needs_redraw) {
    return;
  }

  g_canvas.setTextFont(1);
  g_canvas.setTextSize(1);
  if (g_screen == AppScreen::Browser) {
    draw_browser_screen();
  } else {
    draw_details_screen();
  }

  g_canvas.pushSprite(0, 0);
  g_needs_redraw = false;
}

void show_selected_entry() {
  if (g_browser_entry_count == 0 || g_browser_selected_index >= g_browser_entry_count) {
    return;
  }

  const BrowserEntry& entry = g_browser_entries[g_browser_selected_index];
  if (entry.is_dir) {
    g_browser_path = entry.path;
    refresh_browser_directory();
    set_status("Open " + fit_text(entry.path, 18));
    return;
  }

  if (load_file_details(entry)) {
    g_screen = AppScreen::Details;
    set_status("Show " + fit_text(entry.name, 18));
  }
}

void return_to_browser() {
  g_screen = AppScreen::Browser;
  set_status("Back to browser");
}

void move_browser_selection_up() {
  if (g_browser_selected_index > 0) {
    --g_browser_selected_index;
    ensure_browser_selection_visible();
    g_needs_redraw = true;
  }
}

void move_browser_selection_down() {
  if (g_browser_selected_index + 1 < g_browser_entry_count) {
    ++g_browser_selected_index;
    ensure_browser_selection_visible();
    g_needs_redraw = true;
  }
}

bool browser_row_hit_test(int16_t x, int16_t y, size_t* out_index) {
  const int body_y = 34;
  const int row_h = 11;
  if (x < 0 || x >= app_config::SCREEN_W) {
    return false;
  }
  if (y < body_y || y >= browser_detail_panel_y() - 2) {
    return false;
  }

  const int row = (y - body_y) / row_h;
  const size_t index = g_browser_scroll_offset + static_cast<size_t>(row);
  if (index >= g_browser_entry_count) {
    return false;
  }

  *out_index = index;
  return true;
}

void handle_touch_input() {
  const auto touch_count = M5.Touch.getCount();
  for (size_t i = 0; i < touch_count; ++i) {
    const auto detail = M5.Touch.getDetail(i);
    if (!detail.wasReleased()) {
      continue;
    }

    const int16_t x = detail.base.x;
    const int16_t y = detail.base.y;

    if (g_screen == AppScreen::Browser) {
      size_t hit_index = 0;
      if (browser_row_hit_test(x, y, &hit_index)) {
        if (g_browser_selected_index == hit_index) {
          show_selected_entry();
        } else {
          g_browser_selected_index = hit_index;
          ensure_browser_selection_visible();
          g_needs_redraw = true;
        }
        return;
      }

      if (browser_button_rect(0).contains(x, y)) {
        move_browser_selection_up();
      } else if (browser_button_rect(1).contains(x, y)) {
        move_browser_selection_down();
      } else if (browser_button_rect(2).contains(x, y)) {
        show_selected_entry();
      } else if (browser_button_rect(3).contains(x, y) && !is_root_path(g_browser_path)) {
        g_browser_path = parent_path(g_browser_path);
        refresh_browser_directory();
        set_status("Back " + fit_text(g_browser_path, 18));
      }
      return;
    }

    if (detail_button_rect(0).contains(x, y)) {
      return_to_browser();
    } else if (detail_button_rect(1).contains(x, y)) {
      if (g_browser_entry_count > 0 && g_browser_selected_index < g_browser_entry_count) {
        load_file_details(g_browser_entries[g_browser_selected_index]);
        set_status("Reloaded details");
      }
    }
    return;
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.clear_display = true;
  M5.begin(cfg);
  M5.Display.setRotation(1);

  g_canvas.setColorDepth(16);
  g_canvas.createSprite(app_config::SCREEN_W, app_config::SCREEN_H);

  if (init_sd_card()) {
    refresh_browser_directory();
    set_status("SD card ready");
  } else {
    set_status("Insert SD card");
  }
}

void loop() {
  M5.update();

  if (!g_sd_ready && millis() - g_last_sd_check_ms >= app_config::SD_RETRY_MS) {
    if (init_sd_card()) {
      refresh_browser_directory();
      set_status("SD card mounted");
    }
  }

  handle_touch_input();
  draw_screen();
  delay(8);
}
