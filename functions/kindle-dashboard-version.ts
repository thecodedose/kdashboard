import { createAdminClient } from "npm:@insforge/sdk";

type PlannerItemVersion = {
  id: string;
  list_key: string;
  text: string;
  done: boolean;
  updated_at: string;
};

type PlannerListVersion = {
  key: string;
  title: string;
  sort_order: number;
  updated_at: string;
};

export default async function(req: Request): Promise<Response> {
  if (req.method === "OPTIONS") {
    return new Response(null, { status: 204, headers: corsHeaders() });
  }

  if (req.method !== "GET") {
    return jsonResponse({ ok: false, error: "Method not allowed" }, 405);
  }

  if (!isAuthorizedDashboardRead(req)) {
    return jsonResponse({ ok: false, error: "Unauthorized" }, 401);
  }

  try {
    const admin = createAdminClient({
      baseUrl: requiredEnv("INSFORGE_BASE_URL"),
      apiKey: requiredEnv("INSFORGE_API_KEY")
    });

    const version = await getPlannerVersion(admin);
    return jsonResponse({ ok: true, version, checked_at: new Date().toISOString() });
  } catch (error) {
    return jsonResponse({ ok: false, error: errorMessage(error) }, 500);
  }
}

async function getPlannerVersion(admin: any): Promise<string> {
  const { data: lists, error: listsError } = await admin.database
    .from("planner_lists")
    .select("key,title,sort_order,updated_at")
    .order("sort_order", { ascending: true });

  if (listsError) throw listsError;

  const { data: items, error: itemsError } = await admin.database
    .from("planner_items")
    .select("id,list_key,text,done,updated_at")
    .order("updated_at", { ascending: true });

  if (itemsError) throw itemsError;

  return hashText(JSON.stringify({
    lists: lists as PlannerListVersion[],
    items: items as PlannerItemVersion[]
  }));
}

function hashText(value: string): string {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0).toString(16);
}

function corsHeaders(): HeadersInit {
  return {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type, X-Dashboard-Read-Token, Authorization"
  };
}

function isAuthorizedDashboardRead(req: Request): boolean {
  const configuredToken = Deno.env.get("DASHBOARD_READ_TOKEN");
  const receivedToken =
    req.headers.get("x-dashboard-read-token") ||
    bearerToken(req.headers.get("authorization")) ||
    new URL(req.url).searchParams.get("read_token");
  return Boolean(configuredToken && receivedToken) && receivedToken === configuredToken;
}

function bearerToken(header: string | null): string {
  const match = /^Bearer\s+(.+)$/i.exec(header || "");
  return match?.[1]?.trim() || "";
}

function requiredEnv(key: string): string {
  const value = Deno.env.get(key);
  if (!value) throw new Error(`Missing ${key}`);
  return value;
}

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      ...corsHeaders(),
      "Content-Type": "application/json; charset=utf-8",
      "Cache-Control": "no-store"
    }
  });
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
