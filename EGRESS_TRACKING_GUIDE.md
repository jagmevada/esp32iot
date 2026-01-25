# Egress Tracking & Diagnostics Guide

## What Was Added

Your firmware now includes **automated data usage tracking** that reports every hour on:
1. **Total Egress** - Data downloaded from Supabase (KB and MB)
2. **Total Ingress** - Data uploaded to Supabase (KB and MB)
3. **API Call Count** - Number of HTTP requests made
4. **Average per call** - Bytes per API call (helps identify bloated responses)
5. **Projected daily usage** - What 24-hour egress would be at current rate

## Expected Output

Every hour, you'll see a report like this:

```
📊 ========== EGRESS REPORT ==========
   Total Egress: 1250 KB (1.22 MB)
   Total Ingress: 850 KB (0.83 MB)
   API Calls: 42
   Avg per call: 29.8 KB
   Projected daily: 29.3 MB/day
===================================
```

## What This Means

**With optimized intervals (5min, 30s, 2min):**
- Expected calls per hour: ~140 (288 relay + 720 sensor + 12 schedule)
- Expected egress per call: 0.3 KB (relay) + 0.15 KB (sensor) = ~150 B
- Expected total per hour: **150 × 140 = 21 KB/hour = 504 KB/day**

**But user reports 27 MB/day (50× higher!)**

### Analysis Guide

| Projected Daily | Likely Issue | Next Step |
|---|---|---|
| 1-3 MB/day | ✅ NORMAL (optimization working!) | No action needed |
| 5-10 MB/day | ⚠️ Slightly high | Check WiFi reconnects, review response sizes |
| 15-25 MB/day | ❌ HIGH (2-3x expected) | Check for error loops, retries, excessive parsing |
| >25 MB/day | ❌ CRITICAL (10-100x expected) | Device is likely rebooting constantly |

## Diagnostic Checks

### 1. **Check for Device Reboots** (MOST LIKELY)
Watch Serial output for restart messages. Each restart will show:
```
ets Jun  8 2016 00:22:57 rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_BOOT)
load 0x40078000 at offset 0x0
...
WiFiManager initialized...
🔄 Relay updated from Supabase: OFF
```

**If you see this 5+ times per hour** → Device is in reboot loop
- Check Serial monitor for crash messages
- Look for "Guru Meditation Error" messages
- Possible causes: Stack overflow, watchdog timeout, memory leak

### 2. **Check Response Sizes**
Look at the "Avg per call" in the report:
- Relay check should be ~200-300 bytes → 0.2-0.3 KB ✅
- Sensor send should be ~150 bytes → 0.15 KB ✅
- Schedule fetch should be 2-5 KB ✅

**If average is >5 KB per call** → Responses are bloated
- Check Supabase query filters (make sure you're not fetching all rows)
- Verify `limit=1` is being used for relay commands

### 3. **Monitor API Call Count**
Expected per hour:
- Relay checks: 120 calls/hour (every 30s × 60min)
- Sensor sends: 30 calls/hour (every 2min × 60min)
- Schedule fetches: 12 calls/hour (every 5min × 60min)
- **Total: ~162 calls/hour**

**If you see >300 calls/hour** → Something is polling excessively
- Check WiFi reconnection logs
- Look for error handler retries

### 4. **Check for WiFi Flapping**
Add this to your setup/Serial monitoring:
```
WiFi connected: SSID, RSSI (signal strength)
⚠️ WiFi disconnected
WiFi connected again
```

**If you see "disconnected" 10+ times per hour** → WiFi is unstable
- This triggers checkWiFi() reconnect attempts
- Each reconnect may refetch data
- Solution: Check WiFi strength, router issues, or power supply

## How to Use This Data

### Strategy A: Let It Run 24 Hours
1. Flash the updated firmware
2. Wait 24 hours
3. Collect the hourly reports
4. Average them: `(hr1 + hr2 + ... + hr24) / 24`
5. Compare to user's Supabase report

### Strategy B: Quick Spot Check (1 Hour)
1. Flash and watch Serial for 1 hour
2. Record the first hourly report
3. If projected daily is <5 MB → Optimization worked
4. If projected daily is >20 MB → Something else is wrong

## Example Troubleshooting Scenarios

### Scenario 1: "Projected daily: 2.5 MB/day"
✅ **PROBLEM SOLVED!** 
- Optimization is working as expected
- User's reported 27 MB/day was from old code
- New code should reduce it to this level

### Scenario 2: "Projected daily: 50 MB/day + many reboots"
❌ **Device rebooting loop**
- Check Serial for crash messages
- Possible causes:
  - WiFi reconnect causing watchdog timeout
  - JSON parsing of huge response causing heap overflow
  - Stack overflow in schedule evaluation
- **Fix**: Increase JSON buffer size (already done: 8KB → 16KB), or reduce schedule complexity

### Scenario 3: "Projected daily: 15 MB/day + 300+ API calls/hour"
❌ **Excessive polling detected**
- Something is calling fetchSchedulesFromSupabase() repeatedly
- Or WiFi is constantly reconnecting and triggering refetches
- **Debug steps**:
  - Add log in fetchSchedulesFromSupabase() with timestamp
  - Check if called 12 times/hour (expected) or 100+ times/hour
  - Look for error-driven retry loops

### Scenario 4: "Avg per call: 15 KB"
❌ **Responses are too large**
- Supabase might be returning more data than needed
- Check API endpoint filters:
  - Relay: Should have `limit=1` (returns 1 row)
  - Sensors: POST only, should be 150 bytes
  - Schedules: May have multiple rows, but should be 2-5 KB max
- If response is large, you're fetching too many rows

## Data Usage Equations

**Hourly calculation:**
```
Total Egress = (RelayChecks × AvgRelaySize) + 
               (SensorSends × AvgSensorSize) + 
               (ScheduleFetches × AvgScheduleSize)

Total Egress = (120 × 0.3 KB) + (30 × 0.15 KB) + (12 × 3 KB)
             = 36 + 4.5 + 36 = 76.5 KB/hour
             = 1.8 MB/day ✅
```

## Key Files Modified

1. **src/main.cpp**
   - Added tracking variables (lines 15-23)
   - Updated fetchRelayCommand() to track responses
   - Updated sendSensorData() to track responses
   - Updated fetchSchedulesFromSupabase() to track responses
   - Added hourly reporting to main loop()

## Next Steps After Analysis

1. **If data usage is <5 MB/day**: ✅ Problem solved, no further action
2. **If data usage is 10-20 MB/day**: Look for WiFi instability or response bloat
3. **If data usage is >25 MB/day**: Device is likely in reboot loop
   - Add more detailed logging
   - Check for stack overflow or memory issues
   - Monitor crash messages

## Enable More Detailed Logging (Optional)

If projected daily is high, add these debug lines to identify the exact source:

In `fetchSchedulesFromSupabase()` (around line 650):
```cpp
Serial.printf("📡 fetchSchedulesFromSupabase() called. Response size: %u bytes\n", resp.length());
```

In `checkWiFi()` (around line 175):
```cpp
Serial.printf("📶 WiFi check: Status=%d, RSSI=%d\n", WiFi.status(), WiFi.RSSI());
```

In setup() (around line 1115):
```cpp
Serial.println("🔄 Setup() called - device boot detected");
```

This will help pinpoint exactly where the bandwidth is going.

---

**Question:** After 24 hours of data collection, share the hourly reports in the Serial output, and we can pinpoint the exact cause of the 27 MB/day issue.
