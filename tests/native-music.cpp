#define main dashboard_app_main
#include "../kindle/native/src/kindle_dashboard.cpp"
#undef main
#include <assert.h>
#include <string>

int main() {
  int first = 0, seen = 0, fired = 0;
  assert(allowContactAction(kTouchOpenMusic, &first, &seen, &fired));
  assert(!allowContactAction(kTouchMusicPlaylist, &first, &seen, &fired));
  first = seen = fired = 0;
  assert(allowContactAction(kTouchMusicNext, &first, &seen, &fired));
  assert(!allowContactAction(kTouchMusicNext, &first, &seen, &fired));
  MusicSnapshot parsed;
  assert(!parseMusic("{\"ok\":false}", &parsed));
  assert(parseMusic("{\"ok\":true,\"status\":\"connected\",\"controllable\":true,\"playing\":true,\"title\":\"My song\",\"artist\":\"Artist\",\"device\":\"Mac\",\"playlists\":[{\"id\":\"1234567890123456789012\",\"title\":\"Morning\"},{\"id\":\"bad-id\",\"title\":\"Invalid\"}]}", &parsed));
  assert(parsed.playing && parsed.controllable && parsed.playlist_count == 1);
  assert(strcmp(parsed.title, "My song") == 0);
  g_music = parsed;
  copyText(g_music_url, sizeof(g_music_url), "https://example.invalid/music");
  Dashboard dashboard;
  assert(parseDashboard("{\"ok\":true,\"generated_at\":\"2026-11-15T00:00:00+05:30\",\"lists\":[]}", &dashboard));
  Options options = {};
  for (int width : {760, 1072}) {
    Canvas canvas = {width, width == 760 ? 1024 : 1448, nullptr};
    canvas.pixels = static_cast<unsigned char*>(calloc(canvas.width * canvas.height, 1));
    g_active_music = 0;
    g_music = parsed;
    drawCurrentDashboard(&canvas, &dashboard, "test");
    assert(applyTouchAt(width - 30, 300) && g_pending_action == kTouchOpenMusic);
    assert(handlePendingTouch(&options) == 1 && g_active_music && g_music_pending);
    assert(strcmp(g_music_action, "refresh") == 0); // Opening never starts playback.
    g_music_pending = false; g_music.busy = 0;
    for (int i = 1; i < kMaxMusicPlaylists; i++) g_music.playlists[i] = g_music.playlists[0];
    g_music.playlist_count = kMaxMusicPlaylists;
    drawCurrentDashboard(&canvas, &dashboard, "test");
    assert(g_music_page_count > 1);
    bool selected = false;
    for (int i = 0; i < g_touch_region_count; i++) {
      const auto region = g_touch_regions[i];
      if (region.action != kTouchMusicPlaylist) continue;
      assert(region.rect.y + region.rect.h <= canvas.height - 80);
      assert(applyTouchAt(region.rect.x + 20, region.rect.y + 20, false));
      assert(g_pending_action == kTouchNone);
      assert(applyTouchAt(region.rect.x + 20, region.rect.y + 20));
      assert(handlePendingTouch(&options) == 1 && g_music_pending);
      assert(strcmp(g_music_action, "playlist") == 0);
      assert(strcmp(g_music_playlist, parsed.playlists[0].id) == 0);
      assert(!queueMusicCommand("next")); // One command at a time.
      selected = true; break;
    }
    assert(selected);
    g_music_pending = false; g_music.busy = 0;
    assert(!queueMusicCommand("playlist", -1));
    assert(!queueMusicCommand("playlist", kMaxMusicPlaylists));
    assert(applyTouchAt(width - 100, canvas.height - 50));
    assert(g_pending_action == kTouchMusicPageNext);
    assert(handlePendingTouch(&options) == 1 && g_music_page == 1);
    g_music.controllable = 0;
    drawCurrentDashboard(&canvas, &dashboard, "test");
    for (int i = 0; i < g_touch_region_count; i++)
      assert(g_touch_regions[i].action < kTouchMusicPlaylist || g_touch_regions[i].action > kTouchMusicNext);
    assert(!queueMusicCommand("play"));
    g_music.controllable = 1; g_music.stale = 1;
    assert(!queueMusicCommand("next"));
    g_pending_action = kTouchBack;
    assert(handlePendingTouch(&options) == 1 && !g_active_music && !g_music_detail_open);
    g_music = parsed; g_active_music = 1;
    g_pending_action = kTouchHome;
    assert(handlePendingTouch(&options) == 1 && !g_active_music);
    free(canvas.pixels);
  }
  // Deterministic example previews use fixture playlists, never a real account.
  assert(chdir("../..") == 0);
  char* fixture = readFile("kindle/native/fixtures/dashboard-data.json");
  assert(fixture && parseDashboard(fixture, &dashboard)); free(fixture);
  copyText(dashboard.generated_at, sizeof(dashboard.generated_at), "2026-11-15T00:00:00+05:30");
  g_music = parsed;
  copyText(g_music.title, sizeof(g_music.title), "Sunday morning");
  copyText(g_music.artist, sizeof(g_music.artist), "A little music for your day");
  copyText(g_music.device, sizeof(g_music.device), "Mac / Bluetooth speaker");
  copyText(g_music.notice, sizeof(g_music.notice), "Tap a playlist to play on this device.");
  const char* names[] = {"Slow mornings", "A little focus", "Kitchen favourites", "Evening unwind", "Weekend mix", "On repeat"};
  g_music.playlist_count = 6;
  for (int i = 0; i < 6; i++) {
    copyText(g_music.playlists[i].id, sizeof(g_music.playlists[i].id), parsed.playlists[0].id);
    copyText(g_music.playlists[i].title, sizeof(g_music.playlists[i].title), names[i]);
    copyText(g_music.playlists[i].subtitle, sizeof(g_music.playlists[i].subtitle), "Example playlist");
  }
  for (int width : {760, 1072}) {
    Canvas canvas = {width, width == 760 ? 1024 : 1448, nullptr};
    canvas.pixels = static_cast<unsigned char*>(calloc(canvas.width * canvas.height, 1));
    char path[160];
    g_active_music = 0; drawCurrentDashboard(&canvas, &dashboard, "preview");
    snprintf(path, sizeof(path), "kindle/native/build/music-home-preview-%d.pgm", width);
    assert(writePgm(path, &canvas));
    g_active_music = 1; g_music_page = 0; drawCurrentDashboard(&canvas, &dashboard, "preview");
    snprintf(path, sizeof(path), "kindle/native/build/music-playlists-preview-%d.pgm", width);
    assert(writePgm(path, &canvas));
    free(canvas.pixels);
  }
}
