#define main dashboard_app_main
#include "../kindle/native/src/kindle_dashboard.cpp"
#undef main
#include <assert.h>
#include <string>
int main() {
  const time_t expiry = time(NULL) + 3600;
  char payload[5000];
  snprintf(payload,sizeof(payload),"{\"ok\":true,\"status\":\"connected\",\"checked\":\"01 Jan, 01:00 pm UTC\",\"notice\":\"Email updates every 4 hours.\",\"orders\":[{\"store\":\"Sample Shop\",\"number\":\"EXCHANGE-123\",\"items\":\"Sample item\",\"status\":\"exchange\",\"status_label\":\"Exchange\",\"delivery\":\"02 Jan\",\"otp\":\"999999\",\"otp_expires_unix\":%ld},{\"store\":\"Shop\",\"number\":\"DELIVERY-123\",\"status\":\"out_for_delivery\",\"status_label\":\"Out for delivery\",\"otp\":\"123456\",\"otp_expires_unix\":%ld},{\"store\":\"Shop\",\"number\":\"DONE\",\"status\":\"delivered\",\"otp\":\"111111\",\"otp_expires_unix\":%ld}]}",long(expiry),long(expiry),long(expiry));
  OrdersSnapshot snapshot; assert(parseOrders(payload,&snapshot));
  assert(snapshot.count==3 && snapshot.active_count==2);
  assert(!snapshot.orders[0].otp[0] && orderOtpValid(snapshot.orders[1]) && !snapshot.orders[2].otp[0]);
  snapshot.orders[1].otp_expires=time(NULL)-1;assert(!orderOtpValid(snapshot.orders[1]));
  assert(!parseOrders("{\"ok\":false}",&snapshot));
  assert(parseOrders(payload,&g_orders));
  Dashboard dashboard;assert(parseDashboard("{\"ok\":true,\"generated_at\":\"2026-11-15T00:00:00+05:30\",\"lists\":[]}",&dashboard));
  Options options={};
  for(int width:{760,1072}) {
    Canvas canvas={width,width==760?1024:1448,nullptr};canvas.pixels=static_cast<unsigned char*>(calloc(canvas.width*canvas.height,1));
    g_active_orders=0;drawCurrentDashboard(&canvas,&dashboard,"test");
    assert(applyTouchAt(width/2,300) && g_pending_action==kTouchOpenOrders);
    assert(handlePendingTouch(&options)==1 && g_active_orders && !g_orders_recent);
    g_orders.count=30;for(int i=3;i<30;i++)g_orders.orders[i]=g_orders.orders[0];
    drawCurrentDashboard(&canvas,&dashboard,"test");assert(g_orders_page_count>1);
    assert(applyTouchAt(width-100,canvas.height-50) && g_pending_action==kTouchOrdersNext);
    assert(handlePendingTouch(&options)==1 && g_orders_page==1);
    g_pending_action=kTouchOrdersRecent;assert(handlePendingTouch(&options)==1 && g_orders_recent && !g_orders_page);
    drawCurrentDashboard(&canvas,&dashboard,"test");assert(g_orders_page_count==1);
    g_pending_action=kTouchBack;assert(handlePendingTouch(&options)==1 && !g_active_orders);
    free(canvas.pixels);
  }
  assert(chdir("../..")==0);
  char* fixture=readFile("kindle/native/fixtures/dashboard-data.json");assert(fixture && parseDashboard(fixture,&dashboard));free(fixture);
  assert(parseOrders(payload,&g_orders));g_orders.orders[1].otp[0]='\0';
  for(int width:{760,1072}) {
    Canvas canvas={width,width==760?1024:1448,nullptr};canvas.pixels=static_cast<unsigned char*>(calloc(canvas.width*canvas.height,1));
    g_active_orders=0;drawCurrentDashboard(&canvas,&dashboard,"preview");char path[160];
    snprintf(path,sizeof(path),"kindle/native/build/orders-home-%d.pgm",width);assert(writePgm(path,&canvas));
    g_active_orders=1;g_orders_recent=0;g_orders_page=0;drawCurrentDashboard(&canvas,&dashboard,"preview");
    snprintf(path,sizeof(path),"kindle/native/build/orders-table-%d.pgm",width);assert(writePgm(path,&canvas));free(canvas.pixels);
  }
}
