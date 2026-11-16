import { createAdminClient } from "npm:@insforge/sdk";

type ListKey = "grocery" | "workout" | "meal" | "todo";

type PlannerAction = {
  kind?: "planner";
  action: "add" | "complete" | "uncomplete" | "delete" | "clear";
  list_key: ListKey;
  items: string[];
  all_lists?: boolean;
};

type HealthTargetAction = {
  kind: "target";
  action: "set_target";
  metric: "steps" | "calories";
  value: number;
  unit?: string;
};

type ChallengeAction = {
  kind: "challenge";
  action: "add_water" | "set_sleep" | "add_workout";
  value: number;
};

type RecipeIngredientInput = {
  name: string;
  amount: string;
  calories?: number | null;
};

type RecipeAction = {
  kind: "recipe";
  action: "add_recipe";
  title: string;
  total_calories: number;
  carbs_g: number;
  fat_g: number;
  protein_g: number;
  rating: number;
  instructions?: string;
  ingredients: RecipeIngredientInput[];
};

type RecipeRatingAction = {
  kind: "recipe_rating";
  action: "rate_recipe";
  title: string;
  rating: number;
};

type MealPlanAction = {
  kind: "meal_plan";
  action: "add_meal" | "set_meal_plan" | "clear_meal_plan";
  recipes: string[];
};

type TelegramAction = PlannerAction | HealthTargetAction | RecipeAction | RecipeRatingAction | ChallengeAction | MealPlanAction;

type TelegramUpdate = {
  message?: {
    chat?: { id?: number | string };
    text?: string;
  };
};

export default async function(req: Request): Promise<Response> {
  const started = timeMs();
  if (req.method === "OPTIONS") {
    return new Response(null, { status: 204, headers: corsHeaders() });
  }

  if (req.method === "GET") {
    return jsonResponse({ ok: true, service: "telegram-webhook" });
  }

  if (req.method !== "POST") {
    return jsonResponse({ ok: false, error: "Method not allowed" }, 405);
  }

  const configuredSecret = requiredEnv("TELEGRAM_WEBHOOK_SECRET");
  const receivedSecret = req.headers.get("x-telegram-bot-api-secret-token");
  if (receivedSecret !== configuredSecret) {
    return jsonResponse({ ok: false, error: "Unauthorized" }, 401);
  }

  const update = (await req.json()) as TelegramUpdate;
  const chatId = String(update.message?.chat?.id ?? "");
  const allowedChatId = requiredEnv("TELEGRAM_ALLOWED_CHAT_ID");
  if (chatId !== allowedChatId) {
    return jsonResponse({ ok: true, ignored: true, reason: "chat_not_allowed" });
  }

  const text = update.message?.text?.trim();
  if (!text) {
    return jsonResponse({ ok: true, ignored: true, reason: "no_text" });
  }

  const parseStarted = timeMs();
  const action = await parseTelegramMessage(text);
  const parseMs = elapsedMs(parseStarted);
  if (!action) {
    sendTelegramMessageInBackground(chatId, "I could not understand that update.");
    return jsonResponse({ ok: true, ignored: true, reason: "unparsed" });
  }

  const admin = createAdminClient({
    baseUrl: requiredEnv("INSFORGE_BASE_URL"),
    apiKey: requiredEnv("INSFORGE_API_KEY")
  });

  const applyStarted = timeMs();
  const summary = await applyTelegramAction(admin, action);
  const applyMs = elapsedMs(applyStarted);
  sendTelegramMessageInBackground(chatId, summary);
  logTiming("telegram-webhook", {
    action: action.kind || "planner",
    parse_ms: parseMs,
    apply_ms: applyMs,
    total_ms: elapsedMs(started)
  });

  return jsonResponse({ ok: true, action, summary });
}

