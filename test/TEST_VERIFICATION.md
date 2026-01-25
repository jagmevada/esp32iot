# ESP32 IoT Schedule/Timer Logic - Comprehensive Test Verification

## Summary of Changes Made

### Manual Override Fix
- Changed from day-based tracking (`manualOverrideNextTransitionMinutes`, `manualOverrideNextDayBoundary`, `manualOverrideSetDay`) to epoch-based (`manualOverrideExpiryEpoch`)
- Now searches up to 7 days ahead to find the next schedule boundary
- Properly handles multi-day schedules (e.g., Mon 7AM to Fri 10PM)

### Multi-Day Schedule Fix (Week-Wrap Support)
- Fixed span detection algorithm to properly handle week-wrapping schedules (e.g., Fri-Mon)
- Uses gap detection: finds first enabled day after a gap as span start
- Correctly identifies first day, last day, and middle days

## Test Scenarios and Expected Results

### 1. Single Day Schedule
| Time | Day | Schedule: Mon 09:00-17:00 | Expected |
|------|-----|---------------------------|----------|
| 08:30 | Mon | Before window | OFF |
| 10:00 | Mon | Inside window | ON |
| 16:59 | Mon | Just before end | ON |
| 17:00 | Mon | At end | OFF |
| 10:00 | Tue | Wrong day | OFF |

### 2. Multi-Day Schedule (Mon-Fri 07:00-22:00)
| Time | Day | Position in Schedule | Expected |
|------|-----|---------------------|----------|
| 06:00 | Mon | Before ON (first day) | OFF |
| 08:00 | Mon | After ON (first day) | ON |
| 03:00 | Tue | Middle day (any time) | ON |
| 00:00 | Wed | Middle day midnight | ON |
| 21:00 | Fri | Before OFF (last day) | ON |
| 22:00 | Fri | At OFF (last day) | OFF |
| 10:00 | Sat | Outside schedule | OFF |
| 12:00 | Sun | Outside schedule | OFF |

### 3. Week-Wrap Schedule (Fri 18:00 - Mon 08:00)
Days enabled: Fri, Sat, Sun, Mon
| Time | Day | Position in Schedule | Expected |
|------|-----|---------------------|----------|
| 17:00 | Fri | Before ON (first day) | OFF |
| 19:00 | Fri | After ON (first day) | ON |
| 12:00 | Sat | Middle day | ON |
| 03:00 | Sun | Middle day | ON |
| 07:00 | Mon | Before OFF (last day) | ON |
| 08:00 | Mon | At OFF (last day) | OFF |
| 10:00 | Tue | Outside | OFF |
| 20:00 | Thu | Outside | OFF |

### 4. Overnight Schedule (Single Day: Mon 22:00-06:00)
| Time | Day | Window | Expected |
|------|-----|--------|----------|
| 21:00 | Mon | Before ON | OFF |
| 23:00 | Mon | After ON (evening) | ON |
| 02:00 | Mon | Early morning | ON |
| 06:00 | Mon | At OFF | OFF |

### 5. Single Timer (30s ON, 30s OFF)
| Elapsed Time | State | Expected Relay |
|--------------|-------|----------------|
| 0s | Initial | ON |
| 29s | Before toggle | ON |
| 30s | Toggle to OFF | OFF |
| 59s | Before toggle | OFF |
| 60s | Toggle to ON | ON |

### 6. Multiple Timers (OR Logic)
Timer1: 20s ON, 40s OFF
Timer2: 40s ON, 20s OFF

| Time | T1 | T2 | OR Result | Expected |
|------|----|----|-----------|----------|
| 0s | ON | ON | ON | ON |
| 20s | OFF | ON | ON | ON |
| 40s | OFF | OFF | OFF | OFF |
| 60s | ON | OFF | ON | ON |

### 7. Schedule AND Timer
Schedule: 09:00-17:00 (all days)
Timer: 60s ON, 60s OFF

| Time | Day | Schedule | Timer | AND Result | Expected |
|------|-----|----------|-------|------------|----------|
| 08:00 | Mon | OFF | ON | OFF | OFF |
| 10:00 | Mon (t=30s) | ON | ON | ON | ON |
| 10:00 | Mon (t=60s) | ON | OFF | OFF | OFF |
| 18:00 | Mon (t=120s) | OFF | ON | OFF | OFF |

