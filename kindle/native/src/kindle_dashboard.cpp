#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#ifdef __linux__
#include <linux/input.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

namespace {

const char* kDefaultUrl = "";
const char* kDefaultEventsUrl = "";
const char* kDefaultToggleUrl = "";
const char* kDefaultCache = "/mnt/us/documents/kindle-dashboard-data.json";
const char* kMealCoverPath = "/mnt/us/extensions/kindle-dashboard/assets/meal-planner-cover.pgm";
const char* kMealCoverLocalPath = "kindle/kual/kindle-dashboard/assets/meal-planner-cover.pgm";
const char* kChallengeCoverPath = "/mnt/us/extensions/kindle-dashboard/assets/challenge-75-day.pgm";
const char* kChallengeCoverLocalPath = "kindle/kual/kindle-dashboard/assets/challenge-75-day.pgm";
const char* kProfileCardPath = "/mnt/us/extensions/kindle-dashboard/assets/profile-placeholder.pgm";
const char* kProfileCardLocalPath = "kindle/kual/kindle-dashboard/assets/profile-placeholder.pgm";
const char* kRecipeAssetsPath = "/mnt/us/extensions/kindle-dashboard/assets/recipes";
const char* kRecipeAssetsLocalPath = "kindle/kual/kindle-dashboard/assets/recipes";
const int kDefaultIntervalSeconds = 3600;
const char* kDefaultSleepWindow = "off";
const long kMaxDashboardPayloadBytes = 512 * 1024;
const int kScreenColumns = 40;
const int kMaxRows = 28;
const int kCardInnerWidth = 36;
const int kMaxLists = 4;
const int kMaxItems = 16;
const int kMaxRecipes = 12;
const int kBitmapFallbackWidth = 760;
const int kBitmapFallbackHeight = 1024;
const int kKindleStatusBarHeight = 66;

volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_event_refresh = 0;
volatile sig_atomic_t g_manual_fetch_refresh = 0;
int g_last_screen_width = kBitmapFallbackWidth;
int g_last_screen_height = kBitmapFallbackHeight;
int g_active_list = -1;
int g_active_meal_planner = 0;
int g_active_recipes = 0;
int g_active_recipe = -1;
int g_active_recipe_library = 0;
int g_active_recipe_return_meal_planner = 0;
int g_active_challenge = 0;
int g_invert_images = 0;
int g_day_offset = 0;

enum TouchAction {
  kTouchNone = 0,
  kTouchExit = 1,
  kTouchBack = 2,
  kTouchOpenList = 3,
  kTouchToggleItem = 4,
  kTouchOpenMealPlanner = 5,
  kTouchOpenRecipe = 6,
  kTouchOpenRecipes = 7,
  kTouchHome = 8,
  kTouchOpenChallenge = 9,
  kTouchOpenMealPlanRecipe = 10,
  kTouchPreviousDay = 11,
  kTouchNextDay = 12,
  kTouchToday = 13
};

struct Item {
  char id[48];
  char text[96];
  int done;
};

struct List {
  char key[24];
  char title[40];
  Item items[kMaxItems];
  int item_count;
};

const int kMaxRecipeIngredients = 8;

struct RecipeIngredient {
  const char* name;
  const char* amount;
};

struct RecipeIngredientRecord {
  char name[64];
  char amount[32];
};

struct RecipeRecord {
  char id[48];
  char title[64];
  char instructions[160];
  RecipeIngredientRecord ingredients[kMaxRecipeIngredients];
  int ingredient_count;
  int calories;
  int carbs;
  int fat;
  int protein;
  int rating_tenths;
};

struct Dashboard {
  char generated_at[40];
  char version[32];
  int steps;
  int calories;
  int protein_g;
  int challenge_day;
  int water_tenths;
  int water_target_tenths;
  int sleep_tenths;
  int sleep_target_tenths;
  int workouts;
  int workout_target;
  int steps_target;
  int calories_target;
  char steps_unit[16];
  char calories_unit[16];
  List lists[kMaxLists];
  int list_count;
  RecipeRecord recipes[kMaxRecipes];
  int recipe_count;
  int meal_plan_recipe_indices[kMaxRecipes];
  int meal_plan_count;
};

struct Options {
  char url[256];
  char events_url[256];
  char toggle_url[256];
  char read_token[160];
  char toggle_token[160];
  char cache[256];
  char render_only[256];
  char view[32];
  char dump_pgm[256];
  char save_pgm[256];
  int dump_width;
  int dump_height;
  int interval;
  int sleep_start_minute;
  int sleep_end_minute;
  int once;
  int invert_images;
};

struct Canvas {
  int width;
  int height;
  unsigned char* pixels;
};

struct Rect {
  int x;
  int y;
  int w;
  int h;
};

struct CachedPgm {
  char primary_path[192];
  char fallback_path[192];
  unsigned char* pixels;
  int width;
  int height;
};

struct TouchRegion {
  Rect rect;
  TouchAction action;
  int list_index;
  int item_index;
  char item_id[48];
  int item_done;
};

struct MealPlanEntry {
  const char* meal;
  const char* title;
  const char* time;
  const char* recipe;
  const char* photo_path;
  const char* photo_fallback_path;
  RecipeIngredient ingredients[kMaxRecipeIngredients];
  int ingredient_count;
  const char* steps;
  int calories;
  int carbs;
  int fat;
  int protein;
};

const MealPlanEntry kMealPlan[] = {
  {
    "BREAKFAST",
    "SAVORY OATS",
    "8:30 AM",
    "OATS + EGG + GREENS",
    kMealCoverPath,
    kMealCoverLocalPath,
    {
      {"OATS", "1/2 CUP"},
      {"EGG", "1"},
      {"SPINACH", "1 CUP"},
      {"LEMON", "1 WEDGE"}
    },
    4,
    "SIMMER OATS. FOLD GREENS. TOP WITH EGG.",
    410,
    48,
    14,
    22
  },
  {
    "LUNCH",
    "CHICKPEA WRAP",
    "1:00 PM",
    "CHICKPEA + PESTO WRAP",
    kMealCoverPath,
    kMealCoverLocalPath,
    {
      {"CHICKPEAS", "3/4 CUP"},
      {"PESTO", "1 TBSP"},
      {"TORTILLA", "1 LARGE"},
      {"CUCUMBER", "1/2 CUP"}
    },
    4,
    "MASH CHICKPEAS. SPREAD PESTO. ROLL TIGHT.",
    520,
    62,
    18,
    24
  },
  {
    "DINNER",
    "PIZZA TOAST",
    "7:30 PM",
    "MELTY PIZZA TOAST",
    kMealCoverPath,
    kMealCoverLocalPath,
    {
      {"BREAD", "2 SLICES"},
      {"SAUCE", "1/4 CUP"},
      {"MOZZARELLA", "2 OZ"},
      {"BASIL", "6 LEAVES"}
    },
    4,
    "SAUCE BREAD. ADD TOPPINGS. TOAST UNTIL MELTY.",
    610,
    58,
    26,
    31
  }
};

const int kMealPlanCount = static_cast<int>(sizeof(kMealPlan) / sizeof(kMealPlan[0]));

const int kMaxTouchRegions = 32;
TouchRegion g_touch_regions[kMaxTouchRegions];
int g_touch_region_count = 0;
TouchAction g_pending_action = kTouchNone;
int g_pending_list_index = -1;
int g_pending_recipe_index = -1;
char g_pending_item_id[48];
int g_pending_item_done = 0;
int g_pending_touch_x = -1;
int g_pending_touch_y = -1;
Rect g_pending_touch_rect = {0, 0, 0, 0};
int g_pending_touch_rect_valid = 0;
CachedPgm g_pgm_cache[32];
int g_pgm_cache_count = 0;

long long monotonicMs() {
  timeval tv;
  gettimeofday(&tv, NULL);
  return static_cast<long long>(tv.tv_sec) * 1000LL + static_cast<long long>(tv.tv_usec / 1000);
}

void copyText(char* dest, size_t size, const char* source) {
  if (size == 0) return;
  if (!source) source = "";
  snprintf(dest, size, "%s", source);
}

char* readFile(const char* path) {
  FILE* file = fopen(path, "rb");
  if (!file) return NULL;
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long size = ftell(file);
  if (size < 0 || size > kMaxDashboardPayloadBytes) {
    fclose(file);
    return NULL;
  }
  rewind(file);
  char* data = static_cast<char*>(calloc(static_cast<size_t>(size) + 1, 1));
  if (!data) {
    fclose(file);
    return NULL;
  }
  if (fread(data, 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  return data;
}

const char* skipWhitespace(const char* cursor) {
  while (cursor && *cursor && isspace(static_cast<unsigned char>(*cursor))) cursor++;
  return cursor;
}

const char* findKeyInRange(const char* start, const char* end, const char* key) {
  char pattern[80];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const size_t pattern_len = strlen(pattern);
  const char* cursor = start;
  while (cursor && (!end || cursor + pattern_len <= end)) {
    cursor = strstr(cursor, pattern);
    if (!cursor || (end && cursor + pattern_len > end)) return NULL;
    const char* colon = skipWhitespace(cursor + pattern_len);
    if (*colon == ':') return skipWhitespace(colon + 1);
    cursor += pattern_len;
  }
  return NULL;
}

int parseJsonString(const char* cursor, char* out, size_t out_size, const char** after) {
  cursor = skipWhitespace(cursor);
  if (!cursor || *cursor != '"') return 0;
  cursor++;
  size_t length = 0;
  while (*cursor && *cursor != '"') {
    char ch = *cursor++;
    if (ch == '\\') {
      ch = *cursor++;
      if (ch == 'n') ch = ' ';
      else if (ch == 'r') ch = ' ';
      else if (ch == 't') ch = ' ';
      else if (ch == 'u') {
        ch = '?';
        for (int i = 0; i < 4 && *cursor; i++) cursor++;
      }
    }
    if (length + 1 < out_size) out[length++] = ch;
  }
  if (*cursor != '"') return 0;
  if (out_size > 0) out[length] = '\0';
  if (after) *after = cursor + 1;
  return 1;
}

void jsonEscapeString(const char* input, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  if (!input) input = "";
  size_t written = 0;
  for (const char* cursor = input; *cursor && written + 1 < out_size; cursor++) {
    const unsigned char ch = static_cast<unsigned char>(*cursor);
    const char* replacement = NULL;
    if (ch == '\\') replacement = "\\\\";
    else if (ch == '"') replacement = "\\\"";
    else if (ch == '\n') replacement = "\\n";
    else if (ch == '\r') replacement = "\\r";
    else if (ch == '\t') replacement = "\\t";

    if (replacement) {
      for (const char* r = replacement; *r && written + 1 < out_size; r++) out[written++] = *r;
    } else if (ch >= 0x20) {
      out[written++] = static_cast<char>(ch);
    }
  }
  out[written] = '\0';
}

int extractString(const char* start, const char* end, const char* key, char* out, size_t out_size, const char* fallback) {
  const char* value = findKeyInRange(start, end, key);
  if (value && strncmp(value, "null", 4) == 0) value = NULL;
  if (value && parseJsonString(value, out, out_size, NULL)) return 1;
  copyText(out, out_size, fallback);
  return 0;
}

int extractInt(const char* start, const char* end, const char* key, int fallback) {
  const char* value = findKeyInRange(start, end, key);
  if (!value) return fallback;
  return static_cast<int>(strtol(value, NULL, 10));
}

int extractScaledInt(const char* start, const char* end, const char* key, int scale, int fallback) {
  const char* value = findKeyInRange(start, end, key);
  if (!value) return fallback;
  const double parsed = strtod(value, NULL);
  return static_cast<int>(parsed * scale + (parsed >= 0 ? 0.5 : -0.5));
}

int extractBool(const char* start, const char* end, const char* key, int fallback) {
  const char* value = findKeyInRange(start, end, key);
  if (!value) return fallback;
  if (strncmp(value, "true", 4) == 0) return 1;
  if (strncmp(value, "false", 5) == 0) return 0;
  return fallback;
}

const char* matchingClose(const char* open, char close_char) {
  const char open_char = *open;
  int depth = 0;
  int in_string = 0;
  int escaped = 0;
  for (const char* cursor = open; *cursor; cursor++) {
    const char ch = *cursor;
    if (in_string) {
      if (escaped) escaped = 0;
      else if (ch == '\\') escaped = 1;
      else if (ch == '"') in_string = 0;
      continue;
    }
    if (ch == '"') {
      in_string = 1;
      continue;
    }
    if (ch == open_char) depth++;
    else if (ch == close_char) {
      depth--;
      if (depth == 0) return cursor;
    }
  }
  return NULL;
}

int parseItems(const char* list_start, const char* list_end, List* list) {
  const char* items_value = findKeyInRange(list_start, list_end, "items");
  if (!items_value || *items_value != '[') return 0;
  const char* items_end = matchingClose(items_value, ']');
  if (!items_end || items_end > list_end) return 0;

  const char* cursor = items_value + 1;
  while (cursor < items_end && list->item_count < kMaxItems) {
    const char* object_start = strchr(cursor, '{');
    if (!object_start || object_start >= items_end) break;
    const char* object_end = matchingClose(object_start, '}');
    if (!object_end || object_end > items_end) break;

    Item* item = &list->items[list->item_count];
    extractString(object_start, object_end, "id", item->id, sizeof(item->id), "");
    extractString(object_start, object_end, "text", item->text, sizeof(item->text), "");
    item->done = extractBool(object_start, object_end, "done", 0);
    if (item->text[0]) list->item_count++;
    cursor = object_end + 1;
  }
  return 1;
}

int parseRecipeIngredients(const char* recipe_start, const char* recipe_end, RecipeRecord* recipe) {
  const char* ingredients_value = findKeyInRange(recipe_start, recipe_end, "ingredients");
  if (!ingredients_value || *ingredients_value != '[') return 0;
  const char* ingredients_end = matchingClose(ingredients_value, ']');
  if (!ingredients_end || ingredients_end > recipe_end) return 0;

  const char* cursor = ingredients_value + 1;
  while (cursor < ingredients_end && recipe->ingredient_count < kMaxRecipeIngredients) {
    const char* object_start = strchr(cursor, '{');
    if (!object_start || object_start >= ingredients_end) break;
    const char* object_end = matchingClose(object_start, '}');
    if (!object_end || object_end > ingredients_end) break;

    RecipeIngredientRecord* ingredient = &recipe->ingredients[recipe->ingredient_count];
    extractString(object_start, object_end, "name", ingredient->name, sizeof(ingredient->name), "");
    extractString(object_start, object_end, "amount", ingredient->amount, sizeof(ingredient->amount), "");
    if (ingredient->name[0] || ingredient->amount[0]) recipe->ingredient_count++;
    cursor = object_end + 1;
  }
  return 1;
}

int parseRecipes(const char* json, Dashboard* dashboard) {
  const char* recipes_value = findKeyInRange(json, NULL, "recipes");
  if (!recipes_value || *recipes_value != '[') return 0;
  const char* recipes_end = matchingClose(recipes_value, ']');
  if (!recipes_end) return 0;

  const char* cursor = recipes_value + 1;
  while (cursor < recipes_end && dashboard->recipe_count < kMaxRecipes) {
    const char* object_start = strchr(cursor, '{');
    if (!object_start || object_start >= recipes_end) break;
    const char* object_end = matchingClose(object_start, '}');
    if (!object_end || object_end > recipes_end) break;

    RecipeRecord* recipe = &dashboard->recipes[dashboard->recipe_count];
    extractString(object_start, object_end, "id", recipe->id, sizeof(recipe->id), "");
    extractString(object_start, object_end, "title", recipe->title, sizeof(recipe->title), "");
    extractString(object_start, object_end, "instructions", recipe->instructions, sizeof(recipe->instructions), "");
    recipe->calories = extractInt(object_start, object_end, "total_calories", 0);
    recipe->carbs = extractInt(object_start, object_end, "carbs_g", 0);
    recipe->fat = extractInt(object_start, object_end, "fat_g", 0);
    recipe->protein = extractInt(object_start, object_end, "protein_g", 0);
    recipe->rating_tenths = extractScaledInt(object_start, object_end, "rating", 10, 0);
    parseRecipeIngredients(object_start, object_end, recipe);
    if (recipe->title[0]) dashboard->recipe_count++;
    cursor = object_end + 1;
  }
  return 1;
}

int recipeIndexById(const Dashboard* dashboard, const char* id) {
  if (!dashboard || !id || !id[0]) return -1;
  for (int i = 0; i < dashboard->recipe_count; i++) {
    if (strcmp(dashboard->recipes[i].id, id) == 0) return i;
  }
  return -1;
}

int parseMealPlan(const char* json, Dashboard* dashboard) {
  const char* meal_plan_value = findKeyInRange(json, NULL, "meal_plan");
  if (!meal_plan_value || *meal_plan_value != '[') return 0;
  const char* meal_plan_end = matchingClose(meal_plan_value, ']');
  if (!meal_plan_end) return 0;

  const char* cursor = meal_plan_value + 1;
  while (cursor < meal_plan_end && dashboard->meal_plan_count < kMaxRecipes) {
    const char* object_start = strchr(cursor, '{');
    if (!object_start || object_start >= meal_plan_end) break;
    const char* object_end = matchingClose(object_start, '}');
    if (!object_end || object_end > meal_plan_end) break;

    char id[48];
    extractString(object_start, object_end, "id", id, sizeof(id), "");
    const int recipe_index = recipeIndexById(dashboard, id);
    if (recipe_index >= 0) {
      dashboard->meal_plan_recipe_indices[dashboard->meal_plan_count++] = recipe_index;
    }
    cursor = object_end + 1;
  }
  return 1;
}

int parseDashboard(const char* json, Dashboard* dashboard) {
  memset(dashboard, 0, sizeof(*dashboard));
  copyText(dashboard->steps_unit, sizeof(dashboard->steps_unit), "steps");
  copyText(dashboard->calories_unit, sizeof(dashboard->calories_unit), "kcal");

  if (!json || !extractBool(json, NULL, "ok", 0)) return 0;
  extractString(json, NULL, "generated_at", dashboard->generated_at, sizeof(dashboard->generated_at), "unknown");
  extractString(json, NULL, "version", dashboard->version, sizeof(dashboard->version), "");
  dashboard->steps = extractInt(json, NULL, "steps", 0);
  dashboard->calories = extractInt(json, NULL, "calories", 0);
  dashboard->protein_g = extractInt(json, NULL, "protein_g", 0);
  dashboard->challenge_day = extractInt(json, NULL, "day", 1);
  if (dashboard->challenge_day < 1) dashboard->challenge_day = 1;
  if (dashboard->challenge_day > 75) dashboard->challenge_day = 75;
  dashboard->water_tenths = extractScaledInt(json, NULL, "water_l", 10, 0);
  dashboard->water_target_tenths = extractScaledInt(json, NULL, "water_target_l", 10, 30);
  dashboard->sleep_tenths = extractScaledInt(json, NULL, "sleep_hours", 10, 0);
  dashboard->sleep_target_tenths = extractScaledInt(json, NULL, "sleep_target_hours", 10, 80);
  dashboard->workouts = extractInt(json, NULL, "workouts", 0);
  dashboard->workout_target = extractInt(json, NULL, "workout_target", 2);
  dashboard->steps_target = extractInt(json, NULL, "steps_target", 10000);
  dashboard->calories_target = extractInt(json, NULL, "calories_target", 2000);
  extractString(json, NULL, "steps_unit", dashboard->steps_unit, sizeof(dashboard->steps_unit), "steps");
  extractString(json, NULL, "calories_unit", dashboard->calories_unit, sizeof(dashboard->calories_unit), "kcal");

  const char* lists_value = findKeyInRange(json, NULL, "lists");
  if (!lists_value || *lists_value != '[') return 1;
  const char* lists_end = matchingClose(lists_value, ']');
  if (!lists_end) return 1;

  const char* cursor = lists_value + 1;
  while (cursor < lists_end && dashboard->list_count < kMaxLists) {
    const char* object_start = strchr(cursor, '{');
    if (!object_start || object_start >= lists_end) break;
    const char* object_end = matchingClose(object_start, '}');
    if (!object_end || object_end > lists_end) break;

    List* list = &dashboard->lists[dashboard->list_count];
    extractString(object_start, object_end, "key", list->key, sizeof(list->key), "");
    extractString(object_start, object_end, "title", list->title, sizeof(list->title), list->key);
    parseItems(object_start, object_end, list);
    if (list->key[0] || list->title[0]) dashboard->list_count++;
    cursor = object_end + 1;
  }
  parseRecipes(json, dashboard);
  parseMealPlan(json, dashboard);
  return 1;
}

void freeDashboard(Dashboard* dashboard) {
  (void)dashboard;
}

void fit(char* line) {
  size_t length = strlen(line);
  for (size_t i = 0; i < length; i++) {
    if (line[i] == '\n' || line[i] == '\r' || line[i] == '\t') line[i] = ' ';
  }
  if (length > static_cast<size_t>(kScreenColumns)) {
    line[kScreenColumns - 3] = '.';
    line[kScreenColumns - 2] = '.';
    line[kScreenColumns - 1] = '.';
    line[kScreenColumns] = '\0';
  }
}

void upperCopy(char* dest, size_t size, const char* source) {
  copyText(dest, size, source);
  for (size_t i = 0; dest[i]; i++) dest[i] = static_cast<char>(toupper(static_cast<unsigned char>(dest[i])));
}

void formatNumber(int value, char* out, size_t size) {
  char raw[32];
  snprintf(raw, sizeof(raw), "%d", value);
  const int len = static_cast<int>(strlen(raw));
  int commas = (len - 1) / 3;
  int out_len = len + commas;
  if (out_len + 1 > static_cast<int>(size)) {
    copyText(out, size, raw);
    return;
  }
  out[out_len] = '\0';
  int group = 0;
  for (int i = len - 1, j = out_len - 1; i >= 0; i--, j--) {
    if (group == 3) {
      out[j--] = ',';
      group = 0;
    }
    out[j] = raw[i];
    group++;
  }
}

void formatDisplayDate(const char* iso, const char* status, char* out, size_t size) {
  static const char* months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  static const char* weekdays[] = {"SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY"};
  int year = 0;
  int month = 0;
  int day = 0;
  if (!iso || sscanf(iso, "%4d-%2d-%2d", &year, &month, &day) != 3 || month < 1 || month > 12 || day < 1 || day > 31) {
    snprintf(out, size, "UPDATED UNKNOWN // %s", status ? status : "UNKNOWN");
    return;
  }

  int y = year;
  int m = month;
  if (m < 3) {
    m += 12;
    y--;
  }
  const int k = y % 100;
  const int j = y / 100;
  const int h = (day + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
  const int weekday = (h + 6) % 7;
  snprintf(out, size, "%s, %s %d // %s", weekdays[weekday], months[month - 1], day, status ? status : "UNKNOWN");
}

void addLine(char lines[][96], int* count, const char* text) {
  if (*count >= kMaxRows) return;
  copyText(lines[*count], 96, text);
  fit(lines[*count]);
  (*count)++;
}

void addRule(char lines[][96], int* count) {
  addLine(lines, count, "+--------------------------------------+");
}

void addCardText(char lines[][96], int* count, const char* text) {
  char line[96];
  char clipped[64];
  copyText(clipped, sizeof(clipped), text);
  clipped[kCardInnerWidth] = '\0';
  snprintf(line, sizeof(line), "| %-36s |", clipped);
  addLine(lines, count, line);
}

void addCardPair(char lines[][96], int* count, const char* left, const char* right) {
  char left_clipped[32];
  char right_clipped[24];
  copyText(left_clipped, sizeof(left_clipped), left);
  copyText(right_clipped, sizeof(right_clipped), right);
  left_clipped[27] = '\0';
  right_clipped[8] = '\0';
  char line[96];
  snprintf(line, sizeof(line), "| %-27.27s %8.8s |", left_clipped, right_clipped);
  addLine(lines, count, line);
}

void addSectionTitle(char lines[][96], int* count, const char* title) {
  char upper[48];
  upperCopy(upper, sizeof(upper), title);
  char text[64];
  snprintf(text, sizeof(text), " %s", upper);
  addCardText(lines, count, text);
}

void addMetric(char lines[][96], int* count, const char* label, int value, int target, const char* unit) {
  const int percent = target > 0 ? (value * 100) / target : 0;
  const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  const int filled = (clamped * 14) / 100;
  char bar[20];
  for (int i = 0; i < 14; i++) bar[i] = i < filled ? '#' : '.';
  bar[14] = '\0';

  char value_text[32];
  char target_text[32];
  formatNumber(value, value_text, sizeof(value_text));
  formatNumber(target, target_text, sizeof(target_text));
  char left[40];
  char right[24];
  snprintf(left, sizeof(left), "%s %s/%s %s", label, value_text, target_text, unit);
  snprintf(right, sizeof(right), "%d%%", clamped);
  addCardPair(lines, count, left, right);

  char progress[64];
  snprintf(progress, sizeof(progress), " [%s]", bar);
  addCardText(lines, count, progress);
}

const char* displayListTitle(const List* list);
const char* displayListTitleForIndex(const List* list, int list_index);

void addList(char lines[][96], int* count, const List* list) {
  char title[48];
  copyText(title, sizeof(title), displayListTitle(list));
  addSectionTitle(lines, count, title);

  if (list->item_count == 0) {
    addCardText(lines, count, " [ ] Empty");
    addRule(lines, count);
    return;
  }

  const int shown = list->item_count > 4 ? 4 : list->item_count;
  for (int i = 0; i < shown; i++) {
    char line[96];
    snprintf(line, sizeof(line), " %s %.30s", list->items[i].done ? "[x]" : "[ ]", list->items[i].text);
    addCardText(lines, count, line);
  }
  if (list->item_count > shown) {
    char more[48];
    snprintf(more, sizeof(more), " ... +%d more", list->item_count - shown);
    addCardText(lines, count, more);
  }
  addRule(lines, count);
}

int renderLines(const Dashboard* dashboard, const char* status, char lines[][96]) {
  int count = 0;
  addRule(lines, &count);
  addCardText(lines, &count, " KINDLE DASHBOARD");
  char sync[64];
  snprintf(sync, sizeof(sync), " Sync %.16s", dashboard->generated_at[0] ? dashboard->generated_at : "unknown");
  addCardText(lines, &count, sync);
  char mode[64];
  snprintf(mode, sizeof(mode), " Mode %s | refresh 15m", status);
  addCardText(lines, &count, mode);
  addRule(lines, &count);
  addSectionTitle(lines, &count, "Health");
  addMetric(lines, &count, "STEPS", dashboard->steps, dashboard->steps_target, dashboard->steps_unit);
  addMetric(lines, &count, "CAL", dashboard->calories, dashboard->calories_target, dashboard->calories_unit);
  addRule(lines, &count);
  for (int i = 0; i < dashboard->list_count; i++) addList(lines, &count, &dashboard->lists[i]);
  addCardText(lines, &count, " Telegram updates lists");
  addRule(lines, &count);
  return count;
}

void clearCanvas(Canvas* canvas, unsigned char color) {
  if (!canvas || !canvas->pixels) return;
  memset(canvas->pixels, color, static_cast<size_t>(canvas->width) * static_cast<size_t>(canvas->height));
}

void setPixel(Canvas* canvas, int x, int y, unsigned char color) {
  if (!canvas || !canvas->pixels) return;
  if (x < 0 || y < 0 || x >= canvas->width || y >= canvas->height) return;
  canvas->pixels[y * canvas->width + x] = color;
}

void fillRect(Canvas* canvas, int x, int y, int w, int h, unsigned char color) {
  for (int yy = y; yy < y + h; yy++) {
    if (yy < 0 || yy >= canvas->height) continue;
    for (int xx = x; xx < x + w; xx++) setPixel(canvas, xx, yy, color);
  }
}

void strokeRect(Canvas* canvas, int x, int y, int w, int h, int thickness, unsigned char color) {
  fillRect(canvas, x, y, w, thickness, color);
  fillRect(canvas, x, y + h - thickness, w, thickness, color);
  fillRect(canvas, x, y, thickness, h, color);
  fillRect(canvas, x + w - thickness, y, thickness, h, color);
}

void doubleRect(Canvas* canvas, int x, int y, int w, int h, unsigned char color) {
  strokeRect(canvas, x, y, w, h, 3, color);
  strokeRect(canvas, x + 7, y + 7, w - 14, h - 14, 2, color);
}

void line(Canvas* canvas, int x0, int y0, int x1, int y1, int thickness, unsigned char color) {
  const int dx = abs(x1 - x0);
  const int sx = x0 < x1 ? 1 : -1;
  const int dy = -abs(y1 - y0);
  const int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (1) {
    fillRect(canvas, x0 - thickness / 2, y0 - thickness / 2, thickness, thickness, color);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void circleRing(Canvas* canvas, int cx, int cy, int radius, int thickness, int percent, unsigned char color) {
  const int outer = radius;
  const int inner = radius - thickness;
  const int outer2 = outer * outer;
  const int inner2 = inner * inner;
  const double progress = percent < 0 ? 0.0 : (percent > 100 ? 1.0 : percent / 100.0);
  for (int y = cy - outer; y <= cy + outer; y++) {
    for (int x = cx - outer; x <= cx + outer; x++) {
      const int dx = x - cx;
      const int dy = y - cy;
      const int d2 = dx * dx + dy * dy;
      if (d2 > outer2 || d2 < inner2) continue;
      double angle = atan2(static_cast<double>(dy), static_cast<double>(dx)) + M_PI / 2.0;
      if (angle < 0) angle += M_PI * 2.0;
      if (angle / (M_PI * 2.0) <= progress) setPixel(canvas, x, y, color);
    }
  }
}

void circleTrack(Canvas* canvas, int cx, int cy, int radius, int thickness) {
  const int outer = radius;
  const int inner = radius - thickness;
  const int outer2 = outer * outer;
  const int inner2 = inner * inner;
  for (int y = cy - outer; y <= cy + outer; y++) {
    for (int x = cx - outer; x <= cx + outer; x++) {
      const int dx = x - cx;
      const int dy = y - cy;
      const int d2 = dx * dx + dy * dy;
      if (d2 <= outer2 && d2 >= inner2) setPixel(canvas, x, y, 224);
    }
  }
}

unsigned char glyphRow(char ch, int row) {
  static const unsigned char digits[10][7] = {
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31}, {30, 1, 1, 14, 1, 1, 30}, {2, 6, 10, 18, 31, 2, 2},
    {31, 16, 30, 1, 1, 17, 14}, {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8}, {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12}
  };
  static const unsigned char letters[26][7] = {
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,14},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
  };
  if (ch >= '0' && ch <= '9') return digits[ch - '0'][row];
  if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  if (ch >= 'A' && ch <= 'Z') return letters[ch - 'A'][row];
  switch (ch) {
    case ' ': return 0;
    case '/': { static const unsigned char g[7] = {1,1,2,4,8,16,16}; return g[row]; }
    case ':': { static const unsigned char g[7] = {0,4,4,0,4,4,0}; return g[row]; }
    case '-': { static const unsigned char g[7] = {0,0,0,31,0,0,0}; return g[row]; }
    case '_': { static const unsigned char g[7] = {0,0,0,0,0,0,31}; return g[row]; }
    case '.': { static const unsigned char g[7] = {0,0,0,0,0,12,12}; return g[row]; }
    case ',': { static const unsigned char g[7] = {0,0,0,0,0,4,8}; return g[row]; }
    case '%': { static const unsigned char g[7] = {17,18,4,8,19,17,0}; return g[row]; }
    case '[': { static const unsigned char g[7] = {14,8,8,8,8,8,14}; return g[row]; }
    case ']': { static const unsigned char g[7] = {14,2,2,2,2,2,14}; return g[row]; }
    case '+': { static const unsigned char g[7] = {0,4,4,31,4,4,0}; return g[row]; }
    case '|': { static const unsigned char g[7] = {4,4,4,4,4,4,4}; return g[row]; }
    case '!': { static const unsigned char g[7] = {4,4,4,4,4,0,4}; return g[row]; }
    case '#': { static const unsigned char g[7] = {10,31,10,10,31,10,0}; return g[row]; }
    default: { static const unsigned char g[7] = {31,1,2,4,4,0,4}; return g[row]; }
  }
}

int textWidth(const char* text, int scale) {
  return static_cast<int>(strlen(text ? text : "")) * 6 * scale;
}

void drawText(Canvas* canvas, int x, int y, const char* text, int scale, unsigned char color) {
  int cursor = x;
  for (size_t i = 0; text && text[i]; i++) {
    char ch = text[i];
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    for (int row = 0; row < 7; row++) {
      const unsigned char bits = glyphRow(ch, row);
      for (int col = 0; col < 5; col++) {
        if (bits & (1 << (4 - col))) fillRect(canvas, cursor + col * scale, y + row * scale, scale, scale, color);
      }
    }
    cursor += 6 * scale;
  }
}

void drawTextClipped(Canvas* canvas, int x, int y, int max_width, const char* text, int scale, unsigned char color) {
  char clipped[128];
  copyText(clipped, sizeof(clipped), text);
  const int max_chars = max_width / (6 * scale);
  if (max_chars > 0 && static_cast<int>(strlen(clipped)) > max_chars) clipped[max_chars] = '\0';
  drawText(canvas, x, y, clipped, scale, color);
}

int drawTextWrapped(Canvas* canvas, int x, int y, int max_width, const char* text, int scale, unsigned char color, int max_lines) {
  if (!text || !text[0] || max_lines <= 0) return 0;
  char source[256];
  copyText(source, sizeof(source), text);
  const int max_chars = max_width / (6 * scale);
  if (max_chars <= 0) return 0;

  int lines = 0;
  char line_text[128] = "";
  char* cursor = source;
  while (*cursor && lines < max_lines) {
    while (*cursor && isspace(static_cast<unsigned char>(*cursor))) cursor++;
    if (!*cursor) break;
    char* word = cursor;
    while (*cursor && !isspace(static_cast<unsigned char>(*cursor))) cursor++;
    const char saved = *cursor;
    *cursor = '\0';

    const int line_len = static_cast<int>(strlen(line_text));
    const int word_len = static_cast<int>(strlen(word));
    if (line_len > 0 && line_len + 1 + word_len > max_chars) {
      drawText(canvas, x, y + lines * (8 * scale + 6), line_text, scale, color);
      lines++;
      line_text[0] = '\0';
    }
    if (lines >= max_lines) break;
    if (word_len > max_chars) {
      char clipped[128];
      copyText(clipped, sizeof(clipped), word);
      clipped[max_chars] = '\0';
      drawText(canvas, x, y + lines * (8 * scale + 6), clipped, scale, color);
      lines++;
    } else {
      if (line_text[0]) strncat(line_text, " ", sizeof(line_text) - strlen(line_text) - 1);
      strncat(line_text, word, sizeof(line_text) - strlen(line_text) - 1);
    }

    *cursor = saved;
  }
  if (line_text[0] && lines < max_lines) {
    drawText(canvas, x, y + lines * (8 * scale + 6), line_text, scale, color);
    lines++;
  }
  return lines;
}

void drawTextCentered(Canvas* canvas, int cx, int y, int max_width, const char* text, int scale, unsigned char color) {
  char clipped[128];
  copyText(clipped, sizeof(clipped), text);
  const int max_chars = max_width / (6 * scale);
  if (max_chars > 0 && static_cast<int>(strlen(clipped)) > max_chars) clipped[max_chars] = '\0';
  drawText(canvas, cx - textWidth(clipped, scale) / 2, y, clipped, scale, color);
}

int readPgmToken(FILE* file, char* out, size_t out_size) {
  if (!file || !out || out_size == 0) return 0;
  int ch = 0;
  do {
    ch = fgetc(file);
    if (ch == '#') {
      while (ch != EOF && ch != '\n') ch = fgetc(file);
    }
  } while (ch != EOF && isspace(ch));
  if (ch == EOF) return 0;

  size_t index = 0;
  while (ch != EOF && !isspace(ch)) {
    if (index + 1 < out_size) out[index++] = static_cast<char>(ch);
    ch = fgetc(file);
  }
  out[index] = '\0';
  return index > 0;
}

unsigned char* loadPgmPixels(const char* primary_path, const char* fallback_path, int* width, int* height) {
  FILE* file = fopen(primary_path, "rb");
  if (!file && fallback_path) file = fopen(fallback_path, "rb");
  if (!file) return NULL;

  char token[32];
  if (!readPgmToken(file, token, sizeof(token)) || strcmp(token, "P5") != 0) {
    fclose(file);
    return NULL;
  }
  if (!readPgmToken(file, token, sizeof(token))) {
    fclose(file);
    return NULL;
  }
  const int image_w = atoi(token);
  if (!readPgmToken(file, token, sizeof(token))) {
    fclose(file);
    return NULL;
  }
  const int image_h = atoi(token);
  if (!readPgmToken(file, token, sizeof(token))) {
    fclose(file);
    return NULL;
  }
  const int max_value = atoi(token);
  if (image_w <= 0 || image_h <= 0 || max_value <= 0 || max_value > 255) {
    fclose(file);
    return NULL;
  }

  const size_t size = static_cast<size_t>(image_w) * static_cast<size_t>(image_h);
  unsigned char* pixels = static_cast<unsigned char*>(malloc(size));
  if (!pixels) {
    fclose(file);
    return NULL;
  }
  if (fread(pixels, 1, size, file) != size) {
    free(pixels);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *width = image_w;
  *height = image_h;
  return pixels;
}

const unsigned char* loadCachedPgmPixels(const char* primary_path, const char* fallback_path, int* width, int* height) {
  for (int i = 0; i < g_pgm_cache_count; i++) {
    CachedPgm* cached = &g_pgm_cache[i];
    if (strcmp(cached->primary_path, primary_path ? primary_path : "") == 0 &&
        strcmp(cached->fallback_path, fallback_path ? fallback_path : "") == 0) {
      *width = cached->width;
      *height = cached->height;
      return cached->pixels;
    }
  }

  const long long started = monotonicMs();
  unsigned char* pixels = loadPgmPixels(primary_path, fallback_path, width, height);
  fprintf(stderr, "timing=image-load path=%s ok=%d ms=%lld\n", primary_path ? primary_path : "", pixels ? 1 : 0, monotonicMs() - started);
  if (!pixels) return NULL;
  if (g_pgm_cache_count >= static_cast<int>(sizeof(g_pgm_cache) / sizeof(g_pgm_cache[0]))) return pixels;

  CachedPgm* cached = &g_pgm_cache[g_pgm_cache_count++];
  copyText(cached->primary_path, sizeof(cached->primary_path), primary_path ? primary_path : "");
  copyText(cached->fallback_path, sizeof(cached->fallback_path), fallback_path ? fallback_path : "");
  cached->pixels = pixels;
  cached->width = *width;
  cached->height = *height;
  return cached->pixels;
}

void freePgmCache() {
  for (int i = 0; i < g_pgm_cache_count; i++) {
    free(g_pgm_cache[i].pixels);
    g_pgm_cache[i].pixels = NULL;
  }
  g_pgm_cache_count = 0;
}

void drawPgmImage(Canvas* canvas, int x, int y, int w, int h, const char* primary_path, const char* fallback_path, int invert) {
  int image_w = 0;
  int image_h = 0;
  const unsigned char* pixels = loadCachedPgmPixels(primary_path, fallback_path, &image_w, &image_h);
  if (!pixels) {
    strokeRect(canvas, x, y, w, h, 2, 0);
    drawTextCentered(canvas, x + w / 2, y + h / 2 - 12, w - 20, "MEAL ART", 3, 0);
    return;
  }

  fillRect(canvas, x, y, w, h, invert ? 0 : 255);

  int draw_w = w;
  int draw_h = h;
  if (image_w * h > w * image_h) {
    draw_h = (w * image_h) / image_w;
  } else {
    draw_w = (h * image_w) / image_h;
  }
  if (draw_w < 1) draw_w = 1;
  if (draw_h < 1) draw_h = 1;

  const int draw_x = x + (w - draw_w) / 2;
  const int draw_y = y + (h - draw_h) / 2;
  for (int yy = 0; yy < draw_h; yy++) {
    const int source_y = (yy * image_h) / draw_h;
    for (int xx = 0; xx < draw_w; xx++) {
      const int source_x = (xx * image_w) / draw_w;
      const unsigned char value = pixels[source_y * image_w + source_x];
      setPixel(canvas, draw_x + xx, draw_y + yy, invert ? static_cast<unsigned char>(255 - value) : value);
    }
  }
}

void drawPgmImageCover(Canvas* canvas, int x, int y, int w, int h, const char* primary_path, const char* fallback_path, int invert) {
  int image_w = 0;
  int image_h = 0;
  const unsigned char* pixels = loadCachedPgmPixels(primary_path, fallback_path, &image_w, &image_h);
  if (!pixels) {
    strokeRect(canvas, x, y, w, h, 2, 0);
    drawTextCentered(canvas, x + w / 2, y + h / 2 - 12, w - 20, "IMAGE", 3, 0);
    return;
  }

  fillRect(canvas, x, y, w, h, invert ? 0 : 255);
  int content_left = 0;
  int content_top = 0;
  int content_right = image_w;
  int content_bottom = image_h;
  int found_content = 0;
  for (int yy = 0; yy < image_h; yy++) {
    for (int xx = 0; xx < image_w; xx++) {
      const unsigned char value = pixels[yy * image_w + xx];
      if (value > 246) continue;
      if (!found_content) {
        content_left = xx;
        content_right = xx + 1;
        content_top = yy;
        content_bottom = yy + 1;
        found_content = 1;
      } else {
        if (xx < content_left) content_left = xx;
        if (xx + 1 > content_right) content_right = xx + 1;
        if (yy < content_top) content_top = yy;
        if (yy + 1 > content_bottom) content_bottom = yy + 1;
      }
    }
  }
  if (!found_content || content_right <= content_left || content_bottom <= content_top) {
    content_left = 0;
    content_top = 0;
    content_right = image_w;
    content_bottom = image_h;
  }
  const int source_w = content_right - content_left;
  const int source_h = content_bottom - content_top;
  for (int yy = 0; yy < h; yy++) {
    const int source_y = source_w * h > w * source_h
      ? content_top + (yy * source_h) / h
      : content_top + ((yy + ((w * source_h) / source_w - h) / 2) * source_w) / w;
    if (source_y < 0 || source_y >= image_h) continue;
    for (int xx = 0; xx < w; xx++) {
      const int source_x = source_w * h > w * source_h
        ? content_left + ((xx + ((h * source_w) / source_h - w) / 2) * source_h) / h
        : content_left + (xx * source_w) / w;
      if (source_x < 0 || source_x >= image_w) continue;
      const unsigned char value = pixels[source_y * image_w + source_x];
      setPixel(canvas, x + xx, y + yy, invert ? static_cast<unsigned char>(255 - value) : value);
    }
  }
}

void recipePhotoPath(const char* base_dir, const char* recipe_id, char* out, size_t out_size) {
  char safe_id[64];
  size_t j = 0;
  for (size_t i = 0; recipe_id && recipe_id[i] && j + 1 < sizeof(safe_id); i++) {
    const char ch = recipe_id[i];
    if (isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_') safe_id[j++] = ch;
  }
  safe_id[j] = '\0';
  if (!safe_id[0]) {
    if (out_size > 0) out[0] = '\0';
    return;
  }
  snprintf(out, out_size, "%s/%s.pgm", base_dir, safe_id);
}

int framebufferInvertForVisibleImage(int should_appear_flipped) {
  return g_invert_images && should_appear_flipped;
}

void drawRecipeLocalImage(Canvas* canvas, int x, int y, int w, int h, const RecipeRecord* recipe) {
  char primary[192];
  char fallback[192];
  recipePhotoPath(kRecipeAssetsPath, recipe ? recipe->id : "", primary, sizeof(primary));
  recipePhotoPath(kRecipeAssetsLocalPath, recipe ? recipe->id : "", fallback, sizeof(fallback));
  drawPgmImageCover(canvas, x, y, w, h, primary, fallback, framebufferInvertForVisibleImage(1));
}

const char* displayListTitle(const List* list) {
  if (!list) return "";
  if (strcmp(list->key, "todo") == 0) return "CHORES";
  if (strcmp(list->key, "grocery") == 0) return "GROCERY";
  return list->title[0] ? list->title : list->key;
}

const char* displayListTitleForIndex(const List* list, int list_index) {
  if (list_index == 0) return "CHORES";
  if (list_index == 1) return "GROCERY";
  return displayListTitle(list);
}

Rect exitButtonRectForScreen(int width, int height);
void drawBitmapDashboard(Canvas* canvas, const Dashboard* dashboard, const char* status);
void clearTouchRegions();
void addTouchRegion(Rect rect, TouchAction action, int list_index, int item_index, const char* item_id, int item_done);

void drawRadialMetric(Canvas* canvas, int x, int y, int w, int h, const char* label, int value, int target, const char* unit) {
  strokeRect(canvas, x, y, w, h, 2, 0);
  const int percent = target > 0 ? (value * 100) / target : 0;
  const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  const int min_side = w < h ? w : h;
  const int radius = w < 160 ? min_side / 4 : min_side / 3;
  const int ring_thickness = w < 160 ? 10 : 14;
  const int percent_scale = w < 160 ? 3 : 4;
  const int cx = x + w / 2;
  const int cy = y + h / 2 - 18;
  circleTrack(canvas, cx, cy, radius, ring_thickness);
  circleRing(canvas, cx, cy, radius, ring_thickness, clamped, 0);

  char percent_text[24];
  snprintf(percent_text, sizeof(percent_text), "%d%%", clamped);
  drawTextCentered(canvas, cx, cy - 21, 132, percent_text, percent_scale, 0);
  drawTextCentered(canvas, cx, cy + 24, 132, label, 2, 0);

  char value_text[32];
  char target_text[32];
  char metric[80];
  formatNumber(value, value_text, sizeof(value_text));
  formatNumber(target, target_text, sizeof(target_text));
  const int metric_scale = w < 260 ? 2 : 3;
  if (w < 260) snprintf(metric, sizeof(metric), "%s / %s", value_text, target_text);
  else snprintf(metric, sizeof(metric), "%s / %s %s", value_text, target_text, unit);
  drawTextCentered(canvas, cx, y + h - 38, w - 18, metric, metric_scale, 0);
}

void formatTenths(int value, char* out, size_t size) {
  const int whole = value / 10;
  const int fraction = value < 0 ? -(value % 10) : value % 10;
  if (fraction == 0) snprintf(out, size, "%d", whole);
  else snprintf(out, size, "%d.%d", whole, fraction);
}

void drawStarIcon(Canvas* canvas, int x, int y, int scale, int filled) {
  static const char* filled_mask[] = {
    "......#......",
    ".....###.....",
    ".....###.....",
    "#############",
    ".###########.",
    "..#########..",
    "...#######...",
    "...#######...",
    "..###...###..",
    ".##.......##.",
    "##.........##"
  };
  static const char* empty_mask[] = {
    "......#......",
    ".....#.#.....",
    ".....#.#.....",
    "###..#.#..###",
    ".##.....##..",
    "..##...##...",
    "...#...#....",
    "...#...#....",
    "..##...##...",
    ".##.....##..",
    "##.......##."
  };
  const char** mask = filled ? filled_mask : empty_mask;
  for (int row = 0; row < 11; row++) {
    for (int col = 0; mask[row][col]; col++) {
      if (mask[row][col] == '#') fillRect(canvas, x + col * scale, y + row * scale, scale, scale, 0);
    }
  }
}

void drawStarRating(Canvas* canvas, int x, int y, int rating_tenths, int scale) {
  int filled = (rating_tenths + 5) / 10;
  if (filled < 0) filled = 0;
  if (filled > 5) filled = 5;
  const int star_w = 13 * scale;
  const int gap = 4 * scale;
  for (int i = 0; i < 5; i++) {
    drawStarIcon(canvas, x + i * (star_w + gap), y, scale, i < filled);
  }
}

void drawRadialMetricTenths(Canvas* canvas, int x, int y, int w, int h, const char* label, int value, int target, const char* unit) {
  strokeRect(canvas, x, y, w, h, 2, 0);
  const int percent = target > 0 ? (value * 100) / target : 0;
  const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  const int min_side = w < h ? w : h;
  const int radius = w < 160 ? min_side / 4 : min_side / 3;
  const int ring_thickness = w < 160 ? 10 : 14;
  const int percent_scale = w < 160 ? 3 : 4;
  const int cx = x + w / 2;
  const int cy = y + h / 2 - 18;
  circleTrack(canvas, cx, cy, radius, ring_thickness);
  circleRing(canvas, cx, cy, radius, ring_thickness, clamped, 0);

  char percent_text[24];
  snprintf(percent_text, sizeof(percent_text), "%d%%", clamped);
  drawTextCentered(canvas, cx, cy - 21, 132, percent_text, percent_scale, 0);
  drawTextCentered(canvas, cx, cy + 24, 132, label, 2, 0);

  char value_text[32];
  char target_text[32];
  char metric[80];
  formatTenths(value, value_text, sizeof(value_text));
  formatTenths(target, target_text, sizeof(target_text));
  const int metric_scale = w < 260 ? 2 : 3;
  if (w < 260) snprintf(metric, sizeof(metric), "%s / %s", value_text, target_text);
  else snprintf(metric, sizeof(metric), "%s / %s %s", value_text, target_text, unit);
  drawTextCentered(canvas, cx, y + h - 38, w - 18, metric, metric_scale, 0);
}

void drawChallengeStreakCard(Canvas* canvas, int x, int y, int w, int h, int current_day) {
  strokeRect(canvas, x, y, w, h, 3, 0);
  const int title_scale = textWidth("STREAK", 4) <= w - 24 ? 4 : 3;
  drawTextCentered(canvas, x + w / 2, y + 20, w - 24, "STREAK", title_scale, 0);
  line(canvas, x + 14, y + 62, x + w - 14, y + 62, 2, 0);

  char day_text[32];
  snprintf(day_text, sizeof(day_text), "%d / 75", current_day);
  drawTextCentered(canvas, x + w / 2, y + 84, w - 24, day_text, 3, 0);

  const int columns = 5;
  const int rows = 15;
  const int min_gap = 3;
  const int max_grid_w = w - 24;
  const int max_grid_h = h - 172;
  int square = (max_grid_h - (rows - 1) * min_gap) / rows;
  const int square_by_w = max_grid_w / columns;
  if (square > square_by_w) square = square_by_w;
  if (square > 7) square -= 3;
  else if (square > 5) square -= 2;
  if (square < 4) square = 4;
  int gap_x = columns > 1 ? (max_grid_w - columns * square) / (columns - 1) : 0;
  if (gap_x < min_gap) gap_x = min_gap;
  const int gap_y = min_gap;
  const int grid_h = rows * square + (rows - 1) * gap_y;
  const int grid_x = x + 12;
  const int grid_y = y + 132 + (max_grid_h - grid_h) / 2;
  const int clamped_day = current_day < 1 ? 1 : (current_day > 75 ? 75 : current_day);

  for (int day = 1; day <= 75; day++) {
    const int index = day - 1;
    const int column = index % columns;
    const int row = index / columns;
    const int cell_x = grid_x + column * (square + gap_x);
    const int cell_y = grid_y + row * (square + gap_y);
    const int completed = day <= clamped_day;
    const int current = day == clamped_day;
    if (completed) {
      fillRect(canvas, cell_x, cell_y, square, square, 0);
    } else {
      fillRect(canvas, cell_x, cell_y, square, square, current ? 212 : 244);
      strokeRect(canvas, cell_x, cell_y, square, square, current ? 2 : 1, 0);
    }
  }

  drawTextCentered(canvas, x + w / 2, y + h - 38, w - 24, "75 MEDIUM", 2, 0);
}

void drawImageCard(Canvas* canvas, int x, int y, int w, int h, const char* image_path, const char* fallback_path) {
  strokeRect(canvas, x, y, w, h, 2, 0);
  drawPgmImageCover(canvas, x + 2, y + 2, w - 4, h - 4, image_path, fallback_path, framebufferInvertForVisibleImage(1));
}

void drawListCard(Canvas* canvas, int x, int y, int w, int h, const List* list, int list_index) {
  strokeRect(canvas, x, y, w, h, 3, 0);
  Rect card_rect = {x, y, w, h};
  addTouchRegion(card_rect, kTouchOpenList, list_index, -1, "", 0);

  const char* title = displayListTitleForIndex(list, list_index);
  fprintf(stderr, "render=list-card-header index=%d title=%s rect=%d,%d,%d,%d\n", list_index, title, x, y, w, h);

  if (h < 112) {
    drawTextCentered(canvas, x + w / 2, y + 14, w - 24, title, 4, 0);
    line(canvas, x + 10, y + 52, x + w - 10, y + 52, 2, 0);
    if (list->item_count > 0) {
      char row[128];
      char item_text[96];
      upperCopy(item_text, sizeof(item_text), list->items[0].text);
      snprintf(row, sizeof(row), "%s %.52s", list->items[0].done ? "[X]" : "[ ]", item_text);
      drawTextClipped(canvas, x + 18, y + 66, w - 36, row, 2, 0);
    } else {
      drawTextCentered(canvas, x + w / 2, y + 66, w - 36, "NO ITEMS", 2, 0);
    }
    return;
  }

  drawTextCentered(canvas, x + w / 2, y + 14, w - 24, title, 4, 0);
  line(canvas, x + 10, y + 52, x + w - 10, y + 52, 2, 0);
  int row_capacity = (h - 124) / 42;
  if (row_capacity < 1) row_capacity = 1;
  const int max_preview_rows = list_index == 1 ? 8 : 5;
  if (row_capacity > max_preview_rows) row_capacity = max_preview_rows;
  const int shown = list->item_count > row_capacity ? row_capacity : list->item_count;
  for (int i = 0; i < shown; i++) {
    char row[128];
    char item_text[96];
    upperCopy(item_text, sizeof(item_text), list->items[i].text);
    snprintf(row, sizeof(row), "%s %.52s", list->items[i].done ? "[X]" : "[ ]", item_text);
    drawTextClipped(canvas, x + 18, y + 74 + i * 42, w - 36, row, 3, 0);
  }
  if (list->item_count > shown && h >= 190) {
    char more[48];
    snprintf(more, sizeof(more), "+%d MORE", list->item_count - shown);
    drawTextClipped(canvas, x + 18, y + h - 68, 180, more, 3, 0);
  }
  if (h >= 160) {
    const int hint_scale = h < 172 ? 2 : 3;
    drawTextCentered(canvas, x + w / 2, y + h - 34, w - 24, "[ TAP TO OPEN ]", hint_scale, 0);
  }
}

void drawMealPlannerTile(Canvas* canvas, int x, int y, int w, int h) {
  strokeRect(canvas, x, y, w, h, 3, 0);
  Rect tile_rect = {x, y, w, h};
  addTouchRegion(tile_rect, kTouchOpenMealPlanner, -1, -1, "", 0);
  drawPgmImageCover(canvas, x + 3, y + 3, w - 6, h - 6, kMealCoverPath, kMealCoverLocalPath, framebufferInvertForVisibleImage(0));
  fillRect(canvas, x + 3, y + 3, w - 6, 50, 255);
  line(canvas, x + 10, y + 53, x + w - 10, y + 53, 2, 0);
  drawTextCentered(canvas, x + w / 2, y + 14, w - 24, "MEAL PLANNER", 4, 0);
}

void drawChallengeTile(Canvas* canvas, int x, int y, int size) {
  strokeRect(canvas, x, y, size, size, 3, 0);
  Rect tile_rect = {x, y, size, size};
  addTouchRegion(tile_rect, kTouchOpenChallenge, -1, -1, "", 0);
  const int art_x = x + 3;
  const int art_y = y + 3;
  const int art_w = size - 6;
  const int art_h = size - 6;
  if (art_h > 24) {
    drawPgmImageCover(canvas, art_x, art_y, art_w, art_h, kChallengeCoverPath, kChallengeCoverLocalPath, framebufferInvertForVisibleImage(0));
  }
}

void drawChevron(Canvas* canvas, const Rect& rect, int direction) {
  const int cx = rect.x + rect.w / 2;
  const int cy = rect.y + rect.h / 2;
  const int half_w = 10;
  const int half_h = 14;
  if (direction < 0) {
    line(canvas, cx + half_w, cy - half_h, cx - half_w, cy, 5, 0);
    line(canvas, cx - half_w, cy, cx + half_w, cy + half_h, 5, 0);
  } else {
    line(canvas, cx - half_w, cy - half_h, cx + half_w, cy, 5, 0);
    line(canvas, cx + half_w, cy, cx - half_w, cy + half_h, 5, 0);
  }
}

int drawDaySwitchArrows(Canvas* canvas, int shell_x, int shell_y, int shell_w) {
  (void)shell_w;
  const int button_w = 64;
  const int today_w = 118;
  const int button_h = 52;
  const int gap = 12;
  Rect exit_rect = exitButtonRectForScreen(canvas->width, canvas->height);
  Rect next_rect = {exit_rect.x - 104 - button_w, shell_y + 28, button_w, button_h};
  Rect today_rect = {next_rect.x - gap - today_w, shell_y + 28, today_w, button_h};
  Rect prev_rect = {today_rect.x - gap - button_w, shell_y + 28, button_w, button_h};
  if (prev_rect.x < shell_x + 320) {
    prev_rect.x = shell_x + 320;
    today_rect.x = prev_rect.x + button_w + gap;
    next_rect.x = today_rect.x + today_w + gap;
  }

  strokeRect(canvas, prev_rect.x, prev_rect.y, prev_rect.w, prev_rect.h, 2, 0);
  drawChevron(canvas, prev_rect, -1);
  addTouchRegion(prev_rect, kTouchPreviousDay, -1, -1, "", 0);

  strokeRect(canvas, today_rect.x, today_rect.y, today_rect.w, today_rect.h, 2, 0);
  drawTextCentered(canvas, today_rect.x + today_rect.w / 2, today_rect.y + 16, today_rect.w - 12, "TODAY", 2, 0);
  addTouchRegion(today_rect, kTouchToday, -1, -1, "", 0);

  strokeRect(canvas, next_rect.x, next_rect.y, next_rect.w, next_rect.h, 2, 0);
  drawChevron(canvas, next_rect, 1);
  addTouchRegion(next_rect, kTouchNextDay, -1, -1, "", 0);

  return prev_rect.x;
}

void drawTopHeader(Canvas* canvas, const Dashboard* dashboard, const char* status, int shell_x, int shell_y, int shell_w) {
  const int header_h = 132;
  doubleRect(canvas, shell_x + 10, shell_y + 10, shell_w - 20, header_h, 0);
  const int arrow_left = drawDaySwitchArrows(canvas, shell_x, shell_y, shell_w);
  drawTextClipped(canvas, shell_x + 28, shell_y + 24, arrow_left - shell_x - 44, "DAILY OPS", 4, 0);

  Rect exit_rect = exitButtonRectForScreen(canvas->width, canvas->height);
  Rect exit_hit_rect = {exit_rect.x - 20, kKindleStatusBarHeight, exit_rect.w + 40, exit_rect.y - kKindleStatusBarHeight + exit_rect.h + 20};
  strokeRect(canvas, exit_rect.x, exit_rect.y, exit_rect.w, exit_rect.h, 2, 0);
  drawTextCentered(canvas, exit_rect.x + exit_rect.w / 2, exit_rect.y + 34, exit_rect.w - 16, "EXIT", 3, 0);
  addTouchRegion(exit_hit_rect, kTouchExit, -1, -1, "", 0);
  addTouchRegion(exit_rect, kTouchExit, -1, -1, "", 0);

  line(canvas, shell_x + 20, shell_y + 88, exit_rect.x - 8, shell_y + 88, 2, 0);
  char updated[96];
  formatDisplayDate(dashboard->generated_at, status, updated, sizeof(updated));
  drawTextClipped(canvas, shell_x + 28, shell_y + 102, exit_rect.x - shell_x - 44, updated, 2, 0);
}

void drawSubHeader(Canvas* canvas, int shell_x, int y, int shell_w, const char* title) {
  const int list_header_h = 80;
  doubleRect(canvas, shell_x + 10, y, shell_w - 20, list_header_h, 0);
  const int title_w = shell_w - 310;
  const int title_scale = textWidth(title, 5) <= title_w ? 5 : (textWidth(title, 4) <= title_w ? 4 : 3);
  const int title_y = y + (title_scale == 5 ? 22 : (title_scale == 4 ? 26 : 30));
  drawTextClipped(canvas, shell_x + 28, title_y, title_w, title, title_scale, 0);

  Rect back_rect = {shell_x + shell_w - 136, y + 14, 104, 52};
  Rect home_rect = {back_rect.x - 116, y + 14, 104, 52};
  strokeRect(canvas, home_rect.x, home_rect.y, home_rect.w, home_rect.h, 2, 0);
  drawTextCentered(canvas, home_rect.x + home_rect.w / 2, home_rect.y + 16, home_rect.w - 12, "HOME", 2, 0);
  addTouchRegion(home_rect, kTouchHome, -1, -1, "", 0);

  strokeRect(canvas, back_rect.x, back_rect.y, back_rect.w, back_rect.h, 2, 0);
  drawTextCentered(canvas, back_rect.x + back_rect.w / 2, back_rect.y + 16, back_rect.w - 12, "BACK", 2, 0);
  addTouchRegion(back_rect, kTouchBack, -1, -1, "", 0);
}

void drawMealPlannerDashboard(Canvas* canvas, const Dashboard* dashboard, const char* status) {
  clearCanvas(canvas, 255);
  clearTouchRegions();
  g_last_screen_width = canvas->width;
  g_last_screen_height = canvas->height;
  const int shell_w = canvas->width;
  const int shell_x = 0;
  const int shell_y = kKindleStatusBarHeight;
  const int shell_h = canvas->height - shell_y;
  strokeRect(canvas, shell_x, shell_y, shell_w, shell_h, 3, 0);
  drawTopHeader(canvas, dashboard, status, shell_x, shell_y, shell_w);

  const int sub_y = shell_y + 10 + 132 + 8;
  drawSubHeader(canvas, shell_x, sub_y, shell_w, "MEAL PLANNER");

  const int cover_y = sub_y + 96;
  strokeRect(canvas, shell_x + 18, cover_y, shell_w - 36, 188, 3, 0);
  drawPgmImageCover(canvas, shell_x + 42, cover_y + 18, 250, 140, kMealCoverPath, kMealCoverLocalPath, framebufferInvertForVisibleImage(0));
  drawTextClipped(canvas, shell_x + 320, cover_y + 32, shell_w - 352, "TODAY'S MEALS", 5, 0);
  drawTextClipped(canvas, shell_x + 320, cover_y + 84, shell_w - 352, "EACH ROW OPENS", 3, 0);
  drawTextClipped(canvas, shell_x + 320, cover_y + 118, shell_w - 352, "A RECIPE CARD", 3, 0);

  const int recipes_card_y = cover_y + 206;
  Rect recipes_rect = {shell_x + 18, recipes_card_y, shell_w - 36, 74};
  strokeRect(canvas, recipes_rect.x, recipes_rect.y, recipes_rect.w, recipes_rect.h, 3, 0);
  addTouchRegion(recipes_rect, kTouchOpenRecipes, -1, -1, "", 0);
  drawTextClipped(canvas, recipes_rect.x + 20, recipes_rect.y + 18, recipes_rect.w - 260, "RECIPES", 5, 0);
  char count_text[64];
  snprintf(count_text, sizeof(count_text), "%d SAVED", dashboard->recipe_count);
  drawTextClipped(canvas, recipes_rect.x + recipes_rect.w - 210, recipes_rect.y + 24, 180, count_text, 3, 0);

  const int row_x = shell_x + 18;
  const int row_w = shell_w - 36;
  const int row_h = 82;
  const int row_gap = 10;
  const int first_y = recipes_card_y + 92;
  if (dashboard->meal_plan_count == 0) {
    Rect empty_rect = {row_x, first_y, row_w, 118};
    strokeRect(canvas, empty_rect.x, empty_rect.y, empty_rect.w, empty_rect.h, 2, 0);
    drawTextClipped(canvas, empty_rect.x + 18, empty_rect.y + 22, empty_rect.w - 36, "NO MEALS PLANNED", 4, 0);
    drawTextClipped(canvas, empty_rect.x + 18, empty_rect.y + 66, empty_rect.w - 36, "SET TODAY'S MEAL PLAN VIA TELEGRAM", 2, 0);
    return;
  }
  for (int i = 0; i < dashboard->meal_plan_count; i++) {
    const int row_y = first_y + i * (row_h + row_gap);
    if (row_y + row_h > shell_y + shell_h - 18) break;
    const int recipe_index = dashboard->meal_plan_recipe_indices[i];
    if (recipe_index < 0 || recipe_index >= dashboard->recipe_count) continue;
    const RecipeRecord* recipe = &dashboard->recipes[recipe_index];
    Rect row_rect = {row_x, row_y, row_w, row_h};
    strokeRect(canvas, row_rect.x, row_rect.y, row_rect.w, row_rect.h, 2, 0);
    addTouchRegion(row_rect, kTouchOpenMealPlanRecipe, -1, recipe_index, "", 0);
    char meal_label[32];
    snprintf(meal_label, sizeof(meal_label), "MEAL %d", i + 1);
    drawTextClipped(canvas, row_x + 18, row_y + 14, 170, meal_label, 3, 0);
    drawTextClipped(canvas, row_x + 208, row_y + 14, row_w - 360, recipe->title, 4, 0);
    char macro_hint[96];
    snprintf(macro_hint, sizeof(macro_hint), "%d CAL  C%d F%d P%d", recipe->calories, recipe->carbs, recipe->fat, recipe->protein);
    drawTextClipped(canvas, row_x + 208, row_y + 52, row_w - 390, macro_hint, 2, 0);
    drawStarRating(canvas, row_x + row_w - 190, row_y + 16, recipe->rating_tenths, 2);
    drawTextClipped(canvas, row_x + row_w - 176, row_y + 52, 154, "[ RECIPE ]", 2, 0);
  }
}

void drawRecipesDashboard(Canvas* canvas, const Dashboard* dashboard, const char* status) {
  clearCanvas(canvas, 255);
  clearTouchRegions();
  g_last_screen_width = canvas->width;
  g_last_screen_height = canvas->height;
  const int shell_w = canvas->width;
  const int shell_x = 0;
  const int shell_y = kKindleStatusBarHeight;
  const int shell_h = canvas->height - shell_y;
  strokeRect(canvas, shell_x, shell_y, shell_w, shell_h, 3, 0);
  drawTopHeader(canvas, dashboard, status, shell_x, shell_y, shell_w);

  const int sub_y = shell_y + 10 + 132 + 8;
  drawSubHeader(canvas, shell_x, sub_y, shell_w, "RECIPES");

  const int gap = 10;
  const int card_w = (shell_w - 36 - gap) / 2;
  const int card_h = 132;
  const int first_y = sub_y + 98;
  for (int i = 0; i < dashboard->recipe_count && i < kMaxRecipes; i++) {
    const int column = i % 2;
    const int row = i / 2;
    const int card_x = shell_x + 18 + column * (card_w + gap);
    const int card_y = first_y + row * (card_h + gap);
    if (card_y + card_h > shell_y + shell_h - 18) break;
    Rect card_rect = {card_x, card_y, card_w, card_h};
    strokeRect(canvas, card_rect.x, card_rect.y, card_rect.w, card_rect.h, 3, 0);
    addTouchRegion(card_rect, kTouchOpenRecipe, -1, i, "", 0);
    drawTextClipped(canvas, card_x + 14, card_y + 14, card_w - 28, dashboard->recipes[i].title, 3, 0);
    line(canvas, card_x + 10, card_y + 54, card_x + card_w - 10, card_y + 54, 2, 0);
    char macro_text[96];
    snprintf(macro_text, sizeof(macro_text), "%d CAL C%d F%d P%d", dashboard->recipes[i].calories, dashboard->recipes[i].carbs, dashboard->recipes[i].fat, dashboard->recipes[i].protein);
    drawTextClipped(canvas, card_x + 14, card_y + 72, card_w - 28, macro_text, 2, 0);
    drawStarRating(canvas, card_x + 14, card_y + 102, dashboard->recipes[i].rating_tenths, 1);
    drawTextClipped(canvas, card_x + card_w - 94, card_y + 104, 78, "[ OPEN ]", 2, 0);
  }
}

void drawRecipeRecordDashboard(Canvas* canvas, const Dashboard* dashboard, const char* status, int recipe_index) {
  clearCanvas(canvas, 255);
  clearTouchRegions();
  g_last_screen_width = canvas->width;
  g_last_screen_height = canvas->height;
  const int shell_w = canvas->width;
  const int shell_x = 0;
  const int shell_y = kKindleStatusBarHeight;
  const int shell_h = canvas->height - shell_y;
  strokeRect(canvas, shell_x, shell_y, shell_w, shell_h, 3, 0);
  drawTopHeader(canvas, dashboard, status, shell_x, shell_y, shell_w);

  if (recipe_index < 0 || recipe_index >= dashboard->recipe_count) recipe_index = 0;
  const RecipeRecord* recipe = &dashboard->recipes[recipe_index];
  const int sub_y = shell_y + 10 + 132 + 8;
  drawSubHeader(canvas, shell_x, sub_y, shell_w, "RECIPE");

  const int card_x = shell_x + 18;
  const int card_y = sub_y + 98;
  const int card_w = shell_w - 36;
  const int card_h = shell_y + shell_h - card_y - 18;
  strokeRect(canvas, card_x, card_y, card_w, card_h, 3, 0);
  drawTextClipped(canvas, card_x + 20, card_y + 22, card_w - 40, recipe->title, 5, 0);
  line(canvas, card_x + 14, card_y + 76, card_x + card_w - 14, card_y + 76, 2, 0);
  drawTextClipped(canvas, card_x + 20, card_y + 86, 126, "RATING", 3, 0);
  drawStarRating(canvas, card_x + 164, card_y + 84, recipe->rating_tenths, 2);

  const int content_x = card_x + 20;
  const int content_w = card_w - 40;
  const int top_y = card_y + 126;
  const int column_gap = 14;
  const int photo_w = (content_w - column_gap) / 2;
  const int photo_h = photo_w;
  const int macro_x = content_x + photo_w + column_gap;
  const int macro_w = content_w - photo_w - column_gap;
  strokeRect(canvas, content_x, top_y, photo_w, photo_h, 2, 0);
  drawRecipeLocalImage(canvas, content_x + 8, top_y + 8, photo_w - 16, photo_h - 16, recipe);

  const int macro_gap = 8;
  const int macro_box_h = (photo_h - macro_gap) / 2;
  const int macro_box_w = (macro_w - macro_gap) / 2;
  const char* labels[4] = {"CAL", "CARBS", "FAT", "PROT"};
  const int values[4] = {recipe->calories, recipe->carbs, recipe->fat, recipe->protein};
  for (int i = 0; i < 4; i++) {
    const int column = i % 2;
    const int row = i / 2;
    const int box_x = macro_x + column * (macro_box_w + macro_gap);
    const int box_y = top_y + row * (macro_box_h + macro_gap);
    strokeRect(canvas, box_x, box_y, macro_box_w, macro_box_h, 2, 0);
    drawTextCentered(canvas, box_x + macro_box_w / 2, box_y + 22, macro_box_w - 8, labels[i], 2, 0);
    char value_text[24];
    snprintf(value_text, sizeof(value_text), i == 0 ? "%d" : "%dG", values[i]);
    drawTextCentered(canvas, box_x + macro_box_w / 2, box_y + 64, macro_box_w - 8, value_text, 4, 0);
  }

  const int ingredients_title_y = top_y + photo_h + 28;
  drawTextClipped(canvas, content_x, ingredients_title_y, content_w, "INGREDIENTS", 3, 0);
  const int ingredient_y = ingredients_title_y + 40;
  const int ingredient_row_h = 34;
  const int amount_w = 180;
  int ingredients_shown = 0;
  for (int i = 0; i < recipe->ingredient_count && i < kMaxRecipeIngredients; i++) {
    const int row_y = ingredient_y + i * ingredient_row_h;
    if (row_y + ingredient_row_h > card_y + card_h - 112) break;
    drawTextClipped(canvas, content_x + 4, row_y, content_w - amount_w - 12, recipe->ingredients[i].name, 3, 0);
    drawTextClipped(canvas, card_x + card_w - amount_w - 20, row_y, amount_w, recipe->ingredients[i].amount, 3, 0);
    ingredients_shown++;
  }
  const int steps_y = ingredient_y + ingredients_shown * ingredient_row_h + 24;
  if (steps_y + 58 < card_y + card_h) {
    drawTextClipped(canvas, content_x, steps_y, content_w, "STEPS", 3, 0);
    drawTextWrapped(canvas, content_x, steps_y + 36, content_w, recipe->instructions, 2, 0, 4);
  }
}

void drawRecipeDashboard(Canvas* canvas, const Dashboard* dashboard, const char* status, int recipe_index) {
  clearCanvas(canvas, 255);
  clearTouchRegions();
  g_last_screen_width = canvas->width;
  g_last_screen_height = canvas->height;
  const int shell_w = canvas->width;
  const int shell_x = 0;
  const int shell_y = kKindleStatusBarHeight;
  const int shell_h = canvas->height - shell_y;
  strokeRect(canvas, shell_x, shell_y, shell_w, shell_h, 3, 0);
  drawTopHeader(canvas, dashboard, status, shell_x, shell_y, shell_w);

  if (recipe_index < 0 || recipe_index >= kMealPlanCount) recipe_index = 0;
  const MealPlanEntry* meal = &kMealPlan[recipe_index];
  const int sub_y = shell_y + 10 + 132 + 8;
  drawSubHeader(canvas, shell_x, sub_y, shell_w, "RECIPE");

  const int card_x = shell_x + 18;
  const int card_y = sub_y + 98;
  const int card_w = shell_w - 36;
  const int card_h = shell_y + shell_h - card_y - 18;
  strokeRect(canvas, card_x, card_y, card_w, card_h, 3, 0);
  drawTextClipped(canvas, card_x + 20, card_y + 22, card_w - 40, meal->title, 5, 0);
  line(canvas, card_x + 14, card_y + 76, card_x + card_w - 14, card_y + 76, 2, 0);
  drawTextClipped(canvas, card_x + 20, card_y + 92, card_w - 40, meal->recipe, 3, 0);

  const int content_x = card_x + 20;
  const int content_w = card_w - 40;
  const int top_y = card_y + 142;
  const int column_gap = 14;
  const int photo_w = (content_w - column_gap) / 2;
  const int photo_h = photo_w;
  const int macro_x = content_x + photo_w + column_gap;
  const int macro_w = content_w - photo_w - column_gap;
  strokeRect(canvas, content_x, top_y, photo_w, photo_h, 2, 0);
  drawPgmImageCover(canvas, content_x + 8, top_y + 8, photo_w - 16, photo_h - 16, meal->photo_path, meal->photo_fallback_path, framebufferInvertForVisibleImage(1));

  const int macro_gap = 8;
  const int macro_box_h = (photo_h - macro_gap) / 2;
  const int macro_box_w = (macro_w - macro_gap) / 2;
  const char* labels[4] = {"CAL", "CARBS", "FAT", "PROT"};
  const int values[4] = {meal->calories, meal->carbs, meal->fat, meal->protein};
  for (int i = 0; i < 4; i++) {
    const int column = i % 2;
    const int row = i / 2;
    const int box_x = macro_x + column * (macro_box_w + macro_gap);
    const int box_y = top_y + row * (macro_box_h + macro_gap);
    strokeRect(canvas, box_x, box_y, macro_box_w, macro_box_h, 2, 0);
    drawTextCentered(canvas, box_x + macro_box_w / 2, box_y + 22, macro_box_w - 8, labels[i], 2, 0);
    char value_text[24];
    snprintf(value_text, sizeof(value_text), i == 0 ? "%d" : "%dG", values[i]);
    drawTextCentered(canvas, box_x + macro_box_w / 2, box_y + 64, macro_box_w - 8, value_text, 4, 0);
  }

  const int ingredients_title_y = top_y + photo_h + 28;
  drawTextClipped(canvas, content_x, ingredients_title_y, content_w, "INGREDIENTS", 3, 0);
  const int ingredient_y = ingredients_title_y + 42;
  const int ingredient_row_h = 38;
  const int amount_w = 160;
  int ingredients_shown = 0;
  for (int i = 0; i < meal->ingredient_count && i < kMaxRecipeIngredients; i++) {
    const int row_y = ingredient_y + i * ingredient_row_h;
    if (row_y + ingredient_row_h > card_y + card_h - 122) break;
    drawTextClipped(canvas, card_x + 24, row_y, card_w - amount_w - 52, meal->ingredients[i].name, 3, 0);
    drawTextClipped(canvas, card_x + card_w - amount_w - 20, row_y, amount_w, meal->ingredients[i].amount, 3, 0);
    ingredients_shown++;
  }
  const int steps_y = ingredient_y + ingredients_shown * ingredient_row_h + 24;
  if (steps_y + 68 < card_y + card_h) {
    drawTextClipped(canvas, content_x, steps_y, content_w, "STEPS", 3, 0);
    drawTextWrapped(canvas, content_x, steps_y + 44, content_w, meal->steps, 3, 0, 3);
  }
}

void drawFullListDashboard(Canvas* canvas, const Dashboard* dashboard, int list_index, const char* status) {
  clearCanvas(canvas, 255);
  clearTouchRegions();
  g_last_screen_width = canvas->width;
  g_last_screen_height = canvas->height;
  const int shell_w = canvas->width;
  const int shell_x = 0;
  const int shell_y = kKindleStatusBarHeight;
  const int shell_h = canvas->height - shell_y;
  strokeRect(canvas, shell_x, shell_y, shell_w, shell_h, 3, 0);

  if (list_index < 0 || list_index >= dashboard->list_count) list_index = 0;
  const List* list = &dashboard->lists[list_index];

  drawTopHeader(canvas, dashboard, status, shell_x, shell_y, shell_w);

  const int list_header_y = shell_y + 10 + 132 + 8;
  const int list_header_h = 80;

  const char* title = displayListTitleForIndex(list, list_index);
  drawSubHeader(canvas, shell_x, list_header_y, shell_w, title);

  const int row_x = shell_x + 18;
  const int row_w = shell_w - 36;
  const int row_h = 72;
  const int row_gap = 10;
  const int first_y = list_header_y + list_header_h + 18;
  const int max_rows = (shell_y + shell_h - first_y - 24) / (row_h + row_gap);
  const int shown = list->item_count < max_rows ? list->item_count : max_rows;
  for (int i = 0; i < shown; i++) {
    const int row_y = first_y + i * (row_h + row_gap);
    Rect row_rect = {row_x, row_y, row_w, row_h};
    strokeRect(canvas, row_rect.x, row_rect.y, row_rect.w, row_rect.h, 2, 0);
    addTouchRegion(row_rect, kTouchToggleItem, list_index, i, list->items[i].id, list->items[i].done);
    char row[160];
    char item_text[96];
    upperCopy(item_text, sizeof(item_text), list->items[i].text);
    snprintf(row, sizeof(row), "%s %.46s", list->items[i].done ? "[X]" : "[ ]", item_text);
    drawTextClipped(canvas, row_x + 18, row_y + 20, row_w - 36, row, 3, 0);
  }
}

void drawChallengeDashboard(Canvas* canvas, const Dashboard* dashboard, const char* status) {
  clearCanvas(canvas, 255);
  clearTouchRegions();
  g_last_screen_width = canvas->width;
  g_last_screen_height = canvas->height;
  const int shell_w = canvas->width;
  const int shell_x = 0;
  const int shell_y = kKindleStatusBarHeight;
  const int shell_h = canvas->height - shell_y;
  strokeRect(canvas, shell_x, shell_y, shell_w, shell_h, 3, 0);
  drawTopHeader(canvas, dashboard, status, shell_x, shell_y, shell_w);

  const int sub_y = shell_y + 10 + 132 + 8;
  drawSubHeader(canvas, shell_x, sub_y, shell_w, "75 DAY CHALLENGE");
  char day_text[40];
  snprintf(day_text, sizeof(day_text), "DAY %d // CURRENT DAY", dashboard->challenge_day);
  drawTextClipped(canvas, shell_x + 36, sub_y + 88, shell_w - 72, day_text, 3, 0);

  const int gap = 10;
  const int content_x = shell_x + 18;
  const int content_y = sub_y + 132;
  const int content_w = shell_w - 36;
  const int content_h = shell_y + shell_h - content_y - 18;
  const int center_w = (content_w - gap * 2) / 4;
  const int side_w = (content_w - center_w - gap * 2) / 2;
  const int left_x = content_x;
  const int center_x = left_x + side_w + gap;
  const int right_x = center_x + center_w + gap;
  const int card_h = (content_h - gap * 2) / 3;

  drawRadialMetric(canvas, left_x, content_y, side_w, card_h, "PROTEIN", dashboard->protein_g, 100, "g");
  drawRadialMetricTenths(canvas, left_x, content_y + card_h + gap, side_w, card_h, "WATER", dashboard->water_tenths, dashboard->water_target_tenths, "L");
  drawRadialMetricTenths(canvas, left_x, content_y + (card_h + gap) * 2, side_w, card_h, "SLEEP", dashboard->sleep_tenths, dashboard->sleep_target_tenths, "h");
  drawChallengeStreakCard(canvas, center_x, content_y, center_w, content_h, dashboard->challenge_day);
  drawRadialMetric(canvas, right_x, content_y, side_w, card_h, "WORKOUT", dashboard->workouts, dashboard->workout_target, "done");
  drawRadialMetric(canvas, right_x, content_y + card_h + gap, side_w, card_h, "STEPS", dashboard->steps, dashboard->steps_target, dashboard->steps_unit);
  drawRadialMetric(canvas, right_x, content_y + (card_h + gap) * 2, side_w, card_h, "CALORIES", dashboard->calories, dashboard->calories_target, dashboard->calories_unit);
}

void drawCurrentDashboard(Canvas* canvas, const Dashboard* dashboard, const char* status) {
  if (g_active_challenge) {
    drawChallengeDashboard(canvas, dashboard, status);
    return;
  }
  if (g_active_recipe >= 0) {
    if (g_active_recipe_library) drawRecipeRecordDashboard(canvas, dashboard, status, g_active_recipe);
    else drawRecipeDashboard(canvas, dashboard, status, g_active_recipe);
    return;
  }
  if (g_active_recipes) {
    drawRecipesDashboard(canvas, dashboard, status);
    return;
  }
  if (g_active_meal_planner) {
    drawMealPlannerDashboard(canvas, dashboard, status);
    return;
  }
  if (g_active_list >= 0 && g_active_list < dashboard->list_count) {
    drawFullListDashboard(canvas, dashboard, g_active_list, status);
    return;
  }
  drawBitmapDashboard(canvas, dashboard, status);
}

Rect exitButtonRectForScreen(int width, int) {
  const int shell_w = width;
  const int shell_x = 0;
  Rect rect;
  rect.w = 172;
  rect.h = 96;
  rect.x = shell_x + shell_w - rect.w - 28;
  rect.y = kKindleStatusBarHeight + 20;
  return rect;
}

int containsPoint(const Rect* rect, int x, int y) {
  return rect && x >= rect->x && y >= rect->y && x < rect->x + rect->w && y < rect->y + rect->h;
}

void clearTouchRegions() {
  memset(g_touch_regions, 0, sizeof(g_touch_regions));
  g_touch_region_count = 0;
}

void setPendingTouchRect(Rect rect) {
  g_pending_touch_rect = rect;
  g_pending_touch_rect_valid = rect.w > 0 && rect.h > 0 ? 1 : 0;
}

void addTouchRegion(Rect rect, TouchAction action, int list_index, int item_index, const char* item_id, int item_done) {
  if (g_touch_region_count >= kMaxTouchRegions) return;
  TouchRegion* region = &g_touch_regions[g_touch_region_count++];
  region->rect = rect;
  region->action = action;
  region->list_index = list_index;
  region->item_index = item_index;
  copyText(region->item_id, sizeof(region->item_id), item_id ? item_id : "");
  region->item_done = item_done;
}

[[maybe_unused]] int applyTouchAt(int x, int y) {
  for (int i = g_touch_region_count - 1; i >= 0; i--) {
    TouchRegion* region = &g_touch_regions[i];
    if (!containsPoint(&region->rect, x, y)) continue;
    g_pending_action = region->action;
    g_pending_list_index = region->list_index;
    g_pending_recipe_index = region->item_index;
    copyText(g_pending_item_id, sizeof(g_pending_item_id), region->item_id);
    g_pending_item_done = region->item_done;
    setPendingTouchRect(region->rect);
    return 1;
  }
  return 0;
}

void drawBitmapDashboard(Canvas* canvas, const Dashboard* dashboard, const char* status) {
  clearCanvas(canvas, 255);
  clearTouchRegions();
  g_last_screen_width = canvas->width;
  g_last_screen_height = canvas->height;
  const int shell_w = canvas->width;
  const int shell_x = 0;
  const int shell_y = kKindleStatusBarHeight;
  const int shell_h = canvas->height - shell_y;
  strokeRect(canvas, shell_x, shell_y, shell_w, shell_h, 3, 0);

  const int header_h = 132;
  doubleRect(canvas, shell_x + 10, shell_y + 10, shell_w - 20, header_h, 0);
  const int arrow_left = drawDaySwitchArrows(canvas, shell_x, shell_y, shell_w);
  drawTextClipped(canvas, shell_x + 28, shell_y + 24, arrow_left - shell_x - 44, "DAILY OPS", 4, 0);
  Rect exit_rect = exitButtonRectForScreen(canvas->width, canvas->height);
  Rect exit_hit_rect = {exit_rect.x - 20, kKindleStatusBarHeight, exit_rect.w + 40, exit_rect.y - kKindleStatusBarHeight + exit_rect.h + 20};
  strokeRect(canvas, exit_rect.x, exit_rect.y, exit_rect.w, exit_rect.h, 2, 0);
  drawTextCentered(canvas, exit_rect.x + exit_rect.w / 2, exit_rect.y + 34, exit_rect.w - 16, "EXIT", 3, 0);
  addTouchRegion(exit_hit_rect, kTouchExit, -1, -1, "", 0);
  addTouchRegion(exit_rect, kTouchExit, -1, -1, "", 0);
  line(canvas, shell_x + 20, shell_y + 88, exit_rect.x - 8, shell_y + 88, 2, 0);
  char updated[96];
  formatDisplayDate(dashboard->generated_at, status, updated, sizeof(updated));
  drawTextClipped(canvas, shell_x + 28, shell_y + 102, exit_rect.x - shell_x - 44, updated, 2, 0);

  const int gap = 8;
  const int stat_y = shell_y + 10 + header_h + gap;
  const int stat_h = shell_h < 900 ? 220 : 252;
  const int stat_w = (shell_w - 20 - gap * 2) / 3;
  drawImageCard(canvas, shell_x + 10, stat_y, stat_w, stat_h, kProfileCardPath, kProfileCardLocalPath);
  drawRadialMetric(canvas, shell_x + 10 + stat_w + gap, stat_y, stat_w, stat_h, "STEPS", dashboard->steps, dashboard->steps_target, dashboard->steps_unit);
  drawRadialMetric(canvas, shell_x + 10 + (stat_w + gap) * 2, stat_y, stat_w, stat_h, "CALORIES", dashboard->calories, dashboard->calories_target, dashboard->calories_unit);

  const int footer_h = 44;
  const int lists_y = stat_y + stat_h + gap;
  const int lists_h = shell_y + shell_h - lists_y - footer_h - gap - 10;
  const int list_w = (shell_w - 20 - gap) / 2;
  if (dashboard->list_count > 0) {
    int challenge_side = list_w;
    if (lists_h < challenge_side + 128) challenge_side = lists_h - 128;
    if (challenge_side < 160) challenge_side = 160;
    const int challenge_gap = gap;
    const int chores_h = lists_h - challenge_side - challenge_gap;
    drawListCard(canvas, shell_x + 10, lists_y, list_w, chores_h, &dashboard->lists[0], 0);
    drawChallengeTile(canvas, shell_x + 10, lists_y + chores_h + challenge_gap, challenge_side);
  }
  if (dashboard->list_count > 1) {
    const int right_x = shell_x + 10 + list_w + gap;
    int challenge_side = list_w;
    if (lists_h < challenge_side + 128) challenge_side = lists_h - 128;
    if (challenge_side < 160) challenge_side = 160;
    int meal_tile_h = lists_h - challenge_side - gap;
    const int grocery_h = lists_h - meal_tile_h - gap;
    drawMealPlannerTile(canvas, right_x, lists_y, list_w, meal_tile_h);
    drawListCard(canvas, right_x, lists_y + meal_tile_h + gap, list_w, grocery_h, &dashboard->lists[1], 1);
  }

  doubleRect(canvas, shell_x + 10, shell_y + shell_h - footer_h - 10, shell_w - 20, footer_h, 0);
  drawTextClipped(canvas, shell_x + 28, shell_y + shell_h - footer_h - 2, shell_w - 56, "TELEGRAM UPDATES LISTS // AUTO REFRESH 15M", 3, 0);
}

int writePgm(const char* path, const Canvas* canvas) {
  FILE* file = fopen(path, "wb");
  if (!file) return 0;
  fprintf(file, "P5\n%d %d\n255\n", canvas->width, canvas->height);
  const size_t bytes = static_cast<size_t>(canvas->width) * static_cast<size_t>(canvas->height);
  const int ok = fwrite(canvas->pixels, 1, bytes, file) == bytes;
  fclose(file);
  return ok;
}

#ifdef __linux__
struct TouchInput {
  struct Device {
    int fd;
    int grabbed;
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    int has_x_range;
    int has_y_range;
  } devices[16];
  int count;
  int x;
  int y;
  int has_x;
  int has_y;
  int was_down;
  long last_action_ms;
};

long nowMs() {
  timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

int applyTouchWithDebounce(TouchInput* input) {
  const long now = nowMs();
  if (now - input->last_action_ms < 700) return 0;
  const int w = g_last_screen_width;
  const int h = g_last_screen_height;
  const int x = input->x;
  const int y = input->y;

  if (x >= w - 280 && y >= kKindleStatusBarHeight && y <= kKindleStatusBarHeight + 160) {
    g_pending_action = kTouchExit;
    setPendingTouchRect(exitButtonRectForScreen(w, h));
  } else if (!applyTouchAt(x, y) &&
             !applyTouchAt(w - 1 - x, y) &&
             !applyTouchAt(x, h - 1 - y) &&
             !applyTouchAt(w - 1 - x, h - 1 - y) &&
             !applyTouchAt((static_cast<long>(y) * w) / (h > 1 ? h : 1), (static_cast<long>(x) * h) / (w > 1 ? w : 1)) &&
             !applyTouchAt(w - 1 - (static_cast<long>(y) * w) / (h > 1 ? h : 1), (static_cast<long>(x) * h) / (w > 1 ? w : 1)) &&
             !applyTouchAt((static_cast<long>(y) * w) / (h > 1 ? h : 1), h - 1 - (static_cast<long>(x) * h) / (w > 1 ? w : 1)) &&
             !applyTouchAt(w - 1 - (static_cast<long>(y) * w) / (h > 1 ? h : 1), h - 1 - (static_cast<long>(x) * h) / (w > 1 ? w : 1))) {
    fprintf(stderr, "input=miss x=%d y=%d width=%d height=%d regions=%d\n", x, y, w, h, g_touch_region_count);
    return 0;
  }
  g_pending_touch_x = x;
  g_pending_touch_y = y;
  input->last_action_ms = now;
  return 1;
}

int readAbsRange(int fd, int code, int* minimum, int* maximum) {
  input_absinfo abs_info;
  memset(&abs_info, 0, sizeof(abs_info));
  if (ioctl(fd, EVIOCGABS(code), &abs_info) != 0) return 0;
  if (abs_info.maximum <= abs_info.minimum) return 0;
  *minimum = abs_info.minimum;
  *maximum = abs_info.maximum;
  return 1;
}

int scaleAbsValue(int value, int minimum, int maximum, int screen_size) {
  if (maximum <= minimum || screen_size <= 1) return value;
  long scaled = (static_cast<long>(value - minimum) * static_cast<long>(screen_size - 1)) / static_cast<long>(maximum - minimum);
  if (scaled < 0) scaled = 0;
  if (scaled >= screen_size) scaled = screen_size - 1;
  return static_cast<int>(scaled);
}

void initTouchInput(TouchInput* input) {
  memset(input, 0, sizeof(*input));
  input->x = -1;
  input->y = -1;
  for (int i = 0; i < 16 && input->count < 16; i++) {
    char path[48];
    snprintf(path, sizeof(path), "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;

    TouchInput::Device* device = &input->devices[input->count];
    memset(device, 0, sizeof(*device));
    device->fd = fd;
    device->has_x_range = readAbsRange(fd, ABS_X, &device->min_x, &device->max_x) ||
                          readAbsRange(fd, ABS_MT_POSITION_X, &device->min_x, &device->max_x);
    device->has_y_range = readAbsRange(fd, ABS_Y, &device->min_y, &device->max_y) ||
                          readAbsRange(fd, ABS_MT_POSITION_Y, &device->min_y, &device->max_y);

    if (!device->has_x_range || !device->has_y_range) {
      close(fd);
      continue;
    }

    if (ioctl(fd, EVIOCGRAB, 1) == 0) device->grabbed = 1;
    fprintf(stderr, "input=device path=%s grabbed=%d xrange=%d..%d yrange=%d..%d\n",
            path, device->grabbed, device->min_x, device->max_x, device->min_y, device->max_y);
    input->count++;
  }
  fprintf(stderr, "input=opened count=%d\n", input->count);
}

void closeTouchInput(TouchInput* input) {
  for (int i = 0; i < input->count; i++) {
    if (input->devices[i].grabbed) ioctl(input->devices[i].fd, EVIOCGRAB, 0);
    close(input->devices[i].fd);
  }
  input->count = 0;
}

int pollExitTouch(TouchInput* input) {
  if (!input || input->count <= 0) return 0;
  for (int i = 0; i < input->count; i++) {
    TouchInput::Device* device = &input->devices[i];
    while (1) {
      input_event event;
      const ssize_t bytes = read(device->fd, &event, sizeof(event));
      if (bytes != sizeof(event)) {
        if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          // Keep the fd open; intermittent event read errors are not fatal to rendering.
        }
        break;
      }

      if (event.type == EV_ABS) {
        if (event.code == ABS_X || event.code == ABS_MT_POSITION_X) {
          input->x = scaleAbsValue(event.value, device->min_x, device->max_x, g_last_screen_width);
          input->has_x = 1;
          input->was_down = 1;
        } else if (event.code == ABS_Y || event.code == ABS_MT_POSITION_Y) {
          input->y = scaleAbsValue(event.value, device->min_y, device->max_y, g_last_screen_height);
          input->has_y = 1;
          input->was_down = 1;
        } else if (event.code == ABS_MT_TRACKING_ID) {
          input->was_down = event.value >= 0 ? 1 : 0;
        }
      } else if (event.type == EV_KEY && (event.code == BTN_TOUCH || event.code == BTN_LEFT)) {
        if (event.value > 0) input->was_down = 1;
        if (event.value == 0 && input->was_down && input->has_x && input->has_y) {
          input->was_down = 0;
          if (applyTouchWithDebounce(input)) {
            fprintf(stderr, "input=action tap action=%d x=%d y=%d\n", static_cast<int>(g_pending_action), input->x, input->y);
            return 1;
          }
        }
      } else if (event.type == EV_SYN && input->has_x && input->has_y) {
        if (applyTouchWithDebounce(input)) {
          fprintf(stderr, "input=action touch action=%d x=%d y=%d\n", static_cast<int>(g_pending_action), input->x, input->y);
          return 1;
        }
      }
    }
  }
  return 0;
}
#else
struct TouchInput {
  int unused;
};

void initTouchInput(TouchInput*) {}
void closeTouchInput(TouchInput*) {}
[[maybe_unused]] int pollExitTouch(TouchInput*) { return 0; }
#endif

#ifdef __linux__
void putFramebufferPixel(unsigned char* fb, const fb_var_screeninfo* vinfo, const fb_fix_screeninfo* finfo, int x, int y, unsigned char gray) {
  if (x < 0 || y < 0 || x >= static_cast<int>(vinfo->xres) || y >= static_cast<int>(vinfo->yres)) return;
  if (vinfo->bits_per_pixel == 4) {
    const long location = static_cast<long>(y + vinfo->yoffset) * finfo->line_length +
                          static_cast<long>(x + vinfo->xoffset) / 2;
    const unsigned char nibble = static_cast<unsigned char>(gray >> 4);
    if (((x + vinfo->xoffset) & 1) == 0) {
      fb[location] = static_cast<unsigned char>((fb[location] & 0x0f) | (nibble << 4));
    } else {
      fb[location] = static_cast<unsigned char>((fb[location] & 0xf0) | nibble);
    }
    return;
  }

  if (vinfo->bits_per_pixel == 1) {
    const long location = static_cast<long>(y + vinfo->yoffset) * finfo->line_length +
                          static_cast<long>(x + vinfo->xoffset) / 8;
    const unsigned char mask = static_cast<unsigned char>(0x80 >> ((x + vinfo->xoffset) & 7));
    if (gray < 128) fb[location] &= static_cast<unsigned char>(~mask);
    else fb[location] |= mask;
    return;
  }

  const long location = static_cast<long>(x + vinfo->xoffset) * (vinfo->bits_per_pixel / 8) +
                        static_cast<long>(y + vinfo->yoffset) * finfo->line_length;
  if (vinfo->bits_per_pixel == 8) {
    fb[location] = gray;
  } else if (vinfo->bits_per_pixel == 16) {
    const unsigned short value = static_cast<unsigned short>(((gray >> 3) << 11) | ((gray >> 2) << 5) | (gray >> 3));
    memcpy(fb + location, &value, sizeof(value));
  } else if (vinfo->bits_per_pixel == 32) {
    const unsigned int value = 0xff000000u | (static_cast<unsigned int>(gray) << 16) | (static_cast<unsigned int>(gray) << 8) | gray;
    memcpy(fb + location, &value, sizeof(value));
  }
}

unsigned char getFramebufferPixel(unsigned char* fb, const fb_var_screeninfo* vinfo, const fb_fix_screeninfo* finfo, int x, int y) {
  if (x < 0 || y < 0 || x >= static_cast<int>(vinfo->xres) || y >= static_cast<int>(vinfo->yres)) return 255;

  if (vinfo->bits_per_pixel == 4) {
    const long location = static_cast<long>(y + vinfo->yoffset) * finfo->line_length +
                          static_cast<long>(x + vinfo->xoffset) / 2;
    const unsigned char value = fb[location];
    const unsigned char nibble = ((x + vinfo->xoffset) & 1) == 0
      ? static_cast<unsigned char>(value >> 4)
      : static_cast<unsigned char>(value & 0x0f);
    return static_cast<unsigned char>(nibble * 17);
  }

  if (vinfo->bits_per_pixel == 1) {
    const long location = static_cast<long>(y + vinfo->yoffset) * finfo->line_length +
                          static_cast<long>(x + vinfo->xoffset) / 8;
    const unsigned char mask = static_cast<unsigned char>(0x80 >> ((x + vinfo->xoffset) & 7));
    return (fb[location] & mask) ? 255 : 0;
  }

  const long location = static_cast<long>(x + vinfo->xoffset) * (vinfo->bits_per_pixel / 8) +
                        static_cast<long>(y + vinfo->yoffset) * finfo->line_length;
  if (vinfo->bits_per_pixel == 8) return fb[location];
  if (vinfo->bits_per_pixel == 16) {
    unsigned short value = 0;
    memcpy(&value, fb + location, sizeof(value));
    const int red = ((value >> 11) & 31) * 255 / 31;
    const int green = ((value >> 5) & 63) * 255 / 63;
    const int blue = (value & 31) * 255 / 31;
    return static_cast<unsigned char>((red + green + blue) / 3);
  }
  if (vinfo->bits_per_pixel == 32) {
    const unsigned char red = fb[location + 2];
    const unsigned char green = fb[location + 1];
    const unsigned char blue = fb[location];
    return static_cast<unsigned char>((static_cast<int>(red) + green + blue) / 3);
  }
  return 255;
}

void invertFramebufferArea(unsigned char* fb, const fb_var_screeninfo* vinfo, const fb_fix_screeninfo* finfo, int left, int top, int right, int bottom, int inset) {
  for (int y = top; y < bottom; y++) {
    for (int x = left; x < right; x++) {
      if (inset > 0 && (x < left + inset || x >= right - inset || y < top + inset || y >= bottom - inset)) continue;
      const unsigned char current = getFramebufferPixel(fb, vinfo, finfo, x, y);
      putFramebufferPixel(fb, vinfo, finfo, x, y, static_cast<unsigned char>(255 - current));
    }
  }
}

void flashTouchRectOnFramebuffer(Rect rect) {
  if (rect.w <= 0 || rect.h <= 0) return;
  if (rect.y < kKindleStatusBarHeight) {
    const int shift = kKindleStatusBarHeight - rect.y;
    rect.y += shift;
    rect.h -= shift;
  }
  if (rect.w <= 0 || rect.h <= 0) return;

  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) {
    fprintf(stderr, "visual-feedback=framebuffer open_failed\n");
    return;
  }

  fb_var_screeninfo vinfo;
  fb_fix_screeninfo finfo;
  if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
    fprintf(stderr, "visual-feedback=framebuffer ioctl_failed\n");
    close(fd);
    return;
  }

  const long screensize = static_cast<long>(finfo.line_length) * static_cast<long>(vinfo.yres_virtual ? vinfo.yres_virtual : vinfo.yres);
  unsigned char* fb = static_cast<unsigned char*>(mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (fb == MAP_FAILED) {
    fprintf(stderr, "visual-feedback=framebuffer mmap_failed\n");
    close(fd);
    return;
  }

  const int left = rect.x < 0 ? 0 : rect.x;
  const int top = rect.y < kKindleStatusBarHeight ? kKindleStatusBarHeight : rect.y;
  const int right = rect.x + rect.w > static_cast<int>(vinfo.xres) ? static_cast<int>(vinfo.xres) : rect.x + rect.w;
  const int bottom = rect.y + rect.h > static_cast<int>(vinfo.yres) ? static_cast<int>(vinfo.yres) : rect.y + rect.h;
  invertFramebufferArea(fb, &vinfo, &finfo, left, top, right, bottom, 0);
  system("eips '' >/dev/null 2>&1 || true");
  usleep(120000);
  invertFramebufferArea(fb, &vinfo, &finfo, left, top, right, bottom, 0);
  munmap(fb, screensize);
  close(fd);
  system("eips '' >/dev/null 2>&1 || true");
  fprintf(stderr, "visual-feedback=blink rect=%d,%d,%d,%d\n", rect.x, rect.y, rect.w, rect.h);
}

int renderToFramebuffer(const Dashboard* dashboard, const char* status, const char* save_pgm) {
  int fd = open("/dev/fb0", O_RDWR);
  if (fd < 0) {
    fprintf(stderr, "render=framebuffer open_failed\n");
    return 0;
  }
  fb_var_screeninfo vinfo;
  fb_fix_screeninfo finfo;
  if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) != 0 || ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
    fprintf(stderr, "render=framebuffer ioctl_failed\n");
    close(fd);
    return 0;
  }
  if (vinfo.bits_per_pixel != 1 && vinfo.bits_per_pixel != 4 && vinfo.bits_per_pixel != 8 &&
      vinfo.bits_per_pixel != 16 && vinfo.bits_per_pixel != 32) {
    fprintf(stderr, "render=framebuffer unsupported_bpp width=%d height=%d bpp=%d line=%d\n",
            static_cast<int>(vinfo.xres), static_cast<int>(vinfo.yres), static_cast<int>(vinfo.bits_per_pixel), static_cast<int>(finfo.line_length));
    close(fd);
    return 0;
  }
  const long screensize = static_cast<long>(finfo.line_length) * static_cast<long>(vinfo.yres_virtual ? vinfo.yres_virtual : vinfo.yres);
  unsigned char* fb = static_cast<unsigned char*>(mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (fb == MAP_FAILED) {
    fprintf(stderr, "render=framebuffer mmap_failed\n");
    close(fd);
    return 0;
  }

  Canvas canvas;
  canvas.width = static_cast<int>(vinfo.xres);
  canvas.height = static_cast<int>(vinfo.yres);
  canvas.pixels = static_cast<unsigned char*>(calloc(static_cast<size_t>(canvas.width) * static_cast<size_t>(canvas.height), 1));
  if (!canvas.pixels) {
    fprintf(stderr, "render=framebuffer alloc_failed\n");
    munmap(fb, screensize);
    close(fd);
    return 0;
  }
  drawCurrentDashboard(&canvas, dashboard, status);
  if (save_pgm && save_pgm[0]) {
    writePgm(save_pgm, &canvas);
    fprintf(stderr, "render=save-pgm %s width=%d height=%d\n", save_pgm, canvas.width, canvas.height);
  }
  for (int y = kKindleStatusBarHeight; y < canvas.height; y++) {
    for (int x = 0; x < canvas.width; x++) putFramebufferPixel(fb, &vinfo, &finfo, x, y, canvas.pixels[y * canvas.width + x]);
  }
  free(canvas.pixels);
  munmap(fb, screensize);
  close(fd);
  system("eips '' >/dev/null 2>&1 || true");
  fprintf(stderr, "render=framebuffer ok width=%d height=%d bpp=%d\n", static_cast<int>(vinfo.xres), static_cast<int>(vinfo.yres), static_cast<int>(vinfo.bits_per_pixel));
  return 1;
}
#else
int renderToFramebuffer(const Dashboard*, const char*, const char*) {
  fprintf(stderr, "render=framebuffer unavailable\n");
  return 0;
}

void flashTouchRectOnFramebuffer(Rect) {}
#endif

int commandExists(const char* command) {
  char probe[160];
  snprintf(probe, sizeof(probe), "command -v '%s' >/dev/null 2>&1", command);
  return system(probe) == 0;
}

void shellQuote(const char* text, char* out, size_t out_size) {
  size_t j = 0;
  if (j + 1 < out_size) out[j++] = '\'';
  for (size_t i = 0; text[i] && j + 5 < out_size; i++) {
    if (text[i] == '\'') {
      out[j++] = '\'';
      out[j++] = '\\';
      out[j++] = '\'';
      out[j++] = '\'';
    } else {
      out[j++] = text[i];
    }
  }
  if (j + 1 < out_size) out[j++] = '\'';
  out[j] = '\0';
}

void renderToEips(char lines[][96], int count) {
  if (!commandExists("eips")) {
    for (int i = 0; i < count; i++) printf("%s\n", lines[i]);
    fflush(stdout);
    return;
  }
  for (int i = 0; i < count; i++) {
    char quoted[180];
    char command[240];
    shellQuote(lines[i], quoted, sizeof(quoted));
    snprintf(command, sizeof(command), "eips 1 %d %s >/dev/null 2>&1", i + 3, quoted);
    system(command);
  }
}

void showTouchVisualFeedback(TouchAction action, int x, int y) {
  if (action == kTouchNone) return;
  fprintf(stderr, "visual-feedback=tap action=%d x=%d y=%d\n", static_cast<int>(action), x, y);
  if (g_pending_touch_rect_valid) flashTouchRectOnFramebuffer(g_pending_touch_rect);
}

void returnToKindleHome() {
  fprintf(stderr, "exit=return-home\n");
  system(
    "lipc-set-prop com.lab126.powerd preventScreenSaver 0 >/dev/null 2>&1 || true; "
    "lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home >/dev/null 2>&1 || "
    "lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home/ >/dev/null 2>&1 || true; "
    "sleep 1; "
    "eips '' >/dev/null 2>&1 || true"
  );
}

int renderViaFbink(const Dashboard* dashboard, const char* status, const char* save_pgm) {
  (void)dashboard;
  (void)status;
  (void)save_pgm;
  fprintf(stderr, "render=fbink skipped preserve_status_bar\n");
  return 0;
#if 0
  if (!commandExists("fbink")) {
    fprintf(stderr, "render=fbink unavailable\n");
    return 0;
  }

  Canvas canvas;
  canvas.width = kBitmapFallbackWidth;
  canvas.height = kBitmapFallbackHeight;
  canvas.pixels = static_cast<unsigned char*>(calloc(static_cast<size_t>(canvas.width) * static_cast<size_t>(canvas.height), 1));
  if (!canvas.pixels) {
    fprintf(stderr, "render=fbink alloc_failed\n");
    return 0;
  }

  drawCurrentDashboard(&canvas, dashboard, status);
  const char* path = "/tmp/kindle-dashboard-render.pgm";
  if (!writePgm(path, &canvas)) {
    free(canvas.pixels);
    fprintf(stderr, "render=fbink pgm_failed\n");
    return 0;
  }
  if (save_pgm && save_pgm[0]) {
    writePgm(save_pgm, &canvas);
    fprintf(stderr, "render=save-pgm %s width=%d height=%d\n", save_pgm, canvas.width, canvas.height);
  }
  free(canvas.pixels);

  char quoted_path[180];
  char command[420];
  shellQuote(path, quoted_path, sizeof(quoted_path));
  snprintf(command, sizeof(command), "fbink -c -W GC16 -g file=%s,w=-1,h=-1,dither >/dev/null", quoted_path);
  const int status_code = system(command);
  if (status_code == 0) {
    fprintf(stderr, "render=fbink ok image=%s\n", path);
    return 1;
  }

  fprintf(stderr, "render=fbink failed status=%d\n", status_code);
  return 0;
#endif
}

int fetchToCache(const char* url, const char* read_token, const char* cache) {
  const long long started = monotonicMs();
  char tmp[320];
  snprintf(tmp, sizeof(tmp), "%s.tmp", cache);
  char quoted_tmp[400];
  char quoted_url[400];
  char quoted_header[260];
  char command[1100];
  shellQuote(tmp, quoted_tmp, sizeof(quoted_tmp));
  shellQuote(url, quoted_url, sizeof(quoted_url));
  char header[200];
  header[0] = '\0';
  if (read_token && read_token[0]) {
    snprintf(header, sizeof(header), "X-Dashboard-Read-Token: %.150s", read_token);
    shellQuote(header, quoted_header, sizeof(quoted_header));
  } else {
    quoted_header[0] = '\0';
  }

  if (commandExists("curl")) {
    snprintf(command, sizeof(command), "curl -fsSL --connect-timeout 20 --max-time 55 --max-filesize %ld %s%s%s -o %s %s",
             kMaxDashboardPayloadBytes,
             quoted_header[0] ? "-H " : "",
             quoted_header[0] ? quoted_header : "",
             quoted_header[0] ? " " : "",
             quoted_tmp,
             quoted_url);
  } else if (commandExists("wget")) {
    snprintf(command, sizeof(command), "wget -q -T 55 %s%s%s -O %s %s",
             quoted_header[0] ? "--header=" : "",
             quoted_header[0] ? quoted_header : "",
             quoted_header[0] ? " " : "",
             quoted_tmp,
             quoted_url);
  } else {
    return 0;
  }

  if (system(command) != 0) {
    remove(tmp);
    fprintf(stderr, "timing=fetch ok=0 ms=%lld\n", monotonicMs() - started);
    return 0;
  }
  if (rename(tmp, cache) != 0) {
    remove(tmp);
    fprintf(stderr, "timing=fetch ok=0 ms=%lld\n", monotonicMs() - started);
    return 0;
  }
  char* payload_check = readFile(cache);
  if (!payload_check) {
    remove(cache);
    fprintf(stderr, "timing=fetch ok=0 oversized_or_unreadable ms=%lld\n", monotonicMs() - started);
    return 0;
  }
  free(payload_check);
  fprintf(stderr, "timing=fetch ok=1 ms=%lld\n", monotonicMs() - started);
  return 1;
}

int writeTextFileAtomic(const char* path, const char* data, size_t size) {
  if (!path || !path[0] || !data) return 0;
  char tmp[320];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE* file = fopen(tmp, "wb");
  if (!file) return 0;
  const int ok = fwrite(data, 1, size, file) == size;
  fclose(file);
  if (!ok) {
    remove(tmp);
    return 0;
  }
  if (rename(tmp, path) != 0) {
    remove(tmp);
    return 0;
  }
  return 1;
}

const char* findItemObjectById(const char* payload, const char* item_id, const char** object_end) {
  if (!payload || !item_id || !item_id[0]) return NULL;
  const char* cursor = payload;
  while ((cursor = strstr(cursor, item_id)) != NULL) {
    const char* object_start = cursor;
    while (object_start > payload && *object_start != '{') object_start--;
    if (*object_start != '{') {
      cursor += strlen(item_id);
      continue;
    }

    const char* end = matchingClose(object_start, '}');
    if (!end || cursor > end) {
      cursor += strlen(item_id);
      continue;
    }

    char parsed_id[48];
    extractString(object_start, end, "id", parsed_id, sizeof(parsed_id), "");
    if (strcmp(parsed_id, item_id) == 0) {
      if (object_end) *object_end = end;
      return object_start;
    }
    cursor += strlen(item_id);
  }
  return NULL;
}

int patchCachedItemDone(const char* cache, const char* item_id, int done) {
  char* payload = readFile(cache);
  if (!payload) return 0;

  const char* object_end = NULL;
  const char* object_start = findItemObjectById(payload, item_id, &object_end);
  const char* done_value = object_start ? findKeyInRange(object_start, object_end, "done") : NULL;
  if (!done_value || (strncmp(done_value, "true", 4) != 0 && strncmp(done_value, "false", 5) != 0)) {
    free(payload);
    return 0;
  }

  const char* replacement = done ? "true" : "false";
  const size_t old_value_len = strncmp(done_value, "true", 4) == 0 ? 4 : 5;
  const size_t replacement_len = strlen(replacement);
  const size_t payload_len = strlen(payload);
  const size_t prefix_len = static_cast<size_t>(done_value - payload);
  const size_t suffix_offset = prefix_len + old_value_len;
  const size_t suffix_len = payload_len - suffix_offset;
  const size_t next_len = prefix_len + replacement_len + suffix_len;

  char* next_payload = static_cast<char*>(malloc(next_len + 1));
  if (!next_payload) {
    free(payload);
    return 0;
  }
  memcpy(next_payload, payload, prefix_len);
  memcpy(next_payload + prefix_len, replacement, replacement_len);
  memcpy(next_payload + prefix_len + replacement_len, payload + suffix_offset, suffix_len);
  next_payload[next_len] = '\0';

  const int ok = writeTextFileAtomic(cache, next_payload, next_len);
  free(next_payload);
  free(payload);
  fprintf(stderr, "toggle=optimistic-cache ok=%d id=%s done=%d\n", ok, item_id, done);
  return ok;
}

void buildDashboardUrl(const char* base_url, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  if (!base_url) base_url = "";
  if (g_day_offset == 0) {
    snprintf(out, out_size, "%s", base_url);
    return;
  }
  const char separator = strchr(base_url, '?') ? '&' : '?';
  snprintf(out, out_size, "%s%coffset=%d", base_url, separator, g_day_offset);
}

int postToggleItemAsync(const char* toggle_url, const char* toggle_token, const char* item_id, int done) {
  if (!toggle_url || !toggle_url[0] || !toggle_token || !toggle_token[0] || !item_id || !item_id[0]) {
    fprintf(stderr, "toggle=post-skipped missing_config id=%s\n", item_id ? item_id : "");
    return 0;
  }

  char escaped_id[120];
  jsonEscapeString(item_id, escaped_id, sizeof(escaped_id));
  char body[180];
  snprintf(body, sizeof(body), "{\"id\":\"%s\",\"done\":%s}", escaped_id, done ? "true" : "false");
  char header[200];
  snprintf(header, sizeof(header), "X-Dashboard-Toggle-Token: %.150s", toggle_token);

  char quoted_body[240];
  char quoted_header[260];
  char quoted_url[400];
  char command[900];
  shellQuote(body, quoted_body, sizeof(quoted_body));
  shellQuote(header, quoted_header, sizeof(quoted_header));
  shellQuote(toggle_url, quoted_url, sizeof(quoted_url));
  if (commandExists("curl")) {
    snprintf(command, sizeof(command),
             "curl -fsSL --connect-timeout 5 --max-time 12 -X POST -H 'Content-Type: application/json' -H %s -d %s %s >/dev/null 2>&1 &",
             quoted_header, quoted_body, quoted_url);
  } else if (commandExists("wget")) {
    snprintf(command, sizeof(command),
             "wget -q -T 12 --header='Content-Type: application/json' --header=%s --post-data=%s -O /dev/null %s >/dev/null 2>&1 &",
             quoted_header, quoted_body, quoted_url);
  } else {
    fprintf(stderr, "toggle=post-skipped missing_http_client id=%s\n", item_id);
    return 0;
  }

  const int status = system(command);
  if (status != 0) fprintf(stderr, "toggle=post-start-failed status=%d id=%s\n", status, item_id);
  else fprintf(stderr, "toggle=post-background id=%s done=%d\n", item_id, done);
  return status == 0;
}

int handlePendingTouch(const Options* options) {
  const TouchAction action = g_pending_action;
  const int touch_x = g_pending_touch_x;
  const int touch_y = g_pending_touch_y;
  g_pending_action = kTouchNone;
  g_pending_touch_x = -1;
  g_pending_touch_y = -1;
  showTouchVisualFeedback(action, touch_x, touch_y);
  g_pending_touch_rect_valid = 0;

  if (action == kTouchExit) {
    fprintf(stderr, "touch=exit\n");
    returnToKindleHome();
    g_running = 0;
    return 1;
  }

  if (action == kTouchBack) {
    fprintf(stderr, "touch=back\n");
    if (g_active_recipe >= 0) {
      g_active_recipe = -1;
      if (g_active_recipe_return_meal_planner) {
        g_active_recipe_return_meal_planner = 0;
        g_active_recipe_library = 0;
        g_active_meal_planner = 1;
      } else if (g_active_recipe_library) {
        g_active_recipe_library = 0;
        g_active_recipes = 1;
      } else {
        g_active_meal_planner = 1;
      }
    } else if (g_active_recipes) {
      g_active_recipes = 0;
      g_active_meal_planner = 1;
    } else if (g_active_challenge) {
      g_active_challenge = 0;
    } else {
      g_active_meal_planner = 0;
      g_active_list = -1;
    }
    return 1;
  }

  if (action == kTouchHome) {
    fprintf(stderr, "touch=home\n");
    g_active_list = -1;
    g_active_meal_planner = 0;
    g_active_recipes = 0;
    g_active_recipe = -1;
    g_active_recipe_library = 0;
    g_active_recipe_return_meal_planner = 0;
    g_active_challenge = 0;
    return 1;
  }

  if (action == kTouchOpenList) {
    fprintf(stderr, "touch=open-list index=%d\n", g_pending_list_index);
    g_active_meal_planner = 0;
    g_active_recipes = 0;
    g_active_recipe = -1;
    g_active_recipe_library = 0;
    g_active_recipe_return_meal_planner = 0;
    g_active_challenge = 0;
    g_active_list = g_pending_list_index;
    return 1;
  }

  if (action == kTouchOpenMealPlanner) {
    fprintf(stderr, "touch=open-meal-planner\n");
    g_active_list = -1;
    g_active_recipes = 0;
    g_active_recipe = -1;
    g_active_recipe_library = 0;
    g_active_recipe_return_meal_planner = 0;
    g_active_challenge = 0;
    g_active_meal_planner = 1;
    return 1;
  }

  if (action == kTouchOpenRecipe) {
    fprintf(stderr, "touch=open-recipe index=%d\n", g_pending_recipe_index);
    g_active_list = -1;
    g_active_challenge = 0;
    if (g_active_recipes) {
      g_active_meal_planner = 0;
      g_active_recipe_library = 1;
      g_active_recipe_return_meal_planner = 0;
    } else {
      g_active_meal_planner = 1;
      g_active_recipe_library = 0;
      g_active_recipe_return_meal_planner = 0;
    }
    g_active_recipe = g_pending_recipe_index;
    return 1;
  }

  if (action == kTouchOpenMealPlanRecipe) {
    fprintf(stderr, "touch=open-meal-plan-recipe index=%d\n", g_pending_recipe_index);
    g_active_list = -1;
    g_active_challenge = 0;
    g_active_recipes = 0;
    g_active_meal_planner = 0;
    g_active_recipe_library = 1;
    g_active_recipe_return_meal_planner = 1;
    g_active_recipe = g_pending_recipe_index;
    return 1;
  }

  if (action == kTouchOpenRecipes) {
    fprintf(stderr, "touch=open-recipes\n");
    g_active_list = -1;
    g_active_meal_planner = 0;
    g_active_recipe = -1;
    g_active_recipe_library = 0;
    g_active_recipe_return_meal_planner = 0;
    g_active_challenge = 0;
    g_active_recipes = 1;
    return 1;
  }

  if (action == kTouchOpenChallenge) {
    fprintf(stderr, "touch=open-challenge\n");
    g_active_list = -1;
    g_active_meal_planner = 0;
    g_active_recipes = 0;
    g_active_recipe = -1;
    g_active_recipe_library = 0;
    g_active_recipe_return_meal_planner = 0;
    g_active_challenge = 1;
    return 1;
  }

  if (action == kTouchPreviousDay || action == kTouchNextDay) {
    g_day_offset += action == kTouchPreviousDay ? -1 : 1;
    if (g_day_offset < -365) g_day_offset = -365;
    if (g_day_offset > 365) g_day_offset = 365;
    fprintf(stderr, "touch=date-switch offset=%d\n", g_day_offset);
    g_manual_fetch_refresh = 1;
    g_event_refresh = 1;
    return 2;
  }

  if (action == kTouchToday) {
    g_day_offset = 0;
    fprintf(stderr, "touch=date-switch today offset=0\n");
    g_manual_fetch_refresh = 1;
    g_event_refresh = 1;
    return 2;
  }

  if (action == kTouchToggleItem) {
    const int next_done = g_pending_item_done ? 0 : 1;
    fprintf(stderr, "touch=toggle-list-item id=%s done=%d\n", g_pending_item_id, next_done);
    patchCachedItemDone(options->cache, g_pending_item_id, next_done);
    postToggleItemAsync(options->toggle_url, options->toggle_token, g_pending_item_id, next_done);
    return 1;
  }

  return 0;
}

#ifdef __linux__
struct TouchWatcherArgs {
  TouchInput* touch;
};

void* touchWatcherMain(void* raw) {
  TouchWatcherArgs* args = static_cast<TouchWatcherArgs*>(raw);
  TouchInput* touch = args ? args->touch : NULL;
  free(args);
  if (!touch) return NULL;

  while (g_running) {
    pollExitTouch(touch);
    usleep(250000);
  }
  return NULL;
}

void startTouchWatcher(TouchInput* touch) {
  if (!touch || touch->count <= 0) return;
  TouchWatcherArgs* args = static_cast<TouchWatcherArgs*>(calloc(1, sizeof(TouchWatcherArgs)));
  if (!args) return;
  args->touch = touch;
  pthread_t thread;
  if (pthread_create(&thread, NULL, touchWatcherMain, args) != 0) {
    fprintf(stderr, "input=thread_failed\n");
    free(args);
    return;
  }
  pthread_detach(thread);
  fprintf(stderr, "input=thread_started\n");
}
#else
void startTouchWatcher(TouchInput*) {}
#endif

struct EventWatcherArgs {
  char events_url[256];
  char read_token[160];
  int sleep_start_minute;
  int sleep_end_minute;
};

int parseClockMinute(const char* text, int* minute) {
  int hour = -1;
  int min = -1;
  char tail = '\0';
  if (!text || sscanf(text, "%d:%d%c", &hour, &min, &tail) != 2) return 0;
  if (hour < 0 || hour > 23 || min < 0 || min > 59) return 0;
  *minute = hour * 60 + min;
  return 1;
}

int parseSleepWindow(const char* text, int* start_minute, int* end_minute) {
  if (!text || !text[0] || strcmp(text, "off") == 0 || strcmp(text, "none") == 0) {
    *start_minute = -1;
    *end_minute = -1;
    return 1;
  }

  const char* dash = strchr(text, '-');
  if (!dash) return 0;

  char start[8];
  char end[8];
  const size_t start_len = static_cast<size_t>(dash - text);
  if (start_len == 0 || start_len >= sizeof(start)) return 0;
  const size_t end_len = strlen(dash + 1);
  if (end_len == 0 || end_len >= sizeof(end)) return 0;

  memcpy(start, text, start_len);
  start[start_len] = '\0';
  memcpy(end, dash + 1, end_len + 1);
  return parseClockMinute(start, start_minute) && parseClockMinute(end, end_minute);
}

int currentLocalMinute() {
  time_t now = time(NULL);
  struct tm local_time;
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS) || defined(__linux__)
  localtime_r(&now, &local_time);
#else
  struct tm* local_ptr = localtime(&now);
  if (!local_ptr) return 0;
  local_time = *local_ptr;
#endif
  return local_time.tm_hour * 60 + local_time.tm_min;
}

int inSleepWindow(int start_minute, int end_minute) {
  if (start_minute < 0 || end_minute < 0 || start_minute == end_minute) return 0;
  const int now = currentLocalMinute();
  if (start_minute < end_minute) return now >= start_minute && now < end_minute;
  return now >= start_minute || now < end_minute;
}

void* eventWatcherMain(void* raw) {
  EventWatcherArgs* args = static_cast<EventWatcherArgs*>(raw);
  if (!args || !args->events_url[0]) return NULL;
  if (!commandExists("curl")) {
    fprintf(stderr, "events=disabled missing_curl\n");
    free(args);
    return NULL;
  }

  char quoted_url[400];
  char quoted_header[260];
  shellQuote(args->events_url, quoted_url, sizeof(quoted_url));
  char header[200];
  header[0] = '\0';
  if (args->read_token[0]) {
    snprintf(header, sizeof(header), "X-Dashboard-Read-Token: %.150s", args->read_token);
    shellQuote(header, quoted_header, sizeof(quoted_header));
  } else {
    quoted_header[0] = '\0';
  }
  fprintf(stderr, "events=watching %s\n", args->events_url);

  while (g_running) {
    if (inSleepWindow(args->sleep_start_minute, args->sleep_end_minute)) {
      fprintf(stderr, "events=quiet\n");
      sleep(60);
      continue;
    }

    char command[900];
    snprintf(command, sizeof(command), "curl -fsSL --no-buffer --connect-timeout 20 --max-time 65 %s%s%s %s 2>/dev/null",
             quoted_header[0] ? "-H " : "",
             quoted_header[0] ? quoted_header : "",
             quoted_header[0] ? " " : "",
             quoted_url);
    FILE* stream = popen(command, "r");
    if (!stream) {
      fprintf(stderr, "events=popen_failed\n");
      sleep(10);
      continue;
    }

    char line_buffer[512];
    while (g_running && fgets(line_buffer, sizeof(line_buffer), stream)) {
      if (inSleepWindow(args->sleep_start_minute, args->sleep_end_minute)) {
        fprintf(stderr, "events=quiet close_stream=1\n");
        break;
      }
      if (strncmp(line_buffer, "event: planner", 14) == 0) {
        g_event_refresh = 1;
        fprintf(stderr, "events=planner refresh=1\n");
      } else if (strncmp(line_buffer, "event: planner-error", 20) == 0) {
        fprintf(stderr, "events=planner-error\n");
      }
    }

    const int status = pclose(stream);
    if (g_running) {
      fprintf(stderr, "events=reconnect status=%d\n", status);
      sleep(2);
    }
  }

  free(args);
  return NULL;
}

void startEventWatcher(const char* events_url, const char* read_token, int sleep_start_minute, int sleep_end_minute) {
  if (!events_url || !events_url[0]) {
    fprintf(stderr, "events=disabled empty_url\n");
    return;
  }

  EventWatcherArgs* args = static_cast<EventWatcherArgs*>(calloc(1, sizeof(EventWatcherArgs)));
  if (!args) {
    fprintf(stderr, "events=alloc_failed\n");
    return;
  }
  copyText(args->events_url, sizeof(args->events_url), events_url);
  if (read_token) copyText(args->read_token, sizeof(args->read_token), read_token);
  args->sleep_start_minute = sleep_start_minute;
  args->sleep_end_minute = sleep_end_minute;

  pthread_t thread;
  if (pthread_create(&thread, NULL, eventWatcherMain, args) != 0) {
    fprintf(stderr, "events=thread_failed\n");
    free(args);
    return;
  }
  pthread_detach(thread);
}

int dumpBitmapPreview(const Dashboard* dashboard, const char* status, const char* path, int width, int height) {
  if (!path || !path[0]) return 0;
  Canvas canvas;
  canvas.width = width > 0 ? width : kBitmapFallbackWidth;
  canvas.height = height > 0 ? height : kBitmapFallbackHeight;
  canvas.pixels = static_cast<unsigned char*>(calloc(static_cast<size_t>(canvas.width) * static_cast<size_t>(canvas.height), 1));
  if (!canvas.pixels) return 0;
  drawCurrentDashboard(&canvas, dashboard, status);
  const int ok = writePgm(path, &canvas);
  free(canvas.pixels);
  return ok;
}

void renderPayload(const char* payload, const char* status, const char* dump_pgm, const char* save_pgm, int dump_width, int dump_height) {
  const long long started = monotonicMs();
  char lines[kMaxRows][96];
  Dashboard dashboard;
  if (!parseDashboard(payload, &dashboard)) {
    int count = 0;
    addRule(lines, &count);
    addCardText(lines, &count, " KINDLE DASHBOARD");
    addCardText(lines, &count, " Dashboard unavailable");
    addCardText(lines, &count, " Could not parse dashboard data");
    addRule(lines, &count);
    renderToEips(lines, count);
    fprintf(stderr, "timing=render status=parse_failed ms=%lld\n", monotonicMs() - started);
    return;
  }
  if (dump_pgm && dump_pgm[0]) {
    dumpBitmapPreview(&dashboard, status, dump_pgm, dump_width, dump_height);
    fprintf(stderr, "render=pgm %s width=%d height=%d\n", dump_pgm, dump_width > 0 ? dump_width : kBitmapFallbackWidth, dump_height > 0 ? dump_height : kBitmapFallbackHeight);
    freeDashboard(&dashboard);
    fprintf(stderr, "timing=render status=dump ms=%lld\n", monotonicMs() - started);
    return;
  }
  if (renderViaFbink(&dashboard, status, save_pgm)) {
    freeDashboard(&dashboard);
    fprintf(stderr, "timing=render status=fbink ms=%lld\n", monotonicMs() - started);
    return;
  }
  if (renderToFramebuffer(&dashboard, status, save_pgm)) {
    freeDashboard(&dashboard);
    fprintf(stderr, "timing=render status=framebuffer ms=%lld\n", monotonicMs() - started);
    return;
  }
  if (save_pgm && save_pgm[0]) {
    dumpBitmapPreview(&dashboard, status, save_pgm, kBitmapFallbackWidth, kBitmapFallbackHeight);
    fprintf(stderr, "render=save-pgm %s width=%d height=%d fallback=1\n", save_pgm, kBitmapFallbackWidth, kBitmapFallbackHeight);
  }
  if (getenv("KINDLE_DASHBOARD_TEXT_FALLBACK") == NULL) {
    fprintf(stderr, "render=bitmap unavailable text_fallback=disabled\n");
    freeDashboard(&dashboard);
    fprintf(stderr, "timing=render status=bitmap_unavailable ms=%lld\n", monotonicMs() - started);
    return;
  }
  fprintf(stderr, "render=eips fallback\n");
  const int count = renderLines(&dashboard, status, lines);
  renderToEips(lines, count);
  freeDashboard(&dashboard);
  fprintf(stderr, "timing=render status=eips ms=%lld\n", monotonicMs() - started);
}

int renderCachedPayload(const Options* options, const char* status) {
  char* payload = readFile(options->cache);
  if (!payload) {
    fprintf(stderr, "render=cache-miss path=%s\n", options->cache);
    return 0;
  }
  renderPayload(payload, status, options->dump_pgm, options->save_pgm, options->dump_width, options->dump_height);
  free(payload);
  return 1;
}

int shouldRepaintCachedTick(int tick) {
  return tick == 5;
}

int waitForWakeEvent(const Options* options, int seconds, int allow_repaint) {
  for (int elapsed = 1; elapsed <= seconds && g_running; elapsed++) {
    if (g_pending_action != kTouchNone) {
      const int touch_result = handlePendingTouch(options);
      if (!g_running) return 0;
      if (touch_result == 2) return 1;
      if (touch_result == 1) renderCachedPayload(options, "cached/local");
      continue;
    }
    if (g_event_refresh) {
      fprintf(stderr, "events=refresh_now\n");
      return 1;
    }
    if (allow_repaint && shouldRepaintCachedTick(elapsed)) {
      fprintf(stderr, "render=repaint tick=%d\n", elapsed);
      renderCachedPayload(options, "cached/local");
    }
    sleep(1);
  }
  return 1;
}

void handleSignal(int) {
  g_running = 0;
}

void applyInitialView(const char* view) {
  if (!view || !view[0]) return;
  g_active_list = -1;
  g_active_meal_planner = 0;
  g_active_recipes = 0;
  g_active_recipe = -1;
  g_active_recipe_library = 0;
  g_active_challenge = 0;
  if (strcmp(view, "challenge") == 0) g_active_challenge = 1;
  else if (strcmp(view, "recipe") == 0) {
    g_active_recipes = 1;
    g_active_recipe = 0;
    g_active_recipe_library = 1;
  } else if (strcmp(view, "meal-recipe") == 0) {
    g_active_meal_planner = 1;
    g_active_recipe = 0;
    g_active_recipe_library = 0;
  } else if (strcmp(view, "chores") == 0) {
    g_active_list = 0;
  } else if (strcmp(view, "grocery") == 0) {
    g_active_list = 1;
  }
}

void initOptions(Options* options) {
  copyText(options->url, sizeof(options->url), kDefaultUrl);
  copyText(options->events_url, sizeof(options->events_url), kDefaultEventsUrl);
  copyText(options->toggle_url, sizeof(options->toggle_url), kDefaultToggleUrl);
  options->read_token[0] = '\0';
  options->toggle_token[0] = '\0';
  copyText(options->cache, sizeof(options->cache), kDefaultCache);
  options->render_only[0] = '\0';
  options->view[0] = '\0';
  options->dump_pgm[0] = '\0';
  options->save_pgm[0] = '\0';
  options->dump_width = kBitmapFallbackWidth;
  options->dump_height = kBitmapFallbackHeight;
  options->interval = kDefaultIntervalSeconds;
  parseSleepWindow(kDefaultSleepWindow, &options->sleep_start_minute, &options->sleep_end_minute);
  options->once = 0;
  options->invert_images = 0;
}

int parseOptions(int argc, char** argv, Options* options) {
  initOptions(options);
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--url") == 0 && i + 1 < argc) copyText(options->url, sizeof(options->url), argv[++i]);
    else if (strcmp(argv[i], "--events-url") == 0 && i + 1 < argc) copyText(options->events_url, sizeof(options->events_url), argv[++i]);
    else if (strcmp(argv[i], "--toggle-url") == 0 && i + 1 < argc) copyText(options->toggle_url, sizeof(options->toggle_url), argv[++i]);
    else if (strcmp(argv[i], "--read-token") == 0 && i + 1 < argc) copyText(options->read_token, sizeof(options->read_token), argv[++i]);
    else if (strcmp(argv[i], "--toggle-token") == 0 && i + 1 < argc) copyText(options->toggle_token, sizeof(options->toggle_token), argv[++i]);
    else if (strcmp(argv[i], "--cache") == 0 && i + 1 < argc) copyText(options->cache, sizeof(options->cache), argv[++i]);
    else if (strcmp(argv[i], "--interval") == 0 && i + 1 < argc) {
      options->interval = atoi(argv[++i]);
      if (options->interval < 5) options->interval = 5;
    } else if (strcmp(argv[i], "--sleep-window") == 0 && i + 1 < argc) {
      if (!parseSleepWindow(argv[++i], &options->sleep_start_minute, &options->sleep_end_minute)) {
        fprintf(stderr, "Invalid sleep window. Use HH:MM-HH:MM or off, for example 00:00-08:00.\n");
        return 0;
      }
    } else if (strcmp(argv[i], "--once") == 0) options->once = 1;
    else if (strcmp(argv[i], "--invert-images") == 0) {
      options->invert_images = 1;
      g_invert_images = 1;
    }
    else if (strcmp(argv[i], "--render") == 0 && i + 1 < argc) copyText(options->render_only, sizeof(options->render_only), argv[++i]);
    else if (strcmp(argv[i], "--view") == 0 && i + 1 < argc) copyText(options->view, sizeof(options->view), argv[++i]);
    else if (strcmp(argv[i], "--dump-pgm") == 0 && i + 1 < argc) copyText(options->dump_pgm, sizeof(options->dump_pgm), argv[++i]);
    else if (strcmp(argv[i], "--dump-size") == 0 && i + 1 < argc) {
      if (sscanf(argv[++i], "%dx%d", &options->dump_width, &options->dump_height) != 2 ||
          options->dump_width < 240 || options->dump_height < 320) {
        fprintf(stderr, "Invalid dump size. Use WIDTHxHEIGHT, for example 1072x1448.\n");
        return 0;
      }
    }
    else if (strcmp(argv[i], "--save-pgm") == 0 && i + 1 < argc) copyText(options->save_pgm, sizeof(options->save_pgm), argv[++i]);
    else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
      printf("Usage: %s [--url URL] [--events-url URL] [--toggle-url URL] [--read-token TOKEN] [--toggle-token TOKEN] [--cache PATH] [--interval SECONDS] [--sleep-window HH:MM-HH:MM|off] [--once] [--invert-images]\n", argv[0]);
      printf("       %s --render PATH [--view challenge|recipe|meal-recipe|chores|grocery] [--dump-pgm PATH] [--dump-size WIDTHxHEIGHT] [--save-pgm PATH]\n", argv[0]);
      exit(0);
    } else {
      fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
      return 0;
    }
  }
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parseOptions(argc, argv, &options)) return 1;

  if (options.render_only[0]) {
    applyInitialView(options.view);
    char* payload = readFile(options.render_only);
    if (!payload) {
      fprintf(stderr, "Could not read %s\n", options.render_only);
      return 1;
    }
    renderPayload(payload, "fixture", options.dump_pgm, options.save_pgm, options.dump_width, options.dump_height);
    free(payload);
    return 0;
  }

  signal(SIGINT, handleSignal);
  signal(SIGTERM, handleSignal);
  applyInitialView(options.view);

  TouchInput touch;
  initTouchInput(&touch);
  startTouchWatcher(&touch);
  if (!options.once && !options.render_only[0]) {
    startEventWatcher(options.events_url, options.read_token, options.sleep_start_minute, options.sleep_end_minute);
  }

  while (g_running) {
    int pending_result = 0;
    if (g_pending_action != kTouchNone) pending_result = handlePendingTouch(&options);
    if (!g_running) break;
    if (pending_result == 1 && renderCachedPayload(&options, "cached/local")) {
      g_event_refresh = 0;
      if (options.once) break;
      for (int remaining = options.interval; remaining > 0 && g_running;) {
        if (inSleepWindow(options.sleep_start_minute, options.sleep_end_minute)) break;
        const int chunk = remaining > 60 ? 60 : remaining;
        waitForWakeEvent(&options, chunk, remaining == options.interval);
        if (g_event_refresh) break;
        remaining -= chunk;
      }
      continue;
    }
    g_event_refresh = 0;

    if (!options.once && inSleepWindow(options.sleep_start_minute, options.sleep_end_minute)) {
      fprintf(stderr, "power=quiet sleep_window=1\n");
      if (!renderCachedPayload(&options, "sleep/quiet")) {
        char lines[kMaxRows][96];
        int count = 0;
        addRule(lines, &count);
        addCardText(lines, &count, " KINDLE DASHBOARD");
        addCardText(lines, &count, " Quiet hours");
        addCardText(lines, &count, " Cache unavailable");
        addRule(lines, &count);
        renderToEips(lines, count);
      }
      while (g_running && inSleepWindow(options.sleep_start_minute, options.sleep_end_minute)) {
        waitForWakeEvent(&options, 60, 0);
        if (g_manual_fetch_refresh) break;
        g_event_refresh = 0;
      }
      if (!g_manual_fetch_refresh) continue;
      fprintf(stderr, "power=quiet manual_fetch=1\n");
      g_manual_fetch_refresh = 0;
      g_event_refresh = 0;
    }

    if (g_manual_fetch_refresh) {
      fprintf(stderr, "events=manual-fetch\n");
      g_manual_fetch_refresh = 0;
    }
    char dashboard_url[320];
    buildDashboardUrl(options.url, dashboard_url, sizeof(dashboard_url));
    const int fetched = fetchToCache(dashboard_url, options.read_token, options.cache);
    if (!renderCachedPayload(&options, fetched ? "live" : "cached/offline")) {
      char lines[kMaxRows][96];
      int count = 0;
      addRule(lines, &count);
      addCardText(lines, &count, " KINDLE DASHBOARD");
      addCardText(lines, &count, " Dashboard unavailable");
      addCardText(lines, &count, " Check Wi-Fi or refresh later");
      addRule(lines, &count);
      renderToEips(lines, count);
    }

    if (options.once) break;
    for (int remaining = options.interval; remaining > 0 && g_running;) {
      if (inSleepWindow(options.sleep_start_minute, options.sleep_end_minute)) break;
      const int chunk = remaining > 60 ? 60 : remaining;
      waitForWakeEvent(&options, chunk, remaining == options.interval);
      if (g_event_refresh) break;
      remaining -= chunk;
    }
  }

  closeTouchInput(&touch);
  freePgmCache();
  return 0;
}
