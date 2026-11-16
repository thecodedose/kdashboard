// Widget rendering shares the native app's canvas, navigation and touch regions.
// Return the next byte offset without dropping long words or paragraph breaks.
int noteLine(const char* text, int offset, int columns, char* out, size_t size) {
  const int start = offset;
  int count = 0;
  while (text[offset] && text[offset] != '\n' && count < columns && count + 1 < static_cast<int>(size)) {
    out[count++] = text[offset++];
  }
  if (count == columns && text[offset] && text[offset] != '\n' && text[offset] != ' ') {
    int split = count - 1;
    while (split > 0 && out[split] != ' ') split--;
    if (split > 0) { offset = start + split + 1; count = split; }
  } else if (text[offset] == '\n' || text[offset] == ' ') offset++;
  out[count] = '\0';
  return offset;
}

[[maybe_unused]] void drawQuickNoteTile(Canvas* canvas, int x, int y, int w, int h, const Dashboard* dashboard) {
  strokeRect(canvas, x, y, w, h, 2, 0);
  addTouchRegion({x, y, w, h}, kTouchOpenNote, -1, -1, "", 0);
  drawTextCentered(canvas, x + w / 2, y + 18, w - 24, "QUICK NOTE", w < 300 ? 2 : 3, 0);
  line(canvas, x + 12, y + 54, x + w - 12, y + 54, 2, 0);
  const char* text = dashboard->note_available ? (dashboard->quick_note[0] ? dashboard->quick_note : "No note yet. Add a note from your assistant.") : "Note unavailable. Refresh to reconnect.";
  const int scale = 2;
  const int rows = (h - 104) / 24;
  const int columns = (w - 32) / (6 * scale);
  int offset = 0;
  char row[200];
  for (int i = 0; i < rows && text[offset]; i++) {
    offset = noteLine(text, offset, columns, row, sizeof(row));
    drawText(canvas, x + 16, y + 70 + i * 24, row, scale, 0);
  }
  drawTextCentered(canvas, x + w / 2, y + h - 28, w - 20, text[offset] ? "READ MORE" : "TAP TO OPEN", 2, 0);
}

void widgetPageShell(Canvas* canvas, const Dashboard* dashboard, const char* status, const char* title) {
  clearCanvas(canvas, 255); clearTouchRegions();
  g_last_screen_width = canvas->width; g_last_screen_height = canvas->height;
  strokeRect(canvas, 0, kKindleStatusBarHeight, canvas->width, canvas->height - kKindleStatusBarHeight, 3, 0);
  drawTopHeader(canvas, dashboard, status, 0, kKindleStatusBarHeight, canvas->width);
  drawSubHeader(canvas, 0, kKindleStatusBarHeight + 150, canvas->width, title);
}

void drawWidgetPager(Canvas* canvas, int page, int pages, TouchAction previous, TouchAction next) {
  const int y = canvas->height - 80;
  char label[48]; snprintf(label, sizeof(label), "%d / %d", page + 1, pages);
  drawTextCentered(canvas, canvas->width / 2, y + 18, 160, label, 3, 0);
  if (page > 0) {
    Rect rect = {20, y, 180, 56}; strokeRect(canvas, rect.x, rect.y, rect.w, rect.h, 2, 0);
    drawTextCentered(canvas, rect.x + rect.w / 2, y + 18, rect.w - 16, "PREVIOUS", 2, 0);
    addTouchRegion(rect, previous, -1, -1, "", 0);
  }
  if (page + 1 < pages) {
    Rect rect = {canvas->width - 200, y, 180, 56}; strokeRect(canvas, rect.x, rect.y, rect.w, rect.h, 2, 0);
    drawTextCentered(canvas, rect.x + rect.w / 2, y + 18, rect.w - 16, "NEXT", 2, 0);
    addTouchRegion(rect, next, -1, -1, "", 0);
  }
}

void drawQuickNotePage(Canvas* canvas, const Dashboard* dashboard, const char* status) {
  widgetPageShell(canvas, dashboard, status, "QUICK NOTE");
  const char* text = dashboard->note_available ? (dashboard->quick_note[0] ? dashboard->quick_note : "No note yet. Add a note from your assistant.") : "Note unavailable. Refresh to reconnect.";
  const int y = kKindleStatusBarHeight + 256;
  const int scale = canvas->width >= 1000 ? 3 : 2;
  const int line_h = 8 * scale + 12;
  const int available_rows = (canvas->height - 104 - y) / line_h;
  const int rows = available_rows > 0 ? available_rows : 1;
  const int columns = (canvas->width - 64) / (6 * scale);
  int total = 0, offset = 0;
  char row[200];
  while (text[offset]) { offset = noteLine(text, offset, columns, row, sizeof(row)); total++; }
  g_note_page_count = total > 0 ? (total + rows - 1) / rows : 1;
  if (g_note_page >= g_note_page_count) g_note_page = g_note_page_count - 1;
  offset = 0;
  for (int i = 0; i < total && text[offset]; i++) {
    offset = noteLine(text, offset, columns, row, sizeof(row));
    if (i >= g_note_page * rows && i < (g_note_page + 1) * rows) {
      drawText(canvas, 32, y + (i - g_note_page * rows) * line_h, row, scale, 0);
    }
  }
  drawWidgetPager(canvas, g_note_page, g_note_page_count, kTouchNotePrevious, kTouchNoteNext);
}

