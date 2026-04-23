#include <Arduino.h>
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>
#include <esp32-hal-psram.h>

#include "src/libpxtone/pxtnService.h"

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
constexpr size_t MIN_NOTES = 4096;
constexpr size_t MAX_NOTES_WITH_PSRAM = 65536;

constexpr uint8_t AUDIO_CHANNEL = 0;
constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;
constexpr size_t AUDIO_BUFFER_COUNT = 6;
constexpr size_t AUDIO_BUFFER_SAMPLES = 4096;
constexpr size_t AUDIO_QUEUE_TARGET = 2;
constexpr uint8_t AUDIO_VOLUME = 180;
constexpr uint32_t PLAYER_REDRAW_MS = 66;
constexpr uint32_t PLAYER_REDRAW_MS_LIVE = 100;
constexpr uint32_t AUDIO_STOP_TIMEOUT_MS = 250;

constexpr int32_t CLOCKS_PER_NOTE = 0x100;
constexpr int32_t DEFAULT_VIEW_SPAN = EVENTDEFAULT_BEATCLOCK * 8;
constexpr int32_t MIN_VIEW_SPAN = EVENTDEFAULT_BEATCLOCK * 2;
constexpr int32_t MAX_VIEW_SPAN = EVENTDEFAULT_BEATCLOCK * 64;
}  // namespace app_config

enum class AppScreen : uint8_t {
  Browser = 0,
  Player,
};

enum class PlaybackSource : uint8_t {
  None = 0,
  Service,
  MemoryCache,
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
  bool is_playable = false;
  size_t size_bytes = 0;
};

