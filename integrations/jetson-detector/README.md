# securaCV detector node — NVIDIA Jetson Orin Nano

Turn a Jetson Orin Nano into a **GPU camera-detection box** for your securaCV
hub, with one `docker compose up`. The Jetson runs Frigate with TensorRT
(real-time object detection, **no Coral needed**) and hardware video decode; it
publishes detections to your Pi hub over MQTT, where securaCV turns them into
signed witness claims.

```
  cameras ──RTSP──▶  Jetson Orin  ──MQTT──▶  Pi hub (HAOS + securaCV)
                     Frigate + go2rtc         Mosquitto → witness claims
                     (the eyes)               (the brain)
```

**Why a Jetson:** the Pi can run Frigate on its CPU (works, but slow) or with a
Coral USB TPU. An Orin Nano does real-time detection on its GPU out of the box —
it's the smooth path. It is **not** a hub replacement (Home Assistant OS doesn't
run on Jetson); it's a dedicated detector that feeds the Pi hub.

---

## Prerequisites

- **Jetson Orin Nano** on **JetPack 5 or 6** (`cat /etc/nv_tegra_release` to check).
- **Docker** + the **NVIDIA container runtime** — both ship with JetPack. Verify:
  ```sh
  docker info | grep -i runtime      # should list `nvidia`
  ```
- Your **hub reachable on the LAN**, with the **Mosquitto** add-on running and a
  login for the detector (in HA: Settings → People → add a user `frigate`; the
  Mosquitto add-on authenticates HA users).

## Setup (a few minutes)

1. Copy this folder to the Jetson (or clone the repo and `cd integrations/jetson-detector`).
2. Point it at your hub:
   ```sh
   cp .env.example .env
   nano .env          # FRIGATE_MQTT_HOST = your Pi (homeassistant.local or IP), + user/pass
   ```
3. Pick the image for your JetPack in `docker-compose.yml` — `stable-tensorrt-jp6`
   (JetPack 6) or `stable-tensorrt-jp5` (JetPack 5).
4. Start it:
   ```sh
   docker compose up -d
   docker compose logs -f          # watch the first-boot TensorRT model compile
   ```
   **First start compiles the detector model** (`yolov7-320`) for your GPU — a
   few minutes, once. After that, restarts are instant.
5. Open the Frigate UI at **`http://<jetson-ip>:8971`** — it requires a login
   (Frigate 0.14+ authenticates by default); the first-run **admin password
   prints in the logs** (`docker compose logs | grep -i password`). Empty is
   expected until you add a camera. (Don't publish port 5000 — that's Frigate's
   *unauthenticated* internal API.)

## Add a camera

Edit `config/config.yml` → the `cameras:` block: put your camera's RTSP URL in,
set `enabled: true`, and `docker compose restart`. That's the only per-user step
— everything else is pre-wired.

## Verify it reaches the hub

- Frigate UI shows the camera with live detection boxes.
- On the hub, securaCV's `frigate_bridge` already subscribes to `frigate/events`
  → new signed claims appear in the SecuraCV panel when it detects a person/car.
- (Optional) In Home Assistant, add the **Frigate** integration pointed at
  `http://<jetson-ip>:8971` (with the admin login) to see the Jetson's cameras
  and clips inside HA.

---

## Notes & honesty

- **This is a validated-by-you scaffold, not a tested build.** It follows
  Frigate's official Jetson guidance, but the exact TensorRT model name / env and
  the `hwaccel` preset can shift between Frigate and JetPack versions. If the
  first-boot model compile errors, check Frigate's current Jetson docs for the
  model path/env and adjust `YOLO_MODELS` (compose) + `model.path` (config) to
  match — then tell me what your version wanted and I'll fold it in.
- **The original Jetson Nano (2019) is a different, harder story** — it's stuck
  on JetPack 4.6 / Ubuntu 18.04 (EOL), which modern Frigate-TensorRT images don't
  target. This stack is for the **Orin** Nano.
- **Recordings are off by default** (`record.enabled: false`) — turn them on only
  if the Jetson has an NVMe/SSD; snapshots give you review clips cheaply.
- **Same MQTT contract as the Pi-side pre-bake** (`homeassistant/frigate/config.yaml`),
  so a hub happily takes detections from a Pi-with-Coral, a Jetson, or both.