async function parseTelegramMessage(message: string): Promise<TelegramAction | null> {
  const fastAction = parseFastHeuristicMessage(message);
  if (fastAction) return fastAction;

  const openAiKey = Deno.env.get("OPENAI_API_KEY");
  if (!openAiKey) {
    return parseMessageHeuristically(message);
  }

  const response = await fetch("https://api.openai.com/v1/chat/completions", {
    method: "POST",
    headers: {
      "Authorization": `Bearer ${openAiKey}`,
      "Content-Type": "application/json"
    },
    body: JSON.stringify({
      model: Deno.env.get("OPENAI_MODEL") || "gpt-4o-mini",
      messages: [
        {
          role: "system",
          content:
            [
              "Parse one Telegram dashboard message into strict JSON.",
              "For planner/list updates return: {\"kind\":\"planner\",\"action\":\"add|complete|uncomplete|delete|clear\",\"list_key\":\"grocery|workout|meal|todo\",\"items\":[\"short item\"],\"all_lists\":false}. Use list_key \"todo\" for chores/tasks. Use [] only for clear.",
              "For health targets return: {\"kind\":\"target\",\"action\":\"set_target\",\"metric\":\"steps|calories\",\"value\":12000,\"unit\":\"steps|kcal\"}.",
              "For 75 day challenge check-ins return: {\"kind\":\"challenge\",\"action\":\"add_water|set_sleep|add_workout\",\"value\":1}. Treat XL water as 1 liter, sleep value as hours, and workout value as one completed workout.",
              "For today's meal plan made from saved recipes return: {\"kind\":\"meal_plan\",\"action\":\"add_meal|set_meal_plan|clear_meal_plan\",\"recipes\":[\"Saved Recipe Title\"]}. Use add_meal for adding/include/put another meal; use set_meal_plan only when replacing the whole plan.",
              "For rating a saved meal or recipe return: {\"kind\":\"recipe_rating\",\"action\":\"rate_recipe\",\"title\":\"Saved Recipe Title\",\"rating\":4.5}. Rating is out of 5.",
              "For recipes return: {\"kind\":\"recipe\",\"action\":\"add_recipe\",\"title\":\"Recipe Title\",\"total_calories\":429,\"carbs_g\":47.3,\"fat_g\":10.2,\"protein_g\":38.5,\"rating\":4,\"instructions\":\"optional steps\",\"ingredients\":[{\"name\":\"Paneer\",\"amount\":\"100 g\",\"calories\":163}]}. Rating is out of 5."
            ].join(" ")
        },
        { role: "user", content: message }
      ],
      response_format: { type: "json_object" },
      temperature: 0
    })
  });

  if (!response.ok) {
    return parseMessageHeuristically(message);
  }

  const payload = await response.json();
  const content = payload?.choices?.[0]?.message?.content;
  if (typeof content !== "string") {
    return parseMessageHeuristically(message);
  }

  try {
    return validateTelegramAction(JSON.parse(content)) ?? parseMessageHeuristically(message);
  } catch {
    return parseMessageHeuristically(message);
  }
}

async function applyTelegramAction(admin: any, action: TelegramAction): Promise<string> {
  if (isChallengeAction(action)) return applyChallengeAction(admin, action);
  if (isMealPlanAction(action)) return applyMealPlanAction(admin, action);
  if (isTargetAction(action)) return applyHealthTargetAction(admin, action);
  if (isRecipeRatingAction(action)) return applyRecipeRatingAction(admin, action);
  if (isRecipeAction(action)) return applyRecipeAction(admin, action);
  return applyPlannerAction(admin, action);
}

async function applyMealPlanAction(admin: any, action: MealPlanAction): Promise<string> {
  const date = dashboardLocalDate();
  if (action.action === "clear_meal_plan" || action.recipes.length === 0) {
    const { error: deleteError } = await admin.database
      .from("meal_plan_entries")
      .delete()
      .eq("date", date);
    if (deleteError) throw deleteError;
    return "Cleared today's meal plan.";
  }

  let selected: Array<{ id: string; title: string }> = [];
  const missing: string[] = [];
  const seen = new Set<string>();
  for (const requestedTitle of action.recipes) {
    const { data, error } = await admin.database
      .from("recipes")
      .select("id,title")
      .ilike("title", `%${requestedTitle}%`)
      .limit(1);
    if (error) throw error;
    const row = Array.isArray(data) && data.length > 0 ? data[0] : null;
    if (!row?.id || seen.has(String(row.id))) {
      if (!row?.id) missing.push(requestedTitle);
      continue;
    }
    seen.add(String(row.id));
    selected.push({ id: String(row.id), title: String(row.title) });
  }

  if (selected.length === 0) return `No saved recipes matched: ${action.recipes.join(", ")}.`;

  const { data: existingRows, error: existingError } = await admin.database
    .from("meal_plan_entries")
    .select("recipe_id,sort_order")
    .eq("date", date)
    .order("sort_order", { ascending: true });
  if (existingError) throw existingError;

  const existingRecipeIds = new Set(
    Array.isArray(existingRows) ? existingRows.map((row) => String(row.recipe_id)) : []
  );
  const maxSortOrder = Array.isArray(existingRows)
    ? existingRows.reduce((max, row) => Math.max(max, Number(row.sort_order) || 0), 0)
    : 0;

  if (action.action === "set_meal_plan") {
    const { error: deleteError } = await admin.database
      .from("meal_plan_entries")
      .delete()
      .eq("date", date);
    if (deleteError) throw deleteError;
  } else {
    selected = selected.filter((recipe) => !existingRecipeIds.has(recipe.id));
    if (selected.length === 0) {
      const suffix = missing.length > 0 ? ` Missing: ${missing.join(", ")}.` : "";
      return `Already in today's meal plan: ${action.recipes.join(", ")}.${suffix}`;
    }
  }

  const rows = selected.map((recipe, index) => ({
    date,
    recipe_id: recipe.id,
    sort_order: action.action === "set_meal_plan" ? index + 1 : maxSortOrder + index + 1
  }));
  const { error } = await admin.database.from("meal_plan_entries").insert(rows);
  if (error) throw error;

  const suffix = missing.length > 0 ? ` Missing: ${missing.join(", ")}.` : "";
  const verb = action.action === "set_meal_plan" ? "Set today's meal plan" : "Added to today's meal plan";
  return `${verb}: ${selected.map((recipe) => recipe.title).join(", ")}.${suffix}`;
}

