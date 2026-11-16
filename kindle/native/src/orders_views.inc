void drawOrdersTile(Canvas* canvas, int x, int y, int w, int h) {
  const OrdersSnapshot orders = ordersSnapshot();
  // Match the vacuum card's graph-paper artwork, boxed title and status footer.
  fillRect(canvas, x + 3, y + 3, w - 6, h - 6, 255);
  for (int gx = x + 3; gx < x + w - 3; gx += 16)
    line(canvas, gx, y + 3, gx, y + h - 4, 1, 220);
  for (int gy = y + 3; gy < y + h - 3; gy += 16)
    line(canvas, x + 3, gy, x + w - 4, gy, 1, 220);
  // Fit the complete 3:2 illustration between the title and footer. Neither
  // image edges nor the parcel can be hidden beneath the text panels.
  const int art_h = std::min(h - 108, (w - 32) * 2 / 3);
  const int art_w = art_h * 3 / 2;
  drawPgmImage(canvas, x + (w - art_w) / 2, y + 50 + (h - 108 - art_h) / 2, art_w, art_h,
    "/mnt/us/extensions/kindle-dashboard/assets/orders-parcel.pgm",
    "kindle/kual/kindle-dashboard/assets/orders-parcel.pgm", 0);
  strokeRect(canvas, x, y, w, h, 3, 0);
  fillRect(canvas, x + 14, y + 14, w - 28, 32, 255);
  strokeRect(canvas, x + 14, y + 14, w - 28, 32, 2, 0);
  drawTextCentered(canvas, x + w / 2, y + 23, w - 44, "ORDERS", 2, 0);
  fillRect(canvas, x + 12, y + h - 54, w - 24, 42, 255);
  line(canvas, x + 16, y + h - 54, x + w - 16, y + h - 54, 2, 0);
  char label[64];
  if (strcmp(orders.status, "not_synced") == 0) copyText(label, sizeof(label), "AWAITING FIRST SYNC");
  else snprintf(label, sizeof(label), "%d ONGOING%s", orders.active_count, orders.stale ? " / CACHED" : "");
  drawTextCentered(canvas, x + w / 2, y + h - 46, w - 32, label, 2, 0);
  drawTextCentered(canvas, x + w / 2, y + h - 24, w - 24, "TAP FOR DETAILS", 2, 0);
  addTouchRegion({x, y, w, h}, kTouchOpenOrders, -1, -1, "", 0);
}

void drawOrdersPage(Canvas* canvas, const Dashboard* dashboard, const char* status) {
  widgetPageShell(canvas, dashboard, status, "ORDERS");
  const OrdersSnapshot orders = ordersSnapshot();
  const int w = canvas->width;
  const int top = kKindleStatusBarHeight + 248;
  char checked[120]; snprintf(checked, sizeof(checked), "LAST CHECKED: %s", orders.checked);
  drawTextClipped(canvas, 28, top, w - 56, checked, 2, 0);
  const int bw = (w - 72) / 3;
  const char* tabs[] = {"ONGOING", "RECENT", orders.busy ? "WAIT..." : "REFRESH"};
  const TouchAction actions[] = {kTouchOrdersActive, kTouchOrdersRecent, kTouchOrdersRefresh};
  for (int i = 0; i < 3; i++) {
    Rect rect = {24 + i * (bw + 12), top + 34, bw, 56};
    const bool selected = i < 2 && g_orders_recent == i;
    strokeRect(canvas, rect.x, rect.y, rect.w, rect.h, selected ? 4 : 2, 0);
    drawTextCentered(canvas, rect.x + rect.w / 2, rect.y + 20, rect.w - 16, tabs[i], 2, 0);
    addTouchRegion(rect, i == 2 && orders.busy ? kTouchNone : actions[i], -1, -1, "", 0);
  }
  drawTextWrapped(canvas, 28, top + 108, w - 56, orders.notice, 2, 0, 2);
  const int table_y = top + 174;
  const int left = 24, right = w - 24, table_w = right - left;
  const int xs[] = {left, left + table_w * 43 / 100, left + table_w * 66 / 100, left + table_w * 82 / 100, right};
  fillRect(canvas, left, table_y, table_w, 40, 0);
  const char* headings[] = {"ORDER / ITEMS", "STATUS", "DUE", "OTP"};
  for (int i = 0; i < 4; i++) drawTextClipped(canvas, xs[i] + 10, table_y + 12, xs[i+1] - xs[i] - 18, headings[i], 2, 255);
  int indices[kMaxOrders], total = 0;
  for (int i = 0; i < orders.count; i++) if (orderClosed(orders.orders[i]) == bool(g_orders_recent)) indices[total++] = i;
  const int row_h = w >= 1000 ? 176 : 196;
  int rows = (canvas->height - 104 - table_y - 40) / row_h;
  if (rows < 1) rows = 1;
  g_orders_page_count = total ? (total + rows - 1) / rows : 1;
  if (g_orders_page >= g_orders_page_count) g_orders_page = g_orders_page_count - 1;
  if (!total) drawTextWrapped(canvas, 32, table_y + 66, w - 64,
    strcmp(orders.status, "not_synced") == 0 ? "Waiting for your first Gmail order check." : g_orders_recent ? "No recent completed orders." : "No ongoing orders found in the checked emails.", 3, 0, 3);
  for (int row = 0, n = g_orders_page * rows; n < total && row < rows; row++, n++) {
    const OrderEntry& order = orders.orders[indices[n]];
    const int y = table_y + 40 + row * row_h;
    strokeRect(canvas, left, y, table_w, row_h, 1, 0);
    for (int i = 1; i < 4; i++) line(canvas, xs[i], y, xs[i], y + row_h - 40, 1, 0);
    drawTextClipped(canvas, left + 10, y + 12, xs[1] - left - 20, order.store, 3, 0);
    drawTextClipped(canvas, left + 10, y + 44, xs[1] - left - 20, order.number, 2, 0);
    drawTextWrapped(canvas, left + 10, y + 70, xs[1] - left - 20, order.items, 2, 0, w >= 1000 ? 3 : 4);
    drawTextWrapped(canvas, xs[1] + 10, y + 16, xs[2] - xs[1] - 20, order.status_label, 2, 0, 3);
    drawTextWrapped(canvas, xs[2] + 10, y + 16, xs[3] - xs[2] - 20, order.delivery[0] ? order.delivery : "Not given", 2, 0, 3);
    drawTextClipped(canvas, xs[3] + 10, y + 16, xs[4] - xs[3] - 20, !orders.stale && orderOtpValid(order) ? order.otp : "--", 2, 0);
    char detail[360];
    snprintf(detail, sizeof(detail), "%s%s%s", order.tracking[0] ? order.tracking : "", order.tracking[0] && order.note[0] ? " / " : "", order.note);
    drawTextClipped(canvas, left + 10, y + row_h - 28, table_w - 20, detail, 2, 0);
  }
  drawWidgetPager(canvas, g_orders_page, g_orders_page_count, kTouchOrdersPrevious, kTouchOrdersNext);
}
