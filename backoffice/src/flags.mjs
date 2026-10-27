// Feature flags: a switch that exists in the code before it is on for everyone. Each is declared here,
// once, with what it does and who owns it; nowhere else may invent one (an unknown name throws, so a
// typo cannot read as "off"). Two kinds:
//   rollout  a change shipped dark and turned on when ready; it carries a remove_by date, and the doctor
//            names it once that passes — a rollout flag is temporary by definition, and the code behind
//            the old branch is deleted when it is on for good;
//   switch   an operator's standing choice (Stripe Tax on or off); it has no date and stays.
// Where the answer comes from, in order: the environment (FLAG_<NAME>=1|0 — staging turns tomorrow's
// behavior on without touching the database), then Settings (toggled on the Settings page or the API,
// audited), then the default here. The source is shown beside each flag so nobody wonders why a toggle
// does nothing: an environment override wins and says so.
import { nowIso } from './db.mjs';

export const FLAGS = {
  stripe_tax: { kind: 'switch', default: false, owner: 'owner', since: '2026-10-26', remove_by: null,
    description: 'Stripe Tax computes sales tax at hosted checkout by the shipping address (automatic_tax, a per-transaction fee). Off collects none, which is right only while you owe sales tax nowhere.' },
};

const KEY = (name) => `flag:${name}`;
const known = (name, registry) => { if (!Object.hasOwn(registry, name)) throw new Error(`unknown feature flag "${name}" — flags are declared in src/flags.mjs (${Object.keys(registry).join(', ') || 'none'})`); return registry[name]; };
const truthy = (v) => /^(1|true|on|yes)$/i.test(String(v ?? '').trim());

// The environment's say on a flag, or undefined: FLAG_<NAME> in cfg.flagOverrides (config.mjs collects
// them), and the legacy TAX_MODE=stripe, which is the same switch spelled the old way.
function envOverride(cfg, name) {
  const o = cfg?.flagOverrides?.[name]; if (o !== undefined) return !!o;
  if (name === 'stripe_tax' && cfg?.taxMode === 'stripe') return true;
  return undefined;
}

// Is it on? The one function the code calls at the branch.
export function isOn(db, cfg, name, { registry = FLAGS } = {}) {
  const f = known(name, registry);
  const env = envOverride(cfg, name); if (env !== undefined) return env;
  const set = db.setting(KEY(name), null); if (set !== null) return truthy(set);
  return !!f.default;
}

// Every flag with its state and where the state comes from — the Settings page, the API, the doctor.
export function listFlags(db, cfg, { registry = FLAGS, now = new Date() } = {}) {
  const today = now.toISOString().slice(0, 10);
  return Object.entries(registry).map(([name, f]) => {
    const env = envOverride(cfg, name); const set = db.setting(KEY(name), null);
    const on = env !== undefined ? env : set !== null ? truthy(set) : !!f.default;
    const source = env !== undefined ? 'environment' : set !== null ? 'setting' : 'default';
    const overdue = f.kind === 'rollout' && !!f.remove_by && f.remove_by < today;
    return { name, on, source, kind: f.kind, default: !!f.default, description: f.description, owner: f.owner, since: f.since, remove_by: f.remove_by, overdue, set_at: set !== null ? db.setting(`${KEY(name)}:at`, null) : null };
  });
}

// Flip a flag in Settings. Refused when the environment overrides it (the toggle would be a lie) and for an
// unknown name. Every flip is an audit row under the person's name.
export function setFlag(db, cfg, name, on, { actor = 'web', registry = FLAGS } = {}) {
  known(name, registry);
  if (envOverride(cfg, name) !== undefined) throw new Error(`${name} is set by the environment (FLAG_${name.toUpperCase()}${name === 'stripe_tax' ? ' or TAX_MODE' : ''}) — change it there`);
  const was = isOn(db, cfg, name, { registry });
  db.setSetting(KEY(name), on ? '1' : '0'); db.setSetting(`${KEY(name)}:at`, nowIso());
  db.audit(actor, 'flag.set', 'flag', name, { on: !!on, was });
  return { name, on: !!on, was };
}

// Back to the default: the setting row goes, the environment (if any) or the registry answers again.
export function clearFlag(db, cfg, name, { actor = 'web', registry = FLAGS } = {}) {
  known(name, registry);
  const was = isOn(db, cfg, name, { registry });
  db.run('DELETE FROM settings WHERE key IN (?, ?)', KEY(name), `${KEY(name)}:at`);
  db.audit(actor, 'flag.clear', 'flag', name, { was, now: isOn(db, cfg, name, { registry }) });
  return { name, on: isOn(db, cfg, name, { registry }), was };
}