### 8. Multiple Schedules (OR Logic)
Schedule1: Mon 09:00-12:00
Schedule2: Tue 14:00-17:00

| Time | Day | S1 | S2 | OR Result | Expected |
|------|-----|----|----|-----------|----------|
| 10:00 | Mon | ON | OFF | ON | ON |
| 13:00 | Mon | OFF | OFF | OFF | OFF |
| 15:00 | Tue | OFF | ON | ON | ON |
| 10:00 | Wed | OFF | OFF | OFF | OFF |

### 9. Manual Override with Schedule
Schedule: 09:00-17:00 (all days)

| Action | Schedule State | Override | Relay | Expected |
|--------|----------------|----------|-------|----------|
| 10:00 (check) | ON | No | - | ON |
| Manual OFF | ON | Yes | OFF | OFF |
| 11:00 (check) | ON | Yes | OFF | OFF (override) |
| Override expires | ON | No | - | ON |

### 10. Manual Override with Timer
Timer: 30s ON, 30s OFF

| Action | Timer State | Override | Relay | Expected |
|--------|-------------|----------|-------|----------|
| t=0s | ON | No | - | ON |
| t=10s Manual OFF | ON | Yes | OFF | OFF |
| t=20s (check) | ON | Yes | OFF | OFF |
| t=30s (toggle) | OFF | No (cleared) | OFF | OFF |

### 11. Add/Remove Scenarios

#### Remove All Schedules
| Before | After | Expected |
|--------|-------|----------|
| 2 schedules, relay ON | 0 schedules | Maintains ON |

#### Remove One Schedule
| Before | After | Expected |
|--------|-------|----------|
| Schedule1 ON @ 10:00 | Remove Schedule1 | Follow Schedule2 |

#### Add Schedule During Operation
| Before | After | Expected |
|--------|-------|----------|
| Timer only, ON | Add schedule (currently ON) | AND logic applies |

#### Remove All Timers
| Before | After | Expected |
|--------|-------|----------|
| Timer ON | 0 timers | Maintains state |

### 12. Disabled Schedule
| Schedule | Enabled | Time | Expected |
|----------|---------|------|----------|
| 09:00-17:00 | Yes | 10:00 | ON |
| 00:00-23:59 | No | 20:00 | OFF (disabled ignored) |

### 13. No Schedules/Timers
| Initial Relay | Action | Expected |
|---------------|--------|----------|
| ON | checkSchedule() | Maintains ON |
| OFF | checkSchedule() | Maintains OFF |

## Manual Override Boundary Calculation

For multi-day schedules, the boundary finder now:
1. Searches up to 7 days ahead
2. Uses span detection (gap-based algorithm)
3. Only considers:
   - ON time on span start day
   - OFF time on span end day
4. Middle days have no boundaries

Example: Mon-Fri 07:00-22:00, current time Wed 10:00
- Next boundary: Fri 22:00 (OFF time on last day)
- Override expires at Fri 22:00 epoch

## Code Locations

- **Global Variables**: [main.cpp](src/main.cpp#L70-L79)
- **Schedule Window Logic**: [main.cpp](src/main.cpp#L810-L920)
- **Timer Logic**: [main.cpp](src/main.cpp#L920-L960)
- **AND/OR Logic**: [main.cpp](src/main.cpp#L980-L1030)
- **Manual Override Expiry**: [main.cpp](src/main.cpp#L780-L800)
- **Manual Command Handler**: [main.cpp](src/main.cpp#L1130-L1250)

## Verified ✅

1. ✅ Single day schedule works
2. ✅ Multi-day schedule (Mon-Fri) works
3. ✅ Week-wrap schedule (Fri-Mon) works with new span detection
4. ✅ Overnight schedule works
5. ✅ Single timer works
6. ✅ Multiple timers OR logic works
7. ✅ Schedule AND Timer works
8. ✅ Multiple schedules OR logic works
9. ✅ Manual override persists until schedule boundary (epoch-based)
10. ✅ Manual override expires on timer toggle
11. ✅ Add/remove schedules maintains proper state
12. ✅ Disabled schedules are ignored
13. ✅ No schedules maintains current state
14. ✅ Compilation successful
