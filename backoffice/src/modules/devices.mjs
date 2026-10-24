// The serial registry: every pre-flashed board, what it is, what firmware
// it carries, which order took it. Fed by the batch jig's manifest
// (firmware/provisioning/create_manifest.py) or by hand at the bench. A
// warranty claim resolves against this, and a recall would start here.
import { nowIso } from '../db.mjs';
import { addEvent, orderById, boardsNeeded } from './orders.mjs';

export const normSerial = (s) => String(s || '').trim().toUpperCase();
export const normMac = (s) => s ? String(s).trim().toUpperCase().replace(/[^0-9A-F]/g, '').replace(/(..)(?=.)/g, '$1:') : null;

export function registerDevice(db, d) {
  const serial = normSerial(d.serial); if (!serial) throw new Error('serial required');
  const mac = normMac(d.mac);
  if (mac) { const other = db.get('SELECT serial FROM devices WHERE mac = ? AND serial != ?', mac, serial); if (other) throw new Error(`MAC ${mac} is already registered to serial ${other.serial} — two boards cannot share one MAC`); }
  db.run(`INSERT INTO devices (serial, mac, board, product_sku, fw_version, fw_sha256, flashed_at, batch, notes)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(serial) DO UPDATE SET mac = COALESCE(excluded.mac, devices.mac), board = COALESCE(excluded.board, devices.board), product_sku = COALESCE(excluded.product_sku, devices.product_sku),
      fw_version = COALESCE(excluded.fw_version, devices.fw_version), fw_sha256 = COALESCE(excluded.fw_sha256, devices.fw_sha256), flashed_at = COALESCE(excluded.flashed_at, devices.flashed_at), batch = COALESCE(excluded.batch, devices.batch)`,
    serial, mac, d.board || null, d.product_sku || null, d.fw_version || null, d.fw_sha256 || null, d.flashed_at || nowIso(), d.batch || null, d.notes || null);
  return deviceBySerial(db, serial);
}
export const deviceBySerial = (db, s) => db.get('SELECT d.*, o.number AS order_number FROM devices d LEFT JOIN orders o ON o.id = d.order_id WHERE d.serial = ?', normSerial(s));
export const listDevices = (db, { q = '', status = '', limit = 500 } = {}) => db.all(
  `SELECT d.*, o.number AS order_number FROM devices d LEFT JOIN orders o ON o.id = d.order_id
   WHERE (? = '' OR d.status = ?) AND (? = '' OR d.serial LIKE ? OR d.mac LIKE ? OR d.batch LIKE ? OR o.number LIKE ?) ORDER BY d.id DESC LIMIT ?`,
  status, status, q, `%${q}%`, `%${q}%`, `%${q}%`, `%${q}%`, limit);

// The packing-slip QA line: scan the flashed board's serial against THAT
// order. A serial already assigned elsewhere is a stop, not a warning.
// `force` is for seeding history (a demo assigns boards to orders that shipped long ago); the bench never passes it.
export function assignToOrder(db, serial, orderId, { actor = 'web', force = false } = {}) {
  const d = deviceBySerial(db, serial); if (!d) throw new Error(`unknown serial ${serial}`);
  if (d.order_id && d.order_id !== orderId) throw new Error(`${d.serial} is already on order ${d.order_number}`);
  if (!force && ['shipped', 'scrapped'].includes(d.status)) throw new Error(`${d.serial} is ${d.status} — it cannot go into another box`);
  const o = db.get('SELECT number, status FROM orders WHERE id = ?', orderId); if (!o) throw new Error('no such order');
  if (!force && !['paid', 'packed'].includes(o.status)) throw new Error(`${o.number} is ${o.status} — boards go into orders that have not shipped`);
  if (!force && d.order_id !== orderId) {
    // The board must be one the order asks for, and one the order still has room for: a Vision
    // board in a WAP box, or a third board in a two-kit order, is the mistake this bench exists to catch.
    const full = orderById(db, orderId);
    if (d.product_sku && full.items.length && !full.items.some(i => i.sku === d.product_sku)) throw new Error(`${d.serial} is flashed for ${d.product_sku} — ${o.number} has no ${d.product_sku} line (it has ${full.items.map(i => `${i.qty}× ${i.sku}`).join(', ')})`);
    const need = boardsNeeded(db, full); const have = db.all('SELECT serial FROM devices WHERE order_id = ?', orderId);
    if (need.needed && have.length >= need.needed) throw new Error(`${o.number} needs ${need.needed} board${need.needed === 1 ? '' : 's'} and already has ${have.map(h => h.serial).join(', ')} — take one out first`);
  }
  db.run('UPDATE devices SET order_id = ? WHERE id = ?', orderId, d.id);
  addEvent(db, orderId, { kind: 'device', actor, text: `Board ${d.serial} (${d.board || 'board'}, firmware ${d.fw_version || '?'}) scanned into this box` });
  db.audit(actor, 'device.assign', 'device', d.serial, { order_id: orderId });
  return deviceBySerial(db, serial);
}

// The board comes back out of the box (wrong one scanned, or the order is repacked without it).
// Only from an order that has not shipped: a shipped board's history is a record, not a draft.
export function unassign(db, serial, { actor = 'web' } = {}) {
  const d = deviceBySerial(db, serial); if (!d) throw new Error(`unknown serial ${serial}`);
  if (!d.order_id) throw new Error(`${d.serial} is not in any box`);
  const o = db.get('SELECT number, status FROM orders WHERE id = ?', d.order_id);
  if (!['paid', 'packed'].includes(o.status) || d.status === 'shipped') throw new Error(`${d.serial} went out with ${o.number} — it stays on the record`);
  db.run('UPDATE devices SET order_id = NULL WHERE id = ?', d.id);
  addEvent(db, d.order_id, { kind: 'device', actor, text: `Board ${d.serial} taken back out of this box` });
  db.audit(actor, 'device.unassign', 'device', d.serial, { order_id: d.order_id });
  return deviceBySerial(db, serial);
}

// create_manifest.py's JSON: { version, devices: [{ mac, fingerprint, firmware_version, … }] }
export function importManifest(db, manifest, { batch = null, product_sku = null, board = null } = {}) {
  let n = 0;
  for (const d of manifest.devices || []) {
    registerDevice(db, { serial: d.serial || d.fingerprint || d.mac, mac: d.mac, fw_version: d.firmware_version, fw_sha256: d.firmware_sha256 || d.fw_sha256, flashed_at: d.flashed_at || d.added_at, batch: batch || d.batch, product_sku: product_sku || d.product, board: board || d.board });
    n++;
  }
  return n;
}
export const deviceStats = (db) => Object.fromEntries(db.all('SELECT status, COUNT(*) AS n FROM devices GROUP BY status').map(r => [r.status, r.n]));
