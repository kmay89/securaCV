// Every knob is an environment variable; .env.example documents each one.
// Nothing here is a secret the code embeds — a blank value means "feature off".
import { join } from 'node:path';

const env = (k, d = '') => (process.env[k] ?? d).trim();

export function loadConfig(overrides = {}) {
  const dataDir = env('BACKOFFICE_DATA_DIR', './data');
  const cfg = {
    dataDir,
    dbPath: env('BACKOFFICE_DB', join(dataDir, 'backoffice.db')),
    port: Number(env('BACKOFFICE_PORT', '8787')),
    host: env('BACKOFFICE_HOST', '127.0.0.1'),
    baseUrl: env('BACKOFFICE_BASE_URL', 'http://127.0.0.1:8787').replace(/\/$/, ''),
    siteOrigin: env('SITE_ORIGIN', 'https://securacv.com'),          // the website, for CORS on /forms
    company: env('COMPANY_NAME', 'Errer Labs'),
    product: env('PRODUCT_NAME', 'SecuraCV'),
    supportAddr: env('SUPPORT_ADDR', ''),                           // the inbox the helpdesk reads + replies from
    mailFrom: env('MAIL_FROM', ''),                                 // "Errer Labs <hello@…>"
    smtpUrl: env('SMTP_URL', ''),                                   // smtp://user:pass@host:587 (STARTTLS) | smtps://…:465
    imapUrl: env('IMAP_URL', ''),                                   // imaps://user:pass@host:993
    stripeWebhookSecret: env('STRIPE_WEBHOOK_SECRET', ''),
    stripeApiKey: env('STRIPE_API_KEY', ''),                        // optional, read-only uses (payout list)
    githubToken: env('GITHUB_TOKEN', ''),
    githubRepo: env('GITHUB_REPO', ''),                             // exceptions open issues here when GITHUB_TOKEN is set — a PRIVATE repo: the bodies describe customers' orders
    trustedProxies: env('TRUSTED_PROXIES', 'loopback,private'),      // whose X-Forwarded-For to believe: loopback, private, exact addresses, v4 CIDRs
    storeJson: env('STORE_JSON', 'https://securacv.com/store.json'), // catalog: path or URL
    shippingJson: env('SHIPPING_JSON', ''),                           // boxes + parcels: path or URL; blank = next to store.json
    slaHours: Number(env('SUPPORT_SLA_HOURS', '24')),
    sessionHours: Number(env('SESSION_HOURS', '12')),
    backupKeep: Number(env('BACKUP_KEEP', '14')),
    backupS3Url: env('BACKUP_S3_URL', '').replace(/\/$/, ''),          // https://<endpoint>/<bucket>/<prefix> — any S3-compatible store (B2, R2, Tigris, S3)
    backupEncryptionKey: env('BACKUP_ENCRYPTION_KEY', ''),             // a passphrase; off-site copies are sealed (AES-256-GCM) under it
    backupS3Key: env('BACKUP_S3_KEY', ''), backupS3Secret: env('BACKUP_S3_SECRET', ''), backupS3Region: env('BACKUP_S3_REGION', 'us-east-1'),
    operatorEmail: env('OPERATOR_EMAIL', env('SUPPORT_ADDR', '')),
    tz: env('OPERATOR_TZ', 'UTC'),                                     // every time the admin shows, the digest hour, campaign scheduling
    shipFromState: env('SHIP_FROM_STATE', 'MA'),                        // the delivery estimate counts from here
    batchCloseDay: Number(env('BATCH_CLOSE_DAY', '0')),                 // the batch cadence the store promises: orders in by this weekday (0 = Sunday)…
    batchCloseHour: Number(env('BATCH_CLOSE_HOUR', '23')),              // …at this hour (operator time, 23 = 11 pm)…
    batchShipDay: Number(env('BATCH_SHIP_DAY', '2')),                   // …ship on this weekday (2 = Tuesday)
    taxMode: env('TAX_MODE', 'none'),                                   // none | stripe — the old spelling of the stripe_tax flag (flags.mjs); FLAG_STRIPE_TAX is the new one
    // FLAG_<NAME>=1|0 pins a feature flag from the environment (staging turns tomorrow's behavior on without touching
    // the database); flags.mjs declares the names, Settings shows which source answers
    flagOverrides: Object.fromEntries(Object.entries(process.env).filter(([k, v]) => k.startsWith('FLAG_') && String(v).trim() !== '').map(([k, v]) => [k.slice(5).toLowerCase(), /^(1|true|on|yes)$/i.test(String(v).trim())])),
    checkoutSuccessPath: env('CHECKOUT_SUCCESS_PATH', '/store/thanks'), // on the website, where Stripe sends a paid shopper
    uspsClientId: env('USPS_CLIENT_ID', ''), uspsClientSecret: env('USPS_CLIENT_SECRET', ''), uspsApiBase: env('USPS_API_BASE', 'https://apis.usps.com'),       // the morning digest goes here
    digestHour: Number(env('DIGEST_HOUR', '7')),                       // UTC hour the digest is sent
    secure: env('BACKOFFICE_BASE_URL', '').startsWith('https://'),
    env: env('BACKOFFICE_ENV', ''),                                 // production | staging | demo | development — the label every page, /healthz and the doctor carry; rules in env.mjs
    stagingAllowMail: env('STAGING_ALLOW_MAIL', '') === '1',          // staging mutes SMTP/IMAP unless the operator points it at a throwaway relay and says so
    offsiteWrites: true,                                               // staging turns this off: it reads production's off-site copies and never adds its own
    buildSha: env('BUILD_SHA', 'dev'),                                 // stamped into the image by CI (backoffice-image.yml); 'dev' on a laptop
    buildDate: env('BUILD_DATE', ''),
    demo: env('BACKOFFICE_DEMO', '') === '1',                       // also set by a database the demo seeded
    demoResetMinutes: Number(env('BACKOFFICE_DEMO_RESET_MINUTES', '0')), // a public demo wipes and reseeds itself on this cadence; 0 = never
    ...overrides,
  };
  return cfg;
}
