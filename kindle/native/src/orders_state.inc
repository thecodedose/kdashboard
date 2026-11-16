const int kMaxOrders = 30;
struct OrderEntry {
  char store[64] = "";
  char number[80] = "";
  char items[400] = "";
  char status[32] = "";
  char status_label[48] = "";
  char delivery[48] = "";
  char tracking[96] = "";
  char note[240] = "";
  char otp[16] = "";
  time_t otp_expires = 0;
};
struct OrdersSnapshot {
  char status[32] = "not_synced";
  char checked[80] = "Not checked yet";
  char notice[200] = "Order emails have not been checked yet.";
  int active_count = 0;
  int count = 0;
  int busy = 0;
  int stale = 0;
  OrderEntry orders[kMaxOrders] = {};
};
pthread_mutex_t g_orders_mutex = PTHREAD_MUTEX_INITIALIZER;
OrdersSnapshot g_orders;
int g_active_orders = 0;
int g_orders_recent = 0;
int g_orders_page = 0;
int g_orders_page_count = 1;
bool g_orders_repaint = false;
bool g_orders_pending = false;
char g_orders_url[512] = "";
char g_orders_token[160] = "";
OrdersSnapshot ordersSnapshot() {
  pthread_mutex_lock(&g_orders_mutex); OrdersSnapshot result = g_orders;
  pthread_mutex_unlock(&g_orders_mutex); return result;
}
bool takeOrdersRepaint() {
  pthread_mutex_lock(&g_orders_mutex); bool dirty = g_orders_repaint; g_orders_repaint = false;
  pthread_mutex_unlock(&g_orders_mutex); return dirty;
}
void refreshOrders() {
  pthread_mutex_lock(&g_orders_mutex);
  if (g_orders_url[0] && !g_orders.busy) { g_orders_pending = true; g_orders.busy = 1; }
  pthread_mutex_unlock(&g_orders_mutex);
}
bool orderClosed(const OrderEntry& order) {
  return strcmp(order.status, "delivered") == 0 || strcmp(order.status, "cancelled") == 0 || strcmp(order.status, "returned") == 0;
}
bool orderOtpValid(const OrderEntry& order) {
  if (strcmp(order.status, "out_for_delivery") != 0 || order.otp_expires <= time(NULL)) return false;
  const size_t length = strlen(order.otp);
  if (length < 4 || length > 8) return false;
  for (const char* p = order.otp; *p; p++) if (*p < '0' || *p > '9') return false;
  return true;
}
