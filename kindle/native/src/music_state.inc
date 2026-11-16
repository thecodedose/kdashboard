// The Kindle receives a small, credential-free snapshot from the music endpoint.
const int kMaxMusicPlaylists = 12;
struct MusicPlaylist { char id[64]; char title[160]; char subtitle[160]; };
struct MusicSnapshot {
  char status[32] = "not_connected";
  char title[200] = "";
  char artist[200] = "";
  char device[120] = "";
  char notice[200] = "Connect Spotify to choose your playlists.";
  int playing = 0;
  int controllable = 0;
  int busy = 0;
  int stale = 0;
  int retry_after = 0;
  int playlist_count = 0;
  MusicPlaylist playlists[kMaxMusicPlaylists] = {};
};
pthread_mutex_t g_music_mutex = PTHREAD_MUTEX_INITIALIZER;
MusicSnapshot g_music;
int g_active_music = 0;
int g_music_page = 0;
int g_music_page_count = 1;
bool g_music_detail_open = false;
bool g_music_repaint = false;
bool g_music_pending = false;
char g_music_action[24] = "";
char g_music_playlist[64] = "";
char g_music_url[512] = "";
char g_music_token[160] = "";

MusicSnapshot musicSnapshot() {
  pthread_mutex_lock(&g_music_mutex);
  MusicSnapshot result = g_music;
  pthread_mutex_unlock(&g_music_mutex);
  return result;
}
void musicDetailOpen(bool open) {
  pthread_mutex_lock(&g_music_mutex);
  g_music_detail_open = open;
  pthread_mutex_unlock(&g_music_mutex);
}
bool takeMusicRepaint() {
  pthread_mutex_lock(&g_music_mutex);
  bool repaint = g_music_repaint;
  g_music_repaint = false;
  pthread_mutex_unlock(&g_music_mutex);
  return repaint;
}
bool queueMusicCommand(const char* action, int playlist_index = -1) {
  pthread_mutex_lock(&g_music_mutex);
  bool refresh = strcmp(action, "refresh") == 0;
  bool valid = refresh || (g_music.controllable && !g_music.stale);
  if (strcmp(action, "playlist") == 0)
    valid = valid && playlist_index >= 0 && playlist_index < g_music.playlist_count;
  if (!g_music_url[0] || g_music.busy || !valid) {
    pthread_mutex_unlock(&g_music_mutex);
    return false;
  }
  snprintf(g_music_action, sizeof(g_music_action), "%s", action);
  snprintf(g_music_playlist, sizeof(g_music_playlist), "%s",
           playlist_index >= 0 && playlist_index < g_music.playlist_count ? g_music.playlists[playlist_index].id : "");
  g_music.busy = 1;
  g_music_pending = true;
  pthread_mutex_unlock(&g_music_mutex);
  return true;
}