async function applyChallengeAction(admin: any, action: ChallengeAction): Promise<string> {
  const date = dashboardLocalDate();
  const { data: existingRows, error: selectError } = await admin.database
    .from("challenge_daily_logs")
    .select("date,water_l,sleep_hours,workouts")
    .eq("date", date)
    .limit(1);
  if (selectError) throw selectError;

  const existing = Array.isArray(existingRows) && existingRows.length > 0 ? existingRows[0] : null;
  const currentWater = Math.max(0, Number(existing?.water_l ?? 0));
  const currentSleep = Math.max(0, Number(existing?.sleep_hours ?? 0));
  const currentWorkouts = Math.max(0, Number(existing?.workouts ?? 0));
  const next = {
    water_l: currentWater,
    sleep_hours: currentSleep,
    workouts: currentWorkouts
  };

  if (action.action === "add_water") {
    next.water_l = roundOneDecimal(currentWater + action.value);
  } else if (action.action === "set_sleep") {
    next.sleep_hours = roundOneDecimal(action.value);
  } else if (action.action === "add_workout") {
    next.workouts = currentWorkouts + Math.max(1, Math.round(action.value));
  }

  if (existing) {
    const { error } = await admin.database
      .from("challenge_daily_logs")
      .update(next)
      .eq("date", date);
    if (error) throw error;
  } else {
    const { error } = await admin.database
      .from("challenge_daily_logs")
      .insert([{ date, ...next }]);
    if (error) throw error;
  }

  if (action.action === "add_water") return `Logged ${formatNumber(action.value)}L water today (${formatNumber(next.water_l)}/3L).`;
  if (action.action === "set_sleep") return `Logged sleep: ${formatNumber(next.sleep_hours)}/8h.`;
  return `Logged workout ${next.workouts}/2 today.`;
}

async function applyPlannerAction(admin: any, action: PlannerAction): Promise<string> {
  const listName = plannerListLabel(action.list_key);
  if (action.action === "clear") {
    const { error } = await admin.database
      .from("planner_items")
      .delete()
      .eq("list_key", action.list_key);
    if (error) throw error;
    return `Cleared ${listName}.`;
  }

  if (action.action === "add") {
    const rows = action.items.map((text) => ({ list_key: action.list_key, text, done: false }));
    const { error } = await admin.database.from("planner_items").insert(rows);
    if (error) throw error;
    return `Added ${action.items.join(", ")} to ${listName}.`;
  }

  const done = action.action === "complete";
  if (action.action === "complete" || action.action === "uncomplete") {
    for (const item of action.items) {
      let query = admin.database
        .from("planner_items")
        .update({ done })
        .ilike("text", `%${item}%`);
      if (!action.all_lists) {
        query = query.eq("list_key", action.list_key);
      }
      const { error } = await query;
      if (error) throw error;
    }
    return `${done ? "Marked done" : "Marked open"}: ${action.items.join(", ")}.`;
  }

  for (const item of action.items) {
    let query = admin.database
      .from("planner_items")
      .delete()
      .ilike("text", `%${item}%`);
    if (!action.all_lists) {
      query = query.eq("list_key", action.list_key);
    }
    const { error } = await query;
    if (error) throw error;
  }

  return `Removed ${action.items.join(", ")} from ${listName}.`;
}

function plannerListLabel(listKey: ListKey): string {
  return listKey === "todo" ? "chores" : listKey;
}

async function applyHealthTargetAction(admin: any, action: HealthTargetAction): Promise<string> {
  const unit = action.unit || (action.metric === "steps" ? "steps" : "kcal");
  const label = action.metric === "steps" ? "STEPS" : "CALORIES";

  const { data: existing, error: selectError } = await admin.database
    .from("health_targets")
    .select("metric")
    .eq("metric", action.metric)
    .limit(1);
  if (selectError) throw selectError;

  if (Array.isArray(existing) && existing.length > 0) {
    const { error } = await admin.database
      .from("health_targets")
      .update({ label, target_value: action.value, unit })
      .eq("metric", action.metric);
    if (error) throw error;
  } else {
    const { error } = await admin.database
      .from("health_targets")
      .insert([{ metric: action.metric, label, target_value: action.value, unit }]);
    if (error) throw error;
  }

  return `Set ${action.metric} target to ${formatNumber(action.value)} ${unit}.`;
}

async function applyRecipeRatingAction(admin: any, action: RecipeRatingAction): Promise<string> {
  const { data, error: selectError } = await admin.database
    .from("recipes")
    .select("id,title")
    .ilike("title", `%${action.title}%`)
    .limit(1);
  if (selectError) throw selectError;

  const row = Array.isArray(data) && data.length > 0 ? data[0] : null;
  if (!row?.id) return `No saved meal matched: ${action.title}.`;

  const rating = clampRecipeRating(action.rating);
  const { error } = await admin.database
    .from("recipes")
    .update({ rating })
    .eq("id", row.id);
  if (error) throw error;

  return `Rated ${row.title}: ${formatNumber(rating)}/5.`;
}

