// canary-local/assets/enclosure-sets.js — which device page an enclosure set
// belongs on, read from devices/enclosures.json.
//
// The attribution is the device manifests' (devices/<slug>/device.json
// cad.enclosure_sets), inverted by tools/gen_enclosures.py: `device` is the
// first manifest that claims the set, homed on the Lab card that presents it
// — the card its `lab.card` names (the C3 case is on canary-nightlight, its
// manifest canary-display-nightlight-c3), else its family's device when the
// family is itself a manifest (the DevKit's case is on the canary-vision
// page), else its own slug — and `devices` lists every claimant whenever
// `device` alone does not (the 7" case serves the Dash 7 and the Nightstand
// 7; the C3 case also names its manifest's slug, which the flasher looks it
// up by). A set with no device is universal. gen_enclosures.py's home() makes
// that homing; its serves() is the generator-side twin of setServes() below
// (the workshop's packages) — keep the two in step.

export function setServes(set, deviceId) {
  if (!set || !deviceId) return false;
  return set.device === deviceId
    || (Array.isArray(set.devices) && set.devices.includes(deviceId));
}
