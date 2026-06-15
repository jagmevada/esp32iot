# ESP32 Sensor Device → Server Interface

This document describes how the ESP32 IoT devices (ECS air-handling units and AC
units) push telemetry to the server. Share it with whoever builds/operates the
server side.

The devices speak the **Prometheus Pushgateway** text protocol over plain HTTP.
Each device periodically POSTs its current readings; the server stores them keyed
by a `sensor_id` label.

---

## 1. Endpoint

```
POST http://13.200.74.140:9091/metrics/job/sensors/sensor_id/<device_id>
```

| Item            | Value                                                        |
|-----------------|--------------------------------------------------------------|
| Method          | `POST`                                                       |
| Host / Port     | `13.200.74.140:9091` (Prometheus Pushgateway)               |
| Path            | `/metrics/job/sensors/sensor_id/<device_id>`                |
| `<device_id>`   | one of the devices in §3 (e.g. `ecs_1`, `ac_2`)             |
| Auth            | **none** (plain HTTP, no API key / token)                   |
| `Content-Type`  | `text/plain`                                                |

The path segments after `/metrics/` become Prometheus labels:

- `job="sensors"`
- `sensor_id="<device_id>"`

So a metric `t1` pushed by `ecs_1` is queryable as
`t1{job="sensors", sensor_id="ecs_1"}`.

> **Pushgateway semantics:** a `POST` replaces the values of the metrics named in
> the request body within that group, leaving other metrics in the group intact.
> Metrics are **not** auto-expired — a device that goes offline keeps its last
> pushed value until overwritten or deleted. (Use `DELETE` on the group URL to
> clear a device if needed.)

---

## 2. Request body format

Prometheus **text exposition format**: one metric per line, `name value`,
separated by `\n`, with a **trailing newline** at the end (required by the
Pushgateway parser).

```
t1 22.56
t2 21.78
relay1 1
```

- Values are plain decimal numbers.
- Booleans (relays) are encoded as `1` (on) / `0` (off).
- There is **no `null`** in this format. If a sensor reading is invalid, the
  device simply **omits that line** rather than sending a placeholder. Treat a
  missing metric as "no current reading" (the previous value persists in the
  Pushgateway until the next successful read — see semantics note above).

---

## 3. Device roster

| `device_id` | Type | Description                  |
|-------------|------|------------------------------|
| `ecs_1`     | ECS  | Air-handling / purifier unit |
| `ecs_2`     | ECS  | Air-handling / purifier unit |
| `ecs_3`     | ECS  | Air-handling / purifier unit |
| `ac_1`      | AC   | Air conditioner              |
| `ac_2`      | AC   | Air conditioner              |
| `ac_3`      | AC   | Air conditioner              |

The two device **types** push different metric sets (see §4). The `sensor_id`
label is the only thing that distinguishes individual units.

---

## 4. Metrics

### 4.1 ECS units (`ecs_1`, `ecs_2`, `ecs_3`)

Full environmental + particulate set (SHT31 temp/RH sensors + Sensirion SPS30
particulate sensor + 2 relays).

| Metric              | Unit     | Type    | Meaning                                          |
|---------------------|----------|---------|--------------------------------------------------|
| `t1`                | °C       | gauge   | Temperature, sensor 1 (inside / motor side)      |
| `t2`                | °C       | gauge   | Temperature, sensor 2 (outside / vent side)      |
| `rh1`               | %        | gauge   | Relative humidity, sensor 1 (inside)             |
| `rh2`               | %        | gauge   | Relative humidity, sensor 2 (outside)            |
| `pm1`               | µg/m³    | gauge   | Particulate mass concentration, PM1.0            |
| `pm25`              | µg/m³    | gauge   | Particulate mass concentration, PM2.5            |
| `pm10`              | µg/m³    | gauge   | Particulate mass concentration, PM10             |
| `avg_particle_size` | µm       | gauge   | Average particle size                            |
| `nc0_5`             | #/cm³    | gauge   | Number concentration, particles ≥ 0.5 µm         |
| `nc1_0`             | #/cm³    | gauge   | Number concentration, particles ≥ 1.0 µm         |
| `nc2_5`             | #/cm³    | gauge   | Number concentration, particles ≥ 2.5 µm         |
| `nc10`              | #/cm³    | gauge   | Number concentration, particles ≥ 10 µm          |
| `relay1`            | 0/1      | gauge   | Relay 1 state — **Air Purifier** (1=on, 0=off)   |
| `relay2`            | 0/1      | gauge   | Relay 2 state — **Dehumidifier** (1=on, 0=off)   |

