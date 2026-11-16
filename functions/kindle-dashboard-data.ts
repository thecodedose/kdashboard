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
  protein_g: string | number;
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

type RecipeRow = {
  id: string;
  title: string;
  photo_url: string | null;
  photo_key: string | null;
  total_calories: string | number;
  carbs_g: string | number;
  fat_g: string | number;
  protein_g: string | number;
  rating: string | number;
  instructions: string;
  created_at: string;
  updated_at: string;
};

type RecipeIngredientRow = {
  recipe_id: string;
  name: string;
  amount: string;
  calories: string | number | null;
  sort_order: number;
};

type MealPlanEntryRow = {
  date: string;
  recipe_id: string;
  sort_order: number;
  updated_at: string;
};

type RecipePayload = {
  id: string;
  title: string;
  photo_url: string | null;
  photo_key: string | null;
  total_calories: number;
  carbs_g: number;
  fat_g: number;
  protein_g: number;
  rating: number;
  instructions: string;
  ingredients: Array<{
    name: string;
    amount: string;
    calories: number | null;
    sort_order: number;
  }>;
};

type DashboardPayload = {
  ok: true;
  generated_at: string;
  version: string;
  health: {
    date: string | null;
    steps: number;
    calories: number;
    protein_g: number;
    steps_target: number;
    calories_target: number;
    steps_unit: string;
    calories_unit: string;
    synced_at: string | null;
  };
  challenge: {
    date: string;
    day: number;
    water_l: number;
    water_target_l: number;
    sleep_hours: number;
    sleep_target_hours: number;
    workouts: number;
    workout_target: number;
  };
  lists: Array<{
    key: ListKey;
    title: string;
    items: Array<{
      id: string;
      text: string;
      done: boolean;
      updated_at: string;
    }>;
  }>;
  meal_plan: RecipePayload[];
  recipes: RecipePayload[];
};

const LIST_TITLES: Record<ListKey, string> = {
  todo: "Chores",
  grocery: "Grocery"
};
const COMPLETED_ITEM_HIDE_AFTER_MS = 24 * 60 * 60 * 1000;

export default async function(req: Request): Promise<Response> {
  const requestStarted = timeMs();
  if (req.method === "OPTIONS") {
    return new Response(null, { status: 204, headers: corsHeaders() });
  }

  if (req.method !== "GET") {
    return jsonResponse({ ok: false, error: "Method not allowed" }, 405);
  }

  if (!isAuthorizedDashboardRead(req)) {
    return new Response(JSON.stringify({ ok: false, error: "Unauthorized" }), {
      status: 401, headers: { ...corsHeaders(), "Content-Type": "application/json; charset=utf-8" }
    });
  }

  try {
    const requestedDate = dashboardRequestedDate(req);
    const payload = await loadDashboardPayload(requestedDate);
    const deviceToken = Deno.env.get("DASHBOARD_TOGGLE_TOKEN");
    if (deviceToken && req.headers.get("x-dashboard-toggle-token") === deviceToken) {
      const url = new URL(`${requiredEnv("INSFORGE_BASE_URL")}/functions/kindle-dashboard-widgets`);
      url.searchParams.set("date", requestedDate);
      const response = await fetch(url, { headers: { "x-dashboard-toggle-token": deviceToken }, signal: AbortSignal.timeout(40000) });
      if (!response.ok) throw new Error("Private widgets are temporarily unavailable");
      const widgets = await response.json();
      return jsonResponse({ ...payload, quick_note: widgets.quick_note, calendar: widgets.calendar,
        version: hashText(JSON.stringify([payload.version, widgets.quick_note, widgets.calendar])) });
    }
    return jsonResponse(payload);
  } catch (error) {
    return jsonResponse({ ok: false, error: errorMessage(error) }, 500);
  } finally {
    logTiming("kindle-dashboard-data", { total_ms: elapsedMs(requestStarted) });
  }
}

