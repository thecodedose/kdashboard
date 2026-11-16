import { createAdminClient } from "npm:@insforge/sdk";

type ListKey = "grocery" | "todo";

type PlannerItem = {
  id: string;
  list_key: ListKey;
  text: string;
  done: boolean;
  created_at: string;
  updated_at: string;
};

type HealthSummary = {
  source: string;
  date: string;
  steps: number;
  dietary_energy_kcal: string | number;
  synced_at: string;
  updated_at: string;
};

type HealthTarget = {
  metric: "steps" | "calories";
  label: string;
  target_value: string | number;
  unit: string;
  updated_at: string;
};

type ChallengeLog = {
  date: string;
  water_l: string | number;
  sleep_hours: string | number;
  workouts: number;
  updated_at: string;
};

type MealPlanEntry = {
  date: string;
  recipe_id: string;
  sort_order: number;
  updated_at: string;
};

type RecipeVersion = {
  id: string;
  rating: string | number;
  updated_at: string;
};

type DashboardData = {
  items: PlannerItem[];
  health: HealthSummary | null;
  targets: HealthTarget[];
  challenge: ChallengeLog | null;
  mealPlan: MealPlanEntry[];
  recipes: RecipeVersion[];
  noteUpdatedAt: string | null;
};

export default function(req: Request): Response {
  if (req.method === "OPTIONS") {
    return new Response(null, { status: 204, headers: corsHeaders() });
  }

  if (req.method !== "GET") {
    return new Response("Method not allowed", {
      status: 405,
      headers: { ...corsHeaders(), "Content-Type": "text/plain; charset=utf-8" }
    });
  }

  if (!isAuthorizedDashboardRead(req)) {
    return new Response(JSON.stringify({ ok: false, error: "Unauthorized" }), {
      status: 401, headers: { ...corsHeaders(), "Content-Type": "application/json; charset=utf-8" }
    });
  }

  const encoder = new TextEncoder();
  let lastVersion = "";
  let intervalId: ReturnType<typeof setInterval> | undefined;
  let timeoutId: ReturnType<typeof setTimeout> | undefined;
  let closed = false;

  const stream = new ReadableStream({
    start(controller) {
      async function pushIfChanged(force = false) {
        if (closed) return;
        try {
          const data = await loadDashboardData();
          if (closed) return;
          const version = getDashboardVersion(data);
          if (!force && version === lastVersion) {
            controller.enqueue(encoder.encode(`: heartbeat ${new Date().toISOString()}\n\n`));
            return;
          }

          lastVersion = version;
          controller.enqueue(encoder.encode(`event: planner\n`));
          controller.enqueue(encoder.encode(`data: ${version}\n\n`));
        } catch (error) {
          controller.enqueue(encoder.encode(`event: planner-error\n`));
          controller.enqueue(encoder.encode(`data: ${escapeHtml(errorMessage(error))}\n\n`));
        }
      }

      controller.enqueue(encoder.encode(`: connected ${new Date().toISOString()}\n\n`));
      void pushIfChanged(true);
      intervalId = setInterval(() => void pushIfChanged(), 2000);
      timeoutId = setTimeout(() => {
        closed = true;
        if (intervalId) clearInterval(intervalId);
        controller.close();
      }, 55000);
    },
    cancel() {
      closed = true;
      if (intervalId) clearInterval(intervalId);
      if (timeoutId) clearTimeout(timeoutId);
    }
  });

  return new Response(stream, {
    status: 200,
    headers: {
      ...corsHeaders(),
      "Content-Type": "text/event-stream; charset=utf-8",
      "Cache-Control": "no-store, no-transform",
      "X-Accel-Buffering": "no"
    }
  });
}