void vacuumButton(Canvas* canvas,Rect rect,const char* label,TouchAction action,narwal::Command command,const VacuumSnapshot& vacuum) {
  bool enabled=vacuum.configured && !vacuum.busy &&
    (command!=narwal::Command::Start || (vacuum.map && (!vacuum.plan.rooms_only || !vacuum.plan.rooms.empty()))) &&
    (command!=narwal::Command::Start || time(nullptr)>=vacuum.start_pending_until) &&
    (command==narwal::Command::Refresh || (vacuum.online && narwal::can_run(command,vacuum.status.state)));
  unsigned char color=enabled ? 0 : 160;
  strokeRect(canvas,rect.x,rect.y,rect.w,rect.h,2,color);
  drawTextCentered(canvas,rect.x+rect.w/2,rect.y+(rect.h-16)/2,rect.w-12,label,2,color);
  // Consume disabled buttons too, so touch-orientation fallback cannot hit a different control.
  addTouchRegion(rect,enabled ? action : kTouchNone,-1,-1,"",0);
}

#include "vacuum_views.inc"

const char* calendarMessage(const Dashboard* dashboard) {
  if (strcmp(dashboard->calendar_status, "connected") == 0) return "No events planned";
  if (strcmp(dashboard->calendar_status, "not_connected") == 0) return "Connect Google Calendar";
  return "Calendar unavailable";
}

[[maybe_unused]] void drawCalendarTile(Canvas* canvas, int x, int y, int w, int h, const Dashboard* dashboard) {
  strokeRect(canvas, x, y, w, h, 3, 0);
  addTouchRegion({x, y, w, h}, kTouchOpenCalendar, -1, -1, "", 0);
  int year = 0, month = 0, selected = 0;
  if (sscanf(dashboard->calendar_date, "%d-%d-%d", &year, &month, &selected) != 3 || month < 1 || month > 12 || selected < 1 || selected > 31) return;
  static const char* months[] = {"JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER"};
  static const char* weekdays[] = {"S", "M", "T", "W", "T", "F", "S"};
  struct tm first = {}; first.tm_year = year - 1900; first.tm_mon = month - 1; first.tm_mday = 1;
  const time_t stamp = timegm(&first);
  const int weekday = gmtime(&stamp)->tm_wday;
  static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const int days = month_days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  char title[40]; snprintf(title, sizeof(title), "%s %d", months[month - 1], year);
  drawTextClipped(canvas, x + 22, y + 20, w - 44, title, w >= 480 ? 3 : 2, 0);
  const int cell_w = (w - 32) / 7;
  const int row_h = w >= 480 ? 32 : 24;
  for (int i = 0; i < 7; i++) drawTextCentered(canvas, x + 16 + i * cell_w + cell_w / 2, y + 62, cell_w, weekdays[i], 2, 0);
  for (int day = 1; day <= days; day++) {
    const int index = weekday + day - 1;
    const int cx = x + 16 + (index % 7) * cell_w + cell_w / 2;
    const int day_y = y + 90 + (index / 7) * row_h;
    const int selected_radius = row_h >= 32 ? 17 : 14;
    if (day == selected) circleRing(canvas, cx, day_y + 8, selected_radius, selected_radius, 100, 0);
    char number[8]; snprintf(number, sizeof(number), "%d", day);
    drawTextCentered(canvas, cx, day_y, cell_w, number, 2, day == selected ? 255 : 0);
  }
  const int agenda_y = y + 90 + 6 * row_h + 12;
  line(canvas, x + 20, agenda_y - 8, x + w - 20, agenda_y - 8, 1, 0);
  if (!dashboard->calendar_count) {
    drawTextWrapped(canvas, x + 22, agenda_y + 8, w - 44, calendarMessage(dashboard), 2, 0, 2);
  } else {
    const int rows = (y + h - 34 - agenda_y) / 70;
    for (int i = 0; i < dashboard->calendar_count && i < rows; i++) {
      fillRect(canvas, x + 20, agenda_y + i * 70, 5, 54, 0);
      drawTextClipped(canvas, x + 36, agenda_y + i * 70, w - 58, dashboard->calendar_events[i].title, 2, 0);
      drawTextClipped(canvas, x + 36, agenda_y + i * 70 + 28, w - 58, dashboard->calendar_events[i].time, 2, 0);
    }
  }
  drawTextCentered(canvas, x + w / 2, y + h - 26, w - 32, "TAP FOR AGENDA", 2, 0);
}

void drawCalendarPage(Canvas* canvas, const Dashboard* dashboard, const char* status) {
  widgetPageShell(canvas, dashboard, status, "CALENDAR");
  const int y = kKindleStatusBarHeight + 260;
  const int card_h = 150;
  const int available_rows = (canvas->height - 100 - y) / card_h;
  const int rows = available_rows > 0 ? available_rows : 1;
  g_calendar_page_count = dashboard->calendar_count ? (dashboard->calendar_count + rows - 1) / rows : 1;
  if (g_calendar_page >= g_calendar_page_count) g_calendar_page = g_calendar_page_count - 1;
  if (!dashboard->calendar_count) drawTextWrapped(canvas, 32, y + 20, canvas->width - 64, calendarMessage(dashboard), 3, 0, 3);
  for (int i = g_calendar_page * rows; i < dashboard->calendar_count && i < (g_calendar_page + 1) * rows; i++) {
    const int top = y + (i - g_calendar_page * rows) * card_h;
    fillRect(canvas, 24, top, 5, card_h - 24, 0);
    drawTextWrapped(canvas, 48, top, canvas->width - 80, dashboard->calendar_events[i].title, 3, 0, 2);
    drawTextClipped(canvas, 48, top + 68, canvas->width - 80, dashboard->calendar_events[i].time, 2, 0);
    drawTextClipped(canvas, 48, top + 100, canvas->width - 80, dashboard->calendar_events[i].location, 2, 0);
  }
  drawWidgetPager(canvas, g_calendar_page, g_calendar_page_count, kTouchCalendarPrevious, kTouchCalendarNext);
}
