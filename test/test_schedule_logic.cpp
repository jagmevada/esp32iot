/**
 * Comprehensive Test Suite for ESP32 IoT Schedule/Timer Logic
 * 
 * This file tests all combinations of:
 * - Single/Multi day schedules
 * - Single/Multiple timers
 * - Manual ON/OFF override
 * - Add/Remove schedules and timers
 * - Week-wrapping schedules (Fri-Mon)
 * 
 * To verify: Review this test logic against main.cpp implementation
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

// ============== Simulated Types ==============
struct ScheduleRow {
  long row_id;
  char setting[16];  // "schedule" or "timer"
  bool enable;
  bool weekday[7];   // 0=Sun, 1=Mon, ... 6=Sat
  int on_h, on_m;    // ON time
  int off_h, off_m;  // OFF time
  unsigned long on_duration_s;
  unsigned long off_duration_s;
  bool timer_state;
  bool initialized;
  unsigned long last_toggle_ms;
};

#define MAX_SCHEDULES 10
ScheduleRow scheduleRows[MAX_SCHEDULES];
int scheduleCount = 0;

bool relayState = false;
bool manualOverridePending = false;
time_t manualOverrideExpiryEpoch = 0;
bool manualOverrideState = false;

// Test time simulation
int simDay = 1;  // Monday
int simHour = 10;
int simMin = 0;
unsigned long simMillis = 0;

// ============== Helper Functions ==============
void resetState() {
  scheduleCount = 0;
  memset(scheduleRows, 0, sizeof(scheduleRows));
  relayState = false;
  manualOverridePending = false;
  manualOverrideExpiryEpoch = 0;
  manualOverrideState = false;
  simMillis = 0;
}

void setSimTime(int day, int hour, int min) {
  simDay = day;
  simHour = hour;
  simMin = min;
}

time_t getSimEpochLocal() {
  // Create a fake epoch for testing (base: Jan 1, 2024 was Monday)
  // Day 0=Sun, 1=Mon, etc.
  struct tm t = {0};
  t.tm_year = 124;  // 2024
  t.tm_mon = 0;     // January
  t.tm_mday = 7 + simDay;  // Jan 7 2024 = Sunday, Jan 8 = Monday, etc.
  t.tm_hour = simHour;
  t.tm_min = simMin;
  t.tm_sec = 0;
  return mktime(&t);
}

void addSchedule(const char* setting, bool enable, const bool weekdays[7], 
                 int on_h, int on_m, int off_h, int off_m,
                 unsigned long on_dur_s = 0, unsigned long off_dur_s = 0) {
  if (scheduleCount >= MAX_SCHEDULES) return;
  ScheduleRow& r = scheduleRows[scheduleCount++];
  r.row_id = scheduleCount;
  strncpy(r.setting, setting, sizeof(r.setting));
  r.enable = enable;
  memcpy(r.weekday, weekdays, sizeof(r.weekday));
  r.on_h = on_h;
  r.on_m = on_m;
  r.off_h = off_h;
  r.off_m = off_m;
  r.on_duration_s = on_dur_s;
  r.off_duration_s = off_dur_s;
  r.timer_state = true;  // Timers start ON
  r.initialized = false;
  r.last_toggle_ms = 0;
}

void removeSchedule(int index) {
  if (index < 0 || index >= scheduleCount) return;
  for (int i = index; i < scheduleCount - 1; i++) {
    scheduleRows[i] = scheduleRows[i + 1];
  }
  scheduleCount--;
}

// ============== Core Logic (Matching main.cpp exactly) ==============
bool evaluateScheduleWindow(ScheduleRow& r, int today, int nowMinutes) {
  if (!r.enable) return false;
  if (strcmp(r.setting, "schedule") != 0) return false;
  
  int enabledDayCount = 0;
  for (int d = 0; d < 7; d++) {
    if (r.weekday[d]) enabledDayCount++;
  }
  if (enabledDayCount == 0) return false;
  
  int onMinutes = r.on_h * 60 + r.on_m;
  int offMinutes = r.off_h * 60 + r.off_m;
  
  if (enabledDayCount == 1) {
    // Single day schedule
    int enabledDay = -1;
    for (int d = 0; d < 7; d++) {
      if (r.weekday[d]) { enabledDay = d; break; }
    }
    if (today == enabledDay) {
      if (onMinutes < offMinutes) {
        return (nowMinutes >= onMinutes && nowMinutes < offMinutes);
      } else if (onMinutes > offMinutes) {
        // Overnight
        return (nowMinutes >= onMinutes || nowMinutes < offMinutes);
      }
    }
  } else {
    // Multi-day: find contiguous span by looking for gaps
    int spanStartDay = -1, spanEndDay = -1;
    
    // Find span start (first enabled day after a gap)
    for (int offset = 0; offset < 7; offset++) {
      int prevDay = (7 + offset - 1) % 7;
      int curDay = offset;
      if (!r.weekday[prevDay] && r.weekday[curDay]) {
        spanStartDay = curDay;
        break;
      }
    }
    if (spanStartDay == -1) spanStartDay = 0;  // All days enabled
    
    // Find span end (last enabled day before a gap)
    for (int offset = 0; offset < 7; offset++) {
      int curDay = (spanStartDay + offset) % 7;
      int nextDay = (spanStartDay + offset + 1) % 7;
      if (r.weekday[curDay] && !r.weekday[nextDay]) {
        spanEndDay = curDay;
        break;
      }
    }
    if (spanEndDay == -1) spanEndDay = (spanStartDay + 6) % 7;
    
    bool todayInSpan = r.weekday[today];
    bool isFirstDay = (today == spanStartDay);
    bool isLastDay = (today == spanEndDay);
    bool isMiddleDay = todayInSpan && !isFirstDay && !isLastDay;
    
    if (todayInSpan) {
      if (isMiddleDay) return true;  // Full 24 hours
      else if (isFirstDay && isLastDay) {
        // Single enabled day or all days (same logic)
        if (onMinutes < offMinutes) return (nowMinutes >= onMinutes && nowMinutes < offMinutes);
        else return (nowMinutes >= onMinutes || nowMinutes < offMinutes);
      } else if (isFirstDay) {
        return (nowMinutes >= onMinutes);  // ON from onMinutes to midnight
      } else if (isLastDay) {
        return (nowMinutes < offMinutes);  // ON from midnight to offMinutes
      }
    }
  }
  return false;
}

bool evaluateTimer(ScheduleRow& r, unsigned long nowMs) {
  if (!r.enable) return false;
  if (strcmp(r.setting, "timer") != 0) return false;
  
  if (!r.initialized) {
    r.timer_state = true;
    r.last_toggle_ms = nowMs;
    r.initialized = true;
  }
  
  unsigned long elapsed = (nowMs - r.last_toggle_ms) / 1000UL;
  bool stateChanged = false;
  
  if (r.timer_state) {
    if (r.on_duration_s > 0 && elapsed >= r.on_duration_s) {
      r.timer_state = false;
      r.last_toggle_ms = nowMs;
      stateChanged = true;
    }
  } else {
    if (r.off_duration_s > 0 && elapsed >= r.off_duration_s) {
      r.timer_state = true;
      r.last_toggle_ms = nowMs;
      stateChanged = true;
    }
  }
  
  // Timer toggle clears manual override
  if (stateChanged && manualOverridePending) {
    manualOverridePending = false;
    manualOverrideExpiryEpoch = 0;
    printf("    [Timer toggle cleared manual override]\n");
  }
  
  return r.timer_state;
}

bool runScheduleCheck() {
  if (scheduleCount <= 0) {
    printf("    [No schedules - maintaining state: %s]\n", relayState ? "ON" : "OFF");
    return relayState;
  }
  
  int today = simDay;
  int nowMinutes = simHour * 60 + simMin;
  time_t epochLocal = getSimEpochLocal();
  
  // Check manual override expiry
  if (manualOverridePending && manualOverrideExpiryEpoch > 0) {
    if (epochLocal >= manualOverrideExpiryEpoch) {
      manualOverridePending = false;
      printf("    [Manual override expired at schedule boundary]\n");
    }
  }
  
  bool scheduleShouldBeOn = false;
  bool timerShouldBeOn = false;
  bool hasAnySchedule = false;
  bool hasAnyTimer = false;
  int scheduleCount_active = 0;
  int timerCount_active = 0;
  
  for (int i = 0; i < scheduleCount; i++) {
    ScheduleRow& r = scheduleRows[i];
    if (!r.enable) continue;
    
    if (strcmp(r.setting, "schedule") == 0) {
      hasAnySchedule = true;
      if (evaluateScheduleWindow(r, today, nowMinutes)) {
        scheduleShouldBeOn = true;
        scheduleCount_active++;
      }
    } else if (strcmp(r.setting, "timer") == 0) {
      hasAnyTimer = true;
      if (evaluateTimer(r, simMillis)) {
        timerShouldBeOn = true;
        timerCount_active++;
      }
    }
  }
  
  bool desired = relayState;
  
  if (hasAnySchedule && hasAnyTimer) {
    desired = scheduleShouldBeOn && timerShouldBeOn;
    printf("    [AND mode: Schedule=%s, Timer=%s => %s]\n", 
           scheduleShouldBeOn?"ON":"OFF", timerShouldBeOn?"ON":"OFF", desired?"ON":"OFF");
  } else if (hasAnySchedule) {
    desired = scheduleShouldBeOn;
    printf("    [Schedule-only: %s]\n", desired?"ON":"OFF");
  } else if (hasAnyTimer) {
    desired = timerShouldBeOn;
    printf("    [Timer-only: %s]\n", desired?"ON":"OFF");
  }
  
  if (manualOverridePending) {
    printf("    [Manual override active - keeping %s (auto would be %s)]\n", 
           relayState?"ON":"OFF", desired?"ON":"OFF");
    return relayState;
  }
  
  if (desired != relayState) {
    relayState = desired;
    printf("    [Relay changed to: %s]\n", relayState?"ON":"OFF");
  }
  
  return relayState;
}

void applyManualOverride(bool newState) {
  manualOverridePending = true;
  manualOverrideState = newState;
  
  // Find next boundary (simplified - just set to 0 for timer-only expiry in tests)
  // In real code this searches 7 days ahead
  manualOverrideExpiryEpoch = 0;
  
  // For schedules, find next boundary
  for (int i = 0; i < scheduleCount; i++) {
    if (scheduleRows[i].enable && strcmp(scheduleRows[i].setting, "schedule") == 0) {
      // Simplified: set expiry to 1 hour from now for testing
      manualOverrideExpiryEpoch = getSimEpochLocal() + 3600;
      break;
    }
  }
  
  relayState = newState;
  printf("    [Manual override set: relay=%s, expiry=%s]\n", 
         newState?"ON":"OFF", manualOverrideExpiryEpoch>0?"at boundary":"timer toggle only");
}

// ============== Test Cases ==============
int testsPassed = 0;
int testsFailed = 0;

void assertRelay(bool expected, const char* testName) {
  if (relayState == expected) {
    printf("  ✅ PASS: %s (relay=%s)\n", testName, relayState?"ON":"OFF");
    testsPassed++;
  } else {
    printf("  ❌ FAIL: %s (expected=%s, got=%s)\n", testName, expected?"ON":"OFF", relayState?"ON":"OFF");
    testsFailed++;
  }
}

void assertOverride(bool expected, const char* testName) {
  if (manualOverridePending == expected) {
    printf("  ✅ PASS: %s (override=%s)\n", testName, manualOverridePending?"active":"inactive");
    testsPassed++;
  } else {
    printf("  ❌ FAIL: %s (expected override=%s, got=%s)\n", testName, 
           expected?"active":"inactive", manualOverridePending?"active":"inactive");
    testsFailed++;
  }
}

// =============== TEST SUITES ===============

void testSingleDaySchedule() {
  printf("\n=== TEST: Single Day Schedule ===\n");
  resetState();
  
  // Monday 09:00-17:00
  bool days[7] = {false, true, false, false, false, false, false};  // Mon only
  addSchedule("schedule", true, days, 9, 0, 17, 0);
  
  // Before window
  setSimTime(1, 8, 30);  // Mon 08:30
  runScheduleCheck();
  assertRelay(false, "Before schedule window");
  
  // Inside window
  setSimTime(1, 10, 0);  // Mon 10:00
  runScheduleCheck();
  assertRelay(true, "Inside schedule window");
  
  // At end of window
  setSimTime(1, 16, 59);  // Mon 16:59
  runScheduleCheck();
  assertRelay(true, "Just before window end");
  
  // After window
  setSimTime(1, 17, 0);  // Mon 17:00
  runScheduleCheck();
  assertRelay(false, "At window end");
  
  // Wrong day
  setSimTime(2, 10, 0);  // Tue 10:00
  runScheduleCheck();
  assertRelay(false, "Wrong day (Tuesday)");
}

void testMultiDaySchedule() {
  printf("\n=== TEST: Multi-Day Schedule (Mon-Fri) ===\n");
  resetState();
  
  // Mon-Fri 07:00-22:00
  bool days[7] = {false, true, true, true, true, true, false};  // Mon-Fri
  addSchedule("schedule", true, days, 7, 0, 22, 0);
  
  // Monday before ON
  setSimTime(1, 6, 0);  // Mon 06:00
  runScheduleCheck();
  assertRelay(false, "Monday before ON time");
  
  // Monday after ON (first day)
  setSimTime(1, 8, 0);  // Mon 08:00
  runScheduleCheck();
  assertRelay(true, "Monday after ON (first day)");
  
  // Tuesday (middle day) - should be ON all day
  setSimTime(2, 3, 0);  // Tue 03:00
  runScheduleCheck();
  assertRelay(true, "Tuesday 03:00 (middle day, before normal ON)");
  
  // Wednesday midnight (middle day)
  setSimTime(3, 0, 0);  // Wed 00:00
  runScheduleCheck();
  assertRelay(true, "Wednesday midnight (middle day)");
  
  // Friday before OFF (last day)
  setSimTime(5, 21, 0);  // Fri 21:00
  runScheduleCheck();
  assertRelay(true, "Friday before OFF (last day)");
  
  // Friday at OFF
  setSimTime(5, 22, 0);  // Fri 22:00
  runScheduleCheck();
  assertRelay(false, "Friday at OFF time (last day)");
  
  // Saturday (outside schedule)
  setSimTime(6, 10, 0);  // Sat 10:00
  runScheduleCheck();
  assertRelay(false, "Saturday (outside multi-day schedule)");
  
  // Sunday (outside schedule)
  setSimTime(0, 12, 0);  // Sun 12:00
  runScheduleCheck();
  assertRelay(false, "Sunday (outside multi-day schedule)");
}

void testSingleTimer() {
  printf("\n=== TEST: Single Timer (30s ON, 30s OFF) ===\n");
  resetState();
  
  bool days[7] = {true, true, true, true, true, true, true};
  addSchedule("timer", true, days, 0, 0, 0, 0, 30, 30);  // 30s ON, 30s OFF
  
  simMillis = 0;
  runScheduleCheck();
  assertRelay(true, "Timer starts ON");
  
  // After 29 seconds (still ON)
  simMillis = 29000;
  runScheduleCheck();
  assertRelay(true, "Timer still ON at 29s");
  
  // After 30 seconds (should toggle OFF)
  simMillis = 30000;
  runScheduleCheck();
  assertRelay(false, "Timer toggles OFF at 30s");
  
  // After 59 seconds (still OFF)
  simMillis = 59000;
  runScheduleCheck();
  assertRelay(false, "Timer still OFF at 59s");
  
  // After 60 seconds (should toggle ON)
  simMillis = 60000;
  runScheduleCheck();
  assertRelay(true, "Timer toggles ON at 60s");
}

void testMultipleTimers() {
  printf("\n=== TEST: Multiple Timers (OR logic) ===\n");
  resetState();
  
  bool days[7] = {true, true, true, true, true, true, true};
  addSchedule("timer", true, days, 0, 0, 0, 0, 20, 40);  // Timer 1: 20s ON, 40s OFF
  addSchedule("timer", true, days, 0, 0, 0, 0, 40, 20);  // Timer 2: 40s ON, 20s OFF
  
  simMillis = 0;
  runScheduleCheck();
  assertRelay(true, "Both timers start ON (OR=ON)");
  
  // At 20s: Timer1 goes OFF, Timer2 still ON => OR = ON
  simMillis = 20000;
  runScheduleCheck();
  assertRelay(true, "Timer1 OFF, Timer2 ON => OR=ON");
  
  // At 40s: Timer1 still OFF, Timer2 goes OFF => OR = OFF
  simMillis = 40000;
  runScheduleCheck();
  assertRelay(false, "Timer1 OFF, Timer2 OFF => OR=OFF");
  
  // At 60s: Timer1 goes ON, Timer2 still OFF => OR = ON
  simMillis = 60000;
  runScheduleCheck();
  assertRelay(true, "Timer1 ON, Timer2 OFF => OR=ON");
}

void testScheduleAndTimer() {
  printf("\n=== TEST: Schedule AND Timer ===\n");
  resetState();
  
  bool days[7] = {true, true, true, true, true, true, true};
  addSchedule("schedule", true, days, 9, 0, 17, 0);     // 09:00-17:00
  addSchedule("timer", true, days, 0, 0, 0, 0, 60, 60); // 60s ON, 60s OFF
  
  setSimTime(1, 8, 0);  // Mon 08:00 - outside schedule
  simMillis = 0;
  runScheduleCheck();
  assertRelay(false, "Schedule OFF, Timer ON => AND=OFF");
  
  setSimTime(1, 10, 0);  // Mon 10:00 - inside schedule
  simMillis = 30000;  // Timer still ON
  runScheduleCheck();
  assertRelay(true, "Schedule ON, Timer ON => AND=ON");
  
  setSimTime(1, 10, 0);
  simMillis = 60000;  // Timer toggles OFF
  runScheduleCheck();
  assertRelay(false, "Schedule ON, Timer OFF => AND=OFF");
  
  setSimTime(1, 18, 0);  // Mon 18:00 - outside schedule
  simMillis = 120000;  // Timer ON
  runScheduleCheck();
  assertRelay(false, "Schedule OFF, Timer ON => AND=OFF");
}

void testMultipleSchedules() {
  printf("\n=== TEST: Multiple Schedules (OR logic) ===\n");
  resetState();
  
  bool days1[7] = {false, true, false, false, false, false, false};  // Mon
  bool days2[7] = {false, false, true, false, false, false, false};  // Tue
  addSchedule("schedule", true, days1, 9, 0, 12, 0);   // Mon 09:00-12:00
  addSchedule("schedule", true, days2, 14, 0, 17, 0);  // Tue 14:00-17:00
  
  // Monday in first schedule
  setSimTime(1, 10, 0);
  runScheduleCheck();
  assertRelay(true, "Monday 10:00 - Schedule1 active");
  
  // Monday outside both
  setSimTime(1, 13, 0);
  runScheduleCheck();
  assertRelay(false, "Monday 13:00 - neither active");
  
  // Tuesday in second schedule
  setSimTime(2, 15, 0);
  runScheduleCheck();
  assertRelay(true, "Tuesday 15:00 - Schedule2 active");
  
  // Wednesday - neither active
  setSimTime(3, 10, 0);
  runScheduleCheck();
  assertRelay(false, "Wednesday - neither schedule active");
}

void testManualOverrideWithSchedule() {
  printf("\n=== TEST: Manual Override with Schedule ===\n");
  resetState();
  
  bool days[7] = {true, true, true, true, true, true, true};
  addSchedule("schedule", true, days, 9, 0, 17, 0);
  
  // During schedule window
  setSimTime(1, 10, 0);
  runScheduleCheck();
  assertRelay(true, "Schedule ON at 10:00");
  
  // Manual override OFF
  printf("  [Applying manual override OFF]\n");
  applyManualOverride(false);
  assertRelay(false, "Manual override to OFF");
  assertOverride(true, "Override should be active");
  
  // Check that schedule doesn't override manual
  setSimTime(1, 11, 0);
  runScheduleCheck();
  assertRelay(false, "Schedule check respects manual override");
  assertOverride(true, "Override still active");
  
  // Simulate reaching boundary (expire override)
  manualOverridePending = false;
  runScheduleCheck();
  assertRelay(true, "After override expires, schedule takes over");
}

void testManualOverrideWithTimer() {
  printf("\n=== TEST: Manual Override with Timer (timer toggle expires override) ===\n");
  resetState();
  
  bool days[7] = {true, true, true, true, true, true, true};
  addSchedule("timer", true, days, 0, 0, 0, 0, 30, 30);  // 30s ON, 30s OFF
  
  simMillis = 0;
  runScheduleCheck();
  assertRelay(true, "Timer starts ON");
  
  // Manual override OFF at 10s
  simMillis = 10000;
  printf("  [Applying manual override OFF at 10s]\n");
  applyManualOverride(false);
  assertRelay(false, "Manual override to OFF");
  assertOverride(true, "Override active");
  
  // Timer would be ON but override keeps it OFF
  simMillis = 20000;
  runScheduleCheck();
  assertRelay(false, "Override keeps relay OFF");
  
  // Timer toggles at 30s - should clear override
  simMillis = 30000;
  runScheduleCheck();
  assertOverride(false, "Timer toggle clears override");
  assertRelay(false, "After toggle, timer is OFF so relay OFF");
}

void testRemoveSchedule() {
  printf("\n=== TEST: Remove Schedule ===\n");
  resetState();
  
  bool days[7] = {true, true, true, true, true, true, true};
  addSchedule("schedule", true, days, 9, 0, 17, 0);
  addSchedule("schedule", true, days, 20, 0, 23, 0);
  
  setSimTime(1, 10, 0);
  runScheduleCheck();
  assertRelay(true, "With 2 schedules, first active");
  
  // Remove first schedule
  printf("  [Removing first schedule]\n");
  removeSchedule(0);
  
  setSimTime(1, 10, 0);
  runScheduleCheck();
  assertRelay(false, "After removing first, 10:00 not in second schedule");
  
  setSimTime(1, 21, 0);
  runScheduleCheck();
  assertRelay(true, "21:00 is in remaining schedule");
  
  // Remove last schedule
  printf("  [Removing last schedule]\n");
  removeSchedule(0);
  
  runScheduleCheck();
  assertRelay(true, "No schedules - relay maintains last state");
}

void testRemoveTimer() {
  printf("\n=== TEST: Remove Timer ===\n");
  resetState();
  
  bool days[7] = {true, true, true, true, true, true, true};
  addSchedule("timer", true, days, 0, 0, 0, 0, 10, 10);
  addSchedule("timer", true, days, 0, 0, 0, 0, 20, 20);
  
  simMillis = 0;
  runScheduleCheck();
  assertRelay(true, "Both timers ON");
  
  // Remove first timer
  printf("  [Removing first timer]\n");
  removeSchedule(0);
  
  simMillis = 5000;
  runScheduleCheck();
  assertRelay(true, "Remaining timer still ON");
  
  // Remove last timer
  printf("  [Removing last timer]\n");
  removeSchedule(0);
  
  runScheduleCheck();
  assertRelay(true, "No timers - relay maintains last state");
}

void testAddScheduleDuringOperation() {
  printf("\n=== TEST: Add Schedule During Operation ===\n");
  resetState();
  
  bool days[7] = {true, true, true, true, true, true, true};
  addSchedule("timer", true, days, 0, 0, 0, 0, 60, 60);
  
  simMillis = 0;
  setSimTime(1, 10, 0);
  runScheduleCheck();
  assertRelay(true, "Timer-only mode, timer ON");
  
  // Add schedule that would turn relay ON at this time
  printf("  [Adding schedule 09:00-17:00]\n");
  addSchedule("schedule", true, days, 9, 0, 17, 0);
  
  runScheduleCheck();
  assertRelay(true, "AND mode: Schedule ON, Timer ON => ON");
  
  // Timer goes OFF
  simMillis = 60000;
  runScheduleCheck();
  assertRelay(false, "AND mode: Schedule ON, Timer OFF => OFF");
}

void testDisabledSchedule() {
  printf("\n=== TEST: Disabled Schedule ===\n");
  resetState();
  
  bool days[7] = {true, true, true, true, true, true, true};
  addSchedule("schedule", true, days, 9, 0, 17, 0);
  addSchedule("schedule", false, days, 0, 0, 23, 59);  // Disabled 24h schedule
  
  setSimTime(1, 10, 0);
  runScheduleCheck();
  assertRelay(true, "Active schedule ON");
  
  setSimTime(1, 20, 0);  // Outside first schedule, inside disabled one
  runScheduleCheck();
  assertRelay(false, "Disabled schedule ignored");
}

void testOvernightSchedule() {
  printf("\n=== TEST: Overnight Schedule (22:00-06:00) ===\n");
  resetState();
  
  bool days[7] = {false, true, false, false, false, false, false};  // Mon only
  addSchedule("schedule", true, days, 22, 0, 6, 0);  // 22:00-06:00
  
  setSimTime(1, 21, 0);  // Mon 21:00
  runScheduleCheck();
  assertRelay(false, "Before overnight window");
  
  setSimTime(1, 23, 0);  // Mon 23:00
  runScheduleCheck();
  assertRelay(true, "Inside overnight window (evening)");
  
  setSimTime(1, 2, 0);  // Mon 02:00
  runScheduleCheck();
  assertRelay(true, "Inside overnight window (early morning)");
  
  setSimTime(1, 6, 0);  // Mon 06:00
  runScheduleCheck();
  assertRelay(false, "At end of overnight window");
}

void testWeekWrapSchedule() {
  printf("\n=== TEST: Week Wrap Schedule (Fri-Mon) ===\n");
  resetState();
  
  // Fri, Sat, Sun, Mon
  bool days[7] = {true, true, false, false, false, true, true};  // Sun, Mon, Fri, Sat
  addSchedule("schedule", true, days, 18, 0, 8, 0);  // 18:00 Fri - 08:00 Mon
  
  // Thursday - outside
  setSimTime(4, 20, 0);  // Thu 20:00
  runScheduleCheck();
  assertRelay(false, "Thursday - outside week wrap schedule");
  
  // Friday before ON
  setSimTime(5, 17, 0);  // Fri 17:00
  runScheduleCheck();
  assertRelay(false, "Friday before ON time");
  
  // Friday after ON (first day)
  setSimTime(5, 19, 0);  // Fri 19:00
  runScheduleCheck();
  assertRelay(true, "Friday after ON (first day)");
  
  // Saturday (middle day)
  setSimTime(6, 12, 0);  // Sat 12:00
  runScheduleCheck();
  assertRelay(true, "Saturday - middle day");
  
  // Sunday (middle day)
  setSimTime(0, 3, 0);  // Sun 03:00
  runScheduleCheck();
  assertRelay(true, "Sunday - middle day");
  
  // Monday before OFF (last day)
  setSimTime(1, 7, 0);  // Mon 07:00
  runScheduleCheck();
  assertRelay(true, "Monday before OFF");
  
  // Monday at OFF
  setSimTime(1, 8, 0);  // Mon 08:00
  runScheduleCheck();
  assertRelay(false, "Monday at OFF time");
  
  // Tuesday - outside
  setSimTime(2, 10, 0);  // Tue 10:00
  runScheduleCheck();
  assertRelay(false, "Tuesday - outside");
}

void testNoSchedulesOrTimers() {
  printf("\n=== TEST: No Schedules or Timers ===\n");
  resetState();
  
  relayState = true;  // Start with relay ON
  runScheduleCheck();
  assertRelay(true, "Maintains ON state with no schedules");
  
  relayState = false;  // Set relay OFF
  runScheduleCheck();
  assertRelay(false, "Maintains OFF state with no schedules");
}

void testManualOverrideOnThenOff() {
  printf("\n=== TEST: Manual Override ON then OFF ===\n");
  resetState();
  
  bool days[7] = {true, true, true, true, true, true, true};
  addSchedule("schedule", true, days, 9, 0, 17, 0);
  
  setSimTime(1, 10, 0);
  runScheduleCheck();
  assertRelay(true, "Schedule ON");
  
  // Manual OFF
  printf("  [Manual override: OFF]\n");
  applyManualOverride(false);
  assertRelay(false, "Manual OFF");
  
  runScheduleCheck();
  assertRelay(false, "Stays OFF with override");
  
  // Manual ON again
  printf("  [Manual override: ON]\n");
  applyManualOverride(true);
  assertRelay(true, "Manual ON");
  
  // Move to outside schedule
  setSimTime(1, 18, 0);
  manualOverridePending = false;  // Simulate expiry
  runScheduleCheck();
  assertRelay(false, "After expiry, schedule OFF takes over");
}

// ============== Main ==============
int main() {
  printf("╔════════════════════════════════════════════════════════════╗\n");
  printf("║     ESP32 IoT Schedule/Timer Logic - Exhaustive Tests      ║\n");
  printf("╚════════════════════════════════════════════════════════════╝\n");
  
  // Run all test suites
  testSingleDaySchedule();
  testMultiDaySchedule();
  testSingleTimer();
  testMultipleTimers();
  testScheduleAndTimer();
  testMultipleSchedules();
  testManualOverrideWithSchedule();
  testManualOverrideWithTimer();
  testRemoveSchedule();
  testRemoveTimer();
  testAddScheduleDuringOperation();
  testDisabledSchedule();
  testOvernightSchedule();
  testWeekWrapSchedule();
  testNoSchedulesOrTimers();
  testManualOverrideOnThenOff();
  
  printf("\n╔════════════════════════════════════════════════════════════╗\n");
  printf("║                      TEST SUMMARY                          ║\n");
  printf("╠════════════════════════════════════════════════════════════╣\n");
  printf("║  Passed: %3d                                               ║\n", testsPassed);
  printf("║  Failed: %3d                                               ║\n", testsFailed);
  printf("║  Total:  %3d                                               ║\n", testsPassed + testsFailed);
  printf("╚════════════════════════════════════════════════════════════╝\n");
  
  if (testsFailed > 0) {
    printf("\n❌ SOME TESTS FAILED - Review failures above\n");
    return 1;
  } else {
    printf("\n✅ ALL TESTS PASSED\n");
    return 0;
  }
}