async function loadDashboardData(): Promise<DashboardData> {
  const started = timeMs();
  const admin = createAdminClient({
    baseUrl: requiredEnv("INSFORGE_BASE_URL"),
    apiKey: requiredEnv("INSFORGE_API_KEY")
  });
  const today = dashboardLocalDate();

  const baseStarted = timeMs();
  const [
    itemsResult,
    healthResult,
    challengeResult,
    targetsResult,
    mealPlanResult,
    recipesResult,
    noteResult
  ] = await Promise.all([
    admin.database
      .from("planner_items")
      .select("id,list_key,text,done,created_at,updated_at")
      .in("list_key", ["todo", "grocery"])
      .order("created_at", { ascending: false }),
    admin.database
      .from("health_daily_summaries")
      .select("source,date,steps,dietary_energy_kcal,synced_at,updated_at")
      .in("source", ["google_health", "apple_health_ios"])
      .eq("date", today)
      .limit(2),
    admin.database
      .from("challenge_daily_logs")
      .select("date,water_l,sleep_hours,workouts,updated_at")
      .eq("date", today)
      .limit(1),
    admin.database
      .from("health_targets")
      .select("metric,label,target_value,unit,updated_at")
      .order("metric", { ascending: true }),
    admin.database
      .from("meal_plan_entries")
      .select("date,recipe_id,sort_order,updated_at")
      .eq("date", today)
      .order("sort_order", { ascending: true }),
    admin.database
      .from("recipes")
      .select("id,rating,updated_at")
      .order("id", { ascending: true }),
    // Only a timestamp participates in the public change hash; note text stays private.
    admin.database.from("dashboard_notes").select("updated_at").eq("id", "quick_note").limit(1)
  ]);
  const baseQueryMs = elapsedMs(baseStarted);

  const { data: items, error: itemsError } = itemsResult;
  if (itemsError) throw itemsError;

  const { data: healthRows, error: healthError } = healthResult;
  if (healthError) throw healthError;

  const { data: challengeRows, error: challengeError } = challengeResult;
  if (challengeError) throw challengeError;

  const { data: targets, error: targetsError } = targetsResult;
  if (targetsError) throw targetsError;

  const { data: mealPlanRows, error: mealPlanError } = mealPlanResult;
  if (mealPlanError) throw mealPlanError;

  const { data: recipeRows, error: recipesError } = recipesResult;
  if (recipesError) throw recipesError;

  if (noteResult.error) throw noteResult.error;
  const payload = {
    items: items as PlannerItem[],
    health: preferredHealthSummary(healthRows),
    targets: targets as HealthTarget[],
    challenge: firstRow<ChallengeLog>(challengeRows),
    mealPlan: mealPlanRows as MealPlanEntry[],
    recipes: recipeRows as RecipeVersion[],
    noteUpdatedAt: firstRow<{ updated_at: string }>(noteResult.data)?.updated_at ?? null
  };
  logTiming("kindle-dashboard-events", {
    base_query_ms: baseQueryMs,
    payload_build_ms: elapsedMs(started) - baseQueryMs,
    total_ms: elapsedMs(started)
  });
  return payload;
}

function getDashboardVersion(data: DashboardData): string {
  return hashText(JSON.stringify({
    items: [...data.items].sort((a, b) => a.updated_at.localeCompare(b.updated_at)),
    health: data.health,
    challenge: data.challenge,
    mealPlan: data.mealPlan,
    recipes: data.recipes,
    noteUpdatedAt: data.noteUpdatedAt,
    targets: [...data.targets].sort((a, b) => a.metric.localeCompare(b.metric))
  }));
}

function preferredHealthSummary(rows: unknown): HealthSummary | null {
  if (!Array.isArray(rows)) return null;
  const summaries = rows as HealthSummary[];
  const google = summaries.find((row) => row.source === "google_health");
  if (google || Deno.env.get("GOOGLE_HEALTH_REFRESH_TOKEN")) return google ?? null;
  return summaries.find((row) => row.source === "apple_health_ios") ?? null;
}

function firstRow<T>(rows: unknown): T | null {
  return Array.isArray(rows) && rows.length > 0 ? rows[0] as T : null;
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

function requiredEnv(key: string): string {
  const value = Deno.env.get(key);
  if (!value) throw new Error(`Missing ${key}`);
  return value;
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function timeMs(): number {
  return performance.now();
}

function elapsedMs(started: number): number {
  return Math.round(performance.now() - started);
}

function logTiming(label: string, timing: Record<string, number>): void {
  console.log(`${label} timing ${JSON.stringify(timing)}`);
}

function escapeHtml(value: string): string {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
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


function dashboardLocalDate(): string {
  const timezone = Deno.env.get("DASHBOARD_TIMEZONE") || "UTC";
  const parts = new Intl.DateTimeFormat("en-CA", {
    timeZone: timezone,
    year: "numeric",
    month: "2-digit",
    day: "2-digit"
  }).formatToParts(new Date());
  const byType = Object.fromEntries(parts.map((part) => [part.type, part.value]));
  return `${byType.year}-${byType.month}-${byType.day}`;
}
