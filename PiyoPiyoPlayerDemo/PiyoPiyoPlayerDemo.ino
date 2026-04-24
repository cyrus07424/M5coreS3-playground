#include <Arduino.h>
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>
#include <esp32-hal-psram.h>
#include <math.h>

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

constexpr uint8_t AUDIO_CHANNEL = 0;
constexpr uint32_t AUDIO_SAMPLE_RATE = 22050;
constexpr size_t AUDIO_BUFFER_COUNT = 6;
constexpr size_t AUDIO_BUFFER_SAMPLES = 2048;
constexpr size_t AUDIO_QUEUE_TARGET = 2;
constexpr uint8_t AUDIO_VOLUME = 180;
constexpr uint32_t PLAYER_REDRAW_MS = 66;
constexpr uint32_t AUDIO_STOP_TIMEOUT_MS = 250;

constexpr size_t PIYO_TRACK_COUNT = 4;
constexpr size_t PIYO_MELODY_TRACKS = 3;
constexpr size_t PIYO_NOTES_PER_TRACK = 24;
constexpr size_t PIYO_MAX_RECORDS = 4096;
constexpr size_t PIYO_MAX_TOTAL_SAMPLES = AUDIO_SAMPLE_RATE * 60UL * 6UL;
}  // namespace app_config