async function loadDashboardPayload(today = dashboardLocalDate()): Promise<DashboardPayload> {
  const admin = createAdminClient({
    baseUrl: requiredEnv("INSFORGE_BASE_URL"),
    apiKey: requiredEnv("INSFORGE_API_KEY")
  });

  const baseStarted = timeMs();
  const [
    itemsResult,
    healthResult,
    challengeResult,
    challengeStartResult,
    mealPlanResult,
    targetsResult,
    recipesResult
  ] = await Promise.all([
    admin.database
      .from("planner_items")
      .select("id,list_key,text,done,created_at,updated_at")
      .in("list_key", ["todo", "grocery"])
      .order("created_at", { ascending: false }),
    admin.database
      .from("health_daily_summaries")
      .select("source,date,steps,dietary_energy_kcal,protein_g,synced_at,updated_at")
      .in("source", ["google_health", "apple_health_ios"])
      .eq("date", today)
      .limit(2),
    admin.database
      .from("challenge_daily_logs")
      .select("date,water_l,sleep_hours,workouts,updated_at")
      .eq("date", today)
      .limit(1),
    admin.database
      .from("challenge_daily_logs")
      .select("date")
      .order("date", { ascending: true })
      .limit(1),
    admin.database
      .from("meal_plan_entries")
      .select("date,recipe_id,sort_order,updated_at")
      .eq("date", today)
      .order("sort_order", { ascending: true }),
    admin.database
      .from("health_targets")
      .select("metric,label,target_value,unit,updated_at")
      .order("metric", { ascending: true }),
    admin.database
      .from("recipes")
      .select("id,title,photo_url,photo_key,total_calories,carbs_g,fat_g,protein_g,rating,instructions,created_at,updated_at")
      .order("title", { ascending: true })
  ]);
  const baseQueryMs = elapsedMs(baseStarted);

  const { data: items, error: itemsError } = itemsResult;
  if (itemsError) throw itemsError;

  const { data: healthRows, error: healthError } = healthResult;
  if (healthError) throw healthError;

  const { data: challengeRows, error: challengeError } = challengeResult;
  if (challengeError) throw challengeError;

  const { data: challengeStartRows, error: challengeStartError } = challengeStartResult;
  if (challengeStartError) throw challengeStartError;

  const { data: mealPlanRows, error: mealPlanError } = mealPlanResult;
  if (mealPlanError) throw mealPlanError;

  const { data: targets, error: targetsError } = targetsResult;
  if (targetsError) throw targetsError;

  const { data: recipes, error: recipesError } = recipesResult;
  if (recipesError) throw recipesError;

  const recipeRows = recipes as RecipeRow[];
  const recipeIds = recipeRows.map((recipe) => recipe.id);
  let ingredientRows: RecipeIngredientRow[] = [];
  const ingredientsStarted = timeMs();

  if (recipeIds.length > 0) {
    const { data: recipeIngredients, error: ingredientsError } = await admin.database
      .from("recipe_ingredients")
      .select("recipe_id,name,amount,calories,sort_order")
      .in("recipe_id", recipeIds)
      .order("sort_order", { ascending: true });

    if (ingredientsError) throw ingredientsError;
    ingredientRows = recipeIngredients as RecipeIngredientRow[];
  }
  const ingredientsQueryMs = elapsedMs(ingredientsStarted);

  const buildStarted = timeMs();
  const staleCompletedCutoff = Date.now() - COMPLETED_ITEM_HIDE_AFTER_MS;
  const plannerItems = (items as PlannerItem[]).filter((item) => shouldShowPlannerItem(item, staleCompletedCutoff));
  const health = preferredHealthSummary(healthRows);
  const challenge = firstRow<ChallengeLog>(challengeRows);
  const challengeStart = firstRow<Pick<ChallengeLog, "date">>(challengeStartRows);
  const challengeDay = challengeStart?.date ? challengeDayFor(today, challengeStart.date) : 1;
  const healthTargets = targets as HealthTarget[];
  const mealPlanEntries = mealPlanRows as MealPlanEntryRow[];
  const stepsTarget = metricTarget(healthTargets, "steps", 10000, "steps");
  const caloriesTarget = metricTarget(healthTargets, "calories", 2000, "kcal");
  const ingredientsByRecipeId = groupIngredientsByRecipeId(ingredientRows);
  const recipePayloads = recipeRows.map((recipe) => recipePayload(recipe, ingredientsByRecipeId));
  const recipesById = new Map(recipePayloads.map((recipe) => [recipe.id, recipe]));

  const payloadWithoutVersion = {
    ok: true as const,
    generated_at: `${today}T00:00:00+05:30`,
    health: {
      date: health?.date ?? null,
      steps: Math.max(0, Number(health?.steps ?? 0)),
      calories: Math.max(0, Number(health?.dietary_energy_kcal ?? 0)),
      protein_g: Math.max(0, Number(health?.protein_g ?? 0)),
      steps_target: Math.max(0, Number(stepsTarget.target_value)),
      calories_target: Math.max(0, Number(caloriesTarget.target_value)),
      steps_unit: stepsTarget.unit,
      calories_unit: caloriesTarget.unit,
      synced_at: health?.synced_at ?? null
    },
    challenge: {
      date: today,
      day: challengeDay,
      water_l: Math.max(0, Number(challenge?.water_l ?? 0)),
      water_target_l: 3,
      sleep_hours: Math.max(0, Number(challenge?.sleep_hours ?? 0)),
      sleep_target_hours: 8,
      workouts: Math.max(0, Number(challenge?.workouts ?? 0)),
      workout_target: 2
    },
    lists: (["todo", "grocery"] as const).map((key) => ({
      key,
      title: LIST_TITLES[key],
      items: plannerItems
        .filter((item) => item.list_key === key)
        .sort((a, b) => b.created_at.localeCompare(a.created_at))
        .map((item) => ({
          id: item.id,
          text: item.text,
          done: item.done,
          updated_at: item.updated_at
        }))
    })),
    meal_plan: mealPlanEntries
      .map((entry) => recipesById.get(entry.recipe_id))
      .filter((recipe): recipe is RecipePayload => Boolean(recipe)),
    recipes: recipePayloads
  };

  const payload = {
    ...payloadWithoutVersion,
    version: hashText(JSON.stringify({
      health: payloadWithoutVersion.health,
      challenge: payloadWithoutVersion.challenge,
      lists: payloadWithoutVersion.lists,
      meal_plan: payloadWithoutVersion.meal_plan,
      recipes: payloadWithoutVersion.recipes
    }))
  };
  logTiming("kindle-dashboard-data", {
    base_query_ms: baseQueryMs,
    ingredients_query_ms: ingredientsQueryMs,
    payload_build_ms: elapsedMs(buildStarted),
    recipe_count: recipeRows.length,
    ingredient_count: ingredientRows.length
  });
  return payload;
}