async function applyRecipeAction(admin: any, action: RecipeAction): Promise<string> {
  const { data: existing, error: selectError } = await admin.database
    .from("recipes")
    .select("id")
    .eq("title", action.title)
    .limit(1);
  if (selectError) throw selectError;

  const recipeRow = {
    title: action.title,
    total_calories: action.total_calories,
    carbs_g: action.carbs_g,
    fat_g: action.fat_g,
    protein_g: action.protein_g,
    rating: action.rating,
    instructions: action.instructions || ""
  };

  let recipeId = "";
  if (Array.isArray(existing) && existing.length > 0) {
    recipeId = String(existing[0].id);
    const { error } = await admin.database
      .from("recipes")
      .update(recipeRow)
      .eq("id", recipeId);
    if (error) throw error;
  } else {
    const { data, error } = await admin.database
      .from("recipes")
      .insert([recipeRow])
      .select("id");
    if (error) throw error;
    recipeId = String(Array.isArray(data) ? data[0]?.id ?? "" : data?.id ?? "");
  }

  if (!recipeId) throw new Error("Recipe insert did not return an id");

  const { error: deleteError } = await admin.database
    .from("recipe_ingredients")
    .delete()
    .eq("recipe_id", recipeId);
  if (deleteError) throw deleteError;

  if (action.ingredients.length > 0) {
    const rows = action.ingredients.map((ingredient, index) => ({
      recipe_id: recipeId,
      name: ingredient.name,
      amount: ingredient.amount,
      calories: ingredient.calories ?? null,
      sort_order: index + 1
    }));
    const { error } = await admin.database.from("recipe_ingredients").insert(rows);
    if (error) throw error;
  }

  return `Saved recipe: ${action.title} (${formatNumber(action.total_calories)} cal, C${formatNumber(action.carbs_g)} F${formatNumber(action.fat_g)} P${formatNumber(action.protein_g)}, ${formatNumber(action.rating)}/5).`;
}

async function sendTelegramMessage(chatId: string, text: string): Promise<void> {
  const token = Deno.env.get("TELEGRAM_BOT_TOKEN");
  if (!token) return;

  await fetch(`https://api.telegram.org/bot${token}/sendMessage`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ chat_id: chatId, text })
  });
}

function sendTelegramMessageInBackground(chatId: string, text: string): void {
  sendTelegramMessage(chatId, text).catch((error) => {
    console.error("telegram-webhook reply_error");
  });
}

function corsHeaders(): HeadersInit {
  return {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type, X-Telegram-Bot-Api-Secret-Token"
  };
}

function parseFastHeuristicMessage(message: string): TelegramAction | null {
  const normalized = message.trim().replace(/\s+/g, " ");
  const lower = normalized.toLowerCase();

  const challengeAction = parseChallengeHeuristically(normalized);
  if (challengeAction) return challengeAction;

  const targetAction = parseTargetHeuristically(normalized);
  if (targetAction) return targetAction;

  const hasPlannerVerb = /\b(add|put|include|buy|get|need|mark|check off|complete|completed|done|undo|uncheck|delete|remove|drop|clear|empty|reset)\b/.test(lower);
  if (hasPlannerVerb && hasExplicitList(normalized)) {
    return parseMessageHeuristically(normalized);
  }

  return null;
}

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { ...corsHeaders(), "Content-Type": "application/json; charset=utf-8" }
  });
}

function requiredEnv(key: string): string {
  const value = Deno.env.get(key);
  if (!value) throw new Error(`Missing ${key}`);
  return value;
}

function timeMs(): number {
  return Date.now();
}

function elapsedMs(started: number): number {
  return Date.now() - started;
}

function logTiming(label: string, timing: Record<string, number | string>): void {
  console.log(`${label} timing ${JSON.stringify(timing)}`);
}

const LIST_ALIASES: Record<ListKey, string[]> = {
  grocery: ["grocery", "groceries", "shopping", "market"],
  workout: ["workout", "exercise", "training", "gym"],
  meal: ["meal", "meals", "menu", "food"],
  todo: ["todo", "to-do", "task", "tasks", "errand", "errands"]
};

const LIST_KEYS: ListKey[] = ["grocery", "workout", "meal", "todo"];

function detectListKey(message: string): ListKey {
  const lower = message.toLowerCase();
  for (const key of LIST_KEYS) {
    if (LIST_ALIASES[key].some((alias) => lower.includes(alias))) return key;
  }
  return "todo";
}