struct NoteSegment {
  int32_t start_clock = 0;
  int32_t end_clock = 0;
  int32_t key = EVENTDEFAULT_KEY;
  uint8_t unit = 0;
  uint8_t velocity = EVENTDEFAULT_VELOCITY;
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
constexpr uint16_t COLOR_OK = rgb565(140, 255, 160);
constexpr uint16_t COLOR_KEY_WHITE = rgb565(24, 36, 48);
constexpr uint16_t COLOR_KEY_BLACK = rgb565(12, 20, 28);
constexpr uint16_t COLOR_PLAYHEAD = rgb565(255, 208, 72);
constexpr uint16_t COLOR_LOOP = rgb565(255, 128, 200);

static const uint16_t kTrackPalette[] = {
    rgb565(80, 220, 255), rgb565(255, 128, 116), rgb565(255, 224, 84), rgb565(128, 255, 160),
    rgb565(180, 140, 255), rgb565(255, 164, 84), rgb565(116, 220, 160), rgb565(255, 132, 214),
};

static M5Canvas g_canvas(&M5.Display);
static AppScreen g_screen = AppScreen::Browser;
static bool g_needs_redraw = true;
static bool g_sd_ready = false;
static String g_status_message = "Booting";
static uint32_t g_last_sd_check_ms = 0;
static uint32_t g_last_player_redraw_ms = 0;

static BrowserEntry g_browser_entries[app_config::MAX_BROWSER_ENTRIES];
static size_t g_browser_entry_count = 0;
static size_t g_browser_selected_index = 0;
static size_t g_browser_scroll_offset = 0;
static String g_browser_path = "/";

static File g_song_file;
static pxtnService* g_song_service = nullptr;
static PlaybackSource g_playback_source = PlaybackSource::None;
static String g_song_path;
static String g_song_title;
static bool g_song_cached = false;
static bool g_song_loaded = false;
static bool g_song_playing = false;
static bool g_song_paused = false;
static bool g_song_finished = false;
static bool g_song_has_loop = false;
static bool g_song_queue_draining = false;
static bool g_song_notes_truncated = false;
static int32_t g_song_resume_sample = 0;
static int32_t g_song_total_samples = 0;
static int32_t g_song_current_sample = 0;
static int32_t g_song_end_clock = 0;
static int32_t g_song_repeat_clock = 0;
static int32_t g_view_center_clock = 0;
static int32_t g_view_span_clock = app_config::DEFAULT_VIEW_SPAN;
static int32_t g_min_note_key = EVENTDEFAULT_KEY - app_config::CLOCKS_PER_NOTE * 12;
static int32_t g_max_note_key = EVENTDEFAULT_KEY + app_config::CLOCKS_PER_NOTE * 12;
static uint8_t g_note_lane_count = 0;
static NoteSegment* g_notes = nullptr;
static size_t g_note_capacity = 0;
static size_t g_note_count = 0;
static int16_t* g_song_cache_samples = nullptr;
static size_t g_song_cache_bytes = 0;
static int16_t g_audio_buffers[app_config::AUDIO_BUFFER_COUNT][app_config::AUDIO_BUFFER_SAMPLES];
static size_t g_audio_buffer_index = 0;
static bool g_audio_started = false;

void set_status(const String& message) {
  g_status_message = message;
  g_needs_redraw = true;
}

void set_status_now(const String& message) {
  set_status(message);
  draw_screen();
}

String format_pxtone_error(const char* prefix, pxtnERR error) {
  String message(prefix);
  message += " ";
  message += pxtnError_get_string(error);
  message += " (";
  message += static_cast<int>(error);
  message += ")";
  return message;
}

String format_memory_status() {
  String message("heap:");
  message += String(ESP.getFreeHeap() / 1024);
  message += "K ps:";
  message += String(ESP.getFreePsram() / 1024);
  message += "K";
  return message;
}

bool ensure_note_buffer() {
  if (g_notes) {
    return true;
  }

  const size_t note_capacity = determine_note_capacity();
  const size_t byte_size = sizeof(NoteSegment) * note_capacity;
  void* buffer = nullptr;
  if (psramFound()) {
    buffer = ps_malloc(byte_size);
  }
  if (!buffer) {
    buffer = malloc(byte_size);
  }
  if (!buffer) {
    return false;
  }

  g_notes = static_cast<NoteSegment*>(buffer);
  g_note_capacity = note_capacity;
  memset(g_notes, 0, byte_size);
  return true;
}

void close_file(File& file) {
  if (file) {
    file.close();
  }
}

int32_t bytes_to_samples(size_t byte_count) {
  return static_cast<int32_t>(byte_count / sizeof(int16_t));
}

int32_t clamp_i32(int32_t value, int32_t min_value, int32_t max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
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

int footer_button_bar_y() {
  return app_config::SCREEN_H - 30;
}

int status_line_y() {
  return app_config::SCREEN_H - 44;
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

Rect player_button_rect(size_t index) {
  const int16_t button_y = static_cast<int16_t>(footer_button_bar_y());
  const int16_t button_h = 26;
  const int16_t button_w = static_cast<int16_t>(app_config::SCREEN_W / 3);
  return {
      static_cast<int16_t>(index * button_w),
      button_y,
      index == 2 ? static_cast<int16_t>(app_config::SCREEN_W - button_w * 2) : button_w,
      button_h,
  };
}

size_t determine_note_capacity() {
  size_t capacity = app_config::MIN_NOTES;
  if (psramFound()) {
    const size_t by_memory = static_cast<size_t>(ESP.getFreePsram()) / (sizeof(NoteSegment) * 8);
    capacity = min(app_config::MAX_NOTES_WITH_PSRAM, max(app_config::MIN_NOTES, by_memory));
  }
  return capacity;
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

bool is_pxtone_file(const String& name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".ptcop") || lower.endsWith(".pttune");
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
    g_browser_entries[0].is_playable = false;
    g_browser_entry_count = 1;
  }

  File entry = dir.openNextFile();
  while (entry && g_browser_entry_count < app_config::MAX_BROWSER_ENTRIES) {
    String name = String(entry.name());
    if (name.startsWith(g_browser_path)) {
      name = base_name(name);
    } else {
      name = base_name(name);
    }

    const bool is_dir = entry.isDirectory();
    BrowserEntry browser_entry;
    browser_entry.name = name;
    browser_entry.path = child_path(g_browser_path, name);
    browser_entry.is_dir = is_dir;
    browser_entry.is_playable = !is_dir && is_pxtone_file(name);
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
  const int max_height = footer_button_bar_y() - body_y - 2;
  return max<size_t>(1, max_height / row_h);
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

bool pxtn_io_read(void* user, void* dst, int32_t size, int32_t num) {
  File* file = static_cast<File*>(user);
  return file && file->read(static_cast<uint8_t*>(dst), size * num) == size * num;
}

bool pxtn_io_write(void* user, const void* src, int32_t size, int32_t num) {
  File* file = static_cast<File*>(user);
  return file && file->write(static_cast<const uint8_t*>(src), size * num) == static_cast<size_t>(size * num);
}

bool pxtn_io_seek(void* user, int32_t mode, int32_t size) {
  File* file = static_cast<File*>(user);
  if (!file) {
    return false;
  }

  uint32_t target = 0;
  switch (mode) {
    case SEEK_SET:
      target = size < 0 ? 0U : static_cast<uint32_t>(size);
      break;
    case SEEK_CUR: {
      const int32_t current = static_cast<int32_t>(file->position());
      target = current + size < 0 ? 0U : static_cast<uint32_t>(current + size);
      break;
    }
    case SEEK_END: {
      const int32_t end_pos = static_cast<int32_t>(file->size());
      target = end_pos + size < 0 ? 0U : static_cast<uint32_t>(end_pos + size);
      break;
    }
    default:
      return false;
  }
  return file->seek(target, SeekSet);
}

bool pxtn_io_pos(void* user, int32_t* pos) {
  File* file = static_cast<File*>(user);
  if (!file || !pos) {
    return false;
  }
  *pos = static_cast<int32_t>(file->position());
  return true;
}

int key_to_note_index(int32_t key) {
  return (key - EVENTDEFAULT_KEY) / app_config::CLOCKS_PER_NOTE + 60;
}

bool is_black_key(int note_index) {
  switch ((note_index % 12 + 12) % 12) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
      return true;
    default:
      return false;
  }
}

void stop_audio_output() {
  M5.Speaker.stop(app_config::AUDIO_CHANNEL);
  const uint32_t started_at = millis();
  while (M5.Speaker.isPlaying(app_config::AUDIO_CHANNEL) != 0 &&
         millis() - started_at < app_config::AUDIO_STOP_TIMEOUT_MS) {
    delay(1);
  }
  memset(g_audio_buffers, 0, sizeof(g_audio_buffers));
  g_audio_started = false;
  g_audio_buffer_index = 0;
  g_song_queue_draining = false;
}

void clear_song_cache() {
  if (g_song_cache_samples) {
    free(g_song_cache_samples);
    g_song_cache_samples = nullptr;
  }
  g_song_cache_bytes = 0;
  g_song_cached = false;
  g_song_total_samples = 0;
  g_song_current_sample = 0;
}

void release_song() {
  stop_audio_output();
  close_file(g_song_file);
  if (g_song_service) {
    delete g_song_service;
    g_song_service = nullptr;
  }
  clear_song_cache();
  g_playback_source = PlaybackSource::None;
  g_song_path = "";
  g_song_title = "";
  g_song_cached = false;
  g_song_loaded = false;
  g_song_playing = false;
  g_song_paused = false;
  g_song_finished = false;
  g_song_has_loop = false;
  g_song_resume_sample = 0;
  g_song_total_samples = 0;
  g_song_current_sample = 0;
  g_song_end_clock = 0;
  g_song_repeat_clock = 0;
  g_note_count = 0;
  g_note_lane_count = 0;
  g_song_notes_truncated = false;
}

void extract_note_segments() {
  g_note_count = 0;
  g_note_lane_count = 0;
  g_song_notes_truncated = false;
  g_min_note_key = EVENTDEFAULT_KEY - app_config::CLOCKS_PER_NOTE * 12;
  g_max_note_key = EVENTDEFAULT_KEY + app_config::CLOCKS_PER_NOTE * 12;

  if (!g_song_service || !ensure_note_buffer()) {
    g_song_notes_truncated = true;
    return;
  }

  struct UnitState {
    int32_t key = EVENTDEFAULT_KEY;
    int32_t velocity = EVENTDEFAULT_VELOCITY;
  };

  UnitState unit_states[pxtnMAX_TUNEUNITSTRUCT];
  for (int i = 0; i < pxtnMAX_TUNEUNITSTRUCT; ++i) {
    unit_states[i].key = EVENTDEFAULT_KEY;
    unit_states[i].velocity = EVENTDEFAULT_VELOCITY;
  }

  bool first_note = true;
  for (const EVERECORD* record = g_song_service->evels->get_Records(); record; record = record->next) {
    if (record->unit_no >= pxtnMAX_TUNEUNITSTRUCT) {
      continue;
    }

    switch (record->kind) {
      case EVENTKIND_KEY:
        unit_states[record->unit_no].key = record->value;
        break;
      case EVENTKIND_VELOCITY:
        unit_states[record->unit_no].velocity = record->value;
        break;
      case EVENTKIND_ON:
        if (record->value <= 0) {
          break;
        }
        if (g_note_count >= g_note_capacity) {
          g_song_notes_truncated = true;
          break;
        }
        g_notes[g_note_count].start_clock = record->clock;
        g_notes[g_note_count].end_clock = record->clock + record->value;
        g_notes[g_note_count].key = unit_states[record->unit_no].key;
        g_notes[g_note_count].unit = record->unit_no;
        g_notes[g_note_count].velocity = static_cast<uint8_t>(unit_states[record->unit_no].velocity);
        if (record->unit_no + 1 > g_note_lane_count) {
          g_note_lane_count = record->unit_no + 1;
        }
        if (first_note) {
          g_min_note_key = g_notes[g_note_count].key;
          g_max_note_key = g_notes[g_note_count].key;
          first_note = false;
        } else {
          if (g_notes[g_note_count].key < g_min_note_key) {
            g_min_note_key = g_notes[g_note_count].key;
          }
          if (g_notes[g_note_count].key > g_max_note_key) {
            g_max_note_key = g_notes[g_note_count].key;
          }
        }
        ++g_note_count;
        break;
      default:
        break;
    }
  }

  if (g_max_note_key - g_min_note_key < app_config::CLOCKS_PER_NOTE * 12) {
    const int32_t pad = app_config::CLOCKS_PER_NOTE * 6;
    g_min_note_key -= pad;
    g_max_note_key += pad;
  }
}

bool prepare_song_playback(int32_t start_sample) {
  if (!g_song_service) {
    return false;
  }

  pxtnVOMITPREPARATION preparation = {};
  preparation.start_pos_sample = start_sample;
  preparation.master_volume = 0.9f;

  if (!g_song_service->moo_preparation(&preparation)) {
    return false;
  }

  g_song_service->moo_set_loop(g_song_has_loop);
  g_song_end_clock = g_song_service->moo_get_end_clock();
  g_song_repeat_clock = g_song_service->master->get_repeat_meas() *
                        g_song_service->master->get_beat_num() *
                        g_song_service->master->get_beat_clock();
  g_song_total_samples = g_song_service->moo_get_total_sample();
  g_song_resume_sample = start_sample;
  return true;
}

int32_t current_song_sample() {
  if (g_song_playing) {
    if (g_playback_source == PlaybackSource::Service && g_song_service) {
      return g_song_service->moo_get_sampling_offset();
    }
    if (g_playback_source == PlaybackSource::MemoryCache) {
      return g_song_current_sample;
    }
  }
  return g_song_resume_sample;
}

int32_t current_song_clock() {
  if (g_song_playing && g_playback_source == PlaybackSource::Service && g_song_service) {
    return g_song_service->moo_get_now_clock();
  }
  if (g_song_total_samples > 0 && g_song_end_clock > 0) {
    const int64_t scaled = static_cast<int64_t>(current_song_sample()) * g_song_end_clock;
    return clamp_i32(static_cast<int32_t>(scaled / g_song_total_samples), 0, g_song_end_clock);
  }
  return 0;
}

bool build_song_cache() {
  if (!g_song_service) {
    return false;
  }

  clear_song_cache();
  if (!prepare_song_playback(0)) {
    clear_song_cache();
    return false;
  }

  const int32_t total_samples = g_song_service->moo_get_total_sample();
  if (total_samples <= 0) {
    clear_song_cache();
    return false;
  }

  const size_t cache_bytes = static_cast<size_t>(total_samples) * sizeof(int16_t);
  void* cache_buffer = nullptr;
  if (psramFound()) {
    cache_buffer = ps_malloc(cache_bytes);
  }
  if (!cache_buffer) {
    cache_buffer = malloc(cache_bytes);
  }
  if (!cache_buffer) {
    clear_song_cache();
    return false;
  }

  g_song_cache_samples = static_cast<int16_t*>(cache_buffer);
  g_song_cache_bytes = cache_bytes;

  int32_t rendered_samples = 0;
  int last_reported_percent = -10;
  while (rendered_samples < total_samples) {
    int16_t* buffer = g_song_cache_samples + rendered_samples;
    const int32_t chunk_samples = min<int32_t>(app_config::AUDIO_BUFFER_SAMPLES, total_samples - rendered_samples);
    g_song_service->Moo(buffer, chunk_samples * sizeof(int16_t));
    rendered_samples += chunk_samples;

    const int percent = static_cast<int>((static_cast<int64_t>(rendered_samples) * 100) / total_samples);
    if (percent >= last_reported_percent + 10 || rendered_samples >= total_samples) {
      last_reported_percent = percent;
      set_status_now(String("Caching PCM ") + percent + "%");
    }
  }

  g_song_cached = true;
  g_song_total_samples = total_samples;
  g_song_current_sample = 0;
  return true;
}

bool prepare_cached_playback(int32_t start_sample) {
  if (!g_song_cache_samples || g_song_cache_bytes == 0) {
    return false;
  }

  g_song_total_samples = bytes_to_samples(g_song_cache_bytes);
  const int32_t clamped_sample = clamp_i32(start_sample, 0, g_song_total_samples);
  g_song_current_sample = clamped_sample;
  return true;
}

void finalize_song_playback() {
  stop_audio_output();
  g_playback_source = PlaybackSource::None;
  g_song_playing = false;
  g_song_paused = false;
  g_song_finished = true;
  g_song_resume_sample = 0;
  set_status("Playback finished");
}

bool start_song_playback(int32_t start_sample) {
  if (g_song_cached) {
    if (!prepare_cached_playback(start_sample)) {
      set_status("Failed to open memory cache");
      return false;
    }
    g_playback_source = PlaybackSource::MemoryCache;
  } else {
    if (!prepare_song_playback(start_sample)) {
      set_status("Failed to start playback");
      return false;
    }
    g_playback_source = PlaybackSource::Service;
    g_song_current_sample = start_sample;
  }

  stop_audio_output();
  g_song_playing = true;
  g_song_paused = false;
  g_song_finished = false;
  g_song_queue_draining = false;
  g_needs_redraw = true;
  g_last_player_redraw_ms = millis();
  draw_screen();
  service_song_audio();
  return true;
}

void pause_song_playback() {
  if (!g_song_playing) {
    return;
  }

  g_song_resume_sample = current_song_sample();
  stop_audio_output();
  g_playback_source = PlaybackSource::None;
  g_song_playing = false;
  g_song_paused = true;
  g_song_finished = false;
  set_status("Paused");
}

void service_song_audio() {
  if (!g_song_playing) {
    return;
  }

  if (g_playback_source == PlaybackSource::MemoryCache) {
    while (M5.Speaker.isPlaying(app_config::AUDIO_CHANNEL) < app_config::AUDIO_QUEUE_TARGET) {
      const int32_t remaining_samples = g_song_total_samples - g_song_current_sample;
      if (!g_song_cache_samples || remaining_samples <= 0) {
        g_song_queue_draining = true;
        break;
      }

      int16_t* buffer = g_audio_buffers[g_audio_buffer_index];
      const int32_t sample_count = min<int32_t>(app_config::AUDIO_BUFFER_SAMPLES, remaining_samples);
      memcpy(
          buffer,
          g_song_cache_samples + g_song_current_sample,
          static_cast<size_t>(sample_count) * sizeof(int16_t));
      const bool queued = M5.Speaker.playRaw(
          buffer,
          sample_count,
          app_config::AUDIO_SAMPLE_RATE,
          false,
          1,
          app_config::AUDIO_CHANNEL,
          !g_audio_started);

      if (!queued) {
        break;
      }

      g_audio_started = true;
      g_song_current_sample += sample_count;
      g_audio_buffer_index = (g_audio_buffer_index + 1) % app_config::AUDIO_BUFFER_COUNT;
      if (g_song_current_sample >= g_song_total_samples) {
        g_song_queue_draining = true;
        break;
      }
    }
  } else {
    if (!g_song_service) {
      return;
    }

    while (M5.Speaker.isPlaying(app_config::AUDIO_CHANNEL) < app_config::AUDIO_QUEUE_TARGET) {
      int16_t* buffer = g_audio_buffers[g_audio_buffer_index];
      const bool ok = g_song_service->Moo(buffer, sizeof(g_audio_buffers[0]));
      const bool queued = M5.Speaker.playRaw(
          buffer,
          app_config::AUDIO_BUFFER_SAMPLES,
          app_config::AUDIO_SAMPLE_RATE,
          false,
          1,
          app_config::AUDIO_CHANNEL,
          !g_audio_started);

      if (!queued) {
        break;
      }

      g_audio_started = true;
      g_audio_buffer_index = (g_audio_buffer_index + 1) % app_config::AUDIO_BUFFER_COUNT;
      if (!ok || g_song_service->moo_is_end_vomit()) {
        g_song_queue_draining = true;
        break;
      }
    }
  }

  if (g_song_queue_draining && M5.Speaker.isPlaying(app_config::AUDIO_CHANNEL) == 0) {
    finalize_song_playback();
  }
}

bool load_song(const String& path) {
  release_song();
  set_status_now("Opening " + fit_text(base_name(path), 18));

  g_song_file = SD.open(path.c_str(), FILE_READ);
  if (!g_song_file || g_song_file.isDirectory()) {
    close_file(g_song_file);
    set_status("Failed to open file");
    return false;
  }

  g_song_service = new pxtnService(pxtn_io_read, pxtn_io_write, pxtn_io_seek, pxtn_io_pos);
  if (!g_song_service) {
    close_file(g_song_file);
    set_status("No memory for player");
    return false;
  }

  pxtnERR error = g_song_service->init();
  if (error != pxtnOK) {
    set_status(format_pxtone_error("Init", error) + " " + format_memory_status());
    release_song();
    return false;
  }

  if (!g_song_service->set_destination_quality(1, app_config::AUDIO_SAMPLE_RATE)) {
    set_status("Unsupported audio quality");
    release_song();
    return false;
  }

  set_status_now("Loading song data");
  error = g_song_service->read(&g_song_file);
  if (error != pxtnOK) {
    set_status(format_pxtone_error("Load", error) + " " + format_memory_status());
    release_song();
    return false;
  }

  set_status_now("Preparing tones");
  error = g_song_service->tones_ready();
  if (error != pxtnOK) {
    set_status(format_pxtone_error("Tone", error) + " " + format_memory_status());
    release_song();
    return false;
  }

  close_file(g_song_file);

  g_song_path = path;
  int32_t title_size = 0;
  const char* title = g_song_service->text->get_name_buf(&title_size);
  if (title && title_size > 0) {
    g_song_title = String(title).substring(0, title_size);
  } else {
    g_song_title = base_name(path);
  }

  g_note_count = 0;
  g_note_lane_count = 0;
  g_song_notes_truncated = false;
  g_song_loaded = true;
  g_view_span_clock = app_config::DEFAULT_VIEW_SPAN;
  g_view_center_clock = 0;
  g_song_repeat_clock = g_song_service->master->get_repeat_meas() *
                        g_song_service->master->get_beat_num() *
                        g_song_service->master->get_beat_clock();
  g_song_has_loop = g_song_repeat_clock > 0;
  g_screen = AppScreen::Player;
  set_status_now(g_song_has_loop ? "Loop song: live playback" : "Caching PCM 0%");

  const bool cache_ready = g_song_has_loop ? false : build_song_cache();
  if (cache_ready && g_song_service) {
    delete g_song_service;
    g_song_service = nullptr;
  }

  if (!start_song_playback(0)) {
    release_song();
    return false;
  }

  set_status(String(cache_ready ? "Cached(mem) " : (g_song_has_loop ? "Loop/live " : "Loaded ")) + fit_text(base_name(path), 18));
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

void draw_player_touch_controls() {
  draw_touch_button(player_button_rect(0), "BACK", COLOR_PANEL_ALT, COLOR_TEXT_DIM);
  draw_touch_button(player_button_rect(1), g_song_playing ? "PAUSE" : "PLAY", COLOR_ACCENT_DIM, COLOR_TEXT);
  draw_touch_button(player_button_rect(2), "RESTART", COLOR_PANEL_ALT, COLOR_TEXT);
}

void draw_browser_screen() {
  draw_header("Pxtone Player");

  g_canvas.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  g_canvas.setCursor(4, 22);
  g_canvas.printf("SD:%s  %s", g_sd_ready ? "OK" : "NG", fit_text(g_browser_path, 24).c_str());

  const int body_y = 34;
  const int row_h = 11;
  const size_t visible_rows = browser_visible_rows();
  for (size_t row = 0; row < visible_rows; ++row) {
    const size_t index = g_browser_scroll_offset + row;
    const int y = body_y + static_cast<int>(row) * row_h;
    if (index >= g_browser_entry_count) {
      continue;
    }

    const bool selected = index == g_browser_selected_index;
    const bool playable = g_browser_entries[index].is_dir || g_browser_entries[index].is_playable;
    const uint16_t fill = selected ? COLOR_ACCENT_DIM : COLOR_PANEL_ALT;
    const uint16_t text = selected ? COLOR_TEXT : (playable ? COLOR_TEXT_DIM : COLOR_WARN);
    g_canvas.fillRoundRect(0, y - 1, app_config::SCREEN_W, 10, 3, fill);
    g_canvas.setTextColor(text, fill);
    g_canvas.setCursor(4, y + 1);

    String label = g_browser_entries[index].name;
    if (g_browser_entries[index].is_dir) {
      label = "[" + label + "]";
    } else if (!g_browser_entries[index].is_playable) {
      label += " *";
    }
    g_canvas.print(fit_text(label, app_config::SCREEN_W >= 320 ? 42 : 30));

    if (!g_browser_entries[index].is_dir && g_browser_entries[index].size_bytes > 0) {
      g_canvas.setCursor(app_config::SCREEN_W - 46, y + 1);
      g_canvas.printf("%4lu", static_cast<unsigned long>(g_browser_entries[index].size_bytes / 1024));
    }
  }

  if (g_browser_entry_count == 0) {
    g_canvas.setTextColor(COLOR_WARN, COLOR_BG);
    g_canvas.setCursor(4, 48);
    g_canvas.print(g_sd_ready ? "No files" : "Insert an SD card");
  }

  draw_footer("Touch row:select/open  buttons: up/down/open/back");
  draw_browser_touch_controls();
}

void draw_progress_bar(int32_t current_clock) {
  const int x = 4;
  const int y = 28;
  const int w = app_config::SCREEN_W - 8;
  const int h = 7;
  g_canvas.drawRoundRect(x, y, w, h, 3, COLOR_BORDER);
  if (g_song_end_clock > 0) {
    const int fill_w = clamp_i32((current_clock * (w - 2)) / g_song_end_clock, 0, w - 2);
    g_canvas.fillRoundRect(x + 1, y + 1, fill_w, h - 2, 2, COLOR_ACCENT);
  }
}

void draw_timeline_visualizer(int32_t current_clock) {
  const int area_x = 0;
  const int area_y = 40;
  const int area_w = app_config::SCREEN_W;
  const int area_h = 77;

  g_canvas.fillRoundRect(area_x, area_y, area_w, area_h, 4, COLOR_PANEL_ALT);
  g_canvas.drawRoundRect(area_x, area_y, area_w, area_h, 4, COLOR_BORDER);

  int32_t center_clock = g_song_playing && g_song_service ? current_clock : g_view_center_clock;
  center_clock = clamp_i32(center_clock, 0, g_song_end_clock > 0 ? g_song_end_clock : app_config::DEFAULT_VIEW_SPAN);
  g_view_center_clock = center_clock;
  int32_t start_clock = center_clock - g_view_span_clock / 2;
  if (start_clock < 0) {
    start_clock = 0;
  }
  int32_t end_clock = start_clock + g_view_span_clock;
  if (g_song_end_clock > 0 && end_clock > g_song_end_clock) {
    end_clock = g_song_end_clock;
    start_clock = end_clock - g_view_span_clock;
    if (start_clock < 0) {
      start_clock = 0;
    }
  }
  const int32_t visible_span = end_clock > start_clock ? end_clock - start_clock : 1;

  const uint8_t lane_count = g_note_lane_count == 0 ? 1 : (g_note_lane_count > 6 ? 6 : g_note_lane_count);
  const int lane_gap = 2;
  const int lane_h = (area_h - 10 - (lane_count - 1) * lane_gap) / lane_count;
  const int lane_top = area_y + 6;

  for (uint8_t lane = 0; lane < lane_count; ++lane) {
    const int y = lane_top + lane * (lane_h + lane_gap);
    g_canvas.fillRoundRect(area_x + 28, y, area_w - 32, lane_h, 2, COLOR_KEY_BLACK);
    g_canvas.drawRoundRect(area_x + 28, y, area_w - 32, lane_h, 2, COLOR_BORDER);
    g_canvas.setTextColor(COLOR_TEXT_DIM, COLOR_PANEL_ALT);
    g_canvas.setCursor(4, y + ((lane_h > 8) ? (lane_h - 8) / 2 : 0));
    g_canvas.printf("U%u", static_cast<unsigned>(lane + 1));
  }

  if (g_song_repeat_clock > 0) {
    const int repeat_x = area_x + 28 + ((g_song_repeat_clock - start_clock) * (area_w - 32)) / visible_span;
    if (repeat_x >= area_x + 28 && repeat_x < area_x + area_w - 4) {
      g_canvas.drawFastVLine(repeat_x, area_y + 4, area_h - 8, COLOR_LOOP);
    }
  }

  for (size_t i = 0; i < g_note_count; ++i) {
    const NoteSegment& note = g_notes[i];
    if (note.end_clock < start_clock || note.start_clock > end_clock) {
      continue;
    }

    const uint8_t lane = note.unit % lane_count;
    const int y0 = lane_top + lane * (lane_h + lane_gap) + 1;
    const int height = lane_h > 2 ? lane_h - 2 : 1;

    int x0 = area_x + 28 + ((note.start_clock - start_clock) * (area_w - 32)) / visible_span;
    int x1 = area_x + 28 + ((note.end_clock - start_clock) * (area_w - 32)) / visible_span;
    x0 = clamp_i32(x0, area_x + 29, area_x + area_w - 4);
    x1 = clamp_i32(x1, area_x + 29, area_x + area_w - 4);
    if (x1 <= x0) {
      x1 = x0 + 1;
    }

    const uint16_t color = kTrackPalette[note.unit % (sizeof(kTrackPalette) / sizeof(kTrackPalette[0]))];
    g_canvas.fillRect(x0, y0, x1 - x0, height, color);
  }

  const int playhead_x = area_x + 28 + ((current_clock - start_clock) * (area_w - 32)) / visible_span;
  if (playhead_x >= area_x + 28 && playhead_x < area_x + area_w - 4) {
    g_canvas.drawFastVLine(playhead_x, area_y + 4, area_h - 8, COLOR_PLAYHEAD);
  }
}

void draw_player_screen() {
  draw_header("Pxtone Player");

  const int32_t current_clock = current_song_clock();
  draw_progress_bar(current_clock);

  g_canvas.setTextColor(COLOR_TEXT, COLOR_BG);
  g_canvas.setCursor(4, 20);
  g_canvas.print(fit_text(g_song_title, 22));
  g_canvas.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  g_canvas.setCursor(146, 20);
  if (g_song_finished) {
    g_canvas.print("DONE");
  } else if (g_song_paused) {
    g_canvas.print("PAUSE");
  } else if (g_song_playing) {
    g_canvas.print("PLAY");
  } else {
    g_canvas.print("STOP");
  }

  g_canvas.setCursor(4, 31);
  g_canvas.printf("Clk %ld/%ld",
                  static_cast<long>(current_clock),
                  static_cast<long>(g_song_end_clock));
  g_canvas.setCursor(4, 48);
  g_canvas.printf("Repeat %ld", static_cast<long>(g_song_repeat_clock));
  g_canvas.setCursor(4, 64);
  g_canvas.printf("Rate %luHz", static_cast<unsigned long>(app_config::AUDIO_SAMPLE_RATE));
  g_canvas.setCursor(4, 80);
  g_canvas.printf("Mode %s", g_song_cached ? "Memory" : (g_song_has_loop ? "Live(loop)" : "Live"));
  draw_footer("Touch buttons: back / play-pause / restart");
  draw_player_touch_controls();
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
    draw_player_screen();
  }

  g_canvas.pushSprite(0, 0);
  g_needs_redraw = false;
}

void handle_browser_accept() {
  if (g_browser_entry_count == 0 || g_browser_selected_index >= g_browser_entry_count) {
    return;
  }

  const BrowserEntry& entry = g_browser_entries[g_browser_selected_index];
  if (entry.is_dir) {
    g_browser_path = entry.path;
    refresh_browser_directory();
    set_status("Open " + fit_text(entry.path, 18));
  } else if (!entry.is_playable) {
    set_status("Unsupported file type");
  } else {
    load_song(entry.path);
  }
}

void handle_player_accept() {
  if (!g_song_loaded) {
    return;
  }

  if (g_song_finished) {
    start_song_playback(0);
    set_status("Restarted");
    return;
  }

  if (g_song_playing) {
    pause_song_playback();
    return;
  }

  const bool was_paused = g_song_paused;
  if (start_song_playback(g_song_resume_sample)) {
    set_status(was_paused ? "Resumed" : "Playing");
  }
}

void return_to_browser() {
  release_song();
  g_screen = AppScreen::Browser;
  set_status("Closed player");
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
  if (y < body_y || y >= footer_button_bar_y()) {
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
          handle_browser_accept();
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
        handle_browser_accept();
        g_needs_redraw = true;
      } else if (browser_button_rect(3).contains(x, y) && !is_root_path(g_browser_path)) {
        g_browser_path = parent_path(g_browser_path);
        refresh_browser_directory();
        set_status("Back " + fit_text(g_browser_path, 18));
        g_needs_redraw = true;
      }
      return;
    }

    if (player_button_rect(0).contains(x, y)) {
      return_to_browser();
    } else if (player_button_rect(1).contains(x, y)) {
      handle_player_accept();
      g_needs_redraw = true;
    } else if (player_button_rect(2).contains(x, y)) {
      if (g_song_loaded && start_song_playback(0)) {
        set_status("Restarted");
      }
      g_needs_redraw = true;
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

  auto speaker_cfg = M5.Speaker.config();
  speaker_cfg.sample_rate = app_config::AUDIO_SAMPLE_RATE;
  speaker_cfg.dma_buf_len = 1024;
  speaker_cfg.dma_buf_count = 12;
  speaker_cfg.task_priority = 3;
  speaker_cfg.task_pinned_core = 0;
  M5.Speaker.config(speaker_cfg);
  M5.Speaker.begin();
  M5.Speaker.setVolume(app_config::AUDIO_VOLUME);

  if (init_sd_card()) {
    set_status("SD card ready");
    refresh_browser_directory();
  } else {
    set_status("Insert SD card");
  }
}

void loop() {
  M5.update();

  if (!g_sd_ready && millis() - g_last_sd_check_ms >= app_config::SD_RETRY_MS) {
    if (init_sd_card()) {
      set_status("SD card mounted");
      refresh_browser_directory();
    }
  }

  handle_touch_input();
  service_song_audio();

  const uint32_t redraw_interval =
      (g_song_has_loop && !g_song_cached) ? app_config::PLAYER_REDRAW_MS_LIVE : app_config::PLAYER_REDRAW_MS;
  if (g_song_playing && millis() - g_last_player_redraw_ms >= redraw_interval) {
    g_needs_redraw = true;
    g_last_player_redraw_ms = millis();
  }

  draw_screen();
  delay(8);
}