### 4.2 AC units (`ac_1`, `ac_2`, `ac_3`)

Air conditioners only report two temperatures and a single relay.

| Metric   | Unit | Type  | Meaning                                       |
|----------|------|-------|-----------------------------------------------|
| `t1`     | °C   | gauge | Temperature, sensor 1                         |
| `t2`     | °C   | gauge | Temperature, sensor 2                         |
| `relay1` | 0/1  | gauge | Relay 1 state — **AC compressor** (1=on/0=off)|

> AC units do **not** send `rh*`, `pm*`, `nc*`, `avg_particle_size`, or `relay2`.

---

## 5. Push frequency

- Devices push on a periodic timer (production interval is on the order of tens
  of seconds per device; the test harness pushes every 15 s).
- The server should **not** assume a fixed exact interval — treat each push as
  "latest known state" and rely on the Pushgateway/Prometheus scrape for
  time-series sampling.

---

## 6. Examples

### 6.1 ECS push (full set)

```bash
curl --location 'http://13.200.74.140:9091/metrics/job/sensors/sensor_id/ecs_1' \
  --header 'Content-Type: text/plain' \
  --data 't1 22.56
t2 21.78
rh1 25.12
rh2 23.34
pm1 23.1
pm25 22.3
pm10 54.4
avg_particle_size 0.85
nc0_5 1234
nc1_0 567
nc2_5 89
nc10 40
relay1 1
relay2 1
'
```

### 6.2 AC push (temps + single relay)

```bash
curl --location 'http://13.200.74.140:9091/metrics/job/sensors/sensor_id/ac_1' \
  --header 'Content-Type: text/plain' \
  --data 't1 24.10
t2 18.45
relay1 1
'
```

### 6.3 Successful response

The Pushgateway returns **HTTP 200** (older versions: **202**) with an empty
body on success. The device treats `200` and `202` as success; any other code is
logged as a failure.

---

## 7. Querying the data (Prometheus / dashboard side)

Once the Pushgateway is scraped by Prometheus (`job="sensors"`), example PromQL:

```promql
# Inside temperature of every ECS unit
t1{job="sensors", sensor_id=~"ecs_.*"}

# All relay states for a specific AC unit
relay1{job="sensors", sensor_id="ac_2"}

# PM2.5 across all ECS units
pm25{job="sensors", sensor_id=~"ecs_.*"}
```

Metric-to-label mapping recap:

| Label       | Source                                  | Example   |
|-------------|-----------------------------------------|-----------|
| `job`       | fixed path segment                      | `sensors` |
| `sensor_id` | device id (path segment)                | `ecs_1`   |

---

## 8. Notes for the server-side developer

1. **No authentication** is currently used. If the endpoint is exposed publicly,
   consider a reverse proxy with auth / IP allow-listing.
2. **Plain HTTP** (not HTTPS). Devices are on `http://`.
3. **Stale values:** the Pushgateway keeps the last pushed value indefinitely.
   To detect an offline device, watch `push_time_seconds{...}` (Pushgateway adds
   this automatically per group) or alert on absence of recent change.
4. **New devices** are added simply by pushing to a new `sensor_id` — no
   server-side registration is required, but dashboards/alerts should be updated
   to include them.
5. Metric names are **flat** (no namespace prefix). If you prefer namespaced
   names (e.g. `sensor_t1`), that is a server-side relabeling decision; let the
   firmware team know if the on-wire names should change.

---

## 9. Device network provisioning

On boot, each device attempts WiFi in this order:

1. **Static network first** — it tries to join SSID `dada` (password
   `dadaniruma`) for up to 15 seconds.
2. **Config-portal fallback** — if that network is not available, the device
   starts a WiFiManager captive-portal access point named `TESTHARNESS_SETUP`.
   Connect a phone/laptop to that AP and browse to `192.168.4.1` to enter
   alternate WiFi credentials.

The same logic is used to recover if WiFi drops while running.

> `dada` / `dadaniruma` are **default test-harness credentials** baked into the
> firmware — change them before any production deployment.
