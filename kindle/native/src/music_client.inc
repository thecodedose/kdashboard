bool parseMusic(const char* json, MusicSnapshot* music) {
  if (!json || !extractBool(json, NULL, "ok", 0)) return false;
  *music = MusicSnapshot{};
  extractString(json, NULL, "status", music->status, sizeof(music->status), "unavailable");
  extractString(json, NULL, "title", music->title, sizeof(music->title), "");
  extractString(json, NULL, "artist", music->artist, sizeof(music->artist), "");
  extractString(json, NULL, "device", music->device, sizeof(music->device), "");
  extractString(json, NULL, "notice", music->notice, sizeof(music->notice), "");
  music->playing = extractBool(json, NULL, "playing", 0);
  music->controllable = extractBool(json, NULL, "controllable", 0);
  music->retry_after = extractInt(json, NULL, "retry_after", 0);
  if (music->retry_after < 0) music->retry_after = 0;
  if (music->retry_after > 3600) music->retry_after = 3600;
  const char* list = findKeyInRange(json, NULL, "playlists");
  const char* end = list && *list == '[' ? matchingClose(list, ']') : NULL;
  if (!end) return true;
  const char* cursor = list + 1;
  while (cursor < end && music->playlist_count < kMaxMusicPlaylists) {
    const char* item = strchr(cursor, '{');
    if (!item || item >= end) break;
    const char* item_end = matchingClose(item, '}');
    if (!item_end || item_end > end) return false;
    MusicPlaylist playlist = {};
    extractString(item, item_end, "id", playlist.id, sizeof(playlist.id), "");
    extractString(item, item_end, "title", playlist.title, sizeof(playlist.title), "Untitled playlist");
    extractString(item, item_end, "subtitle", playlist.subtitle, sizeof(playlist.subtitle), "");
    bool valid = strlen(playlist.id) == 22;
    for (const char* p = playlist.id; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9'))) valid = false;
    if (valid) music->playlists[music->playlist_count++] = playlist;
    cursor = item_end + 1;
  }
  return true;
}

bool requestMusic(const char* action, const char* playlist, MusicSnapshot* result) {
  char url[1100], header[400], quoted_header[850], body[160], quoted_body[350], command[2600];
  shellQuote(g_music_url, url, sizeof(url));
  snprintf(header, sizeof(header), "X-Dashboard-Toggle-Token: %s", g_music_token);
  shellQuote(header, quoted_header, sizeof(quoted_header));
  snprintf(body, sizeof(body), "{\"action\":\"%s\",\"playlist_id\":\"%s\"}", action, playlist);
  shellQuote(body, quoted_body, sizeof(quoted_body));
  const bool mutation = strcmp(action, "refresh") != 0;
  snprintf(command, sizeof(command),
    "curl -fsS --connect-timeout 5 --max-time 40 --max-filesize 65536 -H %s %s %s %s %s 2>/dev/null",
    quoted_header, mutation ? "-X POST -H 'Content-Type: application/json' --data" : "",
    mutation ? quoted_body : "", "--", url);
  FILE* pipe = popen(command, "r");
  if (!pipe) return false;
  char payload[65537];
  size_t count = fread(payload, 1, sizeof(payload) - 1, pipe);
  const int overflow = count == sizeof(payload) - 1 ? fgetc(pipe) : EOF;
  int status = pclose(pipe);
  payload[count] = '\0';
  return status == 0 && overflow == EOF && parseMusic(payload, result);
}

void* musicWorkerMain(void*) {
  time_t next_fetch = 0;
  while (g_running) {
    char action[24] = "refresh", playlist[64] = "";
    pthread_mutex_lock(&g_music_mutex);
    MusicSnapshot previous = g_music;
    bool requested = g_music_pending;
    bool fetch = g_music_pending || time(NULL) >= next_fetch;
    if (g_music_pending) {
      copyText(action, sizeof(action), g_music_action);
      copyText(playlist, sizeof(playlist), g_music_playlist);
      g_music_pending = false;
    }
    if (fetch) g_music.busy = 1;
    bool detail = g_music_detail_open;
    pthread_mutex_unlock(&g_music_mutex);
    if (!fetch) { sleep(1); continue; }
    MusicSnapshot result;
    bool ok = requestMusic(action, playlist, &result);
    pthread_mutex_lock(&g_music_mutex);
    if (!ok) {
      result = g_music;
      result.stale = 1;
      copyText(result.notice, sizeof(result.notice), "Music unavailable. Check Wi-Fi, then refresh.");
    }
    result.busy = 0;
    previous.busy = 0;
    if (requested || memcmp(&previous, &result, sizeof(result)) != 0) g_music_repaint = true;
    g_music = result;
    next_fetch = time(NULL) + (strcmp(result.status, "not_connected") == 0 ? 300 : detail ? 15 : 120);
    if (result.retry_after > 0) next_fetch = time(NULL) + result.retry_after;
    pthread_mutex_unlock(&g_music_mutex);
  }
  return NULL;
}

pthread_t g_music_thread;
bool g_music_thread_started = false;
void startMusicWorker(const Options* options) {
  const char* configured = getenv("DASHBOARD_MUSIC_URL");
  if (configured && configured[0]) copyText(g_music_url, sizeof(g_music_url), configured);
  else {
    copyText(g_music_url, sizeof(g_music_url), options->url);
    char* route = strrchr(g_music_url, '/');
    if (route && strcmp(route, "/kindle-dashboard-data") == 0)
      copyText(route, sizeof(g_music_url) - (route - g_music_url), "/kindle-dashboard-music");
    else g_music_url[0] = '\0';
  }
  copyText(g_music_token, sizeof(g_music_token), options->toggle_token);
  if (!g_music_url[0] || !g_music_token[0] || !commandExists("curl")) return;
  g_music_thread_started = pthread_create(&g_music_thread, NULL, musicWorkerMain, NULL) == 0;
}
