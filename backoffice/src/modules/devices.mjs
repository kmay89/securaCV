// The serial registry: every pre-flashed board, what it is, what firmware
// it carries, which order took it. Fed by the batch jig's manifest
// (firmware/provisioning/create_manifest.py) or by hand at the bench. A
// warranty claim resolves against this, and a recall would start here.
import { nowIso } from '../db.mjs';

export const normSerial = (s) => String(s || '').trim().toUpperCase();
export const normMac = (s) => s ? String(s).trim().toUpperCase().replace(/[^0-9A-F]/g, '').replace(/(..)(?=.)/g, '$1:') : null;

export function registerDevice(db, d) {
  const serial = normSerial(d.serial); if (!serial) throw new Error('serial required');
  db.run(`INSERT INTO devices (serial, mac, board, product_sku, fw_version, fw_sha256, flashed_at, batch, notes)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(serial) DO UPDATE SET mac = COALESCE(excluded.mac, devices.mac), board = COALESCE(excluded.board, devices.board), product_sku = COALESCE(excluded.product_sku, devices.product_sku),
      fw_version = COALESCE(excluded.fw_version, devices.fw_version), fw_sha256 = COALESCE(excluded.fw_sha256, devices.fw_sha256), flashed_at = COALESCE(excluded.flashed_at, devices.flashed_at), batch = COALESCE(excluded.batch, devices.batch)`,
    serial, normMac(d.mac), d.board || null, d.product_sku || null, d.fw_version || null, d.fw_sha256 || null, d.flashed_at || nowIso(), d.batch || null, d.notes || null);
  return deviceBySerial(db, serial);
}
export const deviceBySerial = (db, s) => db.get('SELECT d.*, o.number AS order_number FROM devices d LEFT JOIN orders o ON o.id = d.order_id WHERE d.serial = ?', normSerial(s));
export const listDevices = (db, { q = '', status = '', limit = 500 } = {}) => db.all(
  `SELECT d.*, o.number AS order_number FROM devices d LEFT JOIN orders o ON o.id = d.order_id
   WHERE (? = '' OR d.status = ?) AND (? = '' OR d.serial LIKE ? OR d.mac LIKE ? OR d.batch LIKE ? OR o.number LIKE ?) ORDER BY d.id DESC LIMIT ?`,
  status, status, q, `%${q}%`, `%${q}%`, `%${q}%`, `%${q}%`, limit);

// The packing-slip QA line: scan the flashed board's serial against THAT
// order. A serial already assigned elsewhere is a stop, not a warning.
export function assignToOrder(db, serial, orderId, { actor = 'web' } = {}) {
  const d = deviceBySerial(db, serial); if (!d) throw new Error(`unknown serial ${serial}`);
  if (d.order_id && d.order_id !== orderId) throw new Error(`${d.serial} is already on order ${d.order_number}`);
  db.run('UPDATE devices SET order_id = ? WHERE id = ?', orderId, d.id);
  db.audit(actor, 'device.assign', 'device', d.serial, { order_id: orderId });
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
