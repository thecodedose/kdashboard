import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";

// Keep secret values out of command output, including child-process failures.
// execFileSync passes arguments directly, without shell interpolation.
function cli(args) {
  let output;
  try {
    output = execFileSync("npx", ["-y", "@insforge/cli", ...args, "--json"], {
      encoding: "utf8", stdio: ["ignore", "pipe", "pipe"]
    });
  } catch {
    throw new Error(`InsForge ${args[0]} ${args[1] || ""} failed. Check CLI login and project access; secret values have been withheld.`);
  }
  try {
    return JSON.parse(output);
  } catch {
    throw new Error("InsForge returned an unexpected response; secret values have been withheld.");
  }
}

if (process.argv.includes("--help")) {
  console.log("Usage: npm run calendar:configure\nUploads .google-calendar/connection.json as server-only secrets to the linked InsForge project. Does not deploy functions or schedule a job.");
} else {
  try {
    let connection;
    try {
      connection = JSON.parse(readFileSync(".google-calendar/connection.json", "utf8"));
    } catch {
      throw new Error("No Google Calendar connection found. Run npm run calendar:connect first.");
    }
    const values = {
      GOOGLE_CALENDAR_CLIENT_ID: connection.client_id,
      GOOGLE_CALENDAR_CLIENT_SECRET: connection.client_secret,
      GOOGLE_CALENDAR_REFRESH_TOKEN: connection.refresh_token,
      GOOGLE_CALENDAR_ID: "primary"
    };
    if (Object.values(values).some((value) => typeof value !== "string" || !value)) throw new Error("Incomplete Google Calendar connection. Run calendar:connect again.");
    const current = cli(["current"]);
    const linked = JSON.parse(readFileSync(".insforge/project.json", "utf8"));
    if (!current.project || current.project.project_id !== linked.project_id) throw new Error("The InsForge project context changed. Check npx -y @insforge/cli current before retrying.");
    console.log(`Configuring Google Calendar secrets for ${current.project.project_name} (${current.project.oss_host})`);
    const { secrets } = cli(["secrets", "list"]);
    if (!Array.isArray(secrets)) throw new Error("Could not list InsForge secrets.");
    for (const [key, value] of Object.entries(values)) {
      const exists = secrets.some((secret) => secret.key === key);
      cli(exists ? ["secrets", "update", key, "--value", value] : ["secrets", "add", key, value]);
      console.log(`Configured ${key}`);
    }
    console.log("Secrets configured. Deploy and verify the widget functions using docs/kindle-widgets.md. Calendar events are read whenever the Kindle refreshes.");
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}
