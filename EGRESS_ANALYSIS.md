# Supabase Egress Analysis & Optimization

## Current API Call Frequencies

### 1. Schedule Fetching (CRITICAL)
**Location**: [main.cpp#L753](src/main.cpp#L753)
```cpp
// Fetch all schedules for this device
fetchSchedulesFromSupabase();  // Called every 60 seconds
```
- **Frequency**: Every 60 seconds (1440 calls/day)
- **Payload Size**: Variable (typical 2-5 KB per request)
- **Total Daily**: ~7-7.2 MB/day
- **Monthly**: ~210-216 MB/month

**Issue**: Schedules rarely change. Even if they do, 60-second latency is acceptable.

---

### 2. Relay Command Polling (HIGH)
**Location**: [main.cpp#L1127-1130](src/main.cpp#L1127-L1130)
```cpp
if (now - lastRelayCheck >= 5000) {  // Every 5 seconds
    lastRelayCheck = now;
    bool newState = fetchRelayCommand(deviceId, "relay1", relayState1);
```
- **Frequency**: Every 5 seconds (17,280 calls/day)
- **Payload Size**: ~100 bytes per request
- **Total Daily**: ~1.7 MB/day
- **Monthly**: ~52 MB/month

**Issue**: Polling 288 times per hour is excessive for a command system.

---

### 3. Sensor Data Sending (HIGH)
**Location**: [main.cpp#L1271-1281](src/main.cpp#L1271-L1281)
```cpp
if (now - lastSensorSend >= 40000) {  // Every 40 seconds
    // Send sensor data
    sendSensorData(...);
    lastSensorSend = now;
}
```
- **Frequency**: Every 40 seconds (2,160 calls/day)
- **Payload Size**: ~150 bytes per request
- **Total Daily**: ~0.32 MB/day
- **Monthly**: ~9.6 MB/month

**Plus**: Additional sends on manual command (unpredictable)

---

## Total Current Egress Estimate

| Operation | Daily | Monthly |
|-----------|-------|---------|
| Schedules (60s) | 7.0 MB | 210 MB |
| Relay Command (5s) | 1.7 MB | 52 MB |
| Sensor Data (40s) | 0.32 MB | 9.6 MB |
| **TOTAL** | **~9 MB** | **~272 MB** |

At Supabase rates (~$0.125 per GB), this is **~$34/month** in egress costs.

---

## Optimization Recommendations

### Tier 1: Quick Wins (Easy, High Impact)

#### 1.1 Increase Schedule Fetch Interval
**Current**: 60 seconds (1,440 calls/day)
**Proposed**: 5 minutes (288 calls/day)
**Savings**: ~202 MB/month (-94%)
**Latency Impact**: 5-minute delay in schedule changes

**Implementation**:
```cpp
// Change from 60000 to 300000 (5 minutes)
if (now - lastScheduleCheck >= 300000UL) {  // 5 minutes instead of 1
```

#### 1.2 Increase Relay Command Poll Interval
**Current**: 5 seconds (17,280 calls/day)
**Proposed**: 30 seconds (2,880 calls/day)
**Savings**: ~50 MB/month (-86%)
**Latency Impact**: 30-second delay in detecting manual commands

**Implementation**:
```cpp
// Change from 5000 to 30000
if (now - lastRelayCheck >= 30000) {  // 30 seconds instead of 5
```

#### 1.3 Increase Sensor Data Send Interval
**Current**: 40 seconds (2,160 calls/day)
**Proposed**: 120 seconds (720 calls/day)
**Savings**: ~6.4 MB/month (-67%)
**Latency Impact**: 2-minute delay in sensor data updates

**Implementation**:
```cpp
// Change from 40000 to 120000
if (now - lastSensorSend >= 120000) {  // 120 seconds instead of 40
```

---

### Tier 2: Medium Impact (Smart Caching)

#### 2.1 Cache Schedules with Change Detection
```cpp
// Add CRC32 check header to schedule response
// Only re-fetch if CRC changes
// Typical: Still 288 calls/day but 95%+ are just header checks (~100 bytes)
// Saves: ~160 MB/month
```

#### 2.2 Implement Long Polling for Relay Commands
```cpp
// Instead of polling every 5s, use Supabase Realtime subscriptions
// Receives updates only when data changes
// Saves: ~50+ MB/month (eliminates polling entirely)
// Requires: WebSocket client library
```

---

### Tier 3: Advanced (Architecture Change)

#### 3.1 Command Queue with Batch Processing
- Don't fetch relay command every 5 seconds
- Use edge function or webhook to push commands
- Reduces: 17,280 calls/day → 0 calls/day
- Saves: ~52 MB/month

#### 3.2 Local Caching with TTL
- Cache schedule locally for 5-10 minutes
- Cache relay state for 5 minutes
- Only fetch if TTL expired or cache miss

---

## Recommended Implementation (Balanced)

### Phase 1: Immediate (No Architecture Changes)
Modify [main.cpp#L1115-1127](src/main.cpp#L1115-L1127):

```cpp
// Schedule check: run every 5 minutes (was 1 minute)
if (now - lastScheduleCheck >= 300000UL) {
    lastScheduleCheck = now;
    checkSchedule();
}

// Relay command polling: every 30 seconds (was 5 seconds)
if (now - lastRelayCheck >= 30000) {
    lastRelayCheck = now;
    bool newState = fetchRelayCommand(deviceId, "relay1", relayState1);
    ...
}

// Sensor data: every 2 minutes (was 40 seconds)
if (now - lastSensorSend >= 120000) {
    float t1, t2;
    ...
    sendSensorData(...);
    lastSensorSend = now;
}
```

**Expected Savings**: ~150 MB/month (-55%)

---

## Testing Recommendations

Before deployment, test:

1. **5-minute schedule interval**: Can users accept 5-min delay in schedule changes?
2. **30-second relay command**: Can users accept 30-sec delay in manual commands?
3. **2-minute sensor updates**: Adequate for monitoring?

---

## Code Changes Required

File: `src/main.cpp`

1. Line 1115: `60000UL` → `300000UL` (schedule check)
2. Line 1127: `5000` → `30000` (relay check)
3. Line 1271: `40000` → `120000` (sensor send)

Total impact: 3 lines changed, saves ~150 MB/month (~$18/month in egress costs).
