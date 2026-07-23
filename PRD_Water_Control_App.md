# Product Requirements Document (PRD)
## Water Control Terminal — Mobile App

**Version:** 1.0
**Date:** June 24, 2026
**Platform:** Flutter (iOS + Android)
**Backend:** Firebase (Auth, Firestore, Cloud Functions)
**Hardware:** ESP32 + SIM800L GSM module + flow sensor + 2-channel relay valve

---

## 1. Background

A working hardware system already exists: an ESP32 with a SIM800L GSM modem reads a water flow sensor, controls a valve through two interlocked relays, and communicates over MQTT with ThingSpeak. A web dashboard (`work.html`) currently polls ThingSpeak's REST API to show telemetry and sends commands the same way.

The hardware has no WiFi and no awareness of Firebase. Rewriting GSM firmware to talk to Firebase directly is fragile and unnecessary. ThingSpeak therefore stays in place as the **hardware-facing broker**, and Firebase becomes the **app-facing source of truth**, bridged by Cloud Functions.

## 2. Goal

Replace the web dashboard with a native Flutter app that:
- Visually replicates the three provided screens exactly (Dashboard, Configuration, History & Analytics) — no UI redesign
- Adds username/password authentication (no Google Sign-In for now)
- Reads and writes all data through Firebase, never talking to ThingSpeak directly
- Keeps the existing hardware firmware untouched

## 3. Architecture Summary

```
ESP32 + SIM800L  <--MQTT-->  ThingSpeak
                                  ^  |
                          (poll)  |  | (REST update)
                                  v  |
                          Cloud Functions
                                  |
                              Firestore  <-- Firebase Auth
                                  ^
                                  |
                            Flutter App
```

- **Poller function** (scheduled, every 5–10s via Cloud Scheduler or a lightweight always-on poll): reads ThingSpeak `feeds.json`, writes the latest values into Firestore `devices/{deviceId}/telemetry` (current state) and appends to `devices/{deviceId}/history`.
- **Command bridge function**: triggered on writes to `devices/{deviceId}/commands`, calls ThingSpeak's `update` REST endpoint (field6 for command codes, field3 for target).
- The app **never** calls ThingSpeak directly — only Firestore.

## 4. Users

Single role for v1: the device owner/operator. (Multi-user/sharing is out of scope for v1, see §8.)

## 5. Screens (exact replication, see attached mockups)

| Screen | Purpose |
|---|---|
| Login / Sign Up | Username (email) + password auth |
| Dashboard | Live flow rate, valve status, volume progress bar, valve control button, auto mode toggle, live flow graph |
| Configuration | Device management, field assignment (read-only display), device/API settings, calibration constant, auto-close toggle |
| History & Analytics | Day/Week/Month usage, volume trend bar chart, valve events log, aggregate stats, export |

No emojis, no icon/copy changes, no layout changes from the supplied mockups.

## 6. Functional Requirements

### 6.1 Authentication
- FR-1: User can sign up with email + password (Firebase Auth, email/password provider).
- FR-2: User can log in / log out.
- FR-3: Password reset via email link.
- FR-4: Session persists across app restarts.

### 6.2 Dashboard
- FR-5: Flow rate, valve status, and volume update live from Firestore (listener, not polling) — Firestore stream reflects ThingSpeak data with ≤10s latency.
- FR-6: Tapping "Open/Close Valve" writes a command document to Firestore, which the bridge function relays to ThingSpeak field6.
- FR-7: Editing the target volume on Dashboard ("Edit" near Target Volume) writes to Firestore `devices/{id}/config.targetLimit`, relayed to ThingSpeak field3.
- FR-8: Auto Mode toggle is stored in Firestore (`config.autoCloseEnabled`) and mirrored on the Configuration screen's "Auto Close Enable" toggle — both must show the same value.
- FR-9: Live Analytics graph plots the last N flow-rate readings (from `history` subcollection), sliding window matching the mockup's time range.
- FR-10: Volume progress bar computed as `totalVolume / targetLimit`.

### 6.3 Configuration
- FR-11: Device Management shows device ID and live online/offline status (derived from `lastSeen` timestamp — offline if stale > 30s).
- FR-12: Field Assignment list is informational/read-only in v1 (shows the fixed ThingSpeak field mapping).
- FR-13: Device Settings (Channel ID, Write/Read API Key, Username, Client ID, Password) are stored in Firestore under a restricted `devices/{id}/secrets` doc, never exposed to the client in plaintext beyond masked display; "eye" icon reveals, "copy" icon copies to clipboard.
- FR-14: Calibration Constant field updates Firestore `config.calibrationFactor`; relayed to firmware only if firmware is later updated to read it remotely (v1: stored for reference/recalculation of history values; firmware constant stays hardcoded unless you choose to add an MQTT subscribe for it later).
- FR-15: Auto Close Enable toggle writes `config.autoCloseEnabled` to Firestore (same value as Dashboard's Auto Mode).
- FR-16: "Save Settings" persists all Configuration screen edits in one Firestore write.

### 6.4 History & Analytics
- FR-17: Day/Week/Month tabs query aggregated documents from Firestore (`devices/{id}/dailyStats`, `weeklyStats`, `monthlyStats`) maintained by a Cloud Function that rolls up `history` entries.
- FR-18: Volume Trend bar chart renders the selected period's daily volumes.
- FR-19: Valve Events list reads from `devices/{id}/valveEvents`, populated by the bridge/poller function whenever `valveState` changes value (transition logged with timestamp and OPEN/CLOSED).
- FR-20: Additional Statistics (avg flow rate, total volume, total run time, total cycles) computed by the rollup Cloud Function and stored alongside daily stats.
- FR-21: Export Data button exports the currently displayed period's data as CSV (generated client-side or via a callable Cloud Function), shared via the OS share sheet.

### 6.5 Cross-cutting
- FR-22: All reads use Firestore real-time listeners; no client-side polling of ThingSpeak.
- FR-23: Commands written by the app include a `status` field (`pending` / `acknowledged` / `failed`) so the UI can show optimistic state until the bridge confirms.
- FR-24: All Firestore access is scoped by `deviceId`; security rules (see Technical Spec) prevent cross-device or unauthenticated access.

## 7. Non-Functional Requirements
- NFR-1: Telemetry latency app-visible within 10s of hardware publish (bounded by poller interval + Firestore propagation).
- NFR-2: App must clearly indicate "Device Offline" state when poller hasn't received fresh ThingSpeak data within 30s.
- NFR-3: No raw ThingSpeak API keys ever shipped inside the Flutter app binary.
- NFR-4: UI pixel/layout parity with provided mockups — same colors, spacing, components, no added icons or emoji.
- NFR-5: Works on a 2G/GSM-tier round-trip — app doesn't need to be faster than the hardware itself, but must not block UI while waiting.

## 8. Out of Scope (v1)
- Google/social sign-in
- Multi-user device sharing / roles
- Push notifications (can be a fast-follow using FCM + Cloud Function trigger on auto-shutoff)
- Direct firmware-to-Firebase communication
- Multiple device support in UI beyond the single device list shown in Configuration (data model supports it; UI doesn't need to expose it yet)

## 9. Success Metrics
- End-to-end command latency (tap "Open Valve" → relay fires) under ~10–15s, consistent with current ThingSpeak round trip.
- Zero direct ThingSpeak calls from the Flutter client (verifiable via network inspection).
- Auto-shutoff events correctly logged and visible in History within one poll cycle.
