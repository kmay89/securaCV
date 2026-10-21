import { openDb } from '../src/db.mjs';
import { loadConfig } from '../src/config.mjs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { normalize } from '../src/modules/catalog.mjs';
import { readFileSync } from 'node:fs';

export const here = dirname(fileURLToPath(import.meta.url));
export const fixture = (n) => join(here, 'fixtures', n);
export const testDb = () => openDb(':memory:');
export const testCfg = (o = {}) => loadConfig({ port: 0, baseUrl: 'http://127.0.0.1:0', siteOrigin: 'https://example.test', storeJson: fixture('store.json'), mailFrom: 'Errer Labs <hello@errer.test>', company: 'Errer Labs', product: 'SecuraCV', supportAddr: 'hello@errer.test', githubToken: '', smtpUrl: '', imapUrl: '', stripeWebhookSecret: 'whsec_test', dataDir: '/tmp/backoffice-test', ...o });
export const catalog = () => normalize(JSON.parse(readFileSync(fixture('store.json'), 'utf8')));
export const sampleOrder = (over = {}) => ({ external_id: 'cs_test_1', source: 'stripe', email: 'ada@example.test', name: 'Ada Lovelace', currency: 'usd', subtotal_cents: 6900, shipping_cents: 800, tax_cents: 0, total_cents: 7700, ship: { line1: '1 Analytical Way', city: 'Boston', state: 'ma', zip: '02134', country: 'US' }, items: [{ sku: 'wap-kit', name: 'Canary WAP Kit', qty: 1, unit_cents: 6900 }], paid_at: '2026-10-19T12:00:00.000Z', ...over });
