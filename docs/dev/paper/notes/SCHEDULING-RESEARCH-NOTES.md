# Research Notes — Task Scheduling, CPU Affinity, and Tick Rate
# ESP32-S3 TFLite Micro + WiFi Co-scheduling Framework
# Status: Notes for paper — measurements pending
# Date: 2026-08-30

---

## 1. Problem statement

TFLite Micro inference (esp-nn assembly kernels) and WiFi co-existence on
dual-core ESP32-S3 under FreeRTOS causes indefinite Invoke() stall.
Root cause: FreeRTOS tick crosscore ISR preempts convolution kernel mid-iteration.
This problem is unstudied and unpublished for this exact stack.

---

## 2. Complete task inventory

| Task | Nature | Current CPU | Priority | Est. Duration | Interruptible |
|---|---|---|---|---|---|
| WiFi ISR | Hardware interrupt | CPU0 | Highest | ~microseconds | No |
| WiFi task | Protocol stack | CPU0 | 23 | Variable | Yes |
| TCP/IP (lwIP) | Network stack | CPU0 | 18 | Variable | Yes |
| Camera DMA | Hardware interrupt | CPU0 | High | ~microseconds | No |
| detect_task: preprocess | Compute | CPU1 | 5 | ~few ms | Yes |
| detect_task: Invoke() | Compute-bound | CPU1 | 5 | ~190ms | NO |
| detect_task: HTTP POST | I/O-bound | CPU1 | 5 | ~50ms | Yes |
| FreeRTOS tick | Scheduler | Both | System | ~microseconds | No |
| IDLE0 | CPU0 idle | CPU0 | 0 | Variable | Yes |
| IDLE1 | CPU1 idle | CPU1 | 0 | Variable | Yes |

**Key finding:** Invoke() is the only task that must not be preempted mid-execution.
The esp-nn assembly kernel accumulates state across thousands of iterations.
Preemption mid-iteration does not corrupt state but resets the iteration pipeline,
causing near-zero net progress at 1ms tick rate.

---

## 3. Task dependency graph

```
Chain A — data pipeline (sequential, CPU1):
  Camera DMA capture
        ↓  [frame buffer full]
  Preprocessing (RGB565 → grayscale → 96×96)
        ↓  [tensor filled]
  Invoke()
        ↓  [result valid]
  HTTP POST ←──────────────────┐
        ↓                      │
  trigger received             │
                               │
Chain B — network (independent, CPU0):
  WiFi init → associate → IP assigned → maintain keepalive ──┘
```

Chains A and B are independent until HTTP POST.
No synchronization required except at the merge point (POST needs result + WiFi).

---

## 4. Stage boundary classification

| Stage | Entry condition | Exit condition | Safe to interrupt |
|---|---|---|---|
| 1. Capture | Frame buffer empty | DMA complete | YES — waiting on hardware |
| 2. Preprocess | Frame buffer full | Tensor filled | YES — simple arithmetic, resumable |
| 3. Invoke() | Tensor filled | Output tensor valid | NO — kernel state not resumable |
| 4. HTTP POST | Result valid + WiFi up | HTTP 200 | YES — blocking on network I/O |
| 5. WiFi keepalive | Connected | AP timeout | YES — periodic, tolerant |

**Design principle:** tick interruption is safe at stage boundaries, not mid-stage.
Tick period must be longer than the longest atomic (non-interruptible) stage.
Currently: Invoke() ≈ 190ms measured without interruption.

---

## 5. CPU affinity assignment — derived from dependency analysis

| Task | Assigned CPU | Rationale |
|---|---|---|
| Camera DMA ISR | CPU0 | DMA controller bound to CPU0 |
| WiFi ISR | CPU0 | WiFi registered on CPU0 (init core) |
| WiFi task | CPU0 | Follows ISR core |
| TCP/IP | CPU0 | Network stack coupled to WiFi |
| Preprocessing | CPU1 | Data pipeline, no CPU0 dependency |
| Invoke() | CPU1 | Must not share core with WiFi ISRs |
| HTTP POST | CPU1 | Follows inference result |

**Key insight:** WiFi ISRs register to the CPU that calls esp_wifi_init() — CPU0.
Moving the WiFi task to CPU1 (Kconfig) does NOT move ISRs.
Moving inference to CPU1 (xTaskCreatePinnedToCore) is the correct fix.
Cross-core ISRs (FreeRTOS scheduler ticks) still interrupt CPU1 — separate problem.