function parseMessageHeuristically(message: string): TelegramAction {
  const challengeAction = parseChallengeHeuristically(message);
  if (challengeAction) return challengeAction;

  const mealPlanAction = parseMealPlanHeuristically(message);
  if (mealPlanAction) return mealPlanAction;

  const targetAction = parseTargetHeuristically(message);
  if (targetAction) return targetAction;

  const recipeRatingAction = parseRecipeRatingHeuristically(message);
  if (recipeRatingAction) return recipeRatingAction;

  const recipeAction = parseRecipeHeuristically(message);
  if (recipeAction) return recipeAction;

  const normalized = message.trim().replace(/\s+/g, " ");
  const lower = normalized.toLowerCase();
  const listKey = detectListKey(normalized);
  const explicitList = hasExplicitList(normalized);

  let action: PlannerAction["action"] = "add";
  if (/\b(undo|uncheck|not done|incomplete)\b/.test(lower)) {
    action = "uncomplete";
  } else if (/\b(delete|remove|drop)\b/.test(lower)) {
    action = "delete";
  } else if (/\b(clear|empty|reset)\b/.test(lower)) {
    action = "clear";
  } else if (/\b(done|complete|completed|check off|mark)\b/.test(lower)) {
    action = "complete";
  }

  const withoutActionFirst = normalized
    .replace(/^(please\s+)?(add|put|include|buy|get|need|mark|check off|complete|completed|done|undo|uncheck|delete|remove|drop|clear|empty|reset)\s+/i, "")
    .replace(/\s+(done|complete|completed)$/i, "")
    .replace(/\s+(to|in|on|from)\s+(my\s+)?(grocery|groceries|shopping|market|workout|exercise|training|gym|meal|meals|menu|food|todo|to-do|task|tasks|errand|errands)(\s+list|\s+plan)?$/i, "")
    .replace(/\s+(to|in|on|from)$/i, "")
    .trim();
  const withoutAction = stripListWords(withoutActionFirst, listKey)
    .replace(/\s+(to|in|on|from)$/i, "")
    .trim();

  const items =
    action === "clear"
      ? []
      : withoutAction
          .split(/\s*(?:,| and |\+)\s*/i)
          .map((item) => item.replace(/^the\s+/i, "").trim())
          .filter(Boolean);

  return {
    kind: "planner",
    action,
    list_key: listKey,
    items: items.length > 0 ? items : [withoutAction || normalized],
    all_lists: !explicitList && (action === "complete" || action === "uncomplete" || action === "delete")
  };
}

function validateTelegramAction(input: unknown): TelegramAction | null {
  return validateChallengeAction(input) ?? validateMealPlanAction(input) ?? validateTargetAction(input) ?? validateRecipeRatingAction(input) ?? validateRecipeAction(input) ?? validatePlannerAction(input);
}

function validatePlannerAction(input: unknown): PlannerAction | null {
  if (!input || typeof input !== "object") return null;
  const candidate = input as Partial<PlannerAction>;

  if (!["add", "complete", "uncomplete", "delete", "clear"].includes(String(candidate.action))) return null;
  if (!LIST_KEYS.includes(candidate.list_key as ListKey)) return null;

  const items = Array.isArray(candidate.items)
    ? candidate.items.map((item) => String(item).trim()).filter(Boolean)
    : [];

  if (candidate.action !== "clear" && items.length === 0) return null;

  return {
    kind: "planner",
    action: candidate.action as PlannerAction["action"],
    list_key: candidate.list_key as ListKey,
    items,
    all_lists: Boolean(candidate.all_lists)
  };
}

function validateTargetAction(input: unknown): HealthTargetAction | null {
  if (!input || typeof input !== "object") return null;
  const candidate = input as Partial<HealthTargetAction>;
  if (candidate.kind !== "target" && candidate.action !== "set_target") return null;
  if (candidate.action !== "set_target") return null;
  if (candidate.metric !== "steps" && candidate.metric !== "calories") return null;
  const value = Number(candidate.value);
  if (!Number.isFinite(value) || value <= 0) return null;
  const unit = typeof candidate.unit === "string" && candidate.unit.trim()
    ? candidate.unit.trim()
    : candidate.metric === "steps" ? "steps" : "kcal";
  return {
    kind: "target",
    action: "set_target",
    metric: candidate.metric,
    value,
    unit
  };
}

function validateChallengeAction(input: unknown): ChallengeAction | null {
  if (!input || typeof input !== "object") return null;
  const candidate = input as Partial<ChallengeAction>;
  if (candidate.kind !== "challenge") return null;
  if (candidate.action !== "add_water" && candidate.action !== "set_sleep" && candidate.action !== "add_workout") return null;
  const value = Number(candidate.value);
  if (!Number.isFinite(value) || value <= 0) return null;
  return {
    kind: "challenge",
    action: candidate.action,
    value
  };
}

function validateMealPlanAction(input: unknown): MealPlanAction | null {
  if (!input || typeof input !== "object") return null;
  const candidate = input as Partial<MealPlanAction>;
  if (candidate.kind !== "meal_plan") return null;
  if (candidate.action !== "add_meal" && candidate.action !== "set_meal_plan" && candidate.action !== "clear_meal_plan") return null;
  const recipes = Array.isArray(candidate.recipes)
    ? candidate.recipes.map((recipe) => String(recipe).trim()).filter(Boolean)
    : [];
  if (candidate.action !== "clear_meal_plan" && recipes.length === 0) return null;
  return {
    kind: "meal_plan",
    action: candidate.action,
    recipes
  };
}

