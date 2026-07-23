// Thin JavaScript boundary around canary-vision-core.js.
//
// The module behind this class is not a behavior reimplementation. It is built
// from firmware/projects/canary-vision: detection_pipeline.h,
// detect_config.cpp, presence_fsm.cpp and voxel_tracker.cpp. JavaScript only
// supplies the SSCMA boxes that real camera silicon would have supplied.

export const VISION_CORE_SCHEMA = "securacv.canary-vision.core/v1";

export async function createVisionFirmwareCore(factory) {
  if (typeof factory !== "function") {
    throw new Error("the committed Canary Vision firmware core is missing");
  }
  const module = await factory();
  const contractJson = module.cwrap("vision_emu_contract_json", "string", []);
  const resetCore = module.cwrap("vision_emu_reset", null, []);
  const setConfig = module.cwrap("vision_emu_set_config", null,
    ["number", "number", "number", "number"]);
  const beginFrame = module.cwrap("vision_emu_begin_frame", null, []);
  const pushBox = module.cwrap("vision_emu_push_box", "number",
    ["number", "number", "number", "number", "number", "number"]);
  const tickJson = module.cwrap("vision_emu_tick_json", "string", ["number"]);

  const readContract = () => JSON.parse(contractJson());
  const initial = readContract();
  if (initial.schema !== VISION_CORE_SCHEMA) {
    throw new Error(`unsupported Vision firmware core: ${initial.schema || "no schema"}`);
  }

  const core = {
    module,
    get contract() { return readContract(); },
    reset() { resetCore(); },
    configure(cfg) {
      setConfig(cfg.person_target, cfg.score_min,
        cfg.lost_timeout_ms, cfg.dwell_start_ms);
      const actual = readContract();
      // Return the firmware-clamped values. Sliders and MQTT examples should
      // show what the device accepted, not merely what JavaScript requested.
      return {
        person_target: actual.person_target,
        score_min: actual.score_min,
        lost_timeout_ms: actual.lost_timeout_ms,
        dwell_start_ms: actual.dwell_start_ms,
      };
    },
    tick(nowMs, boxes) {
      beginFrame();
      for (const box of boxes || []) {
        if (!pushBox(box.x, box.y, box.w, box.h, box.score, box.target)) {
          throw new Error("Vision firmware core refused a frame with more than 32 boxes");
        }
      }
      return JSON.parse(tickJson(nowMs >>> 0));
    },
    assertGeneratedData(data) {
      const c = readContract();
      const mismatches = [];
      const same = (label, actual, generated) => {
        if (actual !== generated) mismatches.push(`${label}: wasm=${actual}, json=${generated}`);
      };
      same("firmware", c.firmware, data.device.fw_version);
      same("person target", c.person_target, data.detect.person_target);
      same("score", c.score_min, data.detect.score_min);
      same("lost timeout", c.lost_timeout_ms, data.detect.lost_timeout_ms);
      same("dwell start", c.dwell_start_ms, data.detect.dwell_start_ms);
      same("frame width", c.frame_w, data.detect.frame.w);
      same("frame height", c.frame_h, data.detect.frame.h);
      same("voxel rows", c.voxel_rows, data.detect.voxel.rows);
      same("voxel cols", c.voxel_cols, data.detect.voxel.cols);
      same("invoke period", c.invoke_period_ms, data.detect.invoke_period_ms);
      const bounds = data.detect.bounds;
      same("score lower bound", c.score_min_lo, bounds.score[0]);
      same("score upper bound", c.score_min_hi, bounds.score[1]);
      same("lost lower bound", c.lost_ms_lo, bounds.lost_ms[0]);
      same("lost upper bound", c.lost_ms_hi, bounds.lost_ms[1]);
      same("dwell lower bound", c.dwell_ms_lo, bounds.dwell_ms[0]);
      same("dwell upper bound", c.dwell_ms_hi, bounds.dwell_ms[1]);
      if (mismatches.length) {
        throw new Error("Vision generated data is stale against its firmware core: " + mismatches.join("; "));
      }
    },
  };
  core.reset();
  return core;
}