function groupIngredientsByRecipeId(ingredientRows: RecipeIngredientRow[]): Map<string, RecipeIngredientRow[]> {
  const ingredientsByRecipeId = new Map<string, RecipeIngredientRow[]>();
  for (const ingredient of ingredientRows) {
    const existing = ingredientsByRecipeId.get(ingredient.recipe_id);
    if (existing) existing.push(ingredient);
    else ingredientsByRecipeId.set(ingredient.recipe_id, [ingredient]);
  }
  return ingredientsByRecipeId;
}

function recipePayload(recipe: RecipeRow, ingredientsByRecipeId: Map<string, RecipeIngredientRow[]>): RecipePayload {
  const ingredients = ingredientsByRecipeId.get(recipe.id) ?? [];
  return {
    id: recipe.id,
    title: recipe.title,
    photo_url: recipe.photo_url,
    photo_key: recipe.photo_key,
    total_calories: Math.max(0, Number(recipe.total_calories ?? 0)),
    carbs_g: Math.max(0, Number(recipe.carbs_g ?? 0)),
    fat_g: Math.max(0, Number(recipe.fat_g ?? 0)),
    protein_g: Math.max(0, Number(recipe.protein_g ?? 0)),
    rating: clampRating(recipe.rating),
    instructions: recipe.instructions,
    ingredients: [...ingredients]
      .sort((a, b) => a.sort_order - b.sort_order)
      .map((ingredient) => ({
        name: ingredient.name,
        amount: ingredient.amount,
        calories: ingredient.calories == null ? null : Math.max(0, Number(ingredient.calories)),
        sort_order: ingredient.sort_order
      }))
  };
}

