void drawSpotifyLogo(Canvas* canvas, int cx, int cy, int radius) {
  drawPgmImage(canvas, cx - radius, cy - radius, radius * 2, radius * 2,
    "/mnt/us/extensions/kindle-dashboard/assets/spotify-logo.pgm",
    "kindle/kual/kindle-dashboard/assets/spotify-logo.pgm", 0);
}

void drawMusicTile(Canvas* canvas, int x, int y, int w, int h) {
  MusicSnapshot music = musicSnapshot();
  strokeRect(canvas, x, y, w, h, 2, 0);
  addTouchRegion({x, y, w, h}, kTouchOpenMusic, -1, -1, "", 0);
  const bool compact = w < 276;
  const int header_h = compact ? 64 : 54;
  if (compact) {
    drawTextCentered(canvas, x + w / 2, y + 12, w - 24, "PLAY MUSIC", 2, 0);
    drawTextCentered(canvas, x + w / 2, y + 34, w - 24, "ON SPOTIFY", 2, 0);
  } else {
    drawTextCentered(canvas, x + w / 2, y + 20, w - 24, "PLAY MUSIC ON SPOTIFY", 2, 0);
  }
  line(canvas, x + 12, y + header_h, x + w - 12, y + header_h, 2, 0);
  const int radius = h >= 250 ? 31 : 23;
  drawSpotifyLogo(canvas, x + w / 2, y + header_h + 10 + radius, radius);
  const int text_y = y + header_h + 20 + radius * 2;
  drawTextCentered(canvas, x + w / 2, text_y, w - 28,
                   music.title[0] ? music.title : "A LITTLE MUSIC", 2, 0);
  drawTextCentered(canvas, x + w / 2, text_y + 24, w - 28,
                   music.stale ? "OFFLINE" : music.title[0] ? (music.playing ? "NOW PLAYING" : "PAUSED") : "YOUR PLAYLISTS", 2, 0);
  drawTextCentered(canvas, x + w / 2, y + h - 28, w - 20, "TAP TO CHOOSE", 2, 0);
}

void musicButton(Canvas* canvas, Rect rect, const char* text, TouchAction action, bool enabled) {
  const unsigned char ink = enabled ? 0 : 160;
  strokeRect(canvas, rect.x, rect.y, rect.w, rect.h, 2, ink);
  drawTextCentered(canvas, rect.x + rect.w / 2, rect.y + (rect.h - 16) / 2, rect.w - 16, text, 2, ink);
  addTouchRegion(rect, enabled ? action : kTouchNone, -1, -1, "", 0);
}

void drawMusicPage(Canvas* canvas, const Dashboard* dashboard, const char* status) {
  widgetPageShell(canvas, dashboard, status, "MUSIC / YOUR PLAYLISTS");
  const MusicSnapshot music = musicSnapshot();
  const int width = canvas->width;
  const int top = kKindleStatusBarHeight + 248;
  drawSpotifyLogo(canvas, 68, top + 44, 36);
  drawTextClipped(canvas, 124, top + 6, width - 156, music.title[0] ? music.title : "WHAT SHALL WE PLAY?", 3, 0);
  drawTextClipped(canvas, 124, top + 44, width - 156, music.artist[0] ? music.artist : "Choose a playlist below", 2, 0);
  char device[160];
  snprintf(device, sizeof(device), "%s%s", music.stale ? "OFFLINE / " : "", music.device[0] ? music.device : "No playback device connected");
  drawTextClipped(canvas, 32, top + 102, width - 64, device, 2, 0);
  const int gap = 10, button_width = (width - 64 - gap * 3) / 4;
  const bool controls = music.controllable && !music.busy && !music.stale;
  const TouchAction actions[] = {kTouchMusicPrevious, music.playing ? kTouchMusicPause : kTouchMusicPlay, kTouchMusicNext, kTouchMusicRefresh};
  const char* labels[] = {"PREVIOUS", music.playing ? "PAUSE" : "PLAY", "NEXT", music.busy ? "WAIT..." : "REFRESH"};
  for (int i = 0; i < 4; i++) musicButton(canvas, {32 + i * (button_width + gap), top + 138, button_width, 58}, labels[i], actions[i], i == 3 ? !music.busy : controls);
  drawTextWrapped(canvas, 32, top + 216, width - 64, music.notice, 2, 0, 2);
  const int list_y = top + 284;
  drawText(canvas, 32, list_y - 30, "PLAYLISTS", 3, 0);
  int rows = (canvas->height - 100 - list_y) / 96;
  if (rows < 1) rows = 1;
  g_music_page_count = music.playlist_count ? (music.playlist_count + rows - 1) / rows : 1;
  if (g_music_page >= g_music_page_count) g_music_page = g_music_page_count - 1;
  if (!music.playlist_count) {
    drawTextWrapped(canvas, 32, list_y + 14, width - 64,
      strcmp(music.status, "not_connected") == 0 ? "Connect Spotify once to load your playlists here." : "No playlists available. Refresh to try again.", 3, 0, 3);
  }
  for (int i = g_music_page * rows; i < music.playlist_count && i < (g_music_page + 1) * rows; i++) {
    const MusicPlaylist& playlist = music.playlists[i];
    Rect rect = {24, list_y + (i - g_music_page * rows) * 96, width - 48, 84};
    strokeRect(canvas, rect.x, rect.y, rect.w, rect.h, 2, 0);
    drawTextClipped(canvas, 44, rect.y + 16, width - 150, playlist.title, 3, 0);
    drawTextClipped(canvas, 44, rect.y + 52, width - 150, playlist.subtitle, 2, 0);
    const unsigned char arrow_ink = controls ? 0 : 160;
    line(canvas, width - 76, rect.y + 30, width - 64, rect.y + 42, 3, arrow_ink);
    line(canvas, width - 64, rect.y + 42, width - 76, rect.y + 54, 3, arrow_ink);
    addTouchRegion(rect, controls ? kTouchMusicPlaylist : kTouchNone, -1, i, "", 0);
  }
  drawWidgetPager(canvas, g_music_page, g_music_page_count, kTouchMusicPagePrevious, kTouchMusicPageNext);
}
