// Public OAuth app information only. This function never reads account data.
export default function(req: Request): Response {
  if (req.method !== "GET" && req.method !== "HEAD") return new Response("Method not allowed", { status: 405 });
  const page = new URL(req.url).searchParams.get("page") || "home";
  const pages: Record<string, { title: string; body: string }> = {
    home: {
      title: "Kindle Dashboard",
      body: `<p>A personal, single-owner dashboard for a Kindle e-ink display.</p>
      <p>It shows Fitbit steps and food totals from Google Health, Google Calendar events, a quick note, lists, recipes, and meal plans.</p>
      <p>Google connections request read-only access. The dashboard does not create, change, or delete Google Health records or Google Calendar events.</p>
      <p>This project is for its owner's personal use and does not offer public account registration.</p>`
    },
    privacy: {
      title: "Privacy information",
      body: `<p>Last updated: 16 November 2026.</p>
      <h2>Access and purpose</h2><p>With the owner's consent, the dashboard reads activity and nutrition totals from Google Health and event titles, times, and locations from the selected Google Calendar. It uses these records to display the owner's information on the Kindle.</p>
      <h2>Storage and processing</h2><p>InsForge hosts the backend and stores daily health summaries, lists, notes, recipes, and meal plans. Calendar events are fetched when the dashboard refreshes. The Kindle stores a local dashboard cache, which can include health totals, calendar events, and notes.</p>
      <p>OAuth client credentials and refresh tokens are stored in server-side secrets and private local configuration files. Google credentials are not included in the Kindle's dashboard payload.</p>
      <h2>Display access</h2><p>Calendar events and quick notes require the configured device credential. Dashboard data, change events, and version checks require the configured read credential. Orders and music controls require the configured device credential. This informational page does not display or link to account data.</p>
      <h2>Use and sharing</h2><p>The dashboard code uses Google data for its personal display features, not for advertising, data sales, or model training. Backend hosting and the owner's authorized administrative tools can process the data as part of operating the dashboard. Google API data use is subject to the Google API Services User Data Policy, including its Limited Use requirements.</p>
      <h2>Control and deletion</h2><p>The owner can revoke the connection in Google Account settings. Revocation prevents new reads; it does not erase existing backend summaries or Kindle caches. The owner can delete stored summaries and notes through the backend administration tools and remove the local files from the Kindle and configuration directory.</p>
      <p>This is a single-owner project. Its owner manages data requests and the support contact configured on the Google consent screen.</p>`
    },
    terms: {
      title: "Personal use information",
      body: `<p>Kindle Dashboard is maintained by its owner for personal use. It is not offered as a public service and has no paid subscription or public signup.</p>
      <p>The Google connections provide read-only access to the owner's selected records. Displayed information depends on device syncing, network access, Google API availability, and the latest successful dashboard refresh.</p>
      <p>The owner controls the Google authorizations, backend configuration, device credentials, and stored records, and can stop using or disconnect the integration at any time.</p>
      <p>Use of Google APIs remains subject to Google's applicable API terms and user data policies.</p>`
    }
  };
  const selected = pages[page];
  if (!selected) return new Response("Not found", { status: 404 });
  const html = `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>${selected.title} · Kindle Dashboard</title><style>body{font:17px/1.65 system-ui,sans-serif;max-width:760px;margin:48px auto;padding:0 24px;color:#202124;background:#fff}nav{display:flex;gap:24px;flex-wrap:wrap;border-bottom:1px solid #ddd;padding-bottom:20px}a{color:#174ea6}h1{font-size:34px}h2{font-size:22px;margin-top:32px}footer{border-top:1px solid #ddd;padding-top:20px;margin-top:40px;color:#666}</style></head><body><nav><a href="?">Kindle Dashboard</a><a href="?page=privacy">Privacy information</a><a href="?page=terms">Personal use information</a></nav><main><h1>${selected.title}</h1>${selected.body}</main><footer>Personal project information. This page does not grant access to the owner's data.</footer></body></html>`;
  return new Response(req.method === "HEAD" ? null : html, { headers: { "Content-Type": "text/html; charset=utf-8", "Cache-Control": "public, max-age=300", "Content-Security-Policy": "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; frame-ancestors 'none'", "X-Content-Type-Options": "nosniff" } });
}
