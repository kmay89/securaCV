// canary-local/assets/enclosure-sets.js — which device page an enclosure set
// belongs on, read from devices/enclosures.json.
//
// The attribution is the device manifests' (devices/<slug>/device.json
// cad.enclosure_sets), inverted by tools/gen_enclosures.py: `device` is the
// first manifest that claims the set — homed on its family's device when the
// family is itself a manifest (the DevKit's case is on the canary-vision
// page) — and `devices` lists every claimant whenever `device` alone does not
// (the 7" case serves the Dash 7 and the Nightstand 7). A set with no device
// is universal. gen_enclosures.py's serves() is the generator-side twin (the
// workshop's packages); keep the two in step.

export function setServes(set, deviceId) {
  if (!set || !deviceId) return false;
  return set.device === deviceId
    || (Array.isArray(set.devices) && set.devices.includes(deviceId));
}
