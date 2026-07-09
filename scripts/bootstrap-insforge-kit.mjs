import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { randomBytes } from "node:crypto";
import { fileURLToPath } from "node:url";

const cliEntry = fileURLToPath(new URL("../node_modules/@insforge/cli/dist/index.js", import.meta.url));

const schemaMigrations = [
  "migrations/001_planner_lists.sql",
  "migrations/20260627000000_create-health-daily-summaries.sql",
  "migrations/20260627000100_create-health-targets.sql",
  "migrations/20260629052000_create-recipes.sql",
  "migrations/20260629070000_add-recipe-photo-pgm.sql",
  "migrations/20260629083000_create-challenge-daily-logs.sql",
  "migrations/20260629162000_create-meal-plan-entries.sql",
  "migrations/20260701000000_add-recipe-rating.sql",
  "migrations/20260701001000_recipe-rating-out-of-five.sql",
  "migrations/20260707000000_enable_rls_private_tables.sql"
];

const sampleDataMigrations = [
  "migrations/20260629065000_add-sample-toast.sql",
  "migrations/20260629070100_clear-sample-toast-storage-photo.sql"
];

const functions = [
  ["kindle-dashboard-data", "functions/kindle-dashboard-data.ts", "Kindle Dashboard Data"],
  ["kindle-dashboard-events", "functions/kindle-dashboard-events.ts", "Kindle Dashboard Events"],
  ["kindle-dashboard-toggle", "functions/kindle-dashboard-toggle.ts", "Kindle Dashboard Toggle"],
  ["telegram-webhook", "functions/telegram-webhook.ts", "Telegram Planner Webhook"],
  ["health-sync", "functions/health-sync.ts", "Health Sync"]
];

const flags = new Set(process.argv.slice(2));
const includeSampleData = flags.has("--with-sample-data");
const skipSecrets = flags.has("--skip-secrets");
const skipDeploy = flags.has("--skip-deploy");

if (flags.has("--help")) {
  console.log(`Usage: npm run kit:backend -- [--with-sample-data] [--skip-secrets] [--skip-deploy]

Applies the public Kindle Dashboard schema to the currently linked InsForge
project, creates missing generated secrets, and deploys edge functions.

Before running:
  npx @insforge/cli login
  npx @insforge/cli create --name kindle-dashboard --region us-east --template empty
`);
  process.exit(0);
}

console.log("Checking linked InsForge project...");
run(["current"]);

console.log("Applying schema migrations...");
for (const migration of schemaMigrations) {
  applyMigration(migration);
}

if (includeSampleData) {
  console.log("Applying optional sample-data migrations...");
  for (const migration of sampleDataMigrations) {
    applyMigration(migration);
  }
}

if (!skipSecrets) {
  console.log("Ensuring generated backend secrets exist...");
  ensureSecret("TELEGRAM_WEBHOOK_SECRET", randomSecret());
  ensureSecret("HEALTH_SYNC_TOKEN", randomSecret());
  ensureSecret("DASHBOARD_TOGGLE_TOKEN", randomSecret());
}

if (!skipDeploy) {
  console.log("Deploying InsForge functions...");
  for (const [slug, file, name] of functions) {
    run(["functions", "deploy", slug, "--file", file, "--name", name]);
  }
}

console.log("Backend kit bootstrap complete.");
console.log("Next: set Telegram secrets with npm run telegram:configure, then copy the endpoints into kindle-dashboard/config.sh.");

function applyMigration(path) {
  console.log(`- ${path}`);
  run(["db", "query", readFileSync(path, "utf8")]);
}

function ensureSecret(key, value) {
  try {
    run(["secrets", "get", key, "--json"], { silent: true });
    console.log(`- ${key} already exists`);
  } catch {
    run(["secrets", "add", key, value]);
    console.log(`- added ${key}`);
  }
}

function randomSecret() {
  return randomBytes(32).toString("hex");
}

function run(args, options = {}) {
  const stdio = options.silent ? ["ignore", "pipe", "pipe"] : "inherit";
  const output = execFileSync(process.execPath, [cliEntry, ...args], { encoding: "utf8", stdio });
  return output || "";
}