function validateRecipeAction(input: unknown): RecipeAction | null {
  if (!input || typeof input !== "object") return null;
  const candidate = input as Partial<RecipeAction>;
  if (candidate.kind !== "recipe" && candidate.action !== "add_recipe") return null;
  if (candidate.action !== "add_recipe") return null;
  const title = typeof candidate.title === "string" ? candidate.title.trim() : "";
  if (!title) return null;
  const totalCalories = Number(candidate.total_calories);
  const carbs = Number(candidate.carbs_g);
  const fat = Number(candidate.fat_g);
  const protein = Number(candidate.protein_g);
  const rating = candidate.rating == null ? 0 : Number(candidate.rating);
  if (![totalCalories, carbs, fat, protein].every((value) => Number.isFinite(value) && value >= 0)) return null;
  if (!Number.isFinite(rating) || rating < 0 || rating > 5) return null;
  const ingredients = Array.isArray(candidate.ingredients)
    ? candidate.ingredients
        .map((ingredient) => normalizeRecipeIngredient(ingredient))
        .filter((ingredient): ingredient is RecipeIngredientInput => ingredient !== null)
    : [];
  return {
    kind: "recipe",
    action: "add_recipe",
    title,
    total_calories: totalCalories,
    carbs_g: carbs,
    fat_g: fat,
    protein_g: protein,
    rating: Math.round(rating * 10) / 10,
    instructions: typeof candidate.instructions === "string" ? candidate.instructions.trim() : "",
    ingredients
  };
}

function validateRecipeRatingAction(input: unknown): RecipeRatingAction | null {
  if (!input || typeof input !== "object") return null;
  const candidate = input as Partial<RecipeRatingAction>;
  if (candidate.kind !== "recipe_rating" && candidate.action !== "rate_recipe") return null;
  if (candidate.action !== "rate_recipe") return null;
  const title = typeof candidate.title === "string" ? candidate.title.trim() : "";
  const rating = Number(candidate.rating);
  if (!title || !Number.isFinite(rating) || rating < 0 || rating > 5) return null;
  return {
    kind: "recipe_rating",
    action: "rate_recipe",
    title,
    rating: clampRecipeRating(rating)
  };
}

function normalizeRecipeIngredient(input: unknown): RecipeIngredientInput | null {
  if (!input || typeof input !== "object") return null;
  const candidate = input as Partial<RecipeIngredientInput>;
  const name = typeof candidate.name === "string" ? candidate.name.trim() : "";
  const amount = typeof candidate.amount === "string" ? candidate.amount.trim() : "";
  if (!name || !amount) return null;
  const calories = candidate.calories == null ? null : Number(candidate.calories);
  return {
    name,
    amount,
    calories: calories == null || !Number.isFinite(calories) || calories < 0 ? null : calories
  };
}

function parseTargetHeuristically(message: string): HealthTargetAction | null {
  const normalized = message.trim().replace(/\s+/g, " ");
  const lower = normalized.toLowerCase();
  if (!/\b(target|goal|set|change|update)\b/.test(lower)) return null;
  const metric = /\b(step|steps)\b/.test(lower) ? "steps" : /\b(calorie|calories|kcal)\b/.test(lower) ? "calories" : null;
  if (!metric) return null;
  const valueMatch = normalized.match(/(?:to|at|=|goal|target)\s*([0-9][0-9,]*(?:\.[0-9]+)?)/i) ?? normalized.match(/([0-9][0-9,]*(?:\.[0-9]+)?)/);
  if (!valueMatch) return null;
  const value = Number(valueMatch[1].replace(/,/g, ""));
  if (!Number.isFinite(value) || value <= 0) return null;
  return {
    kind: "target",
    action: "set_target",
    metric,
    value,
    unit: metric === "steps" ? "steps" : "kcal"
  };
}

function parseChallengeHeuristically(message: string): ChallengeAction | null {
  const normalized = message.trim().replace(/\s+/g, " ");
  const lower = normalized.toLowerCase();

  if (/\b(slept|sleep)\b/.test(lower)) {
    const hours = extractMacroNumber(normalized, /([0-9][0-9,]*(?:\.[0-9]+)?)\s*(?:h|hr|hrs|hour|hours)\b/i)
      ?? extractMacroNumber(normalized, /\b(?:slept|sleep)\s+([0-9][0-9,]*(?:\.[0-9]+)?)/i);
    if (hours && hours > 0) return { kind: "challenge", action: "set_sleep", value: hours };
  }

  if (/\b(water|hydrated|drank|drink)\b/.test(lower)) {
    const liters = extractMacroNumber(normalized, /([0-9][0-9,]*(?:\.[0-9]+)?)\s*(?:l|liter|liters|litre|litres)\b/i);
    const milliliters = extractMacroNumber(normalized, /([0-9][0-9,]*(?:\.[0-9]+)?)\s*(?:ml|milliliter|milliliters|millilitre|millilitres)\b/i);
    const amount = liters ?? (milliliters ? milliliters / 1000 : /\bxl\s+water\b|\bwater\s+xl\b/.test(lower) ? 1 : 1);
    return { kind: "challenge", action: "add_water", value: roundOneDecimal(amount) };
  }

  if (/\b(workout|exercise|training|gym)\b/.test(lower) && /\b(did|done|complete|completed|finished|marked|mark)\b/.test(lower)) {
    return { kind: "challenge", action: "add_workout", value: 1 };
  }

  return null;
}

