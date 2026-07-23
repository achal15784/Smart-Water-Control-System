# Technical Specification
## Water Control Terminal — Mobile App (Flutter + Firebase, ThingSpeak bridge)

**Version:** 1.0 — June 24, 2026

---

## 1. System Components

| Component | Responsibility | Changes needed |
|---|---|---|
| ESP32 + SIM800L firmware | Flow sensing, valve relays, MQTT to ThingSpeak | None (unchanged) |
| ThingSpeak channel 3403978 | Existing telemetry/command broker | None |
| **Cloud Function: `pollThingSpeak`** | Pull latest feed every few seconds, write to Firestore | New |
| **Cloud Function: `onCommandWrite`** | Firestore trigger → push command/target to ThingSpeak | New |
| **Cloud Function: `rollupStats`** | Scheduled, builds daily/weekly/monthly aggregates + valve event log | New |
| **Cloud Function: `exportCsv`** (callable) | Generates CSV for History export | New |
| Firebase Auth | Email/password accounts | New |
| Firestore | Source of truth for the app | New |
| Flutter App | UI exactly matching the 3 mockups + login | New |

## 2. Data Flow

**Telemetry (hardware → app):**
1. Firmware publishes to ThingSpeak every 15s (existing behavior, unchanged).
2. `pollThingSpeak` (runs on a short interval, e.g. every 5s via a min-instance Cloud Run job or Cloud Scheduler at the platform's minimum granularity, or a lightweight polling loop in a 2nd-gen function with retries) calls `GET https://api.thingspeak.com/channels/3403978/feeds.json?api_key=<READ_KEY>&results=1`.
3. Function parses field2–field5, writes:
   - `devices/{deviceId}/state` (single doc, latest snapshot — what the Dashboard listens to)
   - Appends a point to `devices/{deviceId}/history/{autoId}` (for graphs/rollups)
   - If `state.valveState` changed since last poll → appends to `devices/{deviceId}/valveEvents/{autoId}`

**Commands (app → hardware):**
1. App writes a doc to `devices/{deviceId}/commands/{autoId}`: `{ code: 0|1|2, targetLimit?: number, createdAt, status: "pending" }`.
2. `onCommandWrite` trigger fires, calls `GET https://api.thingspeak.com/update?api_key=<WRITE_KEY>&field6=<code>&field3=<target>`.
3. On success, function updates the command doc `status: "acknowledged"`; on failure, `status: "failed"` with `error`.
4. App listens to the command doc to show optimistic UI ("Opening...") and clears it once `state.valveState` confirms via the next poll.

**Target-only update (Dashboard "Edit" / Configuration "Set Bound"):**
- Same `commands` collection, with `code: null` and only `targetLimit` set; bridge function only sends `field3`.

## 3. Firestore Schema

```
users/{uid}
  email: string
  createdAt: timestamp
  deviceIds: [string]            // devices this user can access

devices/{deviceId}
  ownerUid: string
  name: string                    // e.g. "WTR-CTRL-01"
  channelId: string               // "3403978"
  status: "online" | "offline"
  lastSeen: timestamp

  config (subcollection or map field):
    targetLimit: number
    calibrationFactor: number      // reference copy of 422.791 (firmware constant)
    autoCloseEnabled: boolean

  secrets/{keys}                  // restricted doc, NOT in normal device read scope
    readApiKey: string
    writeApiKey: string
    mqttUsername: string
    mqttClientId: string
    mqttPassword: string

  state/{current}                 // single doc, latest snapshot for Dashboard
    flowRate: number
    totalVolume: number
    targetLimit: number
    valveState: 0 | 1
    pulses: number
    autoStopped: boolean
    updatedAt: timestamp

  history/{autoId}                // time series for graphs + rollups
    flowRate: number
    totalVolume: number
    valveState: number
    timestamp: timestamp

  valveEvents/{autoId}
    event: "OPENED" | "CLOSED"
    timestamp: timestamp

  commands/{autoId}
    code: 0 | 1 | 2 | null
    targetLimit: number | null
    status: "pending" | "acknowledged" | "failed"
    error: string | null
    createdAt: timestamp
    createdBy: uid

  dailyStats/{yyyy-mm-dd}
    totalVolume, avgFlowRate, totalRunTimeSeconds, totalCycles

  weeklyStats/{yyyy-Www}
    same shape, aggregated

  monthlyStats/{yyyy-mm}
    same shape, aggregated
```

## 4. Cloud Functions — Pseudocode

### 4.1 `pollThingSpeak`
```js
exports.pollThingSpeak = onSchedule({ schedule: "every 1 minutes" }, async () => {
  // For tighter than 1-min granularity, run an internal setInterval inside
  // a min-instances=1 2nd-gen function, or use a small always-on Cloud Run service.
  const devices = await db.collection("devices").get();
  for (const doc of devices.docs) {
    const { channelId } = doc.data();
    const secrets = await doc.ref.collection("secrets").doc("keys").get();
    const readKey = secrets.data().readApiKey;

    const res = await fetch(
      `https://api.thingspeak.com/channels/${channelId}/feeds.json?api_key=${readKey}&results=1`
    );
    const json = await res.json();
    const feed = json.feeds[0];
    if (!feed) continue;

    const newState = {
      flowRate: parseFloat(feed.field2 || 0),
      targetLimit: parseFloat(feed.field3 || 0),
      totalVolume: parseFloat(feed.field4 || 0),
      valveState: parseInt(feed.field5 || 0),
      updatedAt: FieldValue.serverTimestamp(),
    };

    const prevSnap = await doc.ref.collection("state").doc("current").get();
    const prev = prevSnap.exists ? prevSnap.data() : null;

    await doc.ref.collection("state").doc("current").set(newState, { merge: true });
    await doc.ref.collection("history").add({ ...newState, timestamp: FieldValue.serverTimestamp() });
    await doc.ref.update({ lastSeen: FieldValue.serverTimestamp(), status: "online" });

    if (prev && prev.valveState !== newState.valveState) {
      await doc.ref.collection("valveEvents").add({
        event: newState.valveState === 1 ? "OPENED" : "CLOSED",
        timestamp: FieldValue.serverTimestamp(),
      });
    }
  }
});
```

### 4.2 `onCommandWrite`
```js
exports.onCommandWrite = onDocumentCreated("devices/{deviceId}/commands/{cmdId}", async (event) => {
  const { deviceId } = event.params;
  const cmd = event.data.data();
  const secrets = await db.doc(`devices/${deviceId}/secrets/keys`).get();
  const writeKey = secrets.data().writeApiKey;
  const channelId = (await db.doc(`devices/${deviceId}`).get()).data().channelId;

  const params = new URLSearchParams({ api_key: writeKey });
  if (cmd.targetLimit !== null && cmd.targetLimit !== undefined) params.set("field3", cmd.targetLimit);
  if (cmd.code !== null && cmd.code !== undefined) params.set("field6", cmd.code);

  try {
    const res = await fetch(`https://api.thingspeak.com/update?${params.toString()}`);
    await event.data.ref.update({ status: "acknowledged" });
  } catch (err) {
    await event.data.ref.update({ status: "failed", error: err.message });
  }
});
```

### 4.3 `rollupStats` (scheduled daily, plus on-demand for week/month)
Aggregates `history` + `valveEvents` into `dailyStats/weeklyStats/monthlyStats` (sum volume, average flow rate, count OPEN→CLOSE durations for run time and cycles).

### 4.4 `exportCsv` (callable)
Takes `{ deviceId, period }`, reads the relevant stats/history docs, returns a CSV string the app saves/shares.

## 5. Firestore Security Rules (outline)

```
match /devices/{deviceId} {
  allow read: if request.auth != null && request.auth.uid in resource.data.allowedUids;
  allow write: if false; // only Cloud Functions (admin SDK) write device docs

  match /secrets/{doc} {
    allow read, write: if false; // server-only, never read by client
  }
  match /state/{doc} {
    allow read: if request.auth != null;
    allow write: if false;
  }
  match /commands/{cmdId} {
    allow create: if request.auth != null;
    allow read: if request.auth != null;
    allow update, delete: if false; // only functions update status
  }
  match /history/{doc} { allow read: if request.auth != null; allow write: if false; }
  match /valveEvents/{doc} { allow read: if request.auth != null; allow write: if false; }
  match /dailyStats/{doc} { allow read: if request.auth != null; allow write: if false; }
  match /weeklyStats/{doc} { allow read: if request.auth != null; allow write: if false; }
  match /monthlyStats/{doc} { allow read: if request.auth != null; allow write: if false; }
}
```

API keys (`secrets/keys`) are never readable by any client — only Admin SDK functions touch them, addressing the fact that the current web dashboard exposes write/read keys directly in client-side JS.

## 6. Flutter App Architecture

```
lib/
  main.dart
  firebase_options.dart
  app/
    app.dart                  // MaterialApp, routes, theme matching mockup palette
    theme.dart                 // colors: --bg-slate #0f172a, --cyan-accent #0284c7, etc.
  features/
    auth/
      login_screen.dart
      signup_screen.dart
      auth_repository.dart     // Firebase Auth wrapper
    dashboard/
      dashboard_screen.dart
      dashboard_controller.dart  // Riverpod/Bloc — streams devices/{id}/state
      widgets/
        flow_rate_card.dart
        valve_status_card.dart
        volume_progress_card.dart
        valve_control_button.dart
        auto_mode_toggle.dart
        live_analytics_chart.dart
    configuration/
      configuration_screen.dart
      configuration_controller.dart
      widgets/
        device_management_card.dart
        field_assignment_card.dart
        device_settings_card.dart   // masked keys + eye/copy icons
        control_settings_card.dart  // calibration constant, auto-close toggle
    history/
      history_screen.dart
      history_controller.dart
      widgets/
        period_selector.dart        // Day / Week / Month
        usage_summary_cards.dart
        volume_trend_chart.dart
        valve_events_list.dart
        additional_stats_card.dart
        export_button.dart
  core/
    services/
      firestore_service.dart
      device_repository.dart
    models/
      device_state.dart
      command.dart
      valve_event.dart
      daily_stats.dart
    widgets/
      bottom_nav.dart              // Dashboard / Configuration / History
