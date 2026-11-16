import { createAdminClient } from "npm:@insforge/sdk@1.5.2";

const labels: Record<string, string> = { placed: "Placed", packed: "Packed", shipped: "Shipped", out_for_delivery: "Out for delivery", partially_delivered: "Partial delivery", awaiting_update: "Awaiting update", exchange: "Exchange", delivered: "Delivered", cancelled: "Cancelled", returned: "Returned" };
export default async function(req: Request): Promise<Response> {
  if (req.method === "OPTIONS") return json(null, 204);
  const token = Deno.env.get("DASHBOARD_TOGGLE_TOKEN");
  if (!token || req.headers.get("x-dashboard-toggle-token") !== token) return json({ ok: false, error: "Unauthorized" }, 401);
  if (req.method !== "GET") return json({ ok: false, error: "Method not allowed" }, 405);
  try {
    const admin = createAdminClient({ baseUrl: required("INSFORGE_BASE_URL"), apiKey: required("INSFORGE_API_KEY") });
    const columns = "store,order_number,items_summary,status,delivery_date,delivery_window,courier,tracking_number,delivery_otp,otp_expires_at,notes,latest_email_at";
    const [activeOrders, recentOrders, sync] = await Promise.all([
      admin.database.from("dashboard_orders").select(columns).in("status", ["placed","packed","shipped","out_for_delivery","partially_delivered","awaiting_update","exchange"]).order("latest_email_at", { ascending: false }).limit(30),
      admin.database.from("dashboard_orders").select(columns).in("status", ["delivered","cancelled","returned"]).gte("latest_email_at", new Date(Date.now() - 14 * 86400000).toISOString()).order("latest_email_at", { ascending: false }).limit(30),
      admin.database.from("dashboard_order_sync").select("checked_at").eq("id", "gmail").limit(1)
    ]);
    if (activeOrders.error || recentOrders.error || sync.error) throw new Error("Order data unavailable");
    const checkedAt = sync.data?.[0]?.checked_at;
    const checkedTime = checkedAt ? Date.parse(checkedAt) : NaN;
    const stale = !Number.isFinite(checkedTime) || Date.now() - checkedTime > 6 * 3600000;
    const active = activeOrders.data || [];
    const recent = recentOrders.data || [];
    const rows = [...active, ...recent].slice(0, 30).map(o => {
      const expiry = Date.parse(o.otp_expires_at || "");
      const validOtp = !stale && o.status === "out_for_delivery" && expiry > Date.now() && /^[0-9]{4,8}$/.test(o.delivery_otp || "");
      return {
        store: o.store, number: o.order_number, items: o.items_summary,
        status: o.status, status_label: labels[o.status] || "Awaiting update",
        delivery: o.delivery_date ? dateLabel(o.delivery_date) + (o.delivery_window ? ` ${o.delivery_window}` : "") : "Not given",
        tracking: [o.courier, o.tracking_number].filter(Boolean).join(" "), note: o.notes,
        otp: validOtp ? o.delivery_otp : "", otp_expires_unix: validOtp ? Math.floor(expiry / 1000) : 0
      };
    });
    return json({ ok: true, status: checkedAt ? "connected" : "not_synced", stale, checked: Number.isFinite(checkedTime) ? new Intl.DateTimeFormat("en-GB", { timeZone: Deno.env.get("DASHBOARD_TIMEZONE") || "UTC", day: "2-digit", month: "short", hour: "2-digit", minute: "2-digit", timeZoneName: "short", hour12: true }).format(new Date(checkedTime)) : "Not checked yet",
      notice: stale ? "Email check overdue. Showing the last known order updates." : "Email updates every 4 hours. Delivery OTP appears only when provided.", orders: rows });
  } catch { return json({ ok: false, error: "Orders are temporarily unavailable" }, 503); }
}
function dateLabel(date: string) { return new Intl.DateTimeFormat("en-GB", { timeZone: "UTC", day: "2-digit", month: "short" }).format(new Date(`${date}T12:00:00Z`)); }
function required(key: string) { const value = Deno.env.get(key); if (!value) throw new Error("Missing configuration"); return value; }
function json(body: unknown, status = 200) { return new Response(status === 204 ? null : JSON.stringify(body), { status, headers: { "Content-Type": "application/json", "Cache-Control": "no-store", "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Methods": "GET, OPTIONS", "Access-Control-Allow-Headers": "X-Dashboard-Toggle-Token" } }); }