function parseMealPlanHeuristically(message: string): MealPlanAction | null {
  const normalized = message.trim().replace(/\s+/g, " ");
  const lower = normalized.toLowerCase();
  const hasMealPlanPhrase = /\b(meal plan|meals today|today'?s meals|plan meals)\b/.test(lower);
  const hasMealListPhrase = /\b(?:to|in|on|for)\s+(?:my\s+)?meals?(?:\s+plan)?\b/.test(lower);
  const hasMealCommandPrefix = /^(please\s+)?(add|put|include|set|make|update|plan|use|save)\s+(my\s+)?meals?\b/.test(lower);
  const hasAddMealCommandPrefix = /^(please\s+)?(add|put|include)\s+(my\s+)?meals?\b/.test(lower);
  const hasMealPlanVerb = /\b(add|put|include|set|make|update|plan|use|save)\b/.test(lower);
  if (!hasMealPlanPhrase && !hasMealCommandPrefix && !(hasMealListPhrase && hasMealPlanVerb)) return null;
  if (/\b(clear|empty|reset|remove)\b/.test(lower)) {
    return { kind: "meal_plan", action: "clear_meal_plan", recipes: [] };
  }
  const action: MealPlanAction["action"] =
    /\b(add|put|include)\b/.test(lower) || hasAddMealCommandPrefix ? "add_meal" : "set_meal_plan";
  const raw = normalized
    .replace(/^(please\s+)?(set|make|update|plan|use|save)\s+(my\s+)?/i, "")
    .replace(/^(please\s+)?(add|put|include)\s+/i, "")
    .replace(/^meals?\s*(to|as|:|-)?\s*/i, "")
    .replace(/^(today'?s\s+)?meal plan\s*(to|as|:|-)?\s*/i, "")
    .replace(/^(meals today)\s*(to|as|:|-)?\s*/i, "")
    .replace(/\s+(to|in|on|for)\s+(my\s+)?meals?(\s+plan)?$/i, "")
    .trim();
  const recipes = raw
    .split(/\s*(?:,|;|\+| and )\s*/i)
    .map((recipe) => recipe.replace(/^(recipe|meal)\s+/i, "").trim())
    .filter(Boolean);
  if (recipes.length === 0) return null;
  return { kind: "meal_plan", action, recipes };
}

function parseRecipeRatingHeuristically(message: string): RecipeRatingAction | null {
  const normalized = message.trim().replace(/\s+/g, " ");
  const lower = normalized.toLowerCase();
  if (!/\b(rate|rating|rated|score|stars?)\b/.test(lower)) return null;

  const rating =
    extractMacroNumber(normalized, /\b(?:rate|rating|rated|score)\b\s*(?:is|to|as|:)?\s*([0-5](?:\.[0-9]+)?)\s*(?:\/\s*5|out of\s+5|stars?)?/i)
    ?? extractMacroNumber(normalized, /([0-5](?:\.[0-9]+)?)\s*(?:\/\s*5|out of\s+5|stars?)\b/i)
    ?? (/^(please\s+)?rate\b/i.test(normalized) ? extractMacroNumber(normalized, /\b([0-5](?:\.[0-9]+)?)\b(?!.*\b[0-5](?:\.[0-9]+)?\b)/i) : null);
  if (rating == null || rating < 0 || rating > 5) return null;

  const title = extractRecipeRatingTitle(normalized);
  if (!title) return null;
  return {
    kind: "recipe_rating",
    action: "rate_recipe",
    title,
    rating: clampRecipeRating(rating)
  };
}

function parseRecipeHeuristically(message: string): RecipeAction | null {
  const normalized = message.trim().replace(/\s+/g, " ");
  const lower = normalized.toLowerCase();
  if (!/\b(recipe|meal)\b/.test(lower) || !/\b(add|save|create)\b/.test(lower)) return null;

  const totalCalories = extractMacroNumber(normalized, /\b(?:calories|cal|kcal)\b\s*:?\s*([0-9][0-9,]*(?:\.[0-9]+)?)/i);
  const carbs = extractMacroNumber(normalized, /\b(?:carbs|carbohydrates|c)\b\s*:?\s*([0-9][0-9,]*(?:\.[0-9]+)?)/i);
  const fat = extractMacroNumber(normalized, /\b(?:fat|f)\b\s*:?\s*([0-9][0-9,]*(?:\.[0-9]+)?)/i);
  const protein = extractMacroNumber(normalized, /\b(?:protein|prot|p)\b\s*:?\s*([0-9][0-9,]*(?:\.[0-9]+)?)/i);
  const rating = extractMacroNumber(normalized, /\b(?:rating|rated|score)\b\s*:?\s*([0-5](?:\.[0-9]+)?)\s*(?:\/\s*5)?/i);
  if ([totalCalories, carbs, fat, protein].some((value) => value === null)) return null;

  const title = extractRecipeTitle(normalized);
  if (!title) return null;

  return {
    kind: "recipe",
    action: "add_recipe",
    title,
    total_calories: totalCalories ?? 0,
    carbs_g: carbs ?? 0,
    fat_g: fat ?? 0,
    protein_g: protein ?? 0,
    rating: rating == null ? 0 : Math.max(0, Math.min(5, Math.round(rating * 10) / 10)),
    instructions: extractAfterLabel(normalized, "instructions") || extractAfterLabel(normalized, "directions") || "",
    ingredients: parseIngredientsList(extractAfterLabel(normalized, "ingredients"))
  };
}

function extractRecipeRatingTitle(message: string): string {
  return message
    .replace(/^(please\s+)?rate\s+(my\s+)?(meal|recipe)?\s*/i, "")
    .replace(/^(please\s+)?(set|update|change)\s+(the\s+)?rating\s+(for|of|on)\s+/i, "")
    .replace(/^(please\s+)?give\s+/i, "")
    .replace(/\b(?:a\s+)?rating\s*(?:of|to|as|:)?\s*[0-5](?:\.[0-9]+)?\s*(?:\/\s*5|out of\s+5|stars?)?\b/i, "")
    .replace(/\b(?:rate|rating|rated|score)\b\s*(?:is|to|as|:)?\s*[0-5](?:\.[0-9]+)?\s*(?:\/\s*5|out of\s+5|stars?)?\b/i, "")
    .replace(/\b[0-5](?:\.[0-9]+)?\s*(?:\/\s*5|out of\s+5|stars?)\b/i, "")
    .replace(/\b[0-5](?:\.[0-9]+)?\b\s*$/i, "")
    .replace(/\s+(?:a\s+)?$/i, "")
    .trim();
}

function extractMacroNumber(value: string, pattern: RegExp): number | null {
  const match = value.match(pattern);
  if (!match) return null;
  const parsed = Number(match[1].replace(/,/g, ""));
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : null;
}

function extractRecipeTitle(message: string): string {
  return message
    .replace(/^(please\s+)?(add|save|create)\s+(a\s+)?(new\s+)?(recipe|meal)\s*/i, "")
    .split(/\b(?:calories|cal|kcal|carbs|carbohydrates|fat|protein|prot|ingredients|instructions|directions)\b/i)[0]
    .replace(/[:,-]\s*$/g, "")
    .trim();
}

function extractAfterLabel(message: string, label: string): string {
  const pattern = new RegExp(`\\b${label}\\b\\s*:?\\s*(.+)$`, "i");
  const match = message.match(pattern);
  if (!match) return "";
  return match[1]
    .split(/\b(?:instructions|directions)\b\s*:?/i)[0]
    .trim();
}

function parseIngredientsList(raw: string): RecipeIngredientInput[] {
  if (!raw) return [];
  return raw
    .split(/\s*(?:,|;| and )\s*/i)
    .map((item) => item.trim())
    .filter(Boolean)
    .map((item) => {
      const match = item.match(/^(.+?)\s+([0-9][0-9./]*(?:\s*(?:g|gram|grams|tbsp|tablespoon|tablespoons|tsp|teaspoon|teaspoons|cup|cups|medium|piece|pieces))?.*)$/i);
      return {
        name: (match ? match[1] : item).trim(),
        amount: (match ? match[2] : "1 serving").trim(),
        calories: null
      };
    })
    .filter((ingredient) => ingredient.name.length > 0 && ingredient.amount.length > 0);
}

function isTargetAction(action: TelegramAction): action is HealthTargetAction {
  return (action as HealthTargetAction).kind === "target";
}

function isRecipeAction(action: TelegramAction): action is RecipeAction {
  return (action as RecipeAction).kind === "recipe";
}

function isRecipeRatingAction(action: TelegramAction): action is RecipeRatingAction {
  return (action as RecipeRatingAction).kind === "recipe_rating";
}

function isChallengeAction(action: TelegramAction): action is ChallengeAction {
  return (action as ChallengeAction).kind === "challenge";
}

function isMealPlanAction(action: TelegramAction): action is MealPlanAction {
  return (action as MealPlanAction).kind === "meal_plan";
}

function formatNumber(value: number): string {
  return Number.isInteger(value) ? String(value) : String(Number(value.toFixed(1)));
}

function roundOneDecimal(value: number): number {
  return Math.round(value * 10) / 10;
}

function clampRecipeRating(value: number): number {
  return Math.max(0, Math.min(5, Math.round(value * 10) / 10));
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

function stripListWords(message: string, listKey: ListKey): string {
  let output = message;
  for (const alias of LIST_ALIASES[listKey]) {
    output = output.replace(new RegExp(`\\b${escapeRegExp(alias)}\\b`, "ig"), "");
  }
  return output.replace(/\s+(list|plan)\b/gi, " ").replace(/\s+/g, " ").trim();
}

function escapeRegExp(value: string): string {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function hasExplicitList(message: string): boolean {
  const lower = message.toLowerCase();
  return LIST_KEYS.some((key) => LIST_ALIASES[key].some((alias) => lower.includes(alias)));
}
