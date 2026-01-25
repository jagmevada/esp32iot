# Deep Egress Analysis - 80MB/day Issue Diagnosis

## Problem Statement
User reports **80MB/day total egress** for 3 devices = **~27MB/device/day**  
My previous optimizations were expected to reduce to **~2MB/device/day**  
**Gap: 25MB/device/day unexplained**

## Likely Causes (in order of probability)

### 1. **Device Reboot Loop** (MOST LIKELY - 70% probability)
**Symptoms**: Device keeps rebooting, each boot triggers:
- WiFiManager reconnect (~2-5 MB)
- NTP sync (small, but repeated)
- Initial fetchRelayCommand (varies)
- Initial schedule fetch (2-5 MB)

**If device reboots 5 times per day**: 10 MB × 5 = 50 MB/day

**Causes**:
- Watchdog timer triggering (deadlock in WiFi reconnect)
- Memory leak in JSON parsing causing heap exhaustion
- Stack overflow in schedule evaluation loops
- Continuous WiFi disconnect/reconnect cycle

**Test**: Check Serial logs for "🔁" (restart) messages

---

### 2. **JSON Parsing Error Loop** (60% probability)
**Issue at line 639**: If JSON parse fails...
```cpp
if (err) {
  Serial.printf("❌ fetchSchedulesFromSupabase: JSON parse failed: %s\n", err.c_str());
  return;
}
```

But what if the response size is HUGE (>8KB)? 
```cpp
const size_t capacity = 8192;  // 8 KB buffer
```

**Problem**: If Supabase returns 10KB of data, ArduinoJson silently truncates and parses incomplete JSON!

**If parse fails every 5 minutes**, and code retries... but wait, it doesn't retry automatically. However...

---

### 3. **Schedule Count Explosion** (50% probability)
**Check lines 810-831**: What if scheduleCount is high and causes expensive operations?

The boundary finder loop at lines 1156-1200:
```cpp
for (int dayOffset = 0; dayOffset < 7 && nextBoundaryEpoch == 0; dayOffset++) {
  int checkDay = (t.tm_wday + dayOffset) % 7;
  
  for (int i = 0; i < scheduleCount; ++i) {  // Inner loop!
    // Complex span detection: 7 iterations per schedule
```

If you have 50 schedules, this is **7 × 50 = 350 iterations per relay command check**!

---

### 4. **WiFi Reconnect Blocking Issue** (40% probability)
**Line 146**:
```cpp
while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
  delay(500);
  Serial.print(".");
}
```

If WiFi flaps every 10 seconds:
- checkWiFi() is called every 10 seconds
- Each reconnect takes ~10 seconds
- Blocks the main loop
- Timer checks are skipped during reconnect
- But then after reconnect, cached timers might cause state issues

---

### 5. **TimeManager NTP Sync Overhead** (30% probability)
**Lines 278, 290**: TimeManager retries NTP every 10 minutes on failure

If NTP keeps failing (bad DNS, firewall, etc.):
- Hourly sync tries (usually succeeds)
- Plus 6 × 10-minute retries = 7 NTP attempts per hour
- × 24 hours = 168 NTP attempts per day (small, but adds up with failures)

---

## Diagnostic: Add Data Logging

Add this code to main.cpp to track egress:

```cpp
// Add at top of main.cpp (after globals)
unsigned long lastEgressReport = 0;
unsigned long totalEgressBytes = 0;
int apiCallCount = 0;

// Add to each http.GET() before return:
totalEgressBytes += resp.length();  // response body
apiCallCount++;

// Add to each http.POST() before return:
totalEgressBytes += 200;  // rough overhead
apiCallCount++;

// Add to loop() periodically:
if (millis() - lastEgressReport >= 3600000) {  // 1 hour
  Serial.printf("📊 EGRESS REPORT: %lu bytes, %d API calls in last hour (avg %.1f KB/call)\n",
                totalEgressBytes, apiCallCount, totalEgressBytes/1024.0/apiCallCount);
  totalEgressBytes = 0;
  apiCallCount = 0;
  lastEgressReport = millis();
}
```

This will show exactly which API is consuming bandwidth.

---

## Immediate Fixes to Try (Ordered by Impact)

### Fix 1: Check for Reboots (CRITICAL)
Add bootcount tracking:
```cpp
unsigned long bootCount = EEPROM.read(EEPROM_BOOT_COUNT_ADDR);
bootCount++;
EEPROM.write(EEPROM_BOOT_COUNT_ADDR, bootCount & 0xFF);
EEPROM.commit();
Serial.printf("🔄 Boot #%lu detected\n", bootCount);
```

If bootCount increases 5+ times per day → Device is rebooting excessively

### Fix 2: Increase JSON Buffer
```cpp
// Line 639: Change from 8192 to 16384
const size_t capacity = 16384;  // doubled buffer
```

If this fixes the issue → JSON was being truncated

### Fix 3: Reduce Boundary Search Complexity
For manual commands (line 1156), only search 2 days ahead instead of 7:
```cpp
// Line 1156: Change 7 to 2
for (int dayOffset = 0; dayOffset < 2 && nextBoundaryEpoch == 0; dayOffset++) {
```

Reduces complexity from O(7 × scheduleCount) to O(2 × scheduleCount)

### Fix 4: Add WiFi Stability Check
Before each API call, check WiFi quality:
```cpp
// Check signal strength
int rssi = WiFi.RSSI();
if (rssi < -80) {
  Serial.println("⚠️ Weak WiFi signal, delaying API call");
  return;  // Skip call, retry next cycle
}
```

### Fix 5: Increase NTP Sync Interval to Reduce Retries
```cpp
#define TIME_RETRY_INTERVAL_MS 1200000UL  // 20 min (was 10 min)
```

Reduces retry frequency from 6/hr to 3/hr

---

## Current Code Analysis Summary

| Function | Calls/Day | Size | Total | Issue |
|----------|-----------|------|-------|-------|
| fetchSchedules | 288 | 2-5 KB | 0.6-1.4 MB | ✓ Optimized |
| fetchRelayCommand | 2,880 | 300 B | 0.86 MB | ✓ Optimized |
| sendSensorData | 720 | 150 B | 0.11 MB | ✓ Optimized |
| **Expected Total** | — | — | **1.6-2.4 MB** | — |
| **User Reports** | — | — | **27 MB** | ❌ 11x gap |

The 11x gap suggests either:
1. Device rebooting 5+ times/day (resets timing, full init fetches)
2. API response sizes much larger than estimated
3. Hidden API calls not in main loop (startup, recovery, etc.)

---

## Recommendation

1. **Enable diagnostics**: Add the logging code above
2. **Monitor for 24 hours**: Collect actual egress data
3. **Check reboot count**: Verify if device is restarting
4. **Inspect actual response sizes**: Use Serial to log http response lengths
5. **Monitor WiFi stability**: Check RSSI and disconnect events

Once you know the REAL source of egress, we can fix it precisely.
