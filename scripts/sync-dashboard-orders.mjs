import { readFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { pathToFileURL } from "node:url";

export function prepareOrders(input, now = new Date()) {
  if (!input || !Array.isArray(input.orders) || input.orders.length > 60) throw new Error("Supply at most 60 orders.");
  const checked = new Date(input.checked_at);
  if (!Number.isFinite(+checked) || +checked > +now + 60000 || +now - +checked > 3600000) throw new Error("checked_at must be the time of a successful recent Gmail check.");
  const statuses = new Set(["placed","packed","shipped","out_for_delivery","partially_delivered","awaiting_update","exchange","delivered","cancelled","returned"]);
  const keys = new Set();
  const rows = input.orders.map(order => {
    const text = (key, max, required = false) => {
      const value = order[key] ?? "";
      if (typeof value !== "string" || value.length > max || /[\x00-\x1f]/.test(value) || (required && !value.trim())) throw new Error(`Invalid ${key}.`);
      return value;
    };
    const row = { order_key: text("order_key",160,true), store: text("store",60,true), order_number: text("order_number",80,true), items_summary: text("items_summary",380), status: text("status",32,true),
      delivery_date: order.delivery_date || null, delivery_window: text("delivery_window",32), courier: text("courier",48), tracking_number: text("tracking_number",48), tracking_url: text("tracking_url",2048), notes: text("notes",220),
      latest_email_at: order.latest_email_at, source_message_ids: order.source_message_ids, delivery_otp: null, otp_expires_at: null };
    if (keys.has(row.order_key) || !statuses.has(row.status)) throw new Error("Duplicate order or invalid status.");
    keys.add(row.order_key);
    if (row.delivery_date && (!/^\d{4}-\d{2}-\d{2}$/.test(row.delivery_date) || new Date(row.delivery_date).toISOString().slice(0,10) !== row.delivery_date)) throw new Error("Invalid delivery date.");
    if (!Number.isFinite(Date.parse(row.latest_email_at)) || Date.parse(row.latest_email_at) > +now + 60000) throw new Error("Invalid email timestamp.");
    if (!Array.isArray(row.source_message_ids) || !row.source_message_ids.length || row.source_message_ids.length > 30 || row.source_message_ids.some(id => typeof id !== "string" || !/^[a-f0-9]{10,40}$/i.test(id))) throw new Error("Gmail source message IDs are required.");
    if (row.tracking_url && new URL(row.tracking_url).protocol !== "https:") throw new Error("Tracking links must use HTTPS.");
    if (order.delivery_otp && row.status === "out_for_delivery") {
      const expiry = Date.parse(order.otp_expires_at);
      if (!/^[0-9]{4,8}$/.test(order.delivery_otp) || !Number.isFinite(expiry) || expiry > Date.parse(row.latest_email_at) + 86400000) throw new Error("Delivery OTP needs an explicit expiry within 24 hours of its source email.");
      if (expiry > +now) { row.delivery_otp = order.delivery_otp; row.otp_expires_at = new Date(expiry).toISOString(); }
    }
    return row;
  });
  return { checked_at: checked.toISOString(), orders: rows };
}
export function ordersSql(prepared) {
  const quote = value => "'" + String(value).replaceAll("'", "''") + "'";
  const columns = "order_key,store,order_number,items_summary,status,delivery_date,delivery_window,courier,tracking_number,tracking_url,delivery_otp,otp_expires_at,notes,latest_email_at,source_message_ids";
  const assignments = columns.split(",").filter(c => c !== "order_key").map(c => `${c}=EXCLUDED.${c}`).join(",");
  return `WITH incoming AS (SELECT * FROM jsonb_to_recordset(${quote(JSON.stringify(prepared.orders))}::jsonb) AS x(order_key text,store text,order_number text,items_summary text,status text,delivery_date date,delivery_window text,courier text,tracking_number text,tracking_url text,delivery_otp text,otp_expires_at timestamptz,notes text,latest_email_at timestamptz,source_message_ids text[])), written AS (INSERT INTO public.dashboard_orders (${columns}) SELECT ${columns} FROM incoming ON CONFLICT (order_key) DO UPDATE SET ${assignments} WHERE EXCLUDED.latest_email_at >= dashboard_orders.latest_email_at RETURNING order_key), sync AS (INSERT INTO public.dashboard_order_sync (id,checked_at) VALUES ('gmail',${quote(prepared.checked_at)}::timestamptz) ON CONFLICT(id) DO UPDATE SET checked_at=GREATEST(dashboard_order_sync.checked_at,EXCLUDED.checked_at) RETURNING checked_at) SELECT (SELECT count(*) FROM written)::integer AS orders_updated,checked_at FROM sync`;
}
function cli(args) {
  try { return JSON.parse(execFileSync("npx", ["-y", "@insforge/cli", ...args, "--json"], { encoding: "utf8", stdio: ["ignore","pipe","pipe"] })); }
  catch { throw new Error(`InsForge ${args[0]} ${args[1] || ""} failed. Private order data was withheld from error output.`); }
}
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try {
    if (process.argv.includes("--help")) console.log("Usage: node scripts/sync-dashboard-orders.mjs path/to/private-orders.json [--dry-run]\nSee docs/kindle-orders.md. Reads an evidence-backed Gmail summary; never sends mail or changes Gmail.");
    else {
      const file = process.argv.slice(2).find(arg => !arg.startsWith("--"));
      if (!file) throw new Error("Supply a private order summary JSON file.");
      const prepared = prepareOrders(JSON.parse(readFileSync(file,"utf8")));
      if (process.argv.includes("--dry-run")) console.log(JSON.stringify({ valid: true, orders: prepared.orders.length, otp_count: prepared.orders.filter(o=>o.delivery_otp).length }));
      else {
        const project = JSON.parse(readFileSync(".insforge/project.json","utf8"));
        const expected = process.env.ORDERS_PROJECT_ID;
        if (!expected || project.project_id !== expected) throw new Error("Set ORDERS_PROJECT_ID to your linked backend project ID before syncing orders.");
        const current = cli(["current"]);
        if (current.project?.project_id !== expected) throw new Error("The active InsForge project does not match ORDERS_PROJECT_ID.");
        cli(["db","query","UPDATE public.dashboard_orders SET delivery_otp=NULL,otp_expires_at=NULL WHERE delivery_otp IS NOT NULL AND otp_expires_at <= now()"]);
        const result = cli(["db","query",ordersSql(prepared)]);
        console.log(JSON.stringify(result.rows?.[0] || { synced: true }));
      }
    }
  } catch (error) { console.error(error.message); process.exitCode = 1; }
}
