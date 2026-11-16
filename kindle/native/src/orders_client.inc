bool parseOrders(const char* json, OrdersSnapshot* snapshot) {
  if (!json || !extractBool(json, NULL, "ok", 0)) return false;
  *snapshot = OrdersSnapshot{};
  extractString(json, NULL, "status", snapshot->status, sizeof(snapshot->status), "unavailable");
  extractString(json, NULL, "checked", snapshot->checked, sizeof(snapshot->checked), "Not checked yet");
  extractString(json, NULL, "notice", snapshot->notice, sizeof(snapshot->notice), "");
  snapshot->stale = extractBool(json, NULL, "stale", 0);
  const char* list = findKeyInRange(json, NULL, "orders");
  const char* end = list && *list == '[' ? matchingClose(list, ']') : NULL;
  if (!end) return true;
  for (const char* cursor = list + 1; cursor < end && snapshot->count < kMaxOrders;) {
    const char* item = strchr(cursor, '{');
    if (!item || item >= end) break;
    const char* item_end = matchingClose(item, '}');
    if (!item_end || item_end > end) return false;
    OrderEntry& order = snapshot->orders[snapshot->count++];
    extractString(item, item_end, "store", order.store, sizeof(order.store), "Store");
    extractString(item, item_end, "number", order.number, sizeof(order.number), "");
    extractString(item, item_end, "items", order.items, sizeof(order.items), "");
    extractString(item, item_end, "status", order.status, sizeof(order.status), "awaiting_update");
    extractString(item, item_end, "status_label", order.status_label, sizeof(order.status_label), "Awaiting update");
    extractString(item, item_end, "delivery", order.delivery, sizeof(order.delivery), "");
    extractString(item, item_end, "tracking", order.tracking, sizeof(order.tracking), "");
    extractString(item, item_end, "note", order.note, sizeof(order.note), "");
    extractString(item, item_end, "otp", order.otp, sizeof(order.otp), "");
    order.otp_expires = extractInt(item, item_end, "otp_expires_unix", 0);
    if (!orderClosed(order)) snapshot->active_count++;
    if (!orderOtpValid(order) || snapshot->stale) order.otp[0] = '\0';
    cursor = item_end + 1;
  }
  return true;
}
bool requestOrders(OrdersSnapshot* result) {
  char url[2100], header[200], quoted_header[850], command[3300];
  shellQuote(g_orders_url, url, sizeof(url));
  snprintf(header, sizeof(header), "X-Dashboard-Toggle-Token: %s", g_orders_token);
  shellQuote(header, quoted_header, sizeof(quoted_header));
  snprintf(command, sizeof(command), "curl -fsS --connect-timeout 5 --max-time 20 --max-filesize 65536 -H %s -- %s 2>/dev/null", quoted_header, url);
  FILE* pipe = popen(command, "r");
  if (!pipe) return false;
  char payload[65537];
  const size_t count = fread(payload, 1, sizeof(payload) - 1, pipe);
  const int overflow = count == sizeof(payload) - 1 ? fgetc(pipe) : EOF;
  const int status = pclose(pipe); payload[count] = '\0';
  return status == 0 && overflow == EOF && parseOrders(payload, result);
}
void* ordersWorkerMain(void*) {
  time_t next = 0;
  while (g_running) {
    pthread_mutex_lock(&g_orders_mutex);
    bool expired = false;
    for (int i = 0; i < g_orders.count; i++) if (g_orders.orders[i].otp[0] && !orderOtpValid(g_orders.orders[i])) {
      g_orders.orders[i].otp[0] = '\0'; expired = true;
    }
    if (expired) g_orders_repaint = true;
    const bool requested = g_orders_pending;
    const bool fetch = requested || time(NULL) >= next;
    OrdersSnapshot before = g_orders; before.busy = 0;
    if (fetch) { g_orders_pending = false; g_orders.busy = 1; }
    pthread_mutex_unlock(&g_orders_mutex);
    if (!fetch) { sleep(1); continue; }
    OrdersSnapshot result;
    const bool ok = requestOrders(&result);
    pthread_mutex_lock(&g_orders_mutex);
    if (!ok) {
      result = g_orders; result.stale = 1;
      copyText(result.notice, sizeof(result.notice), "Orders unavailable. Showing the last check; refresh to retry.");
      for (int i = 0; i < result.count; i++) result.orders[i].otp[0] = '\0';
    }
    result.busy = 0;
    if (requested || memcmp(&before, &result, sizeof(result)) != 0) g_orders_repaint = true;
    g_orders = result; next = time(NULL) + 300;
    pthread_mutex_unlock(&g_orders_mutex);
  }
  return NULL;
}
pthread_t g_orders_thread;
bool g_orders_thread_started = false;
void startOrdersWorker(const Options* options) {
  const char* override_url = getenv("DASHBOARD_ORDERS_URL");
  if (override_url && override_url[0]) copyText(g_orders_url, sizeof(g_orders_url), override_url);
  else {
    copyText(g_orders_url, sizeof(g_orders_url), options->url);
    char* route = strrchr(g_orders_url, '/');
    if (route && strcmp(route, "/kindle-dashboard-data") == 0)
      copyText(route, sizeof(g_orders_url) - (route - g_orders_url), "/kindle-dashboard-orders");
    else g_orders_url[0] = '\0';
  }
  copyText(g_orders_token, sizeof(g_orders_token), options->toggle_token);
  if (!g_orders_url[0] || !g_orders_token[0] || !commandExists("curl")) return;
  g_orders_thread_started = pthread_create(&g_orders_thread, NULL, ordersWorkerMain, NULL) == 0;
}