---

## 6. Tick rate derivation framework

```
tick_period must satisfy two constraints simultaneously:

Constraint 1 (inference):
  tick_period ≥ Invoke() duration
  → kernel completes within one tick window without interruption

Constraint 2 (WiFi):
  tick_period ≤ WiFi keepalive timeout / safety_margin
  → connection maintained between inference cycles

Measured values (to be confirmed empirically):
  Invoke() duration:        ~190ms (measured without interruption, no WiFi)
  WiFi keepalive timeout:   TBD — needs measurement (typically 5-10s for AP)
  HTTP POST latency:        TBD — needs measurement

Derived tick period:
  tick_period = Invoke_duration × 1.1 (10% margin) = ~210ms
  FREERTOS_HZ = 1000 / 210 ≈ 5

MVP first test: CONFIG_FREERTOS_HZ=5 (200ms tick)
```

---

## 7. Optimization framework — three layers

**Layer 1 — Task classification**
Classify every task: event-driven vs compute-bound vs I/O-bound.
Event-driven tasks: short, high-priority, must not be delayed.
Compute-bound tasks: long, can be scheduled around event-driven tasks.
I/O-bound tasks: spend most time waiting, low CPU pressure.

**Layer 2 — Stage boundary identification**
Map every logical stage with entry/exit conditions.
Identify which stages are atomic (non-interruptible).
Design tick period to be longer than the longest atomic stage.

**Layer 3 — CPU affinity assignment**
Group tasks by dependency chain, not by priority alone.
Chain A (data pipeline) → CPU1.
Chain B (network) → CPU0.
No synchronization overhead within a chain.
Only the merge point (POST) requires both chains to be ready.

---

## 8. Planned measurements for the paper

All measurements to be run as single-variable controlled experiments.

### 8.1 Invoke() duration vs tick rate
- Variable: CONFIG_FREERTOS_HZ ∈ {1000, 100, 50, 10, 5, 1}
- Fixed: WiFi connected, detect_task on CPU1, arena in SRAM
- Measure: timestamp delta between tensor fill and result valid
- Expected: Invoke() duration decreases as tick period increases
- Plot: tick_period (ms) vs Invoke() duration (ms)

### 8.2 WiFi keepalive tolerance
- Variable: artificial pause duration before each inference cycle
- Fixed: FREERTOS_HZ=5, standard AP
- Measure: maximum pause before AP drops connection
- Method: esp_wifi_stop() for increasing durations, check reconnect success
- Expected: AP tolerates 5-10s pause before dropping

### 8.3 HTTP POST latency
- Variable: none (characterization measurement)
- Fixed: WiFi connected, standard LAN endpoint
- Measure: time from trigger_send() to HTTP 200 response
- Expected: 10-50ms typical LAN latency

### 8.4 Optimal tick rate validation
- Prediction: FREERTOS_HZ=5 allows Invoke() completion without WiFi drops
- Test: run full pipeline at FREERTOS_HZ=5, measure:
  - Invoke() completion rate (should be 100%)
  - WiFi connection stability (should be continuous)
  - End-to-end cycle time (capture → trigger received)
- Compare measured values against derived formula predictions

---

## 9. Paper contribution statement

A generalizable framework for co-scheduling uninterruptible compute kernels
(TFLite Micro inference) with event-driven network stacks (WiFi + TCP/IP)
on dual-core RTOS (FreeRTOS on ESP32-S3).

Contributions:
1. Task dependency graph methodology for embedded AI + network applications
2. Stage boundary classification (interruptible vs atomic)
3. Tick rate derivation formula from measured stage durations
4. CPU affinity assignment rules derived from dependency chains
5. Empirical validation on real hardware (ESP32-S3, TFLite Micro, esp-nn)

This framework did not exist in published literature before this work.
All measurements are reproducible from the open-source repository.

---

## 10. MVP implementation note

For MVP portfolio demonstration:
  CONFIG_FREERTOS_HZ=5 (first test point — derived from formula)
  If Invoke() completes: MVP closed, trigger reaches cloud.
  If not: step to FREERTOS_HZ=1, then consider WiFi pause approach.

The WiFi pause approach (esp_wifi_stop() before Invoke(), restart after)
is architecturally clean and always works — held as fallback.
Measurements from both approaches feed the paper regardless.

---
*These notes are the re-entry point for the scheduling paper section.*
*Do not begin writing without running measurements 8.1 through 8.4.*