```

**State management:** Riverpod (or Bloc, either works) with `StreamProvider`s wrapping Firestore snapshots — no manual polling timers in the app, matching FR-22.

**Charts:** `fl_chart` package reproduces the bar chart (History) and line/area chart (Dashboard Live Analytics) styles shown in the mockups.

**Theming:** Recreate the exact palette from `work.html`'s CSS variables (`--bg-slate`, `--cyan-accent`, `--success`, `--danger`, `--neutral-grey`) as Flutter `ThemeData`/`ColorScheme` constants, and mirror the mockups' card radii (16–24px), shadows, and typography (Inter/Montserrat via `google_fonts` package) so spacing and visual weight match exactly.

## 7. Auth Flow
1. Splash/check: `FirebaseAuth.instance.authStateChanges()` — if signed in, route to Dashboard; else Login.
2. Login screen: email + password fields, "Sign In" button, "Forgot password?" → `sendPasswordResetEmail`.
3. Sign-up screen: email + password + confirm, creates account via `createUserWithEmailAndPassword`, then creates a `users/{uid}` doc and links to a `deviceId` (for v1, a single pre-provisioned device; assignment can be manual via Firestore console or a simple "Enter Device ID" step in onboarding).

## 8. Migration Notes from Current Web Dashboard
- The current `work.html` embeds the ThingSpeak write/read API keys directly in client JS — this is replaced entirely; keys live only in `secrets/keys`, touched only by Cloud Functions.
- Pulse count display (`Math.round(vl * 422.791)`) and the "Auto Stopped" derived flag (`st === 0 && vl >= tg && tg > 0`) move server-side into `pollThingSpeak`, so the Flutter app receives already-computed fields rather than re-deriving them client-side.
- Firmware, ThingSpeak channel layout, relay logic, and auto-shutoff logic in the `.ino` file require zero changes.

## 9. Open Items for a Future Iteration
- True push notifications on auto-shutoff (FCM trigger from `pollThingSpeak` when `autoStopped` flips true)
- Tighter-than-1-minute polling if needed (small Cloud Run service with a loop, billed continuously, vs. Cloud Scheduler's coarser minimum)
- Multi-device UI, multi-user device sharing/roles
- Remote calibration constant push back to firmware (would need a new MQTT subscribe topic in the `.ino`)