function clampRating(value: string | number): number {
  const rating = Number(value ?? 0);
  if (!Number.isFinite(rating)) return 0;
  return Math.max(0, Math.min(5, Math.round(rating * 10) / 10));
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

function dashboardRequestedDate(req: Request): string {
  const url = new URL(req.url);
  const explicitDate = url.searchParams.get("date");
  if (explicitDate && /^\d{4}-\d{2}-\d{2}$/.test(explicitDate)) {
    return explicitDate;
  }

  const offset = Number(url.searchParams.get("offset") ?? 0);
  if (!Number.isFinite(offset) || !Number.isInteger(offset) || offset === 0) {
    return dashboardLocalDate();
  }

  return addLocalDays(dashboardLocalDate(), Math.max(-365, Math.min(365, offset)));
}

function addLocalDays(date: string, offset: number): string {
  const [year, month, day] = date.split("-").map(Number);
  const value = new Date(Date.UTC(year, month - 1, day + offset));
  const y = value.getUTCFullYear();
  const m = String(value.getUTCMonth() + 1).padStart(2, "0");
  const d = String(value.getUTCDate()).padStart(2, "0");
  return `${y}-${m}-${d}`;
}

function challengeDayFor(today: string, startDate: string): number {
  const diff = localDateIndex(today) - localDateIndex(startDate);
  if (!Number.isFinite(diff)) return 1;
  return Math.max(1, Math.min(75, diff + 1));
}

function localDateIndex(value: string): number {
  const [year, month, day] = value.slice(0, 10).split("-").map(Number);
  if (!year || !month || !day) return Number.NaN;
  return Math.floor(Date.UTC(year, month - 1, day) / 86400000);
}

function metricTarget(
  targets: HealthTarget[],
  metric: HealthTarget["metric"],
  fallbackValue: number,
  fallbackUnit: string
): HealthTarget {
  return targets.find((target) => target.metric === metric) ?? {
    metric,
    label: metric,
    target_value: fallbackValue,
    unit: fallbackUnit,
    updated_at: ""
  };
}

// Google Health owns the entire daily summary once connected, including zero
// food totals after a log is deleted. Never mix providers or add their steps.
// After account setup, a missing Google row must not silently switch providers.
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

function shouldShowPlannerItem(item: PlannerItem, staleCompletedCutoff: number): boolean {
  if (!item.done) return true;
  const updatedAt = Date.parse(item.updated_at);
  if (!Number.isFinite(updatedAt)) return true;
  return updatedAt > staleCompletedCutoff;
}

function corsHeaders(): HeadersInit {
  return {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type, X-Dashboard-Toggle-Token, X-Dashboard-Read-Token, Authorization"
  };
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

function timeMs(): number {
  return performance.now();
}

function elapsedMs(started: number): number {
  return Math.round(performance.now() - started);
}

function logTiming(label: string, timing: Record<string, number>): void {
  console.log(`${label} timing ${JSON.stringify(timing)}`);
}

function hashText(value: string): string {
  let hash = 2166136261;
  for (let index = 0; index < value.length; index += 1) {
    hash ^= value.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0).toString(16);
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
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