enum class AppScreen : uint8_t {
  Browser = 0,
  Player,
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

struct PiyoTrack {
  uint8_t octave = 0;
  uint8_t icon = 0;
  uint32_t length = 0;
  uint32_t volume = 0;
  int8_t waveform[0x100] = {};
  uint8_t envelope[0x40] = {};
  uint32_t* records = nullptr;
};

struct PiyoSong {
  bool loaded = false;
  uint32_t wait_ms = 0;
  int32_t repeat_tick = 0;
  int32_t end_tick = 0;
  int32_t records = 0;
  uint32_t percussion_volume = 0;
  PiyoTrack tracks[app_config::PIYO_TRACK_COUNT];
};

struct MelodyVoice {
  float remaining = 0.0f;
  float phase = 0.0f;
};

struct DrumVoice {
  float remaining = 0.0f;
  float elapsed = 0.0f;
  uint32_t noise = 0;
};

struct PiyoRendererState {
  int32_t record_index = 0;
  int32_t samples_until_tick = 0;
  bool finished = false;
  MelodyVoice melody[app_config::PIYO_MELODY_TRACKS][app_config::PIYO_NOTES_PER_TRACK];
  DrumVoice drums[app_config::PIYO_NOTES_PER_TRACK];
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

static const int kFreqTable[12] = {1551, 1652, 1747, 1848, 1955, 2074, 2205, 2324, 2461, 2616, 2770, 2938};

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

static PiyoSong g_song;
static String g_song_path;
static String g_song_title;
static bool g_song_loaded = false;
static bool g_song_playing = false;
static bool g_song_paused = false;
static bool g_song_finished = false;
static bool g_song_queue_draining = false;
static bool g_audio_started = false;
static size_t g_audio_buffer_index = 0;
static int16_t g_audio_buffers[app_config::AUDIO_BUFFER_COUNT][app_config::AUDIO_BUFFER_SAMPLES];
static int16_t* g_song_cache_samples = nullptr;
static size_t g_song_cache_bytes = 0;
static int32_t g_song_total_samples = 0;
static int32_t g_song_current_sample = 0;
static int32_t g_song_resume_sample = 0;
static int32_t g_song_samples_per_tick = 1;
static int32_t g_song_end_tick = 0;
static int32_t g_song_repeat_tick = 0;
static uint32_t g_song_wait_ms = 0;
static bool g_song_loop_hint = false;

void set_status(const String& message) {
  g_status_message = message;
  g_needs_redraw = true;
}

void set_status_now(const String& message) {
  set_status(message);
  g_canvas.setTextFont(1);
  g_canvas.setTextSize(1);
  g_needs_redraw = true;
  if (g_screen == AppScreen::Browser || g_screen == AppScreen::Player) {
    g_canvas.fillScreen(COLOR_BG);
  }
}

void close_file(File& file) {
  if (file) {
    file.close();
  }
}

float clampf(float value, float min_value, float max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
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

bool is_piyopiyo_file(const String& name) {
  String lower = name;
  lower.toLowerCase();
  return lower.endsWith(".pmd");
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
    String name = base_name(String(entry.name()));
    BrowserEntry browser_entry;
    browser_entry.name = name;
    browser_entry.path = child_path(g_browser_path, name);
    browser_entry.is_dir = entry.isDirectory();
    browser_entry.is_playable = !browser_entry.is_dir && is_piyopiyo_file(name);
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

bool read_exact(File& file, void* dst, size_t size) {
  return file.read(static_cast<uint8_t*>(dst), size) == static_cast<int>(size);
}

bool read_u8(File& file, uint8_t* out_value) {
  return read_exact(file, out_value, sizeof(*out_value));
}

bool read_u32le(File& file, uint32_t* out_value) {
  uint8_t buffer[4];
  if (!read_exact(file, buffer, sizeof(buffer))) {
    return false;
  }
  *out_value = static_cast<uint32_t>(buffer[0]) |
               (static_cast<uint32_t>(buffer[1]) << 8) |
               (static_cast<uint32_t>(buffer[2]) << 16) |
               (static_cast<uint32_t>(buffer[3]) << 24);
  return true;
}

bool read_i32le(File& file, int32_t* out_value) {
  uint32_t raw = 0;
  if (!read_u32le(file, &raw)) {
    return false;
  }
  *out_value = static_cast<int32_t>(raw);
  return true;
}

float volume_to_gain(uint32_t volume) {
  const int scaled = (static_cast<int>(volume) - 300) * 8;
  const int clipped = scaled < -10000 ? -10000 : (scaled > 0 ? 0 : scaled);
  return powf(10.0f, static_cast<float>(clipped) / 2000.0f);
}

uint32_t drum_length_samples(size_t note_index) {
  if (note_index < 2) {
    return 22050 / 8;
  }
  if (note_index < 4) {
    return 22050 / 10;
  }
  if (note_index < 8) {
    return 22050 / 6;
  }
  if (note_index < 10) {
    return 22050 / 12;
  }
  if (note_index < 12) {
    return 22050 / 18;
  }
  return 22050 / 2;
}

float drum_sample(DrumVoice& voice, size_t note_index, float base_gain) {
  if (voice.remaining <= 0.0f) {
    return 0.0f;
  }

  const float total = static_cast<float>(drum_length_samples(note_index));
  const float progress = total > 0.0f ? voice.elapsed / total : 1.0f;
  const float env = clampf(1.0f - progress, 0.0f, 1.0f);
  voice.noise = voice.noise * 1664525UL + 1013904223UL;
  const float noise = (static_cast<int32_t>((voice.noise >> 16) & 0x7FFF) - 16384.0f) / 16384.0f;
  const float t = voice.elapsed / static_cast<float>(app_config::AUDIO_SAMPLE_RATE);
  float sample = 0.0f;

  if (note_index < 2) {
    const float freq = 68.0f - progress * 28.0f;
    sample = (sinf(2.0f * PI * freq * t) * 0.85f + noise * 0.12f) * env * env;
  } else if (note_index < 4) {
    const float freq = 92.0f - progress * 34.0f;
    sample = (sinf(2.0f * PI * freq * t) * 0.7f + noise * 0.18f) * env * env;
  } else if (note_index < 8) {
    sample = (noise * 0.85f + sinf(2.0f * PI * 180.0f * t) * 0.15f) * env * env;
  } else if (note_index < 10) {
    sample = noise * env * env * env * 0.6f;
  } else if (note_index < 12) {
    sample = noise * env * env * env * 0.45f;
  } else {
    sample = noise * env * env * 0.55f;
  }

  voice.remaining -= 1.0f;
  voice.elapsed += 1.0f;
  const float accent = (note_index & 1) ? 0.7f : 1.0f;
  return sample * base_gain * accent * 12000.0f;
}

void clear_song_cache() {
  if (g_song_cache_samples) {
    free(g_song_cache_samples);
    g_song_cache_samples = nullptr;
  }
  g_song_cache_bytes = 0;
  g_song_total_samples = 0;
  g_song_current_sample = 0;
  g_song_resume_sample = 0;
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

void clear_song_records() {
  for (size_t i = 0; i < app_config::PIYO_TRACK_COUNT; ++i) {
    if (g_song.tracks[i].records) {
      free(g_song.tracks[i].records);
      g_song.tracks[i].records = nullptr;
    }
  }
}

void release_song() {
  stop_audio_output();
  clear_song_cache();
  clear_song_records();
  g_song = PiyoSong{};
  g_song_path = "";
  g_song_title = "";
  g_song_loaded = false;
  g_song_playing = false;
  g_song_paused = false;
  g_song_finished = false;
  g_song_wait_ms = 0;
  g_song_end_tick = 0;
  g_song_repeat_tick = 0;
  g_song_loop_hint = false;
}

bool has_active_voices(const PiyoRendererState& state) {
  for (size_t track = 0; track < app_config::PIYO_MELODY_TRACKS; ++track) {
    for (size_t note = 0; note < app_config::PIYO_NOTES_PER_TRACK; ++note) {
      if (state.melody[track][note].remaining > 0.0f) {
        return true;
      }
    }
  }
  for (size_t note = 0; note < app_config::PIYO_NOTES_PER_TRACK; ++note) {
    if (state.drums[note].remaining > 0.0f) {
      return true;
    }
  }
  return false;
}

void advance_song_tick(PiyoRendererState& state) {
  if (state.record_index >= g_song.end_tick) {
    state.finished = true;
    return;
  }

  for (size_t track = 0; track < app_config::PIYO_MELODY_TRACKS; ++track) {
    const uint32_t record = g_song.tracks[track].records[state.record_index];
    for (size_t note = 0; note < app_config::PIYO_NOTES_PER_TRACK; ++note) {
      if (record & (1UL << note)) {
        state.melody[track][note].remaining = static_cast<float>(g_song.tracks[track].length);
        state.melody[track][note].phase = 0.0f;
      }
    }
  }

  const uint32_t drum_record = g_song.tracks[3].records[state.record_index];
  for (size_t note = 0; note < app_config::PIYO_NOTES_PER_TRACK; ++note) {
    if (drum_record & (1UL << note)) {
      state.drums[note].remaining = static_cast<float>(drum_length_samples(note));
      state.drums[note].elapsed = 0.0f;
      state.drums[note].noise = 0x1234ABCDUL + static_cast<uint32_t>(state.record_index * 131 + note * 29);
    }
  }

  ++state.record_index;
}

float render_song_sample(PiyoRendererState& state) {
  float mixed = 0.0f;

  for (size_t track = 0; track < app_config::PIYO_MELODY_TRACKS; ++track) {
    const float track_gain = volume_to_gain(g_song.tracks[track].volume);
    const int octave_shift = 1 << g_song.tracks[track].octave;
    const float note_length = static_cast<float>(g_song.tracks[track].length);

    for (size_t note = 0; note < app_config::PIYO_NOTES_PER_TRACK; ++note) {
      MelodyVoice& voice = state.melody[track][note];
      if (voice.remaining <= 0.0f || note_length <= 0.0f) {
        continue;
      }

      const int elapsed = static_cast<int>(note_length - voice.remaining);
      const int env_index = clamp_i32((elapsed * 64) / static_cast<int>(note_length), 0, 63);
      const int env = static_cast<int>(g_song.tracks[track].envelope[env_index]) * 2;
      const int freq = note < 12 ? kFreqTable[note] / 16 : kFreqTable[note - 12] / 8;
      voice.phase += static_cast<float>(octave_shift * freq);
      const float table_phase = voice.phase / 256.0f;
      const int phase_i = static_cast<int>(table_phase);
      const float phase_f = table_phase - phase_i;

      const int8_t sample0 = g_song.tracks[track].waveform[phase_i & 0xFF];
      const int8_t sample1 = g_song.tracks[track].waveform[(phase_i + octave_shift) & 0xFF];
      const float wave_sample = static_cast<float>(sample0) + phase_f * static_cast<float>(sample1 - sample0);
      mixed += wave_sample * static_cast<float>(env) * track_gain;

      voice.remaining -= 1.0f;
    }
  }

  const float drum_gain = volume_to_gain(g_song.percussion_volume);
  for (size_t note = 0; note < app_config::PIYO_NOTES_PER_TRACK; ++note) {
    mixed += drum_sample(state.drums[note], note, drum_gain);
  }

  return clampf(mixed, -32768.0f, 32767.0f);
}

int32_t estimate_total_samples() {
  const int32_t samples_per_tick = static_cast<int32_t>((static_cast<uint64_t>(app_config::AUDIO_SAMPLE_RATE) * g_song.wait_ms) / 1000ULL);
  g_song_samples_per_tick = samples_per_tick > 0 ? samples_per_tick : 1;

  uint32_t max_tail = 0;
  for (size_t i = 0; i < app_config::PIYO_MELODY_TRACKS; ++i) {
    if (g_song.tracks[i].length > max_tail) {
      max_tail = g_song.tracks[i].length;
    }
  }
  const uint32_t cymbal_tail = drum_length_samples(12);
  if (cymbal_tail > max_tail) {
    max_tail = cymbal_tail;
  }

  const int32_t main_samples = g_song.end_tick * g_song_samples_per_tick;
  return main_samples > 0 ? main_samples + static_cast<int32_t>(max_tail) : static_cast<int32_t>(max_tail);
}

bool build_song_cache() {
  clear_song_cache();

  const int32_t total_samples = estimate_total_samples();
  if (total_samples <= 0) {
    return false;
  }
  if (static_cast<size_t>(total_samples) > app_config::PIYO_MAX_TOTAL_SAMPLES) {
    set_status("Song too long for cache");
    return false;
  }

  const size_t cache_bytes = static_cast<size_t>(total_samples) * sizeof(int16_t);
  void* cache = nullptr;
  if (psramFound()) {
    cache = ps_malloc(cache_bytes);
  }
  if (!cache) {
    cache = malloc(cache_bytes);
  }
  if (!cache) {
    set_status("Not enough memory");
    return false;
  }

  g_song_cache_samples = static_cast<int16_t*>(cache);
  g_song_cache_bytes = cache_bytes;
  g_song_total_samples = total_samples;
  g_song_current_sample = 0;
  g_song_resume_sample = 0;

  PiyoRendererState state = {};
  state.samples_until_tick = 0;
  const int report_step = total_samples / 10 > 0 ? total_samples / 10 : 1;
  int next_report = report_step;

  for (int32_t i = 0; i < total_samples; ++i) {
    if (!state.finished && state.samples_until_tick <= 0) {
      advance_song_tick(state);
      state.samples_until_tick = g_song_samples_per_tick;
    }

    float sample = 0.0f;
    if (!state.finished || has_active_voices(state)) {
      sample = render_song_sample(state);
    }
    g_song_cache_samples[i] = static_cast<int16_t>(sample);

    if (state.samples_until_tick > 0) {
      --state.samples_until_tick;
    }

    if (i + 1 >= next_report || i + 1 == total_samples) {
      const int percent = static_cast<int>((static_cast<int64_t>(i + 1) * 100) / total_samples);
      set_status(String("Rendering PCM ") + percent + "%");
      next_report += report_step;
    }
  }

  return true;
}

bool load_song(const String& path) {
  release_song();

  File file = SD.open(path.c_str(), FILE_READ);
  if (!file) {
    set_status("Failed to open file");
    return false;
  }

  char magic[3] = {};
  if (!read_exact(file, magic, sizeof(magic)) || strncmp(magic, "PMD", 3) != 0) {
    close_file(file);
    set_status("Invalid PMD header");
    return false;
  }

  uint8_t writable = 0;
  uint32_t track_ptr = 0;
  if (!read_u8(file, &writable) ||
      !read_u32le(file, &track_ptr) ||
      !read_u32le(file, &g_song.wait_ms) ||
      !read_i32le(file, &g_song.repeat_tick) ||
      !read_i32le(file, &g_song.end_tick) ||
      !read_i32le(file, &g_song.records)) {
    close_file(file);
    set_status("Failed to read song header");
    return false;
  }

  if (g_song.records <= 0 || g_song.records > static_cast<int32_t>(app_config::PIYO_MAX_RECORDS)) {
    close_file(file);
    set_status("Unsupported record count");
    return false;
  }

  if (g_song.end_tick <= 0 || g_song.end_tick > g_song.records) {
    g_song.end_tick = g_song.records;
  }
  if (g_song.repeat_tick < 0 || g_song.repeat_tick >= g_song.end_tick) {
    g_song.repeat_tick = 0;
  }

  for (size_t i = 0; i < app_config::PIYO_MELODY_TRACKS; ++i) {
    uint8_t align[2] = {};
    uint32_t unused = 0;
    uint8_t wave_bytes[0x100] = {};

    if (!read_u8(file, &g_song.tracks[i].octave) ||
        !read_u8(file, &g_song.tracks[i].icon) ||
        !read_exact(file, align, sizeof(align)) ||
        !read_u32le(file, &g_song.tracks[i].length) ||
        !read_u32le(file, &g_song.tracks[i].volume) ||
        !read_u32le(file, &unused) ||
        !read_u32le(file, &unused) ||
        !read_exact(file, wave_bytes, sizeof(wave_bytes)) ||
        !read_exact(file, g_song.tracks[i].envelope, sizeof(g_song.tracks[i].envelope))) {
      close_file(file);
      clear_song_records();
      set_status("Failed to read track data");
      return false;
    }

    for (size_t j = 0; j < sizeof(wave_bytes); ++j) {
      g_song.tracks[i].waveform[j] = static_cast<int8_t>(wave_bytes[j]);
    }
  }

  if (!read_u32le(file, &g_song.percussion_volume)) {
    close_file(file);
    clear_song_records();
    set_status("Failed to read drum volume");
    return false;
  }

  for (size_t track = 0; track < app_config::PIYO_TRACK_COUNT; ++track) {
    const size_t bytes = static_cast<size_t>(g_song.records) * sizeof(uint32_t);
    void* records = nullptr;
    if (psramFound()) {
      records = ps_malloc(bytes);
    }
    if (!records) {
      records = malloc(bytes);
    }
    if (!records) {
      close_file(file);
      clear_song_records();
      set_status("Failed to allocate records");
      return false;
    }
    g_song.tracks[track].records = static_cast<uint32_t*>(records);
    if (!read_exact(file, g_song.tracks[track].records, bytes)) {
      close_file(file);
      clear_song_records();
      set_status("Failed to read note records");
      return false;
    }
  }

  close_file(file);

  g_song.loaded = true;
  g_song_path = path;
  g_song_title = base_name(path);
  g_song_loaded = true;
  g_song_wait_ms = g_song.wait_ms;
  g_song_end_tick = g_song.end_tick;
  g_song_repeat_tick = g_song.repeat_tick;
  g_song_loop_hint = g_song.repeat_tick > 0 && g_song.repeat_tick < g_song.end_tick;

  if (!build_song_cache()) {
    release_song();
    return false;
  }

  g_song_finished = false;
  g_song_paused = false;
  g_screen = AppScreen::Player;
  set_status(g_song_loop_hint ? "Loaded (one-shot cache)" : "Loaded");
  return true;
}

int32_t current_song_tick() {
  if (g_song_samples_per_tick <= 0) {
    return 0;
  }
  const int32_t tick = g_song_current_sample / g_song_samples_per_tick;
  return clamp_i32(tick, 0, g_song_end_tick);
}

void finalize_song_playback() {
  stop_audio_output();
  g_song_playing = false;
  g_song_paused = false;
  g_song_finished = true;
  g_song_resume_sample = 0;
  g_song_current_sample = g_song_total_samples;
  set_status("Playback finished");
}

bool prepare_cached_playback(int32_t start_sample) {
  if (!g_song_cache_samples || g_song_total_samples <= 0) {
    return false;
  }
  g_song_current_sample = clamp_i32(start_sample, 0, g_song_total_samples);
  return true;
}

void service_song_audio();

bool start_song_playback(int32_t start_sample) {
  if (!prepare_cached_playback(start_sample)) {
    set_status("No cached audio");
    return false;
  }

  stop_audio_output();
  g_song_playing = true;
  g_song_paused = false;
  g_song_finished = false;
  g_song_queue_draining = false;
  g_needs_redraw = true;
  g_last_player_redraw_ms = millis();
  service_song_audio();
  return true;
}

void pause_song_playback() {
  if (!g_song_playing) {
    return;
  }
  g_song_resume_sample = g_song_current_sample;
  stop_audio_output();
  g_song_playing = false;
  g_song_paused = true;
  set_status("Paused");
}

void service_song_audio() {
  if (!g_song_playing) {
    return;
  }

  while (M5.Speaker.isPlaying(app_config::AUDIO_CHANNEL) < app_config::AUDIO_QUEUE_TARGET) {
    const int32_t remaining_samples = g_song_total_samples - g_song_current_sample;
    if (!g_song_cache_samples || remaining_samples <= 0) {
      g_song_queue_draining = true;
      break;
    }

    int16_t* buffer = g_audio_buffers[g_audio_buffer_index];
    const int32_t sample_count = min<int32_t>(app_config::AUDIO_BUFFER_SAMPLES, remaining_samples);
    memcpy(buffer,
           g_song_cache_samples + g_song_current_sample,
           static_cast<size_t>(sample_count) * sizeof(int16_t));

    const bool queued = M5.Speaker.playRaw(buffer,
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

  if (g_song_queue_draining && M5.Speaker.isPlaying(app_config::AUDIO_CHANNEL) == 0) {
    finalize_song_playback();
  }
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

void draw_progress_bar(int32_t current_tick) {
  const int x = 4;
  const int y = 28;
  const int w = app_config::SCREEN_W - 8;
  const int h = 7;
  g_canvas.drawRoundRect(x, y, w, h, 3, COLOR_BORDER);
  if (g_song_end_tick > 0) {
    const int fill_w = clamp_i32((current_tick * (w - 2)) / g_song_end_tick, 0, w - 2);
    g_canvas.fillRoundRect(x + 1, y + 1, fill_w, h - 2, 2, COLOR_ACCENT);
  }
}

void draw_browser_screen() {
  draw_header("PiyoPiyo Player");

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

void draw_player_screen() {
  draw_header("PiyoPiyo Player");

  const int32_t current_tick = current_song_tick();
  draw_progress_bar(current_tick);

  g_canvas.setTextColor(COLOR_TEXT, COLOR_BG);
  g_canvas.setCursor(4, 20);
  g_canvas.print(fit_text(g_song_title, 22));
  g_canvas.setTextColor(COLOR_TEXT_DIM, COLOR_BG);
  g_canvas.setCursor(170, 20);
  if (g_song_finished) {
    g_canvas.print("DONE");
  } else if (g_song_paused) {
    g_canvas.print("PAUSE");
  } else if (g_song_playing) {
    g_canvas.print("PLAY");
  } else {
    g_canvas.print("STOP");
  }

  g_canvas.setCursor(4, 42);
  g_canvas.printf("Tick %ld/%ld", static_cast<long>(current_tick), static_cast<long>(g_song_end_tick));
  g_canvas.setCursor(4, 58);
  g_canvas.printf("Wait %lums  Rate %luHz",
                  static_cast<unsigned long>(g_song_wait_ms),
                  static_cast<unsigned long>(app_config::AUDIO_SAMPLE_RATE));
  g_canvas.setCursor(4, 74);
  const String loop_text = g_song_loop_hint ? String(g_song_repeat_tick) : String("disabled in cache");
  g_canvas.printf("Loop %s", loop_text.c_str());
  g_canvas.setCursor(4, 90);
  const String pcm_text = format_size(g_song_cache_bytes);
  g_canvas.printf("PCM %s", pcm_text.c_str());
  g_canvas.setCursor(4, 106);
  g_canvas.printf("Track vol %lu/%lu/%lu D%lu",
                  static_cast<unsigned long>(g_song.tracks[0].volume),
                  static_cast<unsigned long>(g_song.tracks[1].volume),
                  static_cast<unsigned long>(g_song.tracks[2].volume),
                  static_cast<unsigned long>(g_song.percussion_volume));
  g_canvas.setTextColor(g_song_loop_hint ? COLOR_WARN : COLOR_OK, COLOR_BG);
  g_canvas.setCursor(4, 126);
  g_canvas.print(g_song_loop_hint ? "Loop songs are rendered as one-shot playback." : "Ready");

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
  } else if (load_song(entry.path)) {
    start_song_playback(0);
  }
}

void handle_player_accept() {
  if (!g_song_loaded) {
    return;
  }

  if (g_song_finished) {
    if (start_song_playback(0)) {
      set_status("Restarted");
    }
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
  if (x < 0 || x >= app_config::SCREEN_W || index >= g_browser_entry_count) {
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

  if (g_song_playing && millis() - g_last_player_redraw_ms >= app_config::PLAYER_REDRAW_MS) {
    g_needs_redraw = true;
    g_last_player_redraw_ms = millis();
  }

  draw_screen();
  delay(8);
}
