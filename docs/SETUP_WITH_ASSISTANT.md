# Setup With A Coding Assistant

Use this guide if you want ChatGPT, Codex, Cursor, Claude Code, or another
coding assistant to help set up your private Kindle Dashboard instance.

The assistant can run repo commands, edit config files, and check logs. You
should still keep control of secrets: paste tokens only into terminal commands
or local `.env` files that are ignored by Git.

## What To Prepare First

- A local clone of this repo.
- Node.js 20+ and npm.
- An InsForge account.
- A Telegram bot token from BotFather.
- A jailbroken Kindle with KUAL installed.
- Optional: Zig or an ARM Kindle cross compiler for building the native package.

## Assistant Prompt: Start The Setup

Paste this into your coding assistant from the repo root:

```text
I want to set up this Kindle Dashboard as a bring-your-own-backend kit for my
own Kindle. Please follow docs/INSTALL_FOR_USERS.md and use AGENTS.md.

Important:
- Do not hardcode or commit secrets.
- Use npx -y @insforge/cli for InsForge commands.
- Create or link an InsForge project only after confirming with me.
- Apply the public kit backend setup with npm run kit:backend.
- Help me configure Telegram, generate Kindle config.sh, and verify the deployed
  dashboard endpoints.
- Do not use the developer instance URLs except as examples.
```

## Assistant Prompt: Backend Bootstrap

After you are logged into InsForge, use this prompt:

```text
Please check the current InsForge link with npx -y @insforge/cli current. If no
project is linked, ask me whether to create a fresh project or link an existing
empty one. Then run npm run kit:backend and report any failed migration,
secret, or function deploy step.
```

If the assistant needs you to create a new project manually, use:

```sh
npx -y @insforge/cli create --name kindle-dashboard --region us-east --template empty
```

## Assistant Prompt: Secrets

Use this when it is time to add secrets:

```text
Help me add the required server-side InsForge secrets. Ask me for values one at
a time, and never print the full secret back to me after it is entered.

Required:
- INSFORGE_BASE_URL
- INSFORGE_API_KEY

Optional:
- OPENAI_API_KEY
- OPENAI_MODEL
```

The assistant may run commands like:

```sh
npx -y @insforge/cli secrets add INSFORGE_BASE_URL https://your-project.insforge.app
npx -y @insforge/cli secrets add INSFORGE_API_KEY your-server-only-api-key
```

## Assistant Prompt: Telegram

After creating a bot with BotFather and sending it a message, use:

```text
Help me connect Telegram. I will provide my bot token. First run
npm run telegram:chat-id to find my chat ID. Then run
npm run telegram:configure with my bot token, chat ID, and my project's
telegram-webhook URL. Do not rely on default developer URLs, and do not commit
or echo the bot token in docs.
```

The webhook URL should look like:

```text
https://your-project.insforge.app/functions/telegram-webhook
```

After Telegram is connected, ask the assistant to review the supported message
formats in `docs/INSTALL_FOR_USERS.md`:

```text
Show me the supported Telegram message examples from docs/INSTALL_FOR_USERS.md
and help me test one planner command, one challenge check-in, and one meal-plan
command without exposing tokens.
```

## Assistant Prompt: Kindle Config

Use this once the backend is deployed:

```text
Generate the Kindle KUAL config.sh for my project. Use my InsForge base host for
the data and toggle URLs, and use the direct function2 host for the events URL.
Fetch DASHBOARD_READ_TOKEN and DASHBOARD_TOGGLE_TOKEN from InsForge and include
them in my local config.sh.
Do not modify config.sh.example with my real project values; create or show a
local config.sh instead.
```

The generated config should look like:

```sh
DASHBOARD_DATA_URL="https://your-project.insforge.app/functions/kindle-dashboard-data"
DASHBOARD_EVENTS_URL="https://your-project.function2.insforge.app/kindle-dashboard-events"
DASHBOARD_TOGGLE_URL="https://your-project.insforge.app/functions/kindle-dashboard-toggle"
DASHBOARD_READ_TOKEN="replace-with-your-generated-read-token"
DASHBOARD_TOGGLE_TOKEN="replace-with-your-generated-toggle-token"
INTERVAL="3600"
DASHBOARD_KEEP_AWAKE="1"
DASHBOARD_SLEEP_WINDOW="off"
INVERT_IMAGES="0"
```

## Assistant Prompt: Build The KUAL Package

Before packaging, ask the assistant to run:

```text
Run npm run native:check and tell me whether the native render check passes.
```

If Zig is installed:

```text
Build the Kindle KUAL package with make -C kindle/native extension-zig. If Zig
is not on PATH, ask me for the Zig path. After the build, tell me where
kindle-dashboard-kual.tar.gz was written and remind me to keep config.sh local.
If installing to a mounted Kindle, set DASHBOARD_DATA_URL and
DASHBOARD_READ_TOKEN explicitly before running npm run native:install.
```

If an ARM Kindle compiler is installed:

```text
Build the Kindle KUAL package with make -C kindle/native extension. If the
cross compiler is missing, ask me for KINDLE_CXX or suggest the Zig build path.
```

## Assistant Prompt: Health Sync Companion

Use this if you want Apple Health summaries synced from iPhone:

```text
Help me set up the optional iOS Health Sync companion. Follow
docs/INSTALL_FOR_USERS.md. Confirm the health migration/function/secret are
present without printing secret values, create
ios/HealthSyncCompanion/Config/LocalConfig.xcconfig from the example if needed,
and help me set INSFORGE_HEALTH_SYNC_URL plus HEALTH_SYNC_TOKEN locally. Do not
commit LocalConfig.xcconfig.
```

## Assistant Prompt: Verify

Use this after deployment and Kindle install:

```text
Verify my setup without exposing secrets. Check that npm run check passes.
Check the data endpoint with curl and confirm it returns ok:true JSON. If my
Kindle is mounted, inspect /Volumes/Kindle/extensions/kindle-dashboard/config.sh
for required keys without printing secret values, and inspect the dashboard logs
under /Volumes/Kindle/documents. Do not modify unrelated Kindle files.
```

Useful checks:

```sh
npm run check
curl -sS -H "X-Dashboard-Read-Token: <read-token>" https://your-project.insforge.app/functions/kindle-dashboard-data
curl -N -H "X-Dashboard-Read-Token: <read-token>" https://your-project.function2.insforge.app/kindle-dashboard-events
```

## Human-Only Steps

Do these yourself or explicitly supervise them:

- Creating the Telegram bot in BotFather.
- Entering InsForge admin API keys.
- Entering Telegram bot tokens.
- Copying files onto a Kindle if you are not comfortable with the assistant
  writing to mounted devices.
- Enabling optional boot autostart on Kindle.

## Troubleshooting With An Assistant

If the dashboard is blank or stale, ask:

```text
Please debug my Kindle Dashboard setup. Start by checking the deployed data
endpoint, then the KUAL config.sh URLs, then Kindle logs. Treat the normal
/functions endpoint as unsuitable for SSE and verify events through the
function2.insforge.app URL.
```

If Telegram commands do nothing, ask:

```text
Please debug Telegram for this kit. Check the webhook URL, webhook secret,
TELEGRAM_ALLOWED_CHAT_ID, and recent function behavior. Do not reveal full
tokens in the output.
```

If the KUAL menu opens but nothing renders, ask:

```text
Please inspect the Kindle-side install. Check that the native binary exists,
the shell scripts are executable, config.sh exists, and logs under
/mnt/us/documents or /Volumes/Kindle/documents show the failing command.
```

## Optional feature setup

Follow the integration guides linked from `docs/INSTALL_FOR_USERS.md` for
Google Health, Calendar, notes, vacuum, Spotify and order summaries. Use only the
owner's accounts and backend. Do not copy another installation's OAuth files,
project IDs, speaker address, device identity, personal artwork or cached data.
No automation or external account is preconfigured by this checkout.
