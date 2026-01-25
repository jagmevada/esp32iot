# Quick Action Guide - Egress Tracking Deployment

## What Changed

Your firmware now **automatically logs bandwidth usage every hour** so we can identify exactly where the 80 MB/day is going.

## What You Need to Do

### Step 1: Upload Updated Firmware (5 minutes)
```bash
# In VS Code Terminal or PlatformIO CLI:
cd d:\Project\esp32iot
pio run --environment esp32dev -t upload
```

**For all 3 devices** - upload the same updated firmware to each ESP32

### Step 2: Monitor Serial Output (24 hours)
1. Connect device via USB
2. Open Serial Monitor (115200 baud)
3. Look for hourly reports like:
   ```
   📊 ========== EGRESS REPORT ==========
      Total Egress: 1250 KB (1.22 MB)
      Total Ingress: 850 KB (0.83 MB)
      API Calls: 42
      Avg per call: 29.8 KB
      Projected daily: 29.3 MB/day
   ===================================
   ```

### Step 3: Collect Data
Let firmware run for **24 hours** and collect all hourly reports.

**OR do quick test:**
- Run for 1 hour and multiply "Projected daily" by 1
- If result is <5 MB → Problem likely solved
- If result is >20 MB → We need to dig deeper

### Step 4: Share Results
Once you have data, reply with:
- **Projected daily egress** from the reports
- **Any error messages** you see (Guru Meditation, crashes, etc.)
- **WiFi stability** - any "WiFi disconnected" messages?
- **Device reboots** - does setup() message appear repeatedly?

## What Each Value Means

| Value | What It Indicates | Action if High |
|-------|------------------|---|
| **Projected daily** | Scaled 24-hour bandwidth use | If >10 MB, device has issue |
| **API Calls** | How many requests per hour | If >300, too much polling |
| **Avg per call** | Bytes per request | If >5 KB, responses bloated |
| **Total Egress** | Download from Supabase | Should be 1-2 MB/hour |
| **Total Ingress** | Upload to Supabase | Should be 0.5-1 MB/hour |

## Expected Results by Scenario

### ✅ Scenario: Optimization Worked
```
Projected daily: 2.5 MB/day
API Calls: 162
Avg per call: 0.5 KB
```
→ **Problem solved!** Your optimization worked. User's high egress was from old code.

### ⚠️ Scenario: WiFi Instability
```
Projected daily: 10 MB/day
API Calls: 250+
"WiFi disconnected" messages every few minutes
```
→ **Check WiFi stability.** Reconnecting frequently causes duplicate fetches.

### ❌ Scenario: Device Rebooting
```
Projected daily: 25 MB/day
"Setup" message appears 5+ times per hour
Possible Guru Meditation errors
```
→ **Device in crash loop.** Each reboot adds 1-2 MB overhead.

## Troubleshooting During Setup

**If you don't see hourly reports:**
- Check Serial baud rate is 115200
- Verify device is connected and selected in PlatformIO
- Look for "Egress Report" text in output

**If device won't compile:**
- Check platformio.ini environment name matches `esp32dev`
- Verify all dependencies installed (should auto-install)

**If Serial shows gibberish:**
- Change baud rate to 115200 (not 9600)
- Check USB cable is good
- Try different USB port

## Timeline

| Time | Action | Expected |
|------|--------|----------|
| Now | Upload firmware | Takes 2-3 min |
| Hour 1 | First egress report appears | ~1-2 MB |
| Hour 2-24 | Collect remaining reports | 23 more reports |
| After 24h | Analyze trends | Identify issue |

## Key Questions to Answer

Once you have the data, we'll know:

1. **Is optimization working?**
   - Yes if projected daily is 2-4 MB
   - No if projected daily is 20+ MB

2. **Why is actual egress 27 MB/device/day?**
   - Reboots: Device restarting constantly
   - WiFi: Network instability causing refetches
   - Response bloat: API returning more data than needed
   - Hidden polling: Unknown loop fetching constantly

3. **What needs fixing?**
   - If reboots: Increase buffer, reduce schedule complexity
   - If WiFi: Improve network stability, adjust reconnect logic
   - If bloat: Optimize Supabase queries (add filters, limits)
   - If hidden polling: Find and remove the extra loop

## Don't Do This

- ❌ Don't upload to just one device yet → Upload to all 3 for complete picture
- ❌ Don't test for only 1 hour → 24 hours gives much better data
- ❌ Don't ignore error messages → They're crucial clues
- ❌ Don't change code while testing → We need baseline data first

## Files to Reference

1. [EGRESS_TRACKING_IMPLEMENTATION.md](EGRESS_TRACKING_IMPLEMENTATION.md) - Technical details
2. [EGRESS_TRACKING_GUIDE.md](EGRESS_TRACKING_GUIDE.md) - Interpretation guide  
3. [EGRESS_DIAGNOSTIC.md](EGRESS_DIAGNOSTIC.md) - All possible causes

---

**Status: Ready to deploy** ✅

Just upload the firmware and let it collect data for 24 hours. The tracking system will do all the work and report results hourly to Serial output.
