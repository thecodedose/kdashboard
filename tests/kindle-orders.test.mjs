import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import ts from "typescript";
import { prepareOrders, ordersSql } from "../scripts/sync-dashboard-orders.mjs";

const now = new Date();
function harness({ orders = [], checked = now.toISOString(), error = false } = {}) {
  let reads = 0;
  const exports = {};
  const source = ts.transpileModule(readFileSync("functions/kindle-dashboard-orders.ts","utf8"), { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } }).outputText;
  vm.runInNewContext(source,{ exports, Request, Response, Date, Intl, Deno: { env: { get: key => ({DASHBOARD_TOGGLE_TOKEN:"device",INSFORGE_BASE_URL:"https://example.invalid",INSFORGE_API_KEY:"private"})[key] } },
    require: () => ({ createAdminClient() { return { database: { from(table) { reads++; let selected=orders; const q = { select(){return q;}, order(){return q;}, eq(){return q;}, in(key,values){selected=selected.filter(o=>values.includes(o[key]));return q;}, gte(key,value){selected=selected.filter(o=>o[key]>=value);return q;}, async limit(){return {error:error?{}:null,data:table==="dashboard_orders"?selected:[{checked_at:checked}]};} }; return q; } } }; } })
  });
  return { request: (token="device",method="GET") => exports.default(new Request("https://example.invalid/orders",{method,headers:{"x-dashboard-toggle-token":token}})), reads:()=>reads };
}
const row = { store:"Shop",order_number:"ABC123",items_summary:"A parcel",status:"out_for_delivery",delivery_date:null,delivery_window:"",courier:"",tracking_number:"",notes:"",latest_email_at:now.toISOString(),delivery_otp:"123456",otp_expires_at:new Date(+now+3600000).toISOString() };
test("orders and delivery codes are inaccessible without the device token", async()=>{
  const h=harness({orders:[row]});const r=await h.request("wrong");assert.equal(r.status,401);assert.equal(h.reads(),0);assert.doesNotMatch(await r.text(),/ABC123|123456/);
  assert.equal((await h.request("device","POST")).status,405);
});
test("only fresh delivery OTPs for out-for-delivery orders are returned",async()=>{
  const variants=[row,{...row,order_number:"delivered",status:"delivered"},{...row,order_number:"expired",otp_expires_at:new Date(+now-1).toISOString()},{...row,order_number:"other",status:"exchange"}];
  const result=await(await harness({orders:variants}).request()).json();
  assert.equal(result.orders.find(o=>o.number==="ABC123").otp,"123456");
  for(const item of result.orders.filter(o=>o.number!=="ABC123"))assert.equal(item.otp,"");
});
test("stale sync data suppresses OTPs and exposes the actual last check",async()=>{
  const h=harness({orders:[row],checked:new Date(+now-7*3600000).toISOString()});const data=await(await h.request()).json();
  assert.equal(data.stale,true);assert.equal(data.orders[0].otp,"");assert.match(data.checked,/UTC/);assert.match(data.notice,/overdue/);
});
test("partial orders remain active, unknown dates stay unknown, upstream errors are sanitized",async()=>{
  const data=await(await harness({orders:[{...row,status:"partially_delivered",delivery_otp:null}]}).request()).json();
  assert.equal(data.orders[0].status,"partially_delivered");assert.equal(data.orders[0].delivery,"Not given");
  assert.equal((await harness({error:true}).request()).status,503);
});
const inputRow={order_key:"shop:ABC123",store:"Shop",order_number:"ABC123",items_summary:"Item",status:"out_for_delivery",latest_email_at:now.toISOString(),source_message_ids:["0000000000000001"],delivery_otp:"123456",otp_expires_at:new Date(+now+3600000).toISOString()};
test("sync validates evidence, expiry, duplicates, and order statuses",()=>{
  const valid={checked_at:now.toISOString(),orders:[inputRow]};assert.equal(prepareOrders(valid,now).orders.length,1);
  for(const change of [{source_message_ids:[]},{status:"guessed"},{delivery_otp:"bank-code"},{otp_expires_at:new Date(+now+25*3600000).toISOString()}])assert.throws(()=>prepareOrders({...valid,orders:[{...inputRow,...change}]},now));
  assert.throws(()=>prepareOrders({...valid,orders:[inputRow,inputRow]},now));
  assert.throws(()=>prepareOrders({...valid,checked_at:new Date(+now-2*3600000).toISOString()},now));
  assert.equal(prepareOrders({...valid,orders:[{...inputRow,status:"delivered"}]},now).orders[0].delivery_otp,null);
  assert.equal(prepareOrders({...valid,orders:[{...inputRow,otp_expires_at:new Date(+now-1).toISOString()}]},now).orders[0].delivery_otp,null);
});
test("SQL upserts preserve newer updates and safely quote merchant text",()=>{
  const prepared=prepareOrders({checked_at:now.toISOString(),orders:[{...inputRow,store:"O'Reilly"}]},now);
  const sql=ordersSql(prepared);assert.match(sql,/O''Reilly/);assert.match(sql,/EXCLUDED.latest_email_at >= dashboard_orders.latest_email_at/);assert.doesNotMatch(sql,/DELETE FROM/);
});
