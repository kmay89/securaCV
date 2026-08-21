'use strict';

/* ========================================================================
   Canary Vision — Device-Hosted Companion App
   Vanilla JS, no frameworks, no build step.
   ======================================================================== */

// --------------- Storage ---------------
// Security: Tokens are held in sessionStorage (cleared on tab close).
// Only non-sensitive device metadata is persisted to localStorage.
// This prevents XSS or malicious extensions from exfiltrating tokens
// via localStorage.getItem(). sessionStorage survives page refreshes
// but is cleared on tab close, which is acceptable because the
// provisioning receipt flow supports re-entry.

var SESSION_TOKEN_PREFIX = 'canary_token:';
var SESSION_ISSUED_PREFIX = 'canary_issued:';

// Default 8 hours, configurable via data attribute on <script> tag
var MAX_SESSION_DURATION_MS = (function () {
  try {
    var scripts = document.querySelectorAll('script[data-session-ttl]');
    if (scripts.length > 0) {
      var hours = parseFloat(scripts[0].getAttribute('data-session-ttl'));
      if (!isNaN(hours) && hours > 0) return hours * 3600 * 1000;
    }
  } catch (e) { /* ignore */ }
  return 8 * 3600 * 1000; // 8 hours default
})();

var SESSION_WARNING_MS = 5 * 60 * 1000; // 5 minutes before expiry

var CanarySession = {
  _warningTimers: {},
  _expiryTimers: {},
  _warningShown: {},

  isExpired: function (id) {
    try {
      var issuedStr = sessionStorage.getItem(SESSION_ISSUED_PREFIX + id);
      if (!issuedStr) return false; // No timestamp means legacy token — don't expire
      var elapsed = Date.now() - parseInt(issuedStr, 10);
      return elapsed > MAX_SESSION_DURATION_MS;
    } catch (e) { return false; }
  },

  timeRemaining: function (id) {
    try {
      var issuedStr = sessionStorage.getItem(SESSION_ISSUED_PREFIX + id);
      if (!issuedStr) return Infinity;
      var elapsed = Date.now() - parseInt(issuedStr, 10);
      return Math.max(0, MAX_SESSION_DURATION_MS - elapsed);
    } catch (e) { return Infinity; }
  },

  clearSession: function (id) {
    try {
      sessionStorage.removeItem(SESSION_TOKEN_PREFIX + id);
      sessionStorage.removeItem(SESSION_ISSUED_PREFIX + id);
    } catch (e) { /* ignore */ }
    if (CanarySession._warningTimers[id]) {
      clearTimeout(CanarySession._warningTimers[id]);
      delete CanarySession._warningTimers[id];
    }
    if (CanarySession._expiryTimers[id]) {
      clearTimeout(CanarySession._expiryTimers[id]);
      delete CanarySession._expiryTimers[id];
    }
    delete CanarySession._warningShown[id];
  },

  scheduleExpiry: function (id) {
    var remaining = CanarySession.timeRemaining(id);
    if (remaining === Infinity) return;

    // Schedule warning 5 minutes before expiry
    var warningIn = remaining - SESSION_WARNING_MS;
    if (warningIn > 0 && !CanarySession._warningShown[id]) {
      CanarySession._warningTimers[id] = setTimeout(function () {
        CanarySession._showWarning(id);
      }, warningIn);
    }

    // Schedule expiry
    if (remaining > 0) {
      CanarySession._expiryTimers[id] = setTimeout(function () {
        CanarySession._handleExpiry(id);
      }, remaining);
    }
  },

  _showWarning: function (id) {
    CanarySession._warningShown[id] = true;
    var banner = document.getElementById('session-warning');
    if (banner) banner.parentNode.removeChild(banner);

    var device = CanaryStorage.getDevice(id);
    var name = device ? (device.name || id) : id;

    var warningEl = el('div', {
      id: 'session-warning',
      className: 'alert alert-warning session-expiry-warning',
    }, [
      'Session for ' + name + ' expires in ~5 minutes. ',
      el('button', {
        className: 'btn btn-secondary btn-sm',
        textContent: 'Dismiss',
        onClick: function () { warningEl.parentNode.removeChild(warningEl); },
      }),
    ]);

    var app = document.getElementById('app');
    if (app && app.firstChild) {
      app.insertBefore(warningEl, app.firstChild);
    }
  },

  _handleExpiry: function (id) {
    CanarySession.clearSession(id);

    var device = CanaryStorage.getDevice(id);
    var name = device ? (device.name || id) : id;

    // Check if the user is currently viewing the expired device's page
    var hash = window.location.hash || '';
    var onExpiredDevice = hash.indexOf('#/device/' + id) === 0;

    var app = document.getElementById('app');
    if (!app) return;

    if (onExpiredDevice) {
      // User is on this device's page — replace UI with expiry message
      while (app.firstChild) app.removeChild(app.firstChild);
      app.appendChild(el('div', { className: 'content' }, [
        el('div', { className: 'alert alert-error', textContent: 'Session for ' + name + ' expired. Please re-authenticate.' }),
        el('button', {
          className: 'btn btn-primary mt-12',
          textContent: 'Go to Devices',
          onClick: function () { Router.navigate('#/canaries'); },
        }),
      ]));
    } else {
      // User is on a different page — show a non-intrusive banner
      var existing = document.getElementById('session-expired-' + id);
      if (existing) existing.parentNode.removeChild(existing);

      var banner = el('div', {
        id: 'session-expired-' + id,
        className: 'alert alert-warning session-expiry-warning',
      }, [
        'Session for ' + name + ' has expired. ',
        el('button', {
          className: 'btn btn-secondary btn-sm',
          textContent: 'Dismiss',
          onClick: function () { banner.parentNode.removeChild(banner); },
        }),
      ]);

      if (app.firstChild) {
        app.insertBefore(banner, app.firstChild);
      } else {
        app.appendChild(banner);
      }
    }
  },
};

var CanaryStorage = {
  KEY: 'canary_devices',

  _getToken: function (id) {
    try {
      // Check expiration before returning token
      if (CanarySession.isExpired(id)) {
        CanarySession.clearSession(id);
        return '';
      }
      return sessionStorage.getItem(SESSION_TOKEN_PREFIX + id) || '';
    } catch (e) { return ''; }
  },

  _setToken: function (id, token) {
    try {
      sessionStorage.setItem(SESSION_TOKEN_PREFIX + id, token);
      // Store issued_at timestamp alongside the token
      sessionStorage.setItem(SESSION_ISSUED_PREFIX + id, String(Date.now()));
      // Schedule expiry timers
      CanarySession.scheduleExpiry(id);
    } catch (e) { /* sessionStorage unavailable — tokens will not survive refresh */ }
  },

  _removeToken: function (id) {
    CanarySession.clearSession(id);
  },

  getDevices: function () {
    try {
      var raw = localStorage.getItem(CanaryStorage.KEY);
      var devices = raw ? JSON.parse(raw) : [];
      // Rehydrate tokens from sessionStorage
      for (var i = 0; i < devices.length; i++) {
        var tok = CanaryStorage._getToken(devices[i].id);
        if (tok) devices[i].token = tok;
      }
      return devices;
    } catch (e) {
      return [];
    }
  },

  saveDevices: function (devices) {
    // Tokens move to sessionStorage; everything else is rebuilt from an
    // explicit allowlist so a credential can never reach localStorage —
    // not even via a future field. Extend this list when adding new
    // persisted device metadata.
    var sanitized = devices.map(function (d) {
      if (d.token) {
        CanaryStorage._setToken(d.id, d.token);
      }
      return {
        id: d.id,
        name: d.name,
        base_url: d.base_url,
        room: d.room,
        device_type: d.device_type,
        last_info: d.last_info,
        added_at: d.added_at,
      };
    });
    localStorage.setItem(CanaryStorage.KEY, JSON.stringify(sanitized));
  },

  getDevice: function (id) {
    var devices = CanaryStorage.getDevices();
    for (var i = 0; i < devices.length; i++) {
      if (devices[i].id === id) return devices[i];
    }
    return null;
  },

  addDevice: function (device) {
    // Store token in sessionStorage
    if (device.token) {
      CanaryStorage._setToken(device.id, device.token);
    }
    var devices = CanaryStorage.getDevices();
    devices.push(device);
    CanaryStorage.saveDevices(devices);
  },

  updateDevice: function (id, updates) {
    // Capture token update in sessionStorage
    if (updates.token) {
      CanaryStorage._setToken(id, updates.token);
    }
    var devices = CanaryStorage.getDevices();
    for (var i = 0; i < devices.length; i++) {
      if (devices[i].id === id) {
        Object.assign(devices[i], updates);
        break;
      }
    }
    CanaryStorage.saveDevices(devices);
  },

  removeDevice: function (id) {
    CanaryStorage._removeToken(id);
    var devices = CanaryStorage.getDevices();
    CanaryStorage.saveDevices(devices.filter(function (d) { return d.id !== id; }));
  },

  // Rooms are a client-side organizational label only — devices know
  // nothing about them (no fleet-wide state on the device, per D6).
  setDeviceRoom: function (id, room) {
    CanaryStorage.updateDevice(id, { room: room || '' });
  }
};

// --------------- URL Validation ---------------

function isPrivateUrl(url) {
  try {
    var parsed = new URL(url);
    // Device traffic is HTTP-only (mDNS has no certificates), and the
    // device-served CSP's connect-src lists only http:// LAN sources — a
    // URL with any other scheme could never be fetched, so reject it here
    // with a clear error instead of a silent CSP block.
    if (parsed.protocol !== 'http:') return false;
    var hostname = parsed.hostname;
    if (hostname.endsWith('.local')) return true;
    if (hostname.match(/^192\.168\./)) return true;
    if (hostname.match(/^10\./)) return true;
    if (hostname.match(/^172\.(1[6-9]|2[0-9]|3[01])\./)) return true;
    if (hostname.match(/^169\.254\./)) return true; // IPv4 link-local
    if (hostname === 'localhost' || hostname === '127.0.0.1') return true;
    // IPv6 (URL.hostname keeps the brackets): loopback, link-local
    // (fe80::/10), and Unique Local Addresses (fc00::/7).
    if (hostname === '[::1]') return true;
    if (hostname.match(/^\[fe80:/i)) return true;
    if (hostname.match(/^\[f[cd]/i)) return true;
    return false;
  } catch (e) {
    return false;
  }
}

// --------------- API Client ---------------

var CanaryAPI = {
  TIMEOUT_MS: 5000,

  request: function (baseUrl, path, options) {
    options = options || {};
    var url = baseUrl + path;

    if (!isPrivateUrl(url)) {
      return Promise.reject(new Error('Refused: URL is not a private network address'));
    }

    var device = CanaryStorage.getDevices().find(function (d) {
      return d.base_url === baseUrl;
    });

    // Check session expiration before making the request
    if (device && CanarySession.isExpired(device.id)) {
      CanarySession._handleExpiry(device.id);
      return Promise.reject(new Error('Session expired'));
    }

    var token = options.token || (device ? device.token : '');

    var controller = new AbortController();
    var timeoutId = setTimeout(function () { controller.abort(); }, CanaryAPI.TIMEOUT_MS);

    var fetchOptions = {
      method: options.method || 'GET',
      headers: {},
      signal: controller.signal,
    };

    // Omit the auth header entirely when we have no token. This keeps
    // unauthenticated calls (the pre-pairing provisioning-receipt poll)
    // free of custom headers, so cross-origin GETs need no CORS preflight.
    if (token) {
      fetchOptions.headers['X-Canary-Token'] = token;
    }

    if (options.body) {
      fetchOptions.headers['Content-Type'] = 'application/json';
      fetchOptions.body = JSON.stringify(options.body);
    }

    return fetch(url, fetchOptions).then(function (res) {
      clearTimeout(timeoutId);
      if (res.status === 401) {
        return res.json().then(function (data) {
          // Handle server-side token expiration
          if (data.error === 'token_expired' && device) {
            CanarySession._handleExpiry(device.id);
          }
          var err = new Error(data.message || 'Authentication required');
          err.status = 401;
          err.data = data;
          throw err;
        });
      }
      if (res.status === 429) {
        return res.json().then(function (data) {
          var err = new Error(data.message || 'Rate limited');
          err.status = 429;
          err.data = data;
          throw err;
        });
      }
      if (!res.ok) {
        return res.json().then(function (data) {
          var err = new Error(data.message || 'Request failed');
          err.status = res.status;
          err.data = data;
          throw err;
        });
      }
      return res.json();
    }).catch(function (err) {
      clearTimeout(timeoutId);
      if (err.name === 'AbortError') {
        var timeoutErr = new Error('Device offline or request timed out');
        timeoutErr.status = 0;
        throw timeoutErr;
      }
      throw err;
    });
  }
};

// --------------- Token Masking ---------------

function maskToken(token) {
  if (!token || token.length < 12) return token || '';
  return token.substring(0, 10) + '\u2022\u2022\u2022\u2022\u2022\u2022\u2022\u2022';
}

// --------------- DOM Helpers ---------------

function el(tag, attrs, children) {
  var node = document.createElement(tag);
  if (attrs) {
    Object.keys(attrs).forEach(function (key) {
      if (key === 'className') {
        node.className = attrs[key];
      } else if (key.indexOf('on') === 0) {
        node.addEventListener(key.substring(2).toLowerCase(), attrs[key]);
      } else if (key === 'textContent') {
        node.textContent = attrs[key];
      } else {
        node.setAttribute(key, attrs[key]);
      }
    });
  }
  if (children) {
    if (!Array.isArray(children)) children = [children];
    children.forEach(function (child) {
      if (typeof child === 'string') {
        node.appendChild(document.createTextNode(child));
      } else if (child) {
        node.appendChild(child);
      }
    });
  }
  return node;
}

function clearApp() {
  var app = document.getElementById('app');
  // Views opt back in per render; only the fleet dashboard widens on desktop.
  app.classList.remove('app-wide');
  while (app.firstChild) app.removeChild(app.firstChild);
  return app;
}

// Clipboard fallback for plain-HTTP device contexts where navigator.clipboard
// is unavailable. Returns true if the copy command succeeded.
function copyFallback(text) {
  var ta = document.createElement('textarea');
  ta.value = text;
  ta.setAttribute('readonly', '');
  ta.style.position = 'fixed';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.select();
  var ok;
  try { ok = document.execCommand('copy'); } catch (_) { ok = false; }
  document.body.removeChild(ta);
  return ok;
}

// Humanize API config keys for display: snake_case -> sentence case,
// with overrides for keys whose words aren't plain English.
var FIELD_LABELS = {
  ssid: 'WiFi network (SSID)',
  mdns_hostname: 'Local hostname (mDNS)',
  mqtt_broker: 'MQTT broker',
  mqtt_port: 'MQTT port',
  mqtt_topic_prefix: 'MQTT topic prefix',
  webhook_url: 'Webhook URL',
  dwell_start_ms: 'Dwell start (ms)',
  lost_timeout_ms: 'Lost timeout (ms)',
  static_ip: 'Static IP address'
};
function labelFor(key) {
  if (FIELD_LABELS[key]) return FIELD_LABELS[key];
  var words = key.replace(/_/g, ' ').trim();
  return words.charAt(0).toUpperCase() + words.slice(1);
}

// --------------- Device Types ---------------
//
// The fleet spans three witness variants. Firmware advertises the
// canonical type as `dt` in its mDNS TXT record; the HTTP API exposes it
// as `device_type`. This registry drives type badges, the per-type
// explanation cards, suggested names, and the wizard branch: WAP-class
// devices pair over HTTP with a BOOT tap, while vision/sense have no
// HTTP server and onboard through Home Assistant (MQTT discovery).

var DEVICE_TYPES = {
  'canary-wap': {
    dt: 'canary-wap',
    label: 'Canary WAP',
    tagline: 'Witness beacon — GPS + signed event log with its own WiFi hotspot',
    pairing: 'http',
    whatsDifferent: [
      'Keeps a GPS-timestamped, cryptographically signed event log',
      'Runs its own WiFi hotspot, so it witnesses even with no infrastructure',
      'Pairs right here over HTTP with a short tap of its BOOT button',
    ],
    icon: [
      { tag: 'circle', attrs: { cx: '12', cy: '15', r: '2', fill: 'currentColor' } },
      { tag: 'path', attrs: { d: 'M8.5 11.5a5 5 0 0 1 7 0' } },
      { tag: 'path', attrs: { d: 'M5.5 8.5a9.2 9.2 0 0 1 13 0' } },
    ],
  },
  'canary-vision': {
    dt: 'canary-vision',
    label: 'Canary Vision',
    tagline: 'Camera witness — detects people, never stores video',
    pairing: 'mqtt',
    whatsDifferent: [
      'Person detection happens on the sensor — frames never leave it',
      'No video storage, ever: it emits semantic events only',
      'Joins through Home Assistant automatically — no pairing token',
    ],
    proof: 'Walk in front of it and watch Presence flip on.',
    icon: [
      { tag: 'rect', attrs: { x: '3', y: '6.5', width: '18', height: '12', rx: '2.5' } },
      { tag: 'circle', attrs: { cx: '12', cy: '12.5', r: '3.5' } },
      { tag: 'circle', attrs: { cx: '12', cy: '12.5', r: '0.8', fill: 'currentColor' } },
    ],
  },
  'canary-sense': {
    dt: 'canary-sense',
    label: 'Canary Sense',
    tagline: 'Radar witness — senses presence and breathing through the air, no camera, no microphone',
    pairing: 'mqtt',
    whatsDifferent: [
      '60 GHz radar senses presence, breathing, even heartbeat',
      'No camera and no microphone — there is nothing to record',
      'Joins through Home Assistant automatically — no pairing token',
    ],
    proof: 'Sit still nearby for ten seconds and watch Breathing lock.',
    icon: [
      { tag: 'circle', attrs: { cx: '6', cy: '18', r: '1.6', fill: 'currentColor' } },
      { tag: 'path', attrs: { d: 'M6 12.5a5.5 5.5 0 0 1 5.5 5.5' } },
      { tag: 'path', attrs: { d: 'M6 7.5a10.5 10.5 0 0 1 10.5 10.5' } },
    ],
  },
  // Fallback for devices that predate `device_type`. Pairing stays
  // 'http' — the classic BOOT-tap flow — and no badge is rendered.
  unknown: {
    dt: 'unknown',
    label: 'Canary',
    tagline: '',
    pairing: 'http',
    whatsDifferent: [],
    icon: null,
  },
};

// Fold any spelling ("Canary_Sense", "canary sense") onto the canonical
// lowercase-hyphenated key the registry uses.
function canonicalDeviceType(value) {
  if (typeof value !== 'string') return '';
  return value.trim().toLowerCase().replace(/[_\s]+/g, '-');
}

function deviceTypeInfo(value) {
  return DEVICE_TYPES[canonicalDeviceType(value)] || DEVICE_TYPES.unknown;
}

// Peers relayed by a WAP carry the mDNS TXT `dt` key; API payloads use
// the long `device_type` spelling. Accept either.
function peerDeviceType(peer) {
  if (!peer) return '';
  return canonicalDeviceType(peer.dt || peer.device_type || '');
}

// The wizard branch decision: 'http' devices run the BOOT-tap receipt
// capture; 'mqtt' devices (vision/sense) have no HTTP server and are
// onboarded through Home Assistant instead.
function wizardPathForPeer(peer) {
  return deviceTypeInfo(peerDeviceType(peer)).pairing;
}

function deviceTypeIcon(info) {
  if (!info || !info.icon) return null;
  var svgNs = 'http://www.w3.org/2000/svg';
  var svg = document.createElementNS(svgNs, 'svg');
  svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('class', 'device-type-icon');
  svg.setAttribute('aria-hidden', 'true');
  info.icon.forEach(function (shape) {
    var node = document.createElementNS(svgNs, shape.tag);
    Object.keys(shape.attrs).forEach(function (key) {
      node.setAttribute(key, shape.attrs[key]);
    });
    svg.appendChild(node);
  });
  return svg;
}

// Small icon+label chip. Returns null for unknown types so untyped rows
// render exactly as before.
function renderDeviceTypeBadge(value) {
  var info = deviceTypeInfo(value);
  if (info === DEVICE_TYPES.unknown) return null;
  return el('span', { className: 'device-type-badge' }, [
    deviceTypeIcon(info),
    el('span', { textContent: info.label }),
  ]);
}

// "What makes this one different" — the per-type explanation card shown
// during onboarding.
function renderDeviceTypeCard(info) {
  return el('div', { className: 'card' }, [
    el('div', { className: 'card-title mb-8', textContent: 'What makes this one different' }),
    el('ul', { className: 'device-type-bullets' }, info.whatsDifferent.map(function (line) {
      return el('li', { textContent: line });
    })),
  ]);
}

// --------------- Router ---------------

var Router = {
  routes: {},

  register: function (pattern, handler) {
    Router.routes[pattern] = handler;
  },

  navigate: function (hash) {
    window.location.hash = hash;
  },

  resolve: function () {
    var hash = window.location.hash || '#/canaries';
    if (hash === '#/' || hash === '#' || hash === '') {
      hash = '#/canaries';
      window.location.hash = hash;
      return;
    }

    var path = hash.substring(1); // Remove #

    // Try exact match first (own properties only — the hash is
    // user-controlled, so never dispatch via inherited names like
    // 'constructor')
    if (Object.prototype.hasOwnProperty.call(Router.routes, path) &&
        typeof Router.routes[path] === 'function') {
      Router.routes[path]();
      return;
    }

    // Try pattern matching
    var keys = Object.keys(Router.routes);
    for (var i = 0; i < keys.length; i++) {
      var pattern = keys[i];
      var regex = pattern.replace(/:[^/]+/g, '([^/]+)');
      var match = path.match(new RegExp('^' + regex + '$'));
      if (match) {
        Router.routes[pattern].apply(null, match.slice(1));
        return;
      }
    }

    // 404
    renderNotFound();
  }
};

// --------------- Notification Preferences ---------------

var NOTIF_PREFS_KEY = 'canary_notif_prefs';

var DEFAULT_NOTIF_PREFS = {
  enabled: true,
  sound: false,
  vibrate: true,
  person_detected: true,
  presence_restricted: true,
  vehicle_detected: true,
  object_removed: true,
  acoustic_impulse: true,
  animal_detected: true,
  contact_changed: false,
  motion_detected: false,
  quiet_hours_enabled: false,
  quiet_hours_start: '22:00',
  quiet_hours_end: '07:00',
};

function getNotifPrefs() {
  try {
    var raw = localStorage.getItem(NOTIF_PREFS_KEY);
    if (raw) {
      var parsed = JSON.parse(raw);
      var prefs = {};
      var keys = Object.keys(DEFAULT_NOTIF_PREFS);
      for (var i = 0; i < keys.length; i++) {
        prefs[keys[i]] = parsed[keys[i]] !== undefined ? parsed[keys[i]] : DEFAULT_NOTIF_PREFS[keys[i]];
      }
      return prefs;
    }
  } catch (e) { /* ignore */ }
  return Object.assign({}, DEFAULT_NOTIF_PREFS);
}

function saveNotifPrefs(prefs) {
  try {
    localStorage.setItem(NOTIF_PREFS_KEY, JSON.stringify(prefs));
  } catch (e) { /* ignore */ }
}

function isQuietHoursActive(prefs) {
  if (!prefs.quiet_hours_enabled) return false;
  var now = new Date();
  var hhmm = (now.getHours() < 10 ? '0' : '') + now.getHours() + ':' +
             (now.getMinutes() < 10 ? '0' : '') + now.getMinutes();
  var start = prefs.quiet_hours_start;
  var end = prefs.quiet_hours_end;
  if (start <= end) {
    return hhmm >= start && hhmm < end;
  }
  return hhmm >= start || hhmm < end;
}

function shouldNotify(eventType) {
  var prefs = getNotifPrefs();
  if (!prefs.enabled) return false;
  if (isQuietHoursActive(prefs)) return false;
  return prefs[eventType] !== false;
}

function playNotificationFeedback(eventType) {
  var prefs = getNotifPrefs();
  if (prefs.vibrate && navigator.vibrate) {
    var pattern = eventType === 'person_detected' ? [100, 50, 100] : [80];
    navigator.vibrate(pattern);
  }
  if (prefs.sound) {
    try {
      var ctx = new (window.AudioContext || window.webkitAudioContext)();
      var osc = ctx.createOscillator();
      var gain = ctx.createGain();
      osc.connect(gain);
      gain.connect(ctx.destination);
      gain.gain.value = 0.1;
      osc.frequency.value = eventType === 'person_detected' ? 880 : 660;
      osc.type = 'sine';
      osc.start();
      osc.stop(ctx.currentTime + 0.15);
      osc.onended = function () { ctx.close(); };
    } catch (e) { /* audio not available */ }
  }
}

var _unseenEventCount = 0;

function incrementUnseenEvents() {
  if ((window.location.hash || '') === '#/events') return;
  _unseenEventCount++;
  updateNavBadge();
}

function clearUnseenEvents() {
  _unseenEventCount = 0;
  updateNavBadge();
}

function updateNavBadge() {
  var badge = document.getElementById('events-badge');
  if (!badge) return;
  if (_unseenEventCount > 0) {
    badge.textContent = _unseenEventCount > 99 ? '99+' : String(_unseenEventCount);
    badge.style.display = 'inline-flex';
  } else {
    badge.style.display = 'none';
  }
}

// --------------- Pull-to-Refresh ---------------

function setupPullToRefresh(scrollContainer, contentWrap, indicator, onRefresh) {
  var THRESHOLD = 60;
  var MAX_PULL = 100;
  var startY = 0;
  var pulling = false;
  var refreshing = false;

  function onTouchStart(e) {
    if (refreshing) return;
    if (scrollContainer.scrollTop > 0) return;
    startY = e.touches[0].clientY;
    pulling = false;
  }

  function onTouchMove(e) {
    if (refreshing) return;
    if (scrollContainer.scrollTop > 0) { reset(); return; }
    var deltaY = e.touches[0].clientY - startY;
    if (deltaY < 0) { reset(); return; }

    // Rubber-band: diminishing returns past threshold
    var pull = Math.min(MAX_PULL, deltaY * 0.5);
    if (pulling || pull > 10) {
      pulling = true;
      var displayPull = Math.max(0, pull);
      contentWrap.className = 'ptr-content pulling';
      contentWrap.style.transform = 'translateY(' + displayPull + 'px)';
      indicator.style.opacity = Math.min(1, displayPull / THRESHOLD);
      if (displayPull >= THRESHOLD) {
        indicator.className = 'ptr-indicator armed';
      } else {
        indicator.className = 'ptr-indicator';
      }
      e.preventDefault();
    }
  }

  function onTouchEnd() {
    if (refreshing || !pulling) return;
    var current = parseFloat(contentWrap.style.transform.replace('translateY(', '').replace('px)', '')) || 0;

    if (current >= THRESHOLD) {
      refreshing = true;
      indicator.className = 'ptr-indicator refreshing';
      indicator.style.opacity = '1';
      contentWrap.className = 'ptr-content snapping';
      contentWrap.style.transform = 'translateY(48px)';

      onRefresh(function done() {
        refreshing = false;
        reset();
      });
    } else {
      reset();
    }
  }

  function reset() {
    pulling = false;
    contentWrap.className = 'ptr-content snapping';
    contentWrap.style.transform = 'translateY(0)';
    indicator.className = 'ptr-indicator';
    indicator.style.opacity = '0';
  }

  scrollContainer.addEventListener('touchstart', onTouchStart, { passive: true });
  scrollContainer.addEventListener('touchmove', onTouchMove, { passive: false });
  scrollContainer.addEventListener('touchend', onTouchEnd, { passive: true });
  scrollContainer.addEventListener('touchcancel', reset, { passive: true });

  return function teardown() {
    scrollContainer.removeEventListener('touchstart', onTouchStart);
    scrollContainer.removeEventListener('touchmove', onTouchMove);
    scrollContainer.removeEventListener('touchend', onTouchEnd);
    scrollContainer.removeEventListener('touchcancel', reset);
  };
}

// --------------- Event Timeline ---------------

var EVENT_TYPE_META = {
  person_detected:     { icon: '🚶', label: 'Person',  cssClass: 'type-person',  priority: 4 },
  presence_restricted: { icon: '⛔', label: 'Restricted zone', cssClass: 'type-restricted', priority: 4 },
  vehicle_detected:    { icon: '🚗', label: 'Vehicle', cssClass: 'type-vehicle', priority: 3 },
  object_removed:      { icon: '📦', label: 'Object removed', cssClass: 'type-object-removed', priority: 3 },
  acoustic_impulse:    { icon: '🔊', label: 'Acoustic impulse', cssClass: 'type-acoustic', priority: 3 },
  animal_detected:     { icon: '🐾', label: 'Animal',  cssClass: 'type-animal',  priority: 2 },
  contact_changed:     { icon: '🚪', label: 'Contact change', cssClass: 'type-contact', priority: 2 },
  motion_detected:     { icon: '💨', label: 'Motion',  cssClass: 'type-motion',  priority: 1 },
};

// Canonical envelope events use the kernel's EventType enum (src/lib.rs), not the raw device
// strings. Map them back to friendly labels + the existing timeline dot classes so the in-app
// verified timeline shares the visual language of the live events view. Unknown variants fall back
// to a neutral label rather than guessing.
var ENVELOPE_EVENT_META = {
  BoundaryCrossingObjectLarge:  { icon: '🚶', label: 'Large object crossing',  cssClass: 'type-person' },
  BoundaryCrossingObjectSmall:  { icon: '💨', label: 'Small object / motion',  cssClass: 'type-motion' },
  AcousticImpulseInZone:        { icon: '🔊', label: 'Acoustic impulse',       cssClass: 'type-acoustic' },
  PresenceInRestrictedZone:     { icon: '⛔', label: 'Presence (restricted)',  cssClass: 'type-restricted' },
  VehiclePresenceAfterHours:    { icon: '🚗', label: 'Vehicle (after hours)',  cssClass: 'type-vehicle' },
  ContactStateChange:           { icon: '🚪', label: 'Contact state change',   cssClass: 'type-contact' },
  ObjectRemovedFromZone:        { icon: '📦', label: 'Object removed',          cssClass: 'type-object-removed' },
};

function envelopeEventMeta(eventType) {
  return ENVELOPE_EVENT_META[eventType] ||
    { icon: '•', label: String(eventType || 'unknown'), cssClass: 'type-motion' };
}

// Render a coarse {start_epoch_s, size_s} bucket as a human window in UTC. The whole point of the
// canonical envelope is that this is COARSE — no exact second — so we show the bucket span, not a
// precise instant.
function formatTimeBucket(tb) {
  if (!tb || typeof tb.start_epoch_s !== 'number') return '—';
  var start = new Date(tb.start_epoch_s * 1000);
  var end = new Date((tb.start_epoch_s + (tb.size_s || 0)) * 1000);
  function hm(d) {
    return ('0' + d.getUTCHours()).slice(-2) + ':' + ('0' + d.getUTCMinutes()).slice(-2);
  }
  var date = start.getUTCFullYear() + '-' + ('0' + (start.getUTCMonth() + 1)).slice(-2) + '-' +
    ('0' + start.getUTCDate()).slice(-2);
  var mins = Math.round((tb.size_s || 0) / 60);
  return date + ' ' + hm(start) + '–' + hm(end) + ' UTC (' + mins + ' min window)';
}


var EventsState = {
  allRecords: [],
  clusters: [],
  filteredClusters: [],
  deviceResults: [],
  activeFilter: 'all',
  isLoading: false,
  refreshTimer: null,
  expandedClusters: {},
  _sessionSeq: 0,
  eventSources: [],
  pendingLiveEvents: 0,
};

function fetchAllWitnessRecords() {
  var devices = CanaryStorage.getDevices();
  if (devices.length === 0) {
    return Promise.resolve({ results: [], allRecords: [] });
  }

  var promises = devices.map(function (device) {
    return CanaryAPI.request(device.base_url, '/api/v1/witness?last=100')
      .then(function (data) {
        var records = (data.records || []).map(function (r) {
          r.device_id = device.id;
          r.device_name = device.name || device.id;
          r._ts = new Date(r.timestamp);
          return r;
        }).filter(function (r) {
          return r.timestamp && !isNaN(r._ts.getTime());
        });
        return { device: device, records: records, error: null };
      })
      .catch(function (err) {
        return { device: device, records: [], error: err };
      });
  });

  return Promise.all(promises).then(function (results) {
    var allRecords = [];
    for (var i = 0; i < results.length; i++) {
      for (var j = 0; j < results[i].records.length; j++) {
        allRecords.push(results[i].records[j]);
      }
    }
    allRecords.sort(function (a, b) { return b._ts - a._ts; });
    return { results: results, allRecords: allRecords };
  });
}

function clusterEvents(records) {
  if (records.length === 0) return [];

  var sorted = records.slice().sort(function (a, b) { return a._ts - b._ts; });
  var WINDOW_MS = 3 * 60 * 1000;
  var clusters = [];
  var sessionMap = {};

  for (var i = 0; i < sorted.length; i++) {
    var rec = sorted[i];
    var merged = false;

    // Prefer activity_session grouping when the device provides it
    if (rec.activity_session) {
      var sessKey = rec.device_id + ':' + rec.activity_session;
      if (sessionMap[sessKey] !== undefined) {
        var sessCluster = clusters[sessionMap[sessKey]];
        sessCluster.events.push(rec);
        sessCluster.lastTime = rec._ts;
        sessCluster.count = sessCluster.events.length;
        var sm = EVENT_TYPE_META[rec.event_type];
        var scm = EVENT_TYPE_META[sessCluster.primaryType];
        if (sm && scm && sm.priority > scm.priority) {
          sessCluster.primaryType = rec.event_type;
        }
        merged = true;
      }
    }

    // Fall back to time-window clustering (only for records without a session)
    if (!merged && !rec.activity_session) {
      for (var c = clusters.length - 1; c >= 0; c--) {
        var cluster = clusters[c];
        if (cluster.device_id === rec.device_id &&
            cluster.zone === rec.zone &&
            (rec._ts - cluster.lastTime) < WINDOW_MS) {
          cluster.events.push(rec);
          cluster.lastTime = rec._ts;
          cluster.count = cluster.events.length;
          var meta = EVENT_TYPE_META[rec.event_type];
          var curMeta = EVENT_TYPE_META[cluster.primaryType];
          if (meta && curMeta && meta.priority > curMeta.priority) {
            cluster.primaryType = rec.event_type;
          }
          merged = true;
          break;
        }
      }
    }

    if (!merged) {
      var newCluster = {
        id: rec.hash || (rec.device_id + '-' + rec.seq),
        device_id: rec.device_id,
        device_name: rec.device_name,
        zone: rec.zone,
        primaryType: rec.event_type,
        activitySession: rec.activity_session || null,
        events: [rec],
        firstTime: rec._ts,
        lastTime: rec._ts,
        count: 1,
      };
      clusters.push(newCluster);
      if (rec.activity_session) {
        sessionMap[rec.device_id + ':' + rec.activity_session] = clusters.length - 1;
      }
    }
  }

  clusters.sort(function (a, b) { return b.lastTime - a.lastTime; });
  return clusters;
}

function applyEventsFilter(clusters, filter) {
  if (filter === 'all') return clusters;
  return clusters.filter(function (c) {
    if (EVENT_TYPE_META[filter]) {
      for (var i = 0; i < c.events.length; i++) {
        if (c.events[i].event_type === filter) return true;
      }
      return false;
    }
    return c.device_id === filter;
  });
}

function formatEventTime(date) {
  var now = new Date();
  var diffMs = now - date;
  var diffMin = Math.floor(diffMs / 60000);

  if (diffMin < 1) return 'Just now';
  if (diffMin < 60) return diffMin + ' min ago';

  var isToday = date.toDateString() === now.toDateString();
  var yesterday = new Date(now);
  yesterday.setDate(yesterday.getDate() - 1);
  var isYesterday = date.toDateString() === yesterday.toDateString();

  var timeStr = date.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });

  if (isToday) return timeStr;
  if (isYesterday) return 'Yesterday, ' + timeStr;

  return date.toLocaleDateString([], { weekday: 'short', month: 'short', day: 'numeric' }) + ', ' + timeStr;
}

function formatDayLabel(date) {
  var now = new Date();
  if (date.toDateString() === now.toDateString()) return 'Today';
  var yesterday = new Date(now);
  yesterday.setDate(yesterday.getDate() - 1);
  if (date.toDateString() === yesterday.toDateString()) return 'Yesterday';
  return date.toLocaleDateString([], { weekday: 'short', month: 'short', day: 'numeric' });
}

function buildDensityBuckets(records) {
  var now = new Date();
  var startOfDay = new Date(now.getFullYear(), now.getMonth(), now.getDate());
  var buckets = [];
  for (var i = 0; i < 48; i++) {
    buckets.push({ count: 0, types: {} });
  }

  for (var j = 0; j < records.length; j++) {
    var rec = records[j];
    if (rec._ts >= startOfDay && rec._ts <= now) {
      var minutesSinceMidnight = (rec._ts - startOfDay) / 60000;
      var bucketIdx = Math.min(47, Math.floor(minutesSinceMidnight / 30));
      buckets[bucketIdx].count++;
      buckets[bucketIdx].types[rec.event_type] = (buckets[bucketIdx].types[rec.event_type] || 0) + 1;
    }
  }

  var maxCount = 0;
  for (var k = 0; k < buckets.length; k++) {
    if (buckets[k].count > maxCount) maxCount = buckets[k].count;
  }

  return { buckets: buckets, maxCount: maxCount };
}

function getDominantType(typesObj) {
  var best = 'motion_detected';
  var bestPriority = 0;
  var keys = Object.keys(typesObj);
  for (var i = 0; i < keys.length; i++) {
    var meta = EVENT_TYPE_META[keys[i]];
    if (meta && meta.priority > bestPriority) {
      bestPriority = meta.priority;
      best = keys[i];
    }
  }
  return best;
}

function countTodayByType(records) {
  var now = new Date();
  var startOfDay = new Date(now.getFullYear(), now.getMonth(), now.getDate());
  // Build the per-type tallies from EVENT_TYPE_META so this never drifts as types are added.
  var counts = { total: 0 };
  Object.keys(EVENT_TYPE_META).forEach(function (key) { counts[key] = 0; });
  for (var i = 0; i < records.length; i++) {
    if (records[i]._ts >= startOfDay) {
      counts.total++;
      if (counts[records[i].event_type] !== undefined) {
        counts[records[i].event_type]++;
      }
    }
  }
  return counts;
}

// --------------- Live Event Streaming ---------------

function closeEventStreams() {
  for (var i = 0; i < EventsState.eventSources.length; i++) {
    try { EventsState.eventSources[i].close(); } catch (e) { /* ignore */ }
  }
  EventsState.eventSources = [];
  EventsState.pendingLiveEvents = 0;
}

function connectEventStreams(devices, contentContainer) {
  closeEventStreams();

  for (var d = 0; d < devices.length; d++) {
    (function (device) {
      if (!isPrivateUrl(device.base_url)) return;
      if (!device.token) return;

      connectDeviceStream(device, contentContainer);
    })(devices[d]);
  }
}

function insertSorted(arr, record, maxLen) {
  var lo = 0, hi = arr.length;
  while (lo < hi) {
    var mid = (lo + hi) >>> 1;
    if (arr[mid]._ts > record._ts) lo = mid + 1;
    else hi = mid;
  }
  arr.splice(lo, 0, record);
  if (arr.length > maxLen) arr.length = maxLen;
}

function connectDeviceStream(device, contentContainer) {
  if ((window.location.hash || '') !== '#/events') return;

  var sessionId = EventsState.currentSessionId;

  CanaryAPI.request(device.base_url, '/api/v1/witness/stream/ticket', { method: 'POST' })
    .then(function (data) {
      if (!data.ticket) return;
      if (EventsState.currentSessionId !== sessionId) return;

      var url = device.base_url + '/api/v1/witness/stream?ticket=' + encodeURIComponent(data.ticket);
      var source = new EventSource(url);

      source.addEventListener('witness', function (e) {
        if (EventsState.currentSessionId !== sessionId) { source.close(); return; }
        try {
          var record = JSON.parse(e.data);
          record.device_id = device.id;
          record.device_name = device.name || device.id;
          record._ts = new Date(record.timestamp);
          if (!record.timestamp || isNaN(record._ts.getTime())) return;

          var exists = EventsState.allRecords.some(function (r) {
            return r.hash === record.hash || (r.device_id === record.device_id && r.seq === record.seq);
          });
          if (exists) return;

          insertSorted(EventsState.allRecords, record, 5000);
          EventsState.clusters = clusterEvents(EventsState.allRecords);
          EventsState.filteredClusters = applyEventsFilter(EventsState.clusters, EventsState.activeFilter);

          if (shouldNotify(record.event_type)) {
            incrementUnseenEvents();
            playNotificationFeedback(record.event_type);
          }

          if (!document.body.contains(contentContainer)) return;
          var scrollContainer = contentContainer.parentNode;
          var isScrolledDown = scrollContainer && scrollContainer.scrollTop > 100;

          if (isScrolledDown) {
            EventsState.pendingLiveEvents++;
            showLiveBanner(contentContainer);
          } else {
            renderEventsContent(contentContainer, {
              results: EventsState.deviceResults,
              allRecords: EventsState.allRecords,
            });
          }
        } catch (err) { /* ignore malformed events */ }
      });

      source.addEventListener('error', function () {
        if (source.readyState === EventSource.CLOSED) {
          source.close();
          var idx = EventsState.eventSources.indexOf(source);
          if (idx !== -1) EventsState.eventSources.splice(idx, 1);
          if (EventsState.currentSessionId === sessionId) {
            setTimeout(function () {
              connectDeviceStream(device, contentContainer);
            }, 3000);
          }
        }
      });

      EventsState.eventSources.push(source);
    })
    .catch(function () { /* device doesn't support SSE tickets — polling fallback */ });
}

function showLiveBanner(contentContainer) {
  var existing = contentContainer.querySelector('.live-banner');
  if (existing) {
    existing.textContent = EventsState.pendingLiveEvents + ' new event' +
      (EventsState.pendingLiveEvents !== 1 ? 's' : '');
    return;
  }

  var banner = el('div', {
    className: 'live-banner',
    textContent: EventsState.pendingLiveEvents + ' new event' +
      (EventsState.pendingLiveEvents !== 1 ? 's' : ''),
    onClick: function () {
      EventsState.pendingLiveEvents = 0;
      var scrollContainer = contentContainer.parentNode;
      if (scrollContainer) scrollContainer.scrollTop = 0;
      renderEventsContent(contentContainer, {
        results: EventsState.deviceResults,
        allRecords: EventsState.allRecords,
      });
    }
  });

  contentContainer.insertBefore(banner, contentContainer.firstChild);
}

// --------------- Views ---------------

function renderHeader(title, showBack, showSettings) {
  var children = [];

  if (showBack) {
    children.push(el('button', {
      className: 'back-btn',
      textContent: '\u2190 Back',
      onClick: function () { window.history.back(); }
    }));
  } else {
    children.push(el('span'));
  }

  children.push(el('h1', { textContent: title }));

  if (showSettings) {
    children.push(el('button', {
      className: 'settings-btn',
      textContent: '\u2699',
      onClick: function () { Router.navigate('#/settings'); }
    }));
  } else {
    children.push(el('span'));
  }

  return el('div', { className: 'header' }, children);
}

function renderNav(active) {
  var tabs = [
    { label: 'My Canaries', hash: '#/canaries' },
    { label: 'Events', hash: '#/events' },
    { label: 'Settings', hash: '#/settings' },
  ];

  return el('nav', { className: 'nav' }, tabs.map(function (tab) {
    var link = el('a', { href: tab.hash });
    if (tab.hash === active) link.className = 'active';
    link.appendChild(document.createTextNode(tab.label));
    if (tab.hash === '#/events') {
      var badge = el('span', { id: 'events-badge', className: 'nav-badge' });
      badge.style.display = 'none';
      link.appendChild(badge);
      if (tab.hash === active) clearUnseenEvents();
      setTimeout(updateNavBadge, 0);
    }
    return link;
  }));
}

function renderCanariesView() {
  var app = clearApp();
  // The fleet view is the one screen that benefits from desktop width:
  // device cards flow into a multi-column grid above 768px.
  app.classList.add('app-wide');
  app.appendChild(renderHeader('Canary Vision', false, true));
  app.appendChild(renderNav('#/canaries'));

  var content = el('div', { className: 'content' });
  var devices = CanaryStorage.getDevices();

  if (devices.length === 0) {
    content.appendChild(el('div', { className: 'empty-state' }, [
      el('h2', { textContent: 'Protect every room' }),
      el('p', { textContent: 'Canaries work best as a fleet. Most homes start with one at the entry, then add coverage for living areas, bedrooms, and the garage.' }),
      el('div', { className: 'coverage-chips' }, [
        el('span', { className: 'coverage-chip', textContent: '🚪 Entry' }),
        el('span', { className: 'coverage-chip', textContent: '🛋 Living Room' }),
        el('span', { className: 'coverage-chip', textContent: '🚗 Garage' }),
      ]),
      el('button', {
        className: 'btn btn-primary',
        textContent: 'Add your first Canary',
        onClick: function () { Router.navigate('#/canaries/add'); }
      }),
    ]));
    app.appendChild(content);
    return;
  }

  // Fleet health summary (populated async)
  var fleetCard = el('div', { className: 'card fleet-health-card' });
  fleetCard.appendChild(el('div', { className: 'card-title mb-8', textContent: 'Fleet Health' }));
  var fleetStats = el('div', { className: 'stats-grid', id: 'fleet-stats' });
  fleetStats.appendChild(el('div', { className: 'stat-item' }, [
    el('div', { className: 'stat-value', id: 'fleet-online', textContent: '--' }),
    el('div', { className: 'stat-label', textContent: 'Online' }),
  ]));
  fleetStats.appendChild(el('div', { className: 'stat-item' }, [
    el('div', { className: 'stat-value', id: 'fleet-events', textContent: '--' }),
    el('div', { className: 'stat-label', textContent: 'Events Today' }),
  ]));
  fleetStats.appendChild(el('div', { className: 'stat-item' }, [
    el('div', { className: 'stat-value', id: 'fleet-uptime', textContent: '--' }),
    el('div', { className: 'stat-label', textContent: 'Avg Uptime' }),
  ]));
  fleetStats.appendChild(el('div', { className: 'stat-item' }, [
    el('div', { className: 'stat-value', id: 'fleet-signal', textContent: '--' }),
    el('div', { className: 'stat-label', textContent: 'Avg Signal' }),
  ]));
  fleetCard.appendChild(fleetStats);
  content.appendChild(fleetCard);

  // Device cards, grouped by room when the user has assigned any
  var deviceCards = {};
  function makeDeviceCard(device) {
    var statusDot = el('span', { className: 'status-dot warning' });
    var statusLabel = el('span', {
      className: 'fleet-device-status',
      textContent: 'Checking…',
    });
    var statsRow = el('div', { className: 'fleet-device-stats' });
    var typeInfo = deviceTypeInfo(device.device_type);

    var card = el('div', { className: 'card fleet-device-card cursor-pointer' });
    card.addEventListener('click', function () {
      Router.navigate('#/device/' + device.id);
    });

    var header = el('div', { className: 'card-header' }, [
      el('div', {}, [
        el('div', { className: 'card-title', textContent: device.name || device.id }),
        renderDeviceTypeBadge(typeInfo.dt),
        typeInfo.tagline
          ? el('div', { className: 'device-type-tagline', textContent: typeInfo.tagline })
          : null,
        statusLabel,
      ]),
      statusDot,
    ]);
    card.appendChild(header);
    card.appendChild(statsRow);

    deviceCards[device.id] = { card: card, dot: statusDot, label: statusLabel, stats: statsRow };
    return card;
  }

  var hasRooms = devices.some(function (d) { return d.room; });
  if (hasRooms) {
    var byRoom = {};
    devices.forEach(function (d) {
      var room = d.room || '';
      if (!byRoom[room]) byRoom[room] = [];
      byRoom[room].push(d);
    });
    var rooms = Object.keys(byRoom).filter(function (r) { return r !== ''; }).sort();
    if (byRoom['']) rooms.push('');
    rooms.forEach(function (room) {
      content.appendChild(el('div', {
        className: 'room-section-title',
        textContent: room || 'Elsewhere',
      }));
      var roomGrid = el('div', { className: 'fleet-grid' });
      byRoom[room].forEach(function (device) {
        roomGrid.appendChild(makeDeviceCard(device));
      });
      content.appendChild(roomGrid);
    });
  } else {
    var grid = el('div', { className: 'fleet-grid' });
    devices.forEach(function (device) {
      grid.appendChild(makeDeviceCard(device));
    });
    content.appendChild(grid);
  }

  content.appendChild(el('button', {
    className: 'btn btn-secondary btn-block mt-12',
    textContent: '+ Add Canary',
    onClick: function () { Router.navigate('#/canaries/add'); },
  }));

  // One quiet nudge while the fleet is small. Dismissible, and it stays
  // dismissed — no badges, no counters.
  var hintDismissed = false;
  try {
    hintDismissed = localStorage.getItem('cv_coverage_hint_dismissed') === '1';
  } catch (e) { /* ignore */ }
  if (devices.length < 3 && !hintDismissed) {
    var hint = el('div', { className: 'coverage-hint' }, [
      el('span', {
        className: 'coverage-hint-text cursor-pointer',
        textContent: 'Add a Canary to extend coverage ›',
      }),
      el('button', { className: 'coverage-hint-dismiss', textContent: '✕' }),
    ]);
    hint.querySelector('.coverage-hint-text').addEventListener('click', function () {
      Router.navigate('#/canaries/add');
    });
    hint.querySelector('.coverage-hint-dismiss').addEventListener('click', function () {
      try { localStorage.setItem('cv_coverage_hint_dismissed', '1'); } catch (e) { /* ignore */ }
      if (hint.parentNode) hint.parentNode.removeChild(hint);
    });
    content.appendChild(hint);
  }

  content.appendChild(el('div', { id: 'discovered-peers' }));
  app.appendChild(content);

  // Fetch health data from all devices in parallel
  var onlineCount = 0;
  var totalUptime = 0;
  var totalRssi = 0;
  var rssiCount = 0;
  var totalEvents = 0;
  function updateFleetSummary() {
    if (!document.body.contains(fleetCard)) return;

    var onlineEl = fleetCard.querySelector('#fleet-online');
    var eventsEl = fleetCard.querySelector('#fleet-events');
    var uptimeEl = fleetCard.querySelector('#fleet-uptime');
    var signalEl = fleetCard.querySelector('#fleet-signal');
    if (onlineEl) onlineEl.textContent = onlineCount + ' / ' + devices.length;
    if (eventsEl) eventsEl.textContent = String(totalEvents);
    if (uptimeEl) {
      var avgHrs = rssiCount > 0 ? Math.floor(totalUptime / rssiCount / 3600) : 0;
      uptimeEl.textContent = avgHrs + 'h';
    }
    if (signalEl) {
      signalEl.textContent = rssiCount > 0 ? Math.round(totalRssi / rssiCount) + ' dBm' : '—';
    }

    if (onlineEl) {
      onlineEl.style.color = onlineCount === devices.length
        ? 'var(--color-success)'
        : onlineCount > 0 ? 'var(--color-warning)' : 'var(--color-error)';
    }
  }

  devices.forEach(function (device) {
    var dc = deviceCards[device.id];

    // Fetch device info first; only fetch witness records if auth succeeds
    CanaryAPI.request(device.base_url, '/api/v1/info')
      .then(function (info) {
        if (!document.body.contains(dc.card)) return;

        onlineCount++;
        totalUptime += info.uptime_s || 0;
        totalRssi += info.wifi_rssi || 0;
        rssiCount++;

        dc.dot.className = 'status-dot online';
        dc.label.textContent = 'Online';
        dc.label.style.color = 'var(--color-success)';

        var uptimeH = Math.floor((info.uptime_s || 0) / 3600);
        var rssiVal = info.wifi_rssi || 0;
        var rssiClass = rssiVal > -50 ? 'color: var(--color-success)' :
                        rssiVal > -70 ? 'color: var(--color-warning)' :
                        'color: var(--color-error)';

        dc.stats.appendChild(el('div', { className: 'fleet-device-stats-row' }, [
          el('span', { textContent: 'v' + (info.firmware_version || '?'), className: 'fleet-stat-chip' }),
          el('span', { textContent: uptimeH + 'h up', className: 'fleet-stat-chip' }),
          el('span', {
            textContent: rssiVal + ' dBm',
            className: 'fleet-stat-chip',
            style: rssiClass,
          }),
        ]));

        updateFleetSummary();

        // Only fetch events after successful auth to avoid lockout
        return CanaryAPI.request(device.base_url, '/api/v1/witness?last=100');
      })
      .then(function (data) {
        if (!data || !document.body.contains(dc.card)) return;
        var records = data.records || [];

        var now = new Date();
        var startOfDay = new Date(now.getFullYear(), now.getMonth(), now.getDate());
        var todayCount = 0;
        for (var i = 0; i < records.length; i++) {
          var ts = new Date(records[i].timestamp);
          if (ts >= startOfDay) todayCount++;
        }
        totalEvents += todayCount;

        if (todayCount > 0 && dc.stats) {
          dc.stats.appendChild(el('div', { className: 'fleet-device-events', textContent: todayCount + ' events today' }));
        }

        updateFleetSummary();
      })
      .catch(function (err) {
        if (!document.body.contains(dc.card)) return;

        dc.dot.className = 'status-dot offline';
        dc.label.textContent = err && err.status === 401 ? 'Auth Error' : 'Offline';
        dc.label.style.color = 'var(--color-error)';

        updateFleetSummary();
      });
  });

  if (devices.length > 0) {
    refreshDiscoveredPeers();
  }

  // Keep the fleet view live: re-render every 30 s while it's on screen,
  // so a dashboard left open on a desk stays current. The interval clears
  // itself once the user navigates away (content leaves the DOM).
  if (window._fleetRefreshTimer) clearInterval(window._fleetRefreshTimer);
  window._fleetRefreshTimer = setInterval(function () {
    if (!document.body.contains(content)) {
      clearInterval(window._fleetRefreshTimer);
      window._fleetRefreshTimer = null;
      return;
    }
    renderCanariesView();
  }, 30000);
}

// --------------- Peer Discovery ---------------

// Build the canonical base URL for a peer using its mDNS hostname when
// available, falling back to its LAN IP. Hostname is preferred because
// IPs change and mDNS doesn't.
function peerBaseUrl(peer) {
  var host = peer.mdns_hostname || peer.ip;
  if (!host) return '';
  if (host.indexOf('http') === 0) return host.replace(/\/+$/, '');
  return 'http://' + host.replace(/\/+$/, '');
}

// Returns the set of base_urls and device_ids already managed by the user,
// so we can filter them out of the discovered list.
function collectKnownIdentities(devices) {
  var knownIds = {};
  var knownUrls = {};
  for (var i = 0; i < devices.length; i++) {
    if (devices[i].id) knownIds[devices[i].id] = true;
    if (devices[i].base_url) knownUrls[devices[i].base_url] = true;
  }
  return { ids: knownIds, urls: knownUrls };
}

// Query every known device's peer list in parallel and return the peers
// the user hasn't paired yet. Failures are silent — a device being
// offline shouldn't break the discovery UI.
function fetchDiscoveredPeers() {
  var devices = CanaryStorage.getDevices();
  var requests = devices.map(function (d) {
    return CanaryAPI.request(d.base_url, '/api/v1/peers')
      .then(function (data) { return (data && data.peers) || []; })
      .catch(function () { return []; });
  });

  return Promise.all(requests).then(function (results) {
    var known = collectKnownIdentities(CanaryStorage.getDevices());
    var seen = {};
    var discovered = [];

    for (var i = 0; i < results.length; i++) {
      var peers = results[i];
      for (var j = 0; j < peers.length; j++) {
        var p = peers[j];
        if (!p || !p.device_id) continue;
        if (known.ids[p.device_id]) continue;
        var baseUrl = peerBaseUrl(p);
        if (!baseUrl || known.urls[baseUrl]) continue;
        if (seen[p.device_id]) continue;
        seen[p.device_id] = true;
        discovered.push(p);
      }
    }

    return discovered;
  });
}

function refreshDiscoveredPeers() {
  var slot = document.getElementById('discovered-peers');
  if (!slot) return;

  fetchDiscoveredPeers().then(function (discovered) {
    var slotNow = document.getElementById('discovered-peers');
    if (!slotNow) return; // user navigated away

    while (slotNow.firstChild) slotNow.removeChild(slotNow.firstChild);
    if (discovered.length === 0) return;

    slotNow.appendChild(renderDiscoveredPeerSection(discovered));
  });
}

function renderDiscoveredPeerSection(peers) {
  var section = el('div', { className: 'card mt-20' });
  section.appendChild(el('div', {
    className: 'card-title mb-8',
    textContent: 'Discovered on your network',
  }));
  section.appendChild(el('div', {
    className: 'card-subtitle mb-8',
    textContent: 'Other Canaries are advertising on your home WiFi. Add them here.',
  }));

  peers.forEach(function (peer) {
    var baseUrl = peerBaseUrl(peer);
    var typeInfo = deviceTypeInfo(peerDeviceType(peer));
    var row = el('div', { className: 'card' }, [
      el('div', { className: 'card-header' }, [
        el('div', {}, [
          el('div', { className: 'card-title', textContent: peer.name || peer.device_id }),
          renderDeviceTypeBadge(typeInfo.dt),
          el('div', { className: 'card-subtitle', textContent: baseUrl || peer.device_id }),
          typeInfo.tagline
            ? el('div', { className: 'device-type-tagline', textContent: typeInfo.tagline })
            : null,
        ]),
        el('span', { className: 'status-dot' }),
      ]),
      el('button', {
        className: 'btn btn-primary btn-block mt-12',
        textContent: 'Pair this Canary',
        onClick: function () {
          // Pre-fill the Add form host (and type, so the wizard can pick
          // the right branch) — the user only needs to paste this
          // device's token. The device_id is recovered from /api/v1/info
          // during the pairing request, so we don't stash it separately.
          try {
            sessionStorage.setItem('canary_prefill_host', baseUrl);
            sessionStorage.setItem('canary_prefill_peer', JSON.stringify(peer));
          } catch (e) { /* ignore */ }
          Router.navigate('#/canaries/add');
        },
      }),
    ]);
    section.appendChild(row);
  });

  return section;
}

// --------------- Add Canary Wizard ---------------
//
// Pairing leads with the zero-typing path: pick (or discover) a device,
// short-tap its BOOT button, and the app captures the provisioning
// receipt itself — the same physical gate the device's own dashboard
// uses. Typing a token is the last resort, not the front door.

var ROOM_SUGGESTIONS = ['Entry', 'Living Room', 'Kitchen', 'Bedroom', 'Garage', 'Office'];
var RECEIPT_POLL_INTERVAL_MS = 2000;
var RECEIPT_POLL_TICKS = 30; // x 2 s = 60 s listening window

// SECURITY NOTE: HTTP is used for private-network device communication.
// isPrivateUrl() ensures only RFC 1918 / link-local addresses are
// accepted; public addresses are rejected before any request is made.
// The device API requires X-Canary-Token auth and CORS/PNA middleware
// provides additional browser-level isolation.
function normalizeBaseUrl(host) {
  // Receipts arrive from QR codes and pasted JSON — host may be any type.
  if (typeof host !== 'string') return '';
  var baseUrl = host.trim();
  if (!baseUrl) return '';
  if (baseUrl.indexOf('http') !== 0) baseUrl = 'http://' + baseUrl;
  return baseUrl.replace(/\/+$/, '');
}

// Poll GET /api/provisioning-receipt until the user short-taps BOOT on
// the device. The tap opens a short-lived gate (advertised in the 403
// body's gate_ttl_seconds), so a press anywhere in our 60 s window lands.
// Returns a handle with cancel(); callers must cancel on navigation.
function startReceiptCapture(baseUrl, handlers) {
  var canceled = false;
  var gateTtl = 30;

  function tick(remaining) {
    if (canceled) return;
    CanaryAPI.request(baseUrl, '/api/provisioning-receipt')
      .then(function (receipt) {
        if (canceled) return;
        handlers.onReceipt(receipt);
      })
      .catch(function (err) {
        if (canceled) return;
        if (err && err.status === 404) {
          // Firmware predates the BOOT gate — fall back to manual entry.
          handlers.onUnsupported();
          return;
        }
        if (err && err.status === 403 && err.data && Number.isFinite(err.data.gate_ttl_seconds)) {
          gateTtl = err.data.gate_ttl_seconds;
        }
        if (remaining <= 1) {
          handlers.onTimeout();
          return;
        }
        handlers.onTick(Math.round((remaining - 1) * RECEIPT_POLL_INTERVAL_MS / 1000), gateTtl);
        setTimeout(function () { tick(remaining - 1); }, RECEIPT_POLL_INTERVAL_MS);
      });
  }

  handlers.onTick(Math.round(RECEIPT_POLL_TICKS * RECEIPT_POLL_INTERVAL_MS / 1000), gateTtl);
  tick(RECEIPT_POLL_TICKS);

  return {
    cancel: function () { canceled = true; },
  };
}

// Validate the credentials against the device and persist it. Every
// pairing path (BOOT-tap, QR, pasted receipt, manual token) funnels
// through here so dedupe and storage behave identically.
function completePairing(baseUrl, token, expectedDeviceId) {
  return CanaryAPI.request(baseUrl, '/api/v1/info', { token: token })
    .then(function (info) {
      var id = expectedDeviceId || info.device_id;
      var deviceType = canonicalDeviceType(info.device_type || '');
      var existing = CanaryStorage.getDevice(id);
      if (existing) {
        // Refresh credentials but tell the caller — likely the user
        // tapped BOOT on a box they already paired.
        var updates = { token: token, base_url: baseUrl, last_info: info };
        if (deviceType) updates.device_type = deviceType;
        CanaryStorage.updateDevice(id, updates);
        var dup = new Error('This Canary is already paired');
        dup.code = 'already_paired';
        dup.device = CanaryStorage.getDevice(id);
        throw dup;
      }
      var device = {
        id: id,
        name: info.name || id,
        base_url: baseUrl,
        token: token,
        device_type: deviceType,
        last_info: info,
        added_at: new Date().toISOString(),
      };
      CanaryStorage.addDevice(device);
      return CanaryStorage.getDevice(id);
    });
}

// Rename on the device when the firmware supports it, falling back to a
// local-only label otherwise. Resolves with the stored device either way.
function applyDeviceName(device, newName) {
  return CanaryAPI.request(device.base_url, '/api/device-name', {
    method: 'POST',
    token: device.token,
    body: { name: newName },
  }).then(function (resp) {
    var updates = { name: resp.device_name || newName };
    // If we reach the device by its mDNS name, follow the rename — the
    // old hostname stops resolving once mDNS re-announces. Preserve any
    // explicit port. Device traffic is http-only (the device CSP's
    // connect-src has no https sources), so only http URLs are rewritten.
    var hostMatch = device.base_url.match(/^(http:\/\/)canary-[a-z0-9-]+\.local(:[0-9]+)?$/);
    if (hostMatch && resp.mdns_host) {
      updates.base_url = hostMatch[1] + resp.mdns_host + '.local' + (hostMatch[2] || '');
    }
    CanaryStorage.updateDevice(device.id, updates);
    return CanaryStorage.getDevice(device.id);
  }).catch(function () {
    // Older firmware (404) or a transient error — keep the label locally
    // so the app still shows the user's name.
    CanaryStorage.updateDevice(device.id, { name: newName });
    return CanaryStorage.getDevice(device.id);
  });
}

function pairingErrorMessage(err) {
  if (!err) return 'Connection failed';
  if (err.code === 'already_paired') return 'This Canary is already paired';
  if (err.status === 401) return 'Invalid token. Check your API token and try again.';
  if (err.status === 429) return 'Rate limited. Try again in ' + ((err.data && err.data.retry_after) || 60) + ' seconds.';
  if (err.status === 0) return 'Device offline or unreachable.';
  return err.message || 'Connection failed';
}

// QR pairing needs a secure context for camera access, which a LAN-HTTP
// PWA usually doesn't have — so the button only appears when it can work.
function canScanQr() {
  return !!(window.isSecureContext &&
            window.BarcodeDetector &&
            navigator.mediaDevices &&
            navigator.mediaDevices.getUserMedia);
}

function openQrScanSheet(onReceipt) {
  var stream = null;
  var done = false;

  var video = el('video', { className: 'qr-video' });
  video.setAttribute('playsinline', '');
  var sheet = el('div', { className: 'qr-sheet' }, [
    el('div', { className: 'qr-sheet-title', textContent: 'Scan the pairing QR' }),
    el('div', { className: 'qr-sheet-hint', textContent: 'On the Canary dashboard, open Settings → Device → Show QR.' }),
    video,
    el('button', {
      className: 'btn btn-secondary btn-block mt-12',
      textContent: 'Cancel',
      onClick: close,
    }),
  ]);

  function close() {
    done = true;
    if (stream) {
      stream.getTracks().forEach(function (t) { t.stop(); });
    }
    if (sheet.parentNode) sheet.parentNode.removeChild(sheet);
  }

  document.body.appendChild(sheet);

  navigator.mediaDevices.getUserMedia({ video: { facingMode: 'environment' } })
    .then(function (s) {
      if (done) {
        s.getTracks().forEach(function (t) { t.stop(); });
        return;
      }
      stream = s;
      video.srcObject = s;
      video.play();

      // Constructing BarcodeDetector can throw on platforms that expose
      // the interface but don't support the requested format.
      var detector;
      try {
        detector = new window.BarcodeDetector({ formats: ['qr_code'] });
      } catch (e) {
        close();
        return;
      }
      function detectFrame() {
        if (done) return;
        detector.detect(video).then(function (codes) {
          if (done) return;
          for (var i = 0; i < codes.length; i++) {
            try {
              var receipt = JSON.parse(codes[i].rawValue);
              if (receipt && (receipt.token || receipt.api_token)) {
                close();
                onReceipt(receipt);
                return;
              }
            } catch (e) { /* not our QR — keep looking */ }
          }
          requestAnimationFrame(detectFrame);
        }).catch(function () {
          if (!done) requestAnimationFrame(detectFrame);
        });
      }
      requestAnimationFrame(detectFrame);
    })
    .catch(function () {
      close();
    });
}

// Room chips shared by the wizard and the device detail page. Rooms are
// purely client-side labels (see CanaryStorage.setDeviceRoom).
function renderRoomPicker(initialRoom, onChange) {
  var selected = initialRoom || '';

  var customInput = el('input', {
    className: 'form-input mt-12',
    type: 'text',
    placeholder: 'Room name',
  });
  customInput.style.display = 'none';
  customInput.addEventListener('input', function () {
    selected = customInput.value.trim();
    onChange(selected);
  });

  var chipRow = el('div', { className: 'room-chips' });

  function refreshChips() {
    Array.prototype.forEach.call(chipRow.children, function (chip) {
      var isActive = chip.getAttribute('data-room') === selected ||
        (chip.getAttribute('data-room') === '__custom__' && customInput.style.display !== 'none');
      chip.className = isActive ? 'room-chip active' : 'room-chip';
    });
  }

  ROOM_SUGGESTIONS.forEach(function (room) {
    var chip = el('button', { className: 'room-chip', textContent: room });
    chip.setAttribute('data-room', room);
    chip.addEventListener('click', function () {
      customInput.style.display = 'none';
      selected = (selected === room) ? '' : room;
      onChange(selected);
      refreshChips();
    });
    chipRow.appendChild(chip);
  });

  var customChip = el('button', { className: 'room-chip', textContent: 'Custom…' });
  customChip.setAttribute('data-room', '__custom__');
  customChip.addEventListener('click', function () {
    customInput.style.display = '';
    customInput.focus();
    selected = customInput.value.trim();
    onChange(selected);
    refreshChips();
  });
  chipRow.appendChild(customChip);

  if (selected && ROOM_SUGGESTIONS.indexOf(selected) === -1) {
    customInput.style.display = '';
    customInput.value = selected;
  }
  refreshChips();

  return el('div', {}, [chipRow, customInput]);
}

function renderAddCanaryView() {
  var app = clearApp();
  app.appendChild(renderHeader('Add Canary', true, false));

  var content = el('div', { className: 'content' });
  app.appendChild(content);

  var capture = null;

  // One-shot prefill from "Pair this Canary" in the discovered list —
  // the host (and peer record, when known) are already known, so jump
  // straight to the right branch for the device's type.
  var prefillHost = '';
  var prefillPeer = null;
  try {
    prefillHost = sessionStorage.getItem('canary_prefill_host') || '';
    sessionStorage.removeItem('canary_prefill_host');
    var rawPeer = sessionStorage.getItem('canary_prefill_peer');
    sessionStorage.removeItem('canary_prefill_peer');
    if (rawPeer) prefillPeer = JSON.parse(rawPeer);
  } catch (e) { /* ignore */ }

  function setStep(renderStep) {
    if (capture) { capture.cancel(); capture = null; }
    while (content.firstChild) content.removeChild(content.firstChild);
    var alertSlot = el('div', { id: 'add-alert' });
    content.appendChild(alertSlot);
    renderStep(alertSlot);
  }

  function showError(alertSlot, err) {
    while (alertSlot.firstChild) alertSlot.removeChild(alertSlot.firstChild);
    alertSlot.appendChild(el('div', {
      className: 'alert alert-error',
      textContent: pairingErrorMessage(err),
    }));
  }

  function handleReceipt(alertSlot, receipt, knownBaseUrl) {
    if (!receipt || typeof receipt !== 'object') {
      showError(alertSlot, new Error('Invalid receipt format'));
      return;
    }
    var baseUrl = knownBaseUrl || normalizeBaseUrl(receipt.base_url || receipt.host);
    var token = receipt.token || receipt.api_token;
    if (!baseUrl || !isPrivateUrl(baseUrl)) {
      showError(alertSlot, new Error('Only private network addresses are allowed'));
      return;
    }
    if (!token) {
      showError(alertSlot, new Error('Receipt is missing a token'));
      return;
    }
    completePairing(baseUrl, token, receipt.device_id)
      .then(function (device) { setStep(function () { stepConfirm(device); }); })
      .catch(function (err) {
        if (err.code === 'already_paired' && err.device) {
          stepAlreadyPaired(err.device);
          return;
        }
        showError(alertSlot, err);
      });
  }

  // ---- Step 1: choose a device ----
  function stepChoose(alertSlot) {
    content.appendChild(el('div', { className: 'pair-step-title', textContent: 'Power up your Canary' }));
    content.appendChild(el('div', {
      className: 'pair-step-subtitle',
      textContent: 'Plug it into USB power. Once it joins your WiFi it appears here.',
    }));

    var discoveredSlot = el('div', {});
    content.appendChild(discoveredSlot);
    var searching = el('div', { className: 'pair-searching' }, [
      el('div', { className: 'spinner' }),
      el('span', { textContent: 'Looking for Canaries on your network…' }),
    ]);
    if (CanaryStorage.getDevices().length > 0) {
      discoveredSlot.appendChild(searching);
      fetchDiscoveredPeers().then(function (peers) {
        if (!document.body.contains(discoveredSlot)) return;
        while (discoveredSlot.firstChild) discoveredSlot.removeChild(discoveredSlot.firstChild);
        peers.forEach(function (peer) {
          var baseUrl = peerBaseUrl(peer);
          if (!baseUrl) return;
          var typeInfo = deviceTypeInfo(peerDeviceType(peer));
          var row = el('div', { className: 'card cursor-pointer' }, [
            el('div', { className: 'card-header' }, [
              el('div', {}, [
                el('div', { className: 'card-title', textContent: peer.name || peer.device_id }),
                renderDeviceTypeBadge(typeInfo.dt),
                el('div', { className: 'card-subtitle', textContent: baseUrl }),
                typeInfo.tagline
                  ? el('div', { className: 'device-type-tagline', textContent: typeInfo.tagline })
                  : null,
              ]),
              el('span', { className: 'arrow', textContent: '›' }),
            ]),
          ]);
          row.addEventListener('click', function () {
            // Vision/sense have no HTTP server — the BOOT-tap flow would
            // dead-end, so route them to the Home Assistant path instead.
            if (wizardPathForPeer(peer) === 'mqtt') {
              setStep(function () { stepMqttDevice(peer); });
            } else {
              setStep(function (slot) { stepCapture(slot, baseUrl); });
            }
          });
          discoveredSlot.appendChild(row);
        });
        if (peers.length === 0) {
          discoveredSlot.appendChild(el('div', {
            className: 'pair-searching',
            textContent: 'No new Canaries found yet — enter its address below.',
          }));
        }
      });
    }

    var hostInput = el('input', {
      className: 'form-input',
      type: 'text',
      placeholder: 'e.g. canary-a3f7.local or 192.168.1.47',
    });
    content.appendChild(el('div', { className: 'form-group mt-12' }, [
      el('label', { className: 'form-label', textContent: 'Device address' }),
      hostInput,
    ]));
    content.appendChild(el('button', {
      className: 'btn btn-primary btn-block',
      textContent: 'Continue',
      onClick: function () {
        var baseUrl = normalizeBaseUrl(hostInput.value);
        if (!baseUrl) {
          showError(alertSlot, new Error('Device address is required'));
          return;
        }
        if (!isPrivateUrl(baseUrl)) {
          showError(alertSlot, new Error('Only private network addresses are allowed'));
          return;
        }
        setStep(function (slot) { stepCapture(slot, baseUrl); });
      },
    }));

    content.appendChild(renderFallbackExpander(alertSlot, ''));
  }

  // ---- Step 2: BOOT-tap capture ----
  function stepCapture(alertSlot, baseUrl) {
    var statusLine = el('div', { className: 'pair-status', textContent: 'Listening…' });
    var hero = el('div', { className: 'card pair-hero' }, [
      el('div', { className: 'pair-pulse' }, [
        el('div', { className: 'pair-pulse-core' }),
      ]),
      el('div', { className: 'pair-step-title', textContent: 'Short-tap the BOOT button' }),
      el('div', {
        className: 'pair-step-subtitle',
        textContent: 'One tap on the small button on your Canary pairs it — no codes to type.',
      }),
      el('div', { className: 'card-subtitle', textContent: baseUrl }),
      statusLine,
    ]);
    content.appendChild(hero);
    content.appendChild(el('button', {
      className: 'btn btn-secondary btn-block mt-12',
      textContent: 'Cancel',
      onClick: function () { setStep(stepChoose); },
    }));

    capture = startReceiptCapture(baseUrl, {
      onTick: function (secondsLeft, gateTtl) {
        if (!document.body.contains(hero)) {
          if (capture) capture.cancel();
          return;
        }
        statusLine.textContent = 'Listening for ' + secondsLeft + 's — the device holds a tap for ' + gateTtl + 's.';
      },
      onReceipt: function (receipt) {
        statusLine.textContent = 'Pairing…';
        handleReceipt(alertSlot, receipt, baseUrl);
      },
      onTimeout: function () {
        if (!document.body.contains(hero)) return;
        while (content.firstChild) content.removeChild(content.firstChild);
        content.appendChild(alertSlot);
        content.appendChild(el('div', { className: 'card pair-hero' }, [
          el('div', { className: 'pair-step-title', textContent: "Didn't catch a tap" }),
          el('div', {
            className: 'pair-step-subtitle',
            textContent: 'Make sure the Canary is powered and try again.',
          }),
        ]));
        content.appendChild(el('button', {
          className: 'btn btn-primary btn-block mt-12',
          textContent: 'Try again',
          onClick: function () { setStep(function (slot) { stepCapture(slot, baseUrl); }); },
        }));
        content.appendChild(el('button', {
          className: 'btn btn-secondary btn-block mt-12',
          textContent: 'Back',
          onClick: function () { setStep(stepChoose); },
        }));
        content.appendChild(renderFallbackExpander(alertSlot, baseUrl));
      },
      onUnsupported: function () {
        if (!document.body.contains(hero)) return;
        while (content.firstChild) content.removeChild(content.firstChild);
        content.appendChild(alertSlot);
        content.appendChild(el('div', {
          className: 'alert alert-warning',
          textContent: "This Canary's firmware doesn't support one-tap pairing. Use its API token instead.",
        }));
        var fallback = renderFallbackExpander(alertSlot, baseUrl);
        content.appendChild(fallback);
        var toggle = fallback.querySelector('.pair-expander-toggle');
        if (toggle) toggle.click();
        content.appendChild(el('button', {
          className: 'btn btn-secondary btn-block mt-12',
          textContent: 'Back',
          onClick: function () { setStep(stepChoose); },
        }));
      },
    });
  }

  // ---- MQTT-only devices (vision/sense): no HTTP pairing ----
  // These sensors carry no HTTP server, so there is nothing to pair here.
  // They join through Home Assistant automatically via MQTT discovery;
  // this step explains the type and walks the user to a live proof.
  function stepMqttDevice(peer) {
    var info = deviceTypeInfo(peerDeviceType(peer));

    content.appendChild(el('div', { className: 'card pair-hero' }, [
      el('div', { className: 'pair-type-icon' }, [deviceTypeIcon(info)]),
      el('div', { className: 'pair-step-title', textContent: peer.name || peer.device_id }),
      el('div', { className: 'pair-step-subtitle', textContent: info.tagline }),
      el('div', { className: 'card-subtitle', textContent: peerBaseUrl(peer) || peer.device_id }),
    ]));

    content.appendChild(renderDeviceTypeCard(info));

    content.appendChild(el('div', { className: 'card' }, [
      el('div', { className: 'card-title mb-8', textContent: 'It joins through Home Assistant' }),
      el('ol', { className: 'pair-guide-steps' }, [
        el('li', { textContent: 'It’s already broadcasting on your network — no button to press.' }),
        el('li', { textContent: 'Open Home Assistant: it appears as a device with its entities, no setup needed.' }),
        el('li', { textContent: 'Press its “Identify” button in Home Assistant — the sensor blinks for 10 seconds so you know which one it is.' }),
        el('li', { textContent: info.proof }),
      ]),
    ]));

    content.appendChild(el('button', {
      className: 'btn btn-primary btn-block mt-12',
      textContent: 'Back to devices',
      onClick: function () { Router.navigate('#/canaries'); },
    }));
    content.appendChild(el('button', {
      className: 'btn btn-secondary btn-block mt-12',
      textContent: 'Pair a different Canary',
      onClick: function () { setStep(stepChoose); },
    }));
  }

  // ---- Fallbacks: QR scan, pasted receipt, manual token ----
  function renderFallbackExpander(alertSlot, prefillBaseUrl) {
    var body = el('div', { className: 'pair-expander-body' });
    body.style.display = 'none';

    if (canScanQr()) {
      body.appendChild(el('button', {
        className: 'btn btn-secondary btn-block',
        textContent: 'Scan pairing QR',
        onClick: function () {
          openQrScanSheet(function (receipt) {
            handleReceipt(alertSlot, receipt, '');
          });
        },
      }));
    }

    var receiptInput = el('textarea', {
      className: 'form-input',
      rows: '4',
      placeholder: '{"device_id": "...", "base_url": "...", "token": "..."}',
    });
    body.appendChild(el('div', { className: 'form-group mt-12' }, [
      el('label', { className: 'form-label', textContent: 'Paste provisioning receipt JSON' }),
      receiptInput,
    ]));
    body.appendChild(el('button', {
      className: 'btn btn-secondary btn-block',
      textContent: 'Add from receipt',
      onClick: function () {
        var receipt;
        try {
          receipt = JSON.parse(receiptInput.value.trim());
        } catch (e) {
          showError(alertSlot, new Error('Invalid receipt JSON'));
          return;
        }
        handleReceipt(alertSlot, receipt, '');
      },
    }));

    var hostInput = el('input', {
      className: 'form-input',
      type: 'text',
      placeholder: 'e.g. canary-a3f7.local or 192.168.1.47',
      value: prefillBaseUrl || '',
    });
    var tokenInput = el('input', {
      className: 'form-input',
      type: 'text',
      placeholder: 'cv_xxxx_...',
    });
    body.appendChild(el('div', { className: 'form-group mt-20' }, [
      el('label', { className: 'form-label', textContent: 'Device address' }),
      hostInput,
    ]));
    body.appendChild(el('div', { className: 'form-group' }, [
      el('label', { className: 'form-label', textContent: 'API token' }),
      tokenInput,
    ]));
    body.appendChild(el('button', {
      className: 'btn btn-secondary btn-block',
      textContent: 'Add with token',
      onClick: function () {
        var baseUrl = normalizeBaseUrl(hostInput.value);
        var token = tokenInput.value.trim();
        if (!baseUrl) { showError(alertSlot, new Error('Device address is required')); return; }
        if (!isPrivateUrl(baseUrl)) { showError(alertSlot, new Error('Only private network addresses are allowed')); return; }
        if (!token) { showError(alertSlot, new Error('API token is required')); return; }
        handleReceipt(alertSlot, { token: token }, baseUrl);
      },
    }));

    var toggle = el('button', {
      className: 'pair-expander-toggle',
      textContent: 'Other ways to pair',
      onClick: function () {
        body.style.display = body.style.display === 'none' ? '' : 'none';
      },
    });

    return el('div', { className: 'pair-expander mt-20' }, [toggle, body]);
  }

  // ---- Already paired ----
  function stepAlreadyPaired(device) {
    while (content.firstChild) content.removeChild(content.firstChild);
    content.appendChild(el('div', { className: 'card pair-hero' }, [
      el('div', { className: 'pair-step-title', textContent: 'Already paired' }),
      el('div', {
        className: 'pair-step-subtitle',
        textContent: (device.name || device.id) + ' is already in your fleet. Not the box you meant? Blink it to check.',
      }),
      el('button', {
        className: 'btn btn-secondary btn-block mt-12',
        textContent: 'Blink ' + (device.name || device.id),
        onClick: function () {
          CanaryAPI.request(device.base_url, '/api/identify', {
            method: 'POST',
            token: device.token,
            body: { duration_ms: 15000 },
          }).catch(function () { /* best effort */ });
        },
      }),
    ]));
    content.appendChild(el('button', {
      className: 'btn btn-primary btn-block mt-12',
      textContent: 'Pair a different Canary',
      onClick: function () { setStep(stepChoose); },
    }));
    content.appendChild(el('button', {
      className: 'btn btn-secondary btn-block mt-12',
      textContent: 'Done',
      onClick: function () { Router.navigate('#/canaries'); },
    }));
  }

  // ---- Step 3: confirm, name, locate ----
  function stepConfirm(device) {
    var selectedRoom = device.room || '';
    var nameTouched = false;
    // HTTP-paired devices without a device_type are WAP-class by
    // definition — vision/sense never reach this step.
    var typeInfo = deviceTypeInfo(device.device_type || 'canary-wap');

    content.appendChild(el('div', { className: 'card pair-hero' }, [
      el('div', { className: 'pair-check', textContent: '✓' }),
      el('div', { className: 'pair-step-title', textContent: typeInfo.label + ' paired' }),
      el('div', { className: 'pair-step-subtitle', textContent: typeInfo.tagline }),
      el('div', {
        className: 'card-subtitle',
        textContent: device.id + (device.last_info && device.last_info.firmware_version
          ? ' · v' + device.last_info.firmware_version : ''),
      }),
    ]));

    content.appendChild(renderDeviceTypeCard(typeInfo));

    // Blink the device so the user knows which physical box just joined —
    // essential when unboxing several identical units.
    var blinkBtn = el('button', {
      className: 'btn btn-secondary btn-block mt-12',
      textContent: 'Blink to confirm it’s this one',
    });
    blinkBtn.addEventListener('click', function () {
      blinkBtn.disabled = true;
      blinkBtn.textContent = 'Look for the blinking light…';
      CanaryAPI.request(device.base_url, '/api/identify', {
        method: 'POST',
        token: device.token,
        body: { duration_ms: 15000 },
      }).then(function (resp) {
        setTimeout(function () {
          blinkBtn.disabled = false;
          blinkBtn.textContent = 'Blink to confirm it’s this one';
        }, (resp && resp.duration_ms) || 15000);
      }).catch(function (err) {
        if (err && err.status === 404) {
          // Older firmware — hide the affordance rather than erroring.
          if (blinkBtn.parentNode) blinkBtn.parentNode.removeChild(blinkBtn);
          return;
        }
        blinkBtn.disabled = false;
        blinkBtn.textContent = 'Blink to confirm it’s this one';
      });
    });
    content.appendChild(blinkBtn);

    var nameInput = el('input', {
      className: 'form-input',
      type: 'text',
      placeholder: 'e.g. kitchen',
      value: device.name !== device.id ? device.name : '',
    });
    nameInput.addEventListener('input', function () { nameTouched = true; });
    content.appendChild(el('div', { className: 'form-group mt-20' }, [
      el('label', { className: 'form-label', textContent: 'Name this Canary' }),
      nameInput,
    ]));

    content.appendChild(el('div', { className: 'form-group' }, [
      el('label', { className: 'form-label', textContent: 'Which room is it watching?' }),
      renderRoomPicker(selectedRoom, function (room) {
        selectedRoom = room;
        // Polish: suggest "<Room> <Type label>" as the device name until
        // the user types their own.
        if (!nameTouched && room) {
          nameInput.value = room + ' ' + typeInfo.label;
        }
      }),
    ]));

    content.appendChild(el('button', {
      className: 'btn btn-primary btn-block mt-12',
      textContent: 'Save & continue',
      onClick: function () {
        var newName = nameInput.value.trim();
        CanaryStorage.setDeviceRoom(device.id, selectedRoom);

        if (!newName || newName === device.name) {
          setStep(function () { stepDone(CanaryStorage.getDevice(device.id)); });
          return;
        }

        applyDeviceName(device, newName).then(function (updated) {
          setStep(function () { stepDone(updated); });
        });
      },
    }));

    content.appendChild(el('button', {
      className: 'btn btn-secondary btn-block mt-12',
      textContent: 'Skip for now',
      onClick: function () { setStep(function () { stepDone(device); }); },
    }));
  }

  // ---- Step 4: done — invite the next one ----
  function stepDone(device) {
    var count = CanaryStorage.getDevices().length;
    var fleetLine;
    if (count === 1) {
      fleetLine = '1 Canary protecting your home. Most homes use 3–5 for full coverage — one per entry and shared space.';
    } else if (count < 3) {
      fleetLine = count + ' Canaries protecting your home. Most homes use 3–5 for full coverage.';
    } else {
      fleetLine = count + ' Canaries protecting your home. Your fleet is growing.';
    }

    content.appendChild(el('div', { className: 'card pair-hero' }, [
      el('div', { className: 'pair-check', textContent: '✓' }),
      el('div', {
        className: 'pair-step-title',
        textContent: (device.name || device.id) + ' is watching',
      }),
      el('div', { className: 'pair-step-subtitle', textContent: fleetLine }),
    ]));

    content.appendChild(el('button', {
      className: 'btn btn-primary btn-block mt-12',
      textContent: '+ Add another Canary',
      onClick: function () { setStep(stepChoose); },
    }));
    content.appendChild(el('button', {
      className: 'btn btn-secondary btn-block mt-12',
      textContent: 'Done',
      onClick: function () { Router.navigate('#/canaries'); },
    }));
  }

  if (prefillPeer && wizardPathForPeer(prefillPeer) === 'mqtt') {
    setStep(function () { stepMqttDevice(prefillPeer); });
    return;
  }
  if (prefillHost) {
    var prefillBase = normalizeBaseUrl(prefillHost);
    if (prefillBase && isPrivateUrl(prefillBase)) {
      setStep(function (slot) { stepCapture(slot, prefillBase); });
      return;
    }
  }
  setStep(stepChoose);
}

function renderDeviceView(deviceId) {
  var app = clearApp();
  var device = CanaryStorage.getDevice(deviceId);

  if (!device) {
    app.appendChild(renderHeader('Device Not Found', true, false));
    app.appendChild(el('div', { className: 'content' }, [
      el('div', { className: 'empty-state' }, [
        el('h2', { textContent: 'Device not found' }),
        el('p', { textContent: 'This device is not in your list.' }),
      ]),
    ]));
    return;
  }

  app.appendChild(renderHeader(device.name || device.id, true, false));

  var content = el('div', { className: 'content' });
  var alertArea = el('div', { id: 'device-alert' });
  content.appendChild(alertArea);

  // Type badge + tagline (unknown types render nothing)
  var typeInfo = deviceTypeInfo(device.device_type);
  if (typeInfo !== DEVICE_TYPES.unknown) {
    content.appendChild(el('div', { className: 'device-type-line' }, [
      renderDeviceTypeBadge(typeInfo.dt),
      el('span', { className: 'device-type-tagline', textContent: typeInfo.tagline }),
    ]));
  }

  // Stats grid (placeholder until loaded)
  var statsGrid = el('div', { className: 'stats-grid', id: 'device-stats' }, [
    el('div', { className: 'stat-item' }, [
      el('div', { className: 'stat-value', textContent: '--' }),
      el('div', { className: 'stat-label', textContent: 'Uptime' }),
    ]),
    el('div', { className: 'stat-item' }, [
      el('div', { className: 'stat-value', textContent: '--' }),
      el('div', { className: 'stat-label', textContent: 'WiFi RSSI' }),
    ]),
    el('div', { className: 'stat-item' }, [
      el('div', { className: 'stat-value', textContent: '--' }),
      el('div', { className: 'stat-label', textContent: 'Firmware' }),
    ]),
    el('div', { className: 'stat-item' }, [
      el('div', { className: 'stat-value', textContent: '--' }),
      el('div', { className: 'stat-label', textContent: 'Model' }),
    ]),
  ]);
  content.appendChild(statsGrid);

  // Identity: room label, physical locate, rename. Same primitives the
  // pairing wizard uses, so they're not setup-only.
  var blinkBtn = el('button', {
    className: 'btn btn-secondary btn-block mt-12',
    textContent: 'Blink to find this Canary',
  });
  blinkBtn.addEventListener('click', function () {
    blinkBtn.disabled = true;
    blinkBtn.textContent = 'Look for the blinking light…';
    CanaryAPI.request(device.base_url, '/api/identify', {
      method: 'POST',
      token: device.token,
      body: { duration_ms: 15000 },
    }).then(function (resp) {
      setTimeout(function () {
        blinkBtn.disabled = false;
        blinkBtn.textContent = 'Blink to find this Canary';
      }, (resp && resp.duration_ms) || 15000);
    }).catch(function (err) {
      if (err && err.status === 404 && blinkBtn.parentNode) {
        blinkBtn.parentNode.removeChild(blinkBtn);
        return;
      }
      blinkBtn.disabled = false;
      blinkBtn.textContent = 'Blink to find this Canary';
    });
  });

  var renameInput = el('input', {
    className: 'form-input',
    type: 'text',
    placeholder: 'e.g. kitchen',
    value: device.name || '',
  });
  var renameBtn = el('button', {
    className: 'btn btn-secondary btn-block mt-12',
    textContent: 'Rename',
  });
  renameBtn.addEventListener('click', function () {
    var newName = renameInput.value.trim();
    if (!newName || newName === device.name) return;
    renameBtn.disabled = true;
    renameBtn.textContent = 'Renaming…';
    applyDeviceName(device, newName).then(function () {
      renderDeviceView(deviceId);
    });
  });

  content.appendChild(el('div', { className: 'card' }, [
    el('div', { className: 'card-title mb-8', textContent: 'Identity' }),
    el('div', { className: 'form-group' }, [
      el('label', { className: 'form-label', textContent: 'Room' }),
      renderRoomPicker(device.room || '', function (room) {
        CanaryStorage.setDeviceRoom(deviceId, room);
      }),
    ]),
    el('div', { className: 'form-group' }, [
      el('label', { className: 'form-label', textContent: 'Name' }),
      renameInput,
    ]),
    renameBtn,
    blinkBtn,
  ]));

  // Config sections
  var sections = el('div', { className: 'card' }, [
    el('div', { className: 'card-title mb-8', textContent: 'Configuration' }),
    el('ul', { className: 'section-list' }, [
      el('li', {}, [el('a', { href: '#/device/' + deviceId + '/config/network' }, [
        'Network', el('span', { className: 'arrow', textContent: '\u203A' })
      ])]),
      el('li', {}, [el('a', { href: '#/device/' + deviceId + '/config/privacy' }, [
        'Privacy', el('span', { className: 'arrow', textContent: '\u203A' })
      ])]),
      el('li', {}, [el('a', { href: '#/device/' + deviceId + '/config/detection' }, [
        'Detection', el('span', { className: 'arrow', textContent: '\u203A' })
      ])]),
      el('li', {}, [el('a', { href: '#/device/' + deviceId + '/config/integrations' }, [
        'Integrations', el('span', { className: 'arrow', textContent: '\u203A' })
      ])]),
    ]),
  ]);
  content.appendChild(sections);

  // Logs & Witness links
  content.appendChild(el('div', { className: 'card cursor-pointer' }, [
    el('a', { href: '#/device/' + deviceId + '/logs', className: 'card-link' }, [
      el('span', { textContent: 'View Logs' }),
      el('span', { className: 'arrow', textContent: '\u203A' }),
    ]),
  ]));
  content.appendChild(el('div', { className: 'card cursor-pointer' }, [
    el('a', { href: '#/device/' + deviceId + '/witness', className: 'card-link' }, [
      el('span', { textContent: 'Witness Chain' }),
      el('span', { className: 'arrow', textContent: '\u203A' }),
    ]),
  ]));

  // Token display
  content.appendChild(el('div', { className: 'card' }, [
    el('div', { className: 'card-title mb-8', textContent: 'API Token' }),
    el('div', { className: 'token-display', textContent: maskToken(device.token) }),
  ]));

  // Remove button
  content.appendChild(el('button', {
    className: 'btn btn-danger btn-block mt-20',
    textContent: 'Remove Device',
    onClick: function () {
      if (confirm('Remove this device?')) {
        CanaryStorage.removeDevice(deviceId);
        Router.navigate('#/canaries');
      }
    },
  }));

  app.appendChild(content);

  // Fetch live info
  CanaryAPI.request(device.base_url, '/api/v1/info')
    .then(function (info) {
      var updates = { last_info: info };
      // Backfill the type for devices paired before device_type existed.
      if (info.device_type) updates.device_type = canonicalDeviceType(info.device_type);
      CanaryStorage.updateDevice(deviceId, updates);
      var grid = document.getElementById('device-stats');
      if (grid) {
        var uptimeHrs = Math.floor((info.uptime_s || 0) / 3600);
        var items = grid.querySelectorAll('.stat-item');
        items[0].querySelector('.stat-value').textContent = uptimeHrs + 'h';
        items[1].querySelector('.stat-value').textContent = info.wifi_rssi + ' dBm';
        items[2].querySelector('.stat-value').textContent = 'v' + info.firmware_version;
        items[3].querySelector('.stat-value').textContent = info.model;
      }
    })
    .catch(function (err) {
      var alertArea2 = document.getElementById('device-alert');
      if (alertArea2) {
        alertArea2.appendChild(el('div', { className: 'alert alert-error' }, [
          err.status === 401 ? 'Invalid token.' : 'Device offline.',
          el('button', {
            className: 'btn btn-secondary ml-12',
            textContent: 'Retry',
            onClick: function () { renderDeviceView(deviceId); },
          }),
        ]));
      }
    });
}

function renderConfigView(deviceId, section) {
  var app = clearApp();
  var device = CanaryStorage.getDevice(deviceId);
  if (!device) { renderNotFound(); return; }

  var sectionNames = { network: 'Network', privacy: 'Privacy', detection: 'Detection', integrations: 'Integrations' };
  app.appendChild(renderHeader(sectionNames[section] || section, true, false));

  var content = el('div', { className: 'content' });
  var alertArea = el('div', { id: 'config-alert' });
  content.appendChild(alertArea);

  var formArea = el('div', { id: 'config-form' });
  formArea.appendChild(el('div', { className: 'loading' }, [
    el('div', { className: 'spinner' }),
    'Loading configuration…',
  ]));
  content.appendChild(formArea);
  app.appendChild(content);

  CanaryAPI.request(device.base_url, '/api/v1/config/' + section)
    .then(function (config) {
      while (formArea.firstChild) formArea.removeChild(formArea.firstChild);
      renderConfigForm(formArea, device, section, config);
    })
    .catch(function (err) {
      while (formArea.firstChild) formArea.removeChild(formArea.firstChild);
      formArea.appendChild(el('div', { className: 'alert alert-error', textContent: err.message }));
    });
}

function renderConfigForm(container, device, section, config) {
  var fields = Object.keys(config);
  var inputs = {};

  fields.forEach(function (key) {
    var value = config[key];
    var group = el('div', { className: 'form-group' });

    if (typeof value === 'boolean') {
      var row = el('div', { className: 'toggle-row' });
      row.appendChild(el('span', { className: 'toggle-label', textContent: labelFor(key) }));
      var toggle = el('label', { className: 'toggle' });
      var checkbox = el('input', { type: 'checkbox' });
      checkbox.checked = value;
      toggle.appendChild(checkbox);
      toggle.appendChild(el('span', { className: 'toggle-slider' }));
      row.appendChild(toggle);
      group.appendChild(row);
      inputs[key] = { type: 'boolean', el: checkbox };
    } else if (typeof value === 'number') {
      group.appendChild(el('label', { className: 'form-label', textContent: labelFor(key) }));
      var numInput = el('input', { className: 'form-input', type: 'number', value: String(value) });
      group.appendChild(numInput);
      inputs[key] = { type: 'number', el: numInput };
    } else if (typeof value === 'string') {
      group.appendChild(el('label', { className: 'form-label', textContent: labelFor(key) }));
      var textInput = el('input', { className: 'form-input', type: 'text', value: value });
      group.appendChild(textInput);
      inputs[key] = { type: 'string', el: textInput };
    } else {
      // Arrays/objects — show as read-only JSON
      group.appendChild(el('label', { className: 'form-label', textContent: labelFor(key) }));
      group.appendChild(el('div', { className: 'token-display', textContent: JSON.stringify(value) }));
      return;
    }

    container.appendChild(group);
  });

  container.appendChild(el('button', {
    className: 'btn btn-primary btn-block',
    textContent: 'Save Changes',
    onClick: function () {
      var body = {};
      Object.keys(inputs).forEach(function (key) {
        var inp = inputs[key];
        if (inp.type === 'boolean') body[key] = inp.el.checked;
        else if (inp.type === 'number') {
          var parsed = parseInt(inp.el.value, 10);
          body[key] = isNaN(parsed) ? 0 : parsed;
        }
        else body[key] = inp.el.value;
      });

      var alertArea = document.getElementById('config-alert');
      while (alertArea.firstChild) alertArea.removeChild(alertArea.firstChild);

      CanaryAPI.request(device.base_url, '/api/v1/config/' + section, {
        method: 'PUT',
        body: body,
      })
        .then(function (res) {
          alertArea.appendChild(el('div', { className: 'alert alert-success', textContent: 'Configuration saved.' }));
          // The server strips API-immutable keys (camera_peek_enabled) and
          // reports them in rejected_immutable — surface that so the user
          // knows the toggle did not take.
          if (res.rejected_immutable && res.rejected_immutable.length > 0) {
            alertArea.appendChild(el('div', {
              className: 'alert alert-warning',
              textContent: 'Not applied: ' + res.rejected_immutable.join(', ') +
                ' — this setting can’t be changed from the app. It requires a physical button press on the device.',
            }));
          }
        })
        .catch(function (err) {
          alertArea.appendChild(el('div', { className: 'alert alert-error', textContent: err.message }));
        });
    },
  }));

  if (section === 'integrations') {
    container.appendChild(el('button', {
      className: 'btn btn-block',
      textContent: 'Test Webhook',
      style: 'margin-top: 0.5rem',
      onClick: function () {
        var alertArea = document.getElementById('config-alert');
        while (alertArea.firstChild) alertArea.removeChild(alertArea.firstChild);
        alertArea.appendChild(el('div', { className: 'alert', textContent: 'Sending test webhook…' }));

        CanaryAPI.request(device.base_url, '/api/v1/webhook/test', { method: 'POST' })
          .then(function (res) {
            while (alertArea.firstChild) alertArea.removeChild(alertArea.firstChild);
            alertArea.appendChild(el('div', {
              className: 'alert alert-success',
              textContent: 'Webhook test succeeded (HTTP ' + res.status + ')',
            }));
          })
          .catch(function (err) {
            while (alertArea.firstChild) alertArea.removeChild(alertArea.firstChild);
            alertArea.appendChild(el('div', {
              className: 'alert alert-error',
              textContent: 'Webhook test failed: ' + err.message,
            }));
          });
      },
    }));
  }
}

function renderLogsView(deviceId) {
  var app = clearApp();
  var device = CanaryStorage.getDevice(deviceId);
  if (!device) { renderNotFound(); return; }

  app.appendChild(renderHeader('Logs', true, false));

  var content = el('div', { className: 'content' });
  var alertArea = el('div', { id: 'logs-alert' });
  content.appendChild(alertArea);

  var logContainer = el('div', { id: 'log-list' });
  logContainer.appendChild(el('div', { className: 'loading' }, [
    el('div', { className: 'spinner' }),
    'Loading logs…',
  ]));
  content.appendChild(logContainer);
  app.appendChild(content);

  CanaryAPI.request(device.base_url, '/api/v1/logs?tail=200')
    .then(function (data) {
      while (logContainer.firstChild) logContainer.removeChild(logContainer.firstChild);
      if (data.logs.length === 0) {
        logContainer.appendChild(el('div', { className: 'empty-state', textContent: 'No logs available' }));
        return;
      }
      data.logs.forEach(function (log) {
        var entry = el('div', { className: 'log-entry' }, [
          el('span', { className: 'log-ts', textContent: log.ts.substring(11, 19) }),
          el('span', {
            className: 'log-level ' + (['INFO', 'WARN', 'ERROR', 'DEBUG'].indexOf(log.level) !== -1 ? log.level : 'INFO'),
            textContent: log.level
          }),
          el('span', { className: 'log-msg', textContent: log.msg }),
        ]);
        logContainer.appendChild(entry);
      });
    })
    .catch(function (err) {
      while (logContainer.firstChild) logContainer.removeChild(logContainer.firstChild);
      logContainer.appendChild(el('div', { className: 'alert alert-error', textContent: err.message }));
    });
}

// Collect the verified, coarsened events from a canonical envelope's signed sealed_events ledger,
// flattened and sorted by time bucket. We render from the SEALED LEDGER (the signed record), not
// the artifact projection, so the timeline reflects exactly what was cryptographically attested.
function envelopeTimelineEvents(envelope) {
  var entries = (envelope && envelope.ledgers && envelope.ledgers.sealed_events &&
    envelope.ledgers.sealed_events.entries) || [];
  var events = [];
  entries.forEach(function (entry) {
    var rec;
    try { rec = JSON.parse(entry.payload_json); } catch (e) { return; }
    if (!rec || typeof rec !== 'object') return;
    if (rec.record_type === 'failure' || rec.failure_type !== undefined) {
      events.push({ failure: true, type: rec.failure_type, time_bucket: rec.time_bucket });
    } else {
      events.push({
        failure: false,
        type: rec.event_type,
        zone: rec.zone_id,
        time_bucket: rec.time_bucket,
      });
    }
  });
  events.sort(function (a, b) {
    var sa = (a.time_bucket && a.time_bucket.start_epoch_s) || 0;
    var sb = (b.time_bucket && b.time_bucket.start_epoch_s) || 0;
    return sb - sa; // newest first
  });
  return events;
}

// Build the integrity banner from the device's shared-verifier report (the /witness/verify
// response carries an `evidence_envelope` block produced by viewer/verify_core.js server-side).
// `displayedDigest` is the whole_envelope_digest of the envelope actually being rendered: the
// integrity report and the timeline are built from two independent requests, so a witness record
// arriving between them could make the report describe a DIFFERENT envelope than the one shown. We
// only claim "verified" when the two digests match; otherwise we surface a mismatch warning and let
// the operator re-load, rather than painting a green banner over an unrelated timeline.
function renderEnvelopeIntegrityBanner(verifyReport, displayedDigest) {
  var ee = verifyReport && verifyReport.evidence_envelope;
  if (!ee) {
    return el('div', { className: 'alert alert-warning',
      textContent: 'Integrity status unavailable for this bundle.' });
  }
  var digestsMatch = !displayedDigest || !ee.whole_envelope_digest ||
    ee.whole_envelope_digest === displayedDigest;
  if (!digestsMatch) {
    return el('div', { className: 'alert alert-warning' }, [
      el('div', { className: 'timeline-label',
        textContent: '⚠ Integrity report does not match the displayed bundle' }),
      el('div', { className: 'timeline-time',
        textContent: 'The chain changed between fetching the timeline and verifying it. Reload to re-verify.' }),
    ]);
  }
  var cls = ee.ok ? (ee.status === 'ok' ? 'alert-success' : 'alert-warning') : 'alert-error';
  var label = ee.ok
    ? (ee.status === 'ok' ? '✓ Verified — integrity intact' : '✓ Verified — with notes')
    : '✗ Not verified — bundle rejected';
  var children = [el('div', { className: 'timeline-label', textContent: label })];
  if (ee.whole_envelope_digest) {
    children.push(el('div', {
      className: 'token-display',
      style: 'font-size: 0.7rem; word-break: break-all; margin-top: 0.25rem',
      textContent: 'fingerprint ' + ee.whole_envelope_digest,
    }));
  }
  if (!ee.ok && ee.error) {
    children.push(el('div', { className: 'timeline-time', textContent: ee.error }));
  }
  return el('div', { className: 'alert ' + cls }, children);
}

// Render the verified, privacy-coarsened timeline from a canonical envelope into `container`.
// `verifyReport` is the device's /witness/verify response (shared-verifier integrity result).
function renderVerifiedTimeline(container, envelope, verifyReport) {
  while (container.firstChild) container.removeChild(container.firstChild);

  // Bind the integrity claim to the envelope actually shown (guards against a chain change between
  // the /envelope and /verify requests).
  container.appendChild(renderEnvelopeIntegrityBanner(
    verifyReport, envelope && envelope.whole_envelope_digest));

  if (!envelope) {
    container.appendChild(el('div', { className: 'alert alert-error',
      textContent: 'No envelope data available.' }));
    return;
  }

  // Provenance summary — what produced this, and under which ruleset.
  var p = envelope.provenance || {};
  var prov = el('div', { className: 'card' }, [
    el('div', { className: 'card-title mb-8', textContent: 'Provenance' }),
    el('div', { className: 'card-subtitle', textContent: 'Kernel: ' + (p.kernel_version || '—') }),
    el('div', { className: 'card-subtitle', textContent: 'Ruleset: ' + (p.ruleset_id || '—') }),
    el('div', {
      className: 'token-display',
      style: 'font-size: 0.7rem; word-break: break-all; margin-top: 0.25rem',
      textContent: 'device key ' + (p.device_public_key || '—'),
    }),
  ]);
  container.appendChild(prov);

  var events = envelopeTimelineEvents(envelope);
  var card = el('div', { className: 'card' }, [
    el('div', { className: 'card-title mb-8', textContent: 'Verified Timeline (' + events.length + ')' }),
    el('div', { className: 'card-subtitle mb-8',
      textContent: 'Coarse time buckets, no precise timestamps · zone-level only · from the signed ledger' }),
  ]);

  if (events.length === 0) {
    card.appendChild(el('div', { className: 'empty-state', textContent: 'No verified events in this bundle.' }));
    container.appendChild(card);
    return;
  }

  var timeline = el('div', { className: 'event-detail-timeline' });
  events.forEach(function (ev, i) {
    var isLast = i === events.length - 1;
    var item = el('div', { className: 'timeline-item' + (isLast ? ' timeline-item-last' : '') });
    var meta = ev.failure
      ? { icon: '⚠️', label: 'Gap: ' + (ev.type || 'failure'), cssClass: 'type-motion' }
      : envelopeEventMeta(ev.type);
    item.appendChild(el('div', { className: 'timeline-dot ' + meta.cssClass }));
    if (!isLast) item.appendChild(el('div', { className: 'timeline-line' }));
    var body = el('div', { className: 'timeline-body' });
    body.appendChild(el('div', { className: 'timeline-label', textContent: meta.icon + ' ' + meta.label }));
    if (!ev.failure) {
      body.appendChild(el('div', { className: 'timeline-time', textContent: 'Zone: ' + (ev.zone || '—') }));
    }
    body.appendChild(el('div', { className: 'timeline-time', textContent: formatTimeBucket(ev.time_bucket) }));
    item.appendChild(body);
    timeline.appendChild(item);
  });
  card.appendChild(timeline);
  container.appendChild(card);
}

function renderWitnessView(deviceId) {
  var app = clearApp();
  var device = CanaryStorage.getDevice(deviceId);
  if (!device) { renderNotFound(); return; }

  app.appendChild(renderHeader('Witness Chain', true, false));

  var content = el('div', { className: 'content' });
  var alertArea = el('div', { id: 'witness-alert' });
  content.appendChild(alertArea);

  // Action buttons
  var actions = el('div', { className: 'card' }, [
    el('div', { className: 'card-title mb-8', textContent: 'Actions' }),
  ]);

  actions.appendChild(el('button', {
    className: 'btn btn-primary btn-block',
    textContent: 'Verify Chain Integrity',
    onClick: function () {
      while (alertArea.firstChild) alertArea.removeChild(alertArea.firstChild);
      alertArea.appendChild(el('div', { className: 'alert', textContent: 'Verifying…' }));
      CanaryAPI.request(device.base_url, '/api/v1/witness/verify', { method: 'POST' })
        .then(function (res) {
          while (alertArea.firstChild) alertArea.removeChild(alertArea.firstChild);
          var cls = res.integrity === 'ok' ? 'alert-success' : 'alert-error';
          var msg = res.valid + '/' + res.total + ' records valid — ' + res.integrity;
          alertArea.appendChild(el('div', { className: 'alert ' + cls, textContent: msg }));
        })
        .catch(function (err) {
          while (alertArea.firstChild) alertArea.removeChild(alertArea.firstChild);
          alertArea.appendChild(el('div', { className: 'alert alert-error', textContent: err.message }));
        });
    },
  }));

  actions.appendChild(el('button', {
    className: 'btn btn-block',
    textContent: 'Export Chain (JSON)',
    style: 'margin-top: 0.5rem',
    onClick: function () {
      CanaryAPI.request(device.base_url, '/api/v1/witness/export')
        .then(function (data) {
          var blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
          var url = URL.createObjectURL(blob);
          var a = document.createElement('a');
          a.href = url;
          a.download = 'witness-chain-' + device.device_id + '.json';
          document.body.appendChild(a);
          a.click();
          document.body.removeChild(a);
          URL.revokeObjectURL(url);
        })
        .catch(function (err) {
          while (alertArea.firstChild) alertArea.removeChild(alertArea.firstChild);
          alertArea.appendChild(el('div', { className: 'alert alert-error', textContent: err.message }));
        });
    },
  }));

  actions.appendChild(el('button', {
    className: 'btn btn-block',
    textContent: 'Export Evidence Envelope',
    style: 'margin-top: 0.5rem',
    title: 'Privacy-coarsened, self-verifying bundle — open in the offline evidence viewer',
    onClick: function () {
      while (alertArea.firstChild) alertArea.removeChild(alertArea.firstChild);
      CanaryAPI.request(device.base_url, '/api/v1/witness/envelope')
        .then(function (data) {
          var blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
          var url = URL.createObjectURL(blob);
          var a = document.createElement('a');
          a.href = url;
          a.download = 'evidence-envelope-' + device.device_id + '.json';
          document.body.appendChild(a);
          a.click();
          document.body.removeChild(a);
          URL.revokeObjectURL(url);
        })
        .catch(function (err) {
          while (alertArea.firstChild) alertArea.removeChild(alertArea.firstChild);
          // A 422 means coarsening refused to emit (precise data still in the raw chain).
          var msg = err.message || 'Envelope export failed';
          alertArea.appendChild(el('div', { className: 'alert alert-error', textContent: msg }));
        });
    },
  }));

  // Verified, privacy-coarsened timeline (from the canonical evidence envelope). This is the
  // in-app counterpart to the offline viewer: the same signed bundle, rendered here with its
  // integrity status, so an operator can review the attested events without leaving the app.
  var timelineSection = el('div', { id: 'witness-timeline' });
  var timelineBtn = el('button', {
    className: 'btn btn-block',
    textContent: 'View Verified Timeline',
    style: 'margin-top: 0.5rem',
    title: 'Render the privacy-coarsened, cryptographically verified events from the evidence envelope',
    onClick: function () {
      // Guard against overlapping requests from rapid clicks (race conditions / stale overwrites).
      if (timelineBtn.disabled) return;
      timelineBtn.disabled = true;
      while (timelineSection.firstChild) timelineSection.removeChild(timelineSection.firstChild);
      timelineSection.appendChild(el('div', { className: 'loading' }, [
        el('div', { className: 'spinner' }), 'Building and verifying evidence envelope…',
      ]));
      // Fetch the envelope and its integrity report together; the device builds + signs the
      // envelope and runs the shared verifier (viewer/verify_core.js) server-side.
      Promise.all([
        CanaryAPI.request(device.base_url, '/api/v1/witness/envelope'),
        CanaryAPI.request(device.base_url, '/api/v1/witness/verify', { method: 'POST' }),
      ]).then(function (results) {
        timelineBtn.disabled = false;
        renderVerifiedTimeline(timelineSection, results[0], results[1]);
      }).catch(function (err) {
        timelineBtn.disabled = false;
        while (timelineSection.firstChild) timelineSection.removeChild(timelineSection.firstChild);
        timelineSection.appendChild(el('div', { className: 'alert alert-error',
          textContent: err.message || 'Failed to load verified timeline' }));
      });
    },
  });
  actions.appendChild(timelineBtn);

  content.appendChild(actions);
  content.appendChild(timelineSection);

  // Record list
  var recordList = el('div', { id: 'witness-records' });
  recordList.appendChild(el('div', { className: 'loading' }, [
    el('div', { className: 'spinner' }),
    'Loading records…',
  ]));
  content.appendChild(el('div', { className: 'card-title mb-8', style: 'margin-top: 1rem',
    textContent: 'Raw chain records' }));
  content.appendChild(recordList);
  app.appendChild(content);

  CanaryAPI.request(device.base_url, '/api/v1/witness?last=50')
    .then(function (data) {
      while (recordList.firstChild) recordList.removeChild(recordList.firstChild);
      var records = data.records || [];
      if (records.length === 0) {
        recordList.appendChild(el('div', { className: 'empty-state', textContent: 'No witness records.' }));
        return;
      }
      records.reverse().forEach(function (r) {
        var card = el('div', { className: 'card' }, [
          el('div', { className: 'card-title', textContent: '#' + r.seq + ' ' + r.event_type }),
          el('div', { className: 'card-subtitle', textContent: r.zone + ' — ' + r.timestamp }),
          el('div', {
            className: 'token-display',
            style: 'font-size: 0.7rem; word-break: break-all; margin-top: 0.25rem',
            textContent: r.hash,
          }),
        ]);
        recordList.appendChild(card);
      });
    })
    .catch(function (err) {
      while (recordList.firstChild) recordList.removeChild(recordList.firstChild);
      recordList.appendChild(el('div', { className: 'alert alert-error', textContent: err.message }));
    });
}

function renderEventsView() {
  var app = clearApp();
  app.appendChild(renderHeader('Events', false, false));
  app.appendChild(renderNav('#/events'));

  // Pull-to-refresh wrapper
  var ptrContainer = el('div', { className: 'content ptr-container' });
  var ptrIndicator = el('div', { className: 'ptr-indicator' }, [
    el('span', { className: 'ptr-arrow', textContent: '↓' }),
    el('div', { className: 'ptr-spinner' }),
    el('span', { className: 'ptr-label', textContent: 'Pull to refresh' }),
  ]);
  var ptrContent = el('div', { className: 'ptr-content' });
  ptrContainer.appendChild(ptrIndicator);
  ptrContainer.appendChild(ptrContent);
  app.appendChild(ptrContainer);

  var content = ptrContent;

  // Clear any previous state
  if (EventsState.refreshTimer) {
    clearInterval(EventsState.refreshTimer);
    EventsState.refreshTimer = null;
  }
  if (EventsState.onHashChange) {
    window.removeEventListener('hashchange', EventsState.onHashChange);
    EventsState.onHashChange = null;
  }
  if (EventsState.ptrTeardown) {
    EventsState.ptrTeardown();
    EventsState.ptrTeardown = null;
  }
  closeEventStreams();
  EventsState.expandedClusters = {};
  EventsState.activeFilter = 'all';
  EventsState._sessionSeq++;
  var sessionId = EventsState._sessionSeq;
  EventsState.currentSessionId = sessionId;

  var devices = CanaryStorage.getDevices();

  if (devices.length === 0) {
    content.appendChild(el('div', { className: 'empty-state' }, [
      el('h2', { textContent: 'No Canaries Yet' }),
      el('p', { textContent: 'Add a Canary device to start seeing events here.' }),
      el('button', {
        className: 'btn btn-primary',
        textContent: 'Add Canary',
        onClick: function () { Router.navigate('#/canaries/add'); }
      }),
    ]));
    return;
  }

  // Loading state
  content.appendChild(el('div', { className: 'loading' }, [
    el('div', { className: 'spinner' }),
    el('span', { textContent: 'Fetching events from ' + devices.length + ' device' + (devices.length > 1 ? 's' : '') + '…' }),
  ]));

  function refreshData() {
    return fetchAllWitnessRecords().then(function (data) {
      EventsState.deviceResults = data.results;
      EventsState.allRecords = data.allRecords;
      EventsState.clusters = clusterEvents(data.allRecords);
      EventsState.filteredClusters = applyEventsFilter(EventsState.clusters, EventsState.activeFilter);
      renderEventsContent(content, data);
    });
  }

  function loadAndRender() {
    return refreshData().catch(function () {
      renderEventsContent(content, { results: [], allRecords: [] });
    });
  }

  loadAndRender().then(function () {
    if (EventsState.currentSessionId !== sessionId) return;
    if ((window.location.hash || '') !== '#/events') return;

    // Set up pull-to-refresh
    EventsState.ptrTeardown = setupPullToRefresh(ptrContainer, ptrContent, ptrIndicator, function (done) {
      refreshData().then(done).catch(done);
    });

    // Connect SSE streams to all devices for live events
    connectEventStreams(devices, content);

    // Fallback polling every 30s for devices that don't support SSE
    EventsState.refreshTimer = setInterval(function () {
      var currentHash = window.location.hash || '';
      if (currentHash !== '#/events') {
        clearInterval(EventsState.refreshTimer);
        EventsState.refreshTimer = null;
        return;
      }
      if (EventsState.eventSources.length < devices.length) {
        refreshData();
      }
    }, 30000);
  });

  // Clean up on navigation
  EventsState.onHashChange = function () {
    if ((window.location.hash || '') !== '#/events') {
      if (EventsState.refreshTimer) {
        clearInterval(EventsState.refreshTimer);
        EventsState.refreshTimer = null;
      }
      if (EventsState.ptrTeardown) {
        EventsState.ptrTeardown();
        EventsState.ptrTeardown = null;
      }
      closeEventStreams();
      window.removeEventListener('hashchange', EventsState.onHashChange);
      EventsState.onHashChange = null;
    }
  };
  window.addEventListener('hashchange', EventsState.onHashChange);
}

function renderEventsContent(container, data) {
  var scrollContainer = container.parentNode;
  var savedScroll = scrollContainer ? scrollContainer.scrollTop : 0;
  EventsState.chainIntegrityCache = {};

  while (container.firstChild) container.removeChild(container.firstChild);

  var results = data.results;
  var allRecords = data.allRecords;
  var totalDevices = results.length;
  var onlineDevices = 0;
  var offlineNames = [];
  for (var i = 0; i < results.length; i++) {
    if (!results[i].error) {
      onlineDevices++;
    } else {
      offlineNames.push(results[i].device.name || results[i].device.id);
    }
  }

  // Summary card
  container.appendChild(renderEventsSummary(allRecords, totalDevices, onlineDevices, offlineNames, container));

  // Density bar (only show if there are records today)
  var todayCounts = countTodayByType(allRecords);
  if (todayCounts.total > 0) {
    container.appendChild(renderDensityBar(allRecords));
  }

  // Filter chips
  container.appendChild(renderFilterChips(container, data));

  // Event list
  renderEventList(container);

  if (scrollContainer) {
    scrollContainer.scrollTop = savedScroll;
  }
}

function renderEventsSummary(allRecords, totalDevices, onlineDevices, offlineNames, contentContainer) {
  var todayCounts = countTodayByType(allRecords);
  var card = el('div', { className: 'card events-summary' });

  var headerRow = el('div', { className: 'card-header' });
  headerRow.appendChild(el('div', { className: 'card-title', textContent: 'Today' }));

  var refreshBtn = el('button', {
    className: 'refresh-btn',
    textContent: '↻',
    onClick: function () {
      if (refreshBtn.classList.contains('spinning')) return;
      refreshBtn.className = 'refresh-btn spinning';
      fetchAllWitnessRecords().then(function (freshData) {
        refreshBtn.className = 'refresh-btn';
        EventsState.deviceResults = freshData.results;
        EventsState.allRecords = freshData.allRecords;
        EventsState.clusters = clusterEvents(freshData.allRecords);
        EventsState.filteredClusters = applyEventsFilter(EventsState.clusters, EventsState.activeFilter);
        renderEventsContent(contentContainer, freshData);
      }).catch(function () {
        refreshBtn.className = 'refresh-btn';
      });
    }
  });
  headerRow.appendChild(refreshBtn);
  card.appendChild(headerRow);

  if (todayCounts.total === 0) {
    var reassurance = el('div', { className: 'events-reassurance' });
    reassurance.appendChild(el('span', { className: 'shield', textContent: '🛡️' }));
    reassurance.appendChild(el('h3', { textContent: 'All Quiet' }));
    reassurance.appendChild(el('p', { textContent: 'No events detected across your fleet today.' }));
    card.appendChild(reassurance);
  } else {
    var grid = el('div', { className: 'stats-grid' });
    grid.appendChild(el('div', { className: 'stat-item' }, [
      el('div', { className: 'stat-value', textContent: String(todayCounts.total) }),
      el('div', { className: 'stat-label', textContent: 'Events Today' }),
    ]));
    grid.appendChild(el('div', { className: 'stat-item' }, [
      el('div', { className: 'stat-value', textContent: String(todayCounts.person_detected) }),
      el('div', { className: 'stat-label', textContent: 'People' }),
    ]));
    grid.appendChild(el('div', { className: 'stat-item' }, [
      el('div', { className: 'stat-value', textContent: onlineDevices + ' / ' + totalDevices }),
      el('div', { className: 'stat-label', textContent: 'Devices Active' }),
    ]));

    var lastEventTime = allRecords.length > 0 ? formatEventTime(allRecords[0]._ts) : '—';
    grid.appendChild(el('div', { className: 'stat-item' }, [
      el('div', { className: 'stat-value', textContent: lastEventTime }),
      el('div', { className: 'stat-label', textContent: 'Last Event' }),
    ]));
    card.appendChild(grid);
  }

  if (offlineNames.length > 0) {
    card.appendChild(el('div', {
      className: 'events-offline-note',
      textContent: '⚠ Could not reach: ' + offlineNames.join(', ')
    }));
  }

  return card;
}

function renderDensityBar(records) {
  var data = buildDensityBuckets(records);
  var container = el('div', { className: 'density-bar-container' });
  container.appendChild(el('div', { className: 'density-bar-title', textContent: 'Activity' }));

  var bar = el('div', { className: 'density-bar' });
  var now = new Date();
  var startOfDay = new Date(now.getFullYear(), now.getMonth(), now.getDate());
  var currentBucket = Math.min(47, Math.floor((now - startOfDay) / (30 * 60000)));

  for (var i = 0; i < 48; i++) {
    var bucket = data.buckets[i];
    var heightPct = data.maxCount > 0 ? Math.max(5, (bucket.count / data.maxCount) * 100) : 5;
    var cssClass = 'density-bucket';

    if (bucket.count > 0) {
      var dominant = getDominantType(bucket.types);
      cssClass += ' ' + EVENT_TYPE_META[dominant].cssClass;
    }

    if (i > currentBucket) {
      heightPct = 0;
    }

    var bucketEl = el('div', { className: cssClass });
    bucketEl.style.height = bucket.count > 0 ? heightPct + '%' : '2px';
    if (i > currentBucket) {
      bucketEl.style.visibility = 'hidden';
    }
    bar.appendChild(bucketEl);
  }

  // "Now" line
  var nowPct = ((now - startOfDay) / (24 * 60 * 60000)) * 100;
  var nowLine = el('div', { className: 'density-now-line' });
  nowLine.style.left = Math.min(100, nowPct) + '%';
  bar.appendChild(nowLine);

  container.appendChild(bar);

  var labels = el('div', { className: 'density-labels' });
  var labelTexts = ['12a', '6a', '12p', '6p', 'Now'];
  for (var l = 0; l < labelTexts.length; l++) {
    labels.appendChild(el('span', { textContent: labelTexts[l] }));
  }
  container.appendChild(labels);

  return container;
}

function renderFilterChips(contentContainer, _data) {
  var chipsRow = el('div', { className: 'filter-chips' });

  // Derive the type chips from EVENT_TYPE_META (highest priority first) so any new event type is
  // automatically filterable without touching this list.
  var filters = [{ key: 'all', label: 'All' }];
  Object.keys(EVENT_TYPE_META)
    .sort(function (a, b) { return EVENT_TYPE_META[b].priority - EVENT_TYPE_META[a].priority; })
    .forEach(function (key) {
      var meta = EVENT_TYPE_META[key];
      filters.push({ key: key, label: meta.icon + ' ' + meta.label });
    });

  // Add device filters if multiple devices
  var deviceIds = {};
  for (var i = 0; i < EventsState.allRecords.length; i++) {
    var rec = EventsState.allRecords[i];
    deviceIds[rec.device_id] = rec.device_name;
  }
  var uniqueDevices = Object.keys(deviceIds);
  if (uniqueDevices.length > 1) {
    for (var d = 0; d < uniqueDevices.length; d++) {
      filters.push({ key: uniqueDevices[d], label: deviceIds[uniqueDevices[d]] });
    }
  }

  for (var f = 0; f < filters.length; f++) {
    (function (filter) {
      var chip = el('button', {
        className: 'filter-chip' + (EventsState.activeFilter === filter.key ? ' active' : ''),
        textContent: filter.label,
        onClick: function () {
          EventsState.activeFilter = filter.key;
          EventsState.filteredClusters = applyEventsFilter(EventsState.clusters, filter.key);

          // Update chip active states
          var chips = chipsRow.querySelectorAll('.filter-chip');
          for (var c = 0; c < chips.length; c++) {
            chips[c].className = 'filter-chip';
          }
          chip.className = 'filter-chip active';

          // Re-render the event list only
          var existingList = contentContainer.querySelector('.events-list');
          if (existingList) {
            contentContainer.removeChild(existingList);
          }
          renderEventList(contentContainer);
        }
      });
      chipsRow.appendChild(chip);
    })(filters[f]);
  }

  return chipsRow;
}

function renderEventList(container) {
  var existing = container.querySelector('.events-list');
  if (existing) container.removeChild(existing);

  var listEl = el('div', { className: 'events-list' });
  var clusters = EventsState.filteredClusters;

  if (clusters.length === 0) {
    var emptyMsg = EventsState.activeFilter !== 'all'
      ? 'No events match this filter.'
      : 'No events recorded yet.';
    listEl.appendChild(el('div', { className: 'empty-state' }, [
      el('p', { textContent: emptyMsg, style: 'padding: 20px 0;' }),
    ]));
    container.appendChild(listEl);
    return;
  }

  // Group by day
  var currentDay = '';
  var dayCount;

  for (var i = 0; i < clusters.length; i++) {
    var cluster = clusters[i];
    var dayKey = cluster.lastTime.toDateString();

    if (dayKey !== currentDay) {
      // Count events for this day
      dayCount = 0;
      for (var dc = i; dc < clusters.length; dc++) {
        if (clusters[dc].lastTime.toDateString() === dayKey) {
          dayCount += clusters[dc].count;
        } else {
          break;
        }
      }

      currentDay = dayKey;
      var sep = el('div', { className: 'day-separator' });
      sep.appendChild(el('span', { textContent: formatDayLabel(cluster.lastTime) }));
      sep.appendChild(el('span', { className: 'day-count', textContent: dayCount + ' event' + (dayCount !== 1 ? 's' : '') }));
      listEl.appendChild(sep);
    }

    var card = renderEventCard(cluster, i);
    listEl.appendChild(card);
  }

  container.appendChild(listEl);
}

function renderEventCard(cluster, index) {
  var meta = EVENT_TYPE_META[cluster.primaryType] || EVENT_TYPE_META.motion_detected;
  var isExpanded = EventsState.expandedClusters[cluster.id] || false;

  var card = el('div', { className: 'event-card' + (isExpanded ? ' expanded' : '') });
  card.style.animationDelay = Math.min(index * 0.04, 0.5) + 's';

  // Main row
  var row = el('div', { className: 'event-card-row' });

  // Thumbnail or icon fallback
  var latestEvent = cluster.events[cluster.events.length - 1];
  var hasThumbnail = latestEvent && latestEvent.thumbnail;
  var iconEl;

  if (hasThumbnail) {
    iconEl = el('div', { className: 'event-card-icon event-card-thumb' });
    var thumbImg = el('img', {
      className: 'event-thumb-img',
      src: latestEvent.thumbnail,
    });
    thumbImg.setAttribute('alt', meta.label + ' edge detection');
    thumbImg.setAttribute('loading', 'lazy');
    iconEl.appendChild(thumbImg);
  } else {
    iconEl = el('div', { className: 'event-card-icon ' + meta.cssClass });
    iconEl.appendChild(el('span', { textContent: meta.icon }));
  }

  if (cluster.count > 1) {
    iconEl.appendChild(el('span', { className: 'event-cluster-badge', textContent: '×' + cluster.count }));
  }
  row.appendChild(iconEl);

  // Body
  var body = el('div', { className: 'event-card-body' });
  var title = cluster.count > 1
    ? cluster.count + ' ' + meta.label + ' Events'
    : meta.label + ' Detected';
  body.appendChild(el('div', { className: 'event-card-title', textContent: title }));
  body.appendChild(el('div', { className: 'event-card-meta', textContent: cluster.device_name + ' · ' + cluster.zone }));
  body.appendChild(el('div', { className: 'event-card-time', textContent: formatEventTime(cluster.lastTime) }));
  row.appendChild(body);

  // Chevron
  row.appendChild(el('span', { className: 'event-card-chevron', textContent: '›' }));

  card.appendChild(row);

  // Detail panel
  var detail = el('div', { className: 'event-card-detail' });
  var detailInner = el('div', { className: 'event-detail-inner' });

  // Sub-events (if clustered)
  if (cluster.count > 1) {
    var subSection = el('div', { className: 'event-detail-section' });
    subSection.appendChild(el('div', { className: 'event-detail-section-title', textContent: 'Events in Cluster' }));
    var subList = el('div', { className: 'event-detail-subevents' });
    for (var s = 0; s < cluster.events.length; s++) {
      var evt = cluster.events[s];
      var evtMeta = EVENT_TYPE_META[evt.event_type] || EVENT_TYPE_META.motion_detected;
      var subRow = el('div', { className: 'event-detail-subevent' });
      subRow.appendChild(el('span', { className: 'subevent-dot ' + evtMeta.cssClass }));
      subRow.appendChild(el('span', { textContent: evtMeta.label }));
      subRow.appendChild(el('span', {
        textContent: evt._ts.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit', second: '2-digit' }),
        style: 'margin-left: auto; color: var(--color-text-muted); font-size: 11px;'
      }));
      subList.appendChild(subRow);
    }
    subSection.appendChild(subList);
    detailInner.appendChild(subSection);
  }

  // Witness chain info
  var chainSection = el('div', { className: 'event-detail-section' });
  chainSection.appendChild(el('div', { className: 'event-detail-section-title', textContent: 'Witness Chain' }));

  var firstEvt = cluster.events[0];
  var lastEvt = cluster.events[cluster.events.length - 1];

  // Sequence range
  var seqRange = cluster.count > 1
    ? 'seq ' + firstEvt.seq + '–' + lastEvt.seq
    : 'seq ' + firstEvt.seq;
  chainSection.appendChild(el('div', { className: 'event-detail-row' }, [
    el('span', { className: 'event-detail-label', textContent: 'Sequence' }),
    el('span', { className: 'event-detail-value', textContent: seqRange }),
  ]));

  // Hash
  chainSection.appendChild(el('div', { className: 'event-detail-row' }, [
    el('span', { className: 'event-detail-label', textContent: 'Hash' }),
    el('span', { className: 'event-detail-value', textContent: lastEvt.hash ? lastEvt.hash.substring(0, 16) + '…' : '—' }),
  ]));

  // Chain linkage — cached per device since it's the same result for every cluster on that device
  if (!EventsState.chainIntegrityCache) {
    EventsState.chainIntegrityCache = {};
  }
  var chainValid = EventsState.chainIntegrityCache[cluster.device_id];
  if (chainValid === undefined) {
    var deviceRecords = EventsState.allRecords.filter(function (r) {
      return r.device_id === cluster.device_id;
    }).sort(function (a, b) { return a.seq - b.seq; });

    chainValid = true;
    for (var v = 1; v < deviceRecords.length; v++) {
      if (!deviceRecords[v].prev_hash || !deviceRecords[v - 1].hash ||
          deviceRecords[v].prev_hash !== deviceRecords[v - 1].hash) {
        chainValid = false;
        break;
      }
    }
    EventsState.chainIntegrityCache[cluster.device_id] = chainValid;
  }

  chainSection.appendChild(el('div', { className: 'event-detail-row' }, [
    el('span', { className: 'event-detail-label', textContent: 'Chain Integrity' }),
    el('span', {
      className: chainValid ? 'event-detail-value chain-verified' : 'event-detail-value chain-unverified',
      textContent: chainValid ? '✓ Verified' : '⚠ Gap detected'
    }),
  ]));

  // Signature note
  chainSection.appendChild(el('div', { className: 'event-detail-row' }, [
    el('span', { className: 'event-detail-label', textContent: 'Signature' }),
    el('span', { className: 'event-detail-value', textContent: 'Ed25519 · on-device' }),
  ]));

  detailInner.appendChild(chainSection);
  detail.appendChild(detailInner);
  card.appendChild(detail);

  // Click to expand/collapse
  row.addEventListener('click', function () {
    var wasExpanded = card.className.indexOf('expanded') !== -1;
    if (wasExpanded) {
      card.className = 'event-card';
      EventsState.expandedClusters[cluster.id] = false;
    } else {
      card.className = 'event-card expanded';
      EventsState.expandedClusters[cluster.id] = true;
    }
  });

  // "View Details" link in detail panel
  var detailLink = el('button', {
    className: 'btn btn-secondary btn-block mt-12 event-detail-link',
    textContent: 'View Full Details',
    onClick: function (e) {
      e.stopPropagation();
      Router.navigate('#/events/' + cluster.id);
    }
  });
  detailInner.appendChild(detailLink);

  return card;
}

function renderEventDetailView(eventId) {
  var app = clearApp();
  app.appendChild(renderHeader('Event Detail', true, false));

  var content = el('div', { className: 'content' });
  app.appendChild(content);

  function renderDetails() {
    while (content.firstChild) content.removeChild(content.firstChild);

    var record = null;
    for (var i = 0; i < EventsState.allRecords.length; i++) {
      if (EventsState.allRecords[i].hash === eventId) {
        record = EventsState.allRecords[i];
        break;
      }
    }

    var cluster = null;
    for (var c = 0; c < EventsState.clusters.length; c++) {
      if (EventsState.clusters[c].id === eventId) {
        cluster = EventsState.clusters[c];
        break;
      }
      for (var e = 0; e < EventsState.clusters[c].events.length; e++) {
        if (EventsState.clusters[c].events[e].hash === eventId) {
          cluster = EventsState.clusters[c];
          if (!record) record = EventsState.clusters[c].events[e];
          break;
        }
      }
      if (cluster) break;
    }

    if (!record && !cluster) {
      content.appendChild(el('div', { className: 'empty-state' }, [
        el('h2', { textContent: 'Event Not Found' }),
        el('p', { textContent: 'This event may have expired or the device is not reachable.' }),
        el('button', {
          className: 'btn btn-primary',
          textContent: 'Back to Events',
          onClick: function () { Router.navigate('#/events'); }
        }),
      ]));
      return;
    }

    var events = cluster ? cluster.events : [record];
    var primaryRecord = record || events[0];
    var meta = EVENT_TYPE_META[primaryRecord.event_type] || EVENT_TYPE_META.motion_detected;

    // Hero header card
    var heroCard = el('div', { className: 'card event-detail-hero' });
    var heroThumbEvent = events[events.length - 1];
    if (heroThumbEvent && heroThumbEvent.thumbnail) {
      var heroThumb = el('img', {
        className: 'event-detail-hero-thumb',
        src: heroThumbEvent.thumbnail,
      });
      heroThumb.setAttribute('alt', meta.label + ' edge detection');
      heroCard.appendChild(heroThumb);
    } else {
      var heroIcon = el('div', { className: 'event-detail-hero-icon ' + meta.cssClass });
      heroIcon.appendChild(el('span', { textContent: meta.icon }));
      heroCard.appendChild(heroIcon);
    }

    var heroTitle = cluster && cluster.count > 1
      ? cluster.count + ' ' + meta.label + ' Events'
      : meta.label + ' Detected';
    heroCard.appendChild(el('h2', { className: 'event-detail-hero-title', textContent: heroTitle }));
    heroCard.appendChild(el('div', { className: 'event-detail-hero-meta', textContent: primaryRecord.device_name + ' · ' + primaryRecord.zone }));
    heroCard.appendChild(el('div', { className: 'event-detail-hero-time', textContent: formatEventTime(primaryRecord._ts) }));
    content.appendChild(heroCard);

    // Zone visualization
    var device = CanaryStorage.getDevice(primaryRecord.device_id);
    if (device) {
      var zoneCard = el('div', { className: 'card' });
      zoneCard.appendChild(el('div', { className: 'card-title mb-8', textContent: 'Detection Zone' }));

      var zoneCanvas = el('div', { className: 'zone-viz' });

      CanaryAPI.request(device.base_url, '/api/v1/config/detection')
        .then(function (data) {
          var zones = (data.zones || []);
          var matchedZone = null;
          for (var z = 0; z < zones.length; z++) {
            if (zones[z].name === primaryRecord.zone) {
              matchedZone = zones[z];
              break;
            }
          }

          if (matchedZone && matchedZone.points && matchedZone.points.length > 0) {
            var svgNs = 'http://www.w3.org/2000/svg';
            var svg = document.createElementNS(svgNs, 'svg');
            svg.setAttribute('viewBox', '0 0 100 100');
            svg.setAttribute('class', 'zone-svg');

            for (var gx = 0; gx <= 100; gx += 20) {
              var gridLine = document.createElementNS(svgNs, 'line');
              gridLine.setAttribute('x1', gx); gridLine.setAttribute('y1', 0);
              gridLine.setAttribute('x2', gx); gridLine.setAttribute('y2', 100);
              gridLine.setAttribute('class', 'zone-grid-line');
              svg.appendChild(gridLine);
            }
            for (var gy = 0; gy <= 100; gy += 20) {
              var gridLineH = document.createElementNS(svgNs, 'line');
              gridLineH.setAttribute('x1', 0); gridLineH.setAttribute('y1', gy);
              gridLineH.setAttribute('x2', 100); gridLineH.setAttribute('y2', gy);
              gridLineH.setAttribute('class', 'zone-grid-line');
              svg.appendChild(gridLineH);
            }

            var pointsStr = matchedZone.points.map(function (p) { return p[0] + ',' + p[1]; }).join(' ');
            var polygon = document.createElementNS(svgNs, 'polygon');
            polygon.setAttribute('points', pointsStr);
            polygon.setAttribute('class', 'zone-polygon ' + meta.cssClass);
            svg.appendChild(polygon);

            var cx = 0, cy = 0;
            for (var pi = 0; pi < matchedZone.points.length; pi++) {
              cx += matchedZone.points[pi][0]; cy += matchedZone.points[pi][1];
            }
            cx /= matchedZone.points.length; cy /= matchedZone.points.length;

            var label = document.createElementNS(svgNs, 'text');
            label.setAttribute('x', cx); label.setAttribute('y', cy);
            label.setAttribute('class', 'zone-label');
            label.textContent = matchedZone.name;
            svg.appendChild(label);

            zoneCanvas.appendChild(svg);
          } else {
            zoneCanvas.appendChild(el('div', {
              className: 'card-subtitle',
              textContent: 'Zone "' + primaryRecord.zone + '" — full frame'
            }));
          }
        })
        .catch(function () {
          zoneCanvas.appendChild(el('div', {
            className: 'card-subtitle',
            textContent: 'Zone: ' + primaryRecord.zone
          }));
        });

      zoneCard.appendChild(zoneCanvas);
      content.appendChild(zoneCard);
    }

    // Events timeline (individual events in cluster)
    if (events.length > 1) {
      var timelineCard = el('div', { className: 'card' });
      timelineCard.appendChild(el('div', { className: 'card-title mb-8', textContent: 'Event Timeline' }));

      var timeline = el('div', { className: 'event-detail-timeline' });
      for (var t = 0; t < events.length; t++) {
        var evt = events[t];
        var evtMeta = EVENT_TYPE_META[evt.event_type] || EVENT_TYPE_META.motion_detected;
        var isLast = t === events.length - 1;

        var timelineItem = el('div', { className: 'timeline-item' + (isLast ? ' timeline-item-last' : '') });
        var dot = el('div', { className: 'timeline-dot ' + evtMeta.cssClass });
        timelineItem.appendChild(dot);
        if (!isLast) {
          timelineItem.appendChild(el('div', { className: 'timeline-line' }));
        }

        var timelineBody = el('div', { className: 'timeline-body' });
        timelineBody.appendChild(el('div', { className: 'timeline-label', textContent: evtMeta.icon + ' ' + evtMeta.label + ' Detected' }));
        timelineBody.appendChild(el('div', {
          className: 'timeline-time',
          textContent: evt._ts.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit', second: '2-digit' })
        }));
        timelineItem.appendChild(timelineBody);
        timeline.appendChild(timelineItem);
      }
      timelineCard.appendChild(timeline);
      content.appendChild(timelineCard);
    }

    // Witness chain card
    var chainCard = el('div', { className: 'card' });
    chainCard.appendChild(el('div', { className: 'card-title mb-8', textContent: 'Witness Chain' }));

    var chainGrid = el('div', { className: 'event-detail-chain' });

    for (var r = 0; r < events.length; r++) {
      var rec = events[r];
      var recSection = el('div', { className: 'chain-record' });

      recSection.appendChild(el('div', { className: 'event-detail-row' }, [
        el('span', { className: 'event-detail-label', textContent: 'Sequence' }),
        el('span', { className: 'event-detail-value', textContent: String(rec.seq) }),
      ]));
      recSection.appendChild(el('div', { className: 'event-detail-row' }, [
        el('span', { className: 'event-detail-label', textContent: 'Timestamp' }),
        el('span', { className: 'event-detail-value', textContent: rec.timestamp }),
      ]));
      recSection.appendChild(el('div', { className: 'event-detail-row' }, [
        el('span', { className: 'event-detail-label', textContent: 'Hash' }),
        el('span', { className: 'event-detail-value', textContent: rec.hash || '—' }),
      ]));
      recSection.appendChild(el('div', { className: 'event-detail-row' }, [
        el('span', { className: 'event-detail-label', textContent: 'Prev Hash' }),
        el('span', { className: 'event-detail-value', textContent: rec.prev_hash || '—' }),
      ]));
      recSection.appendChild(el('div', { className: 'event-detail-row' }, [
        el('span', { className: 'event-detail-label', textContent: 'Signature' }),
        el('span', { className: 'event-detail-value', textContent: rec.signature ? rec.signature.substring(0, 24) + '…' : '—' }),
      ]));

      if (r < events.length - 1) {
        recSection.appendChild(el('div', { className: 'chain-arrow', textContent: '↓' }));
      }
      chainGrid.appendChild(recSection);
    }

    // Chain integrity — verify against full per-device stream
    var chainValid = true;
    var deviceRecords = EventsState.allRecords.filter(function (dr) {
      return dr.device_id === primaryRecord.device_id;
    }).sort(function (a, b) { return a.seq - b.seq; });

    for (var cv = 1; cv < deviceRecords.length; cv++) {
      if (!deviceRecords[cv].prev_hash || !deviceRecords[cv - 1].hash ||
          deviceRecords[cv].prev_hash !== deviceRecords[cv - 1].hash) {
        chainValid = false;
        break;
      }
    }

    chainCard.appendChild(chainGrid);
    chainCard.appendChild(el('div', { className: 'event-detail-row mt-12' }, [
      el('span', { className: 'event-detail-label', textContent: 'Chain Integrity' }),
      el('span', {
        className: chainValid ? 'event-detail-value chain-verified' : 'event-detail-value chain-unverified',
        textContent: chainValid ? '✓ Linked' : '⚠ Gap detected'
      }),
    ]));
    chainCard.appendChild(el('div', { className: 'event-detail-row' }, [
      el('span', { className: 'event-detail-label', textContent: 'Signing Algorithm' }),
      el('span', { className: 'event-detail-value', textContent: 'Ed25519' }),
    ]));
    content.appendChild(chainCard);

    // Deep-link / share section
    var shareCard = el('div', { className: 'card' });
    shareCard.appendChild(el('div', { className: 'card-title mb-8', textContent: 'Deep Link' }));
    var deepLink = window.location.origin + window.location.pathname + '#/events/' + eventId;
    shareCard.appendChild(el('div', { className: 'token-display', textContent: deepLink }));

    var copyBtn = el('button', {
      className: 'btn btn-secondary btn-block mt-12',
      textContent: 'Copy Link',
      onClick: function () {
        function copied() {
          copyBtn.textContent = '✓ Copied';
          setTimeout(function () { copyBtn.textContent = 'Copy Link'; }, 2000);
        }
        function failed() {
          copyBtn.textContent = 'Copy failed — long-press the link to copy';
          setTimeout(function () { copyBtn.textContent = 'Copy Link'; }, 3000);
        }
        if (navigator.clipboard && navigator.clipboard.writeText) {
          navigator.clipboard.writeText(deepLink).then(copied, function () {
            copyFallback(deepLink) ? copied() : failed();
          });
        } else {
          copyFallback(deepLink) ? copied() : failed();
        }
      }
    });
    shareCard.appendChild(copyBtn);
    content.appendChild(shareCard);

    if (device) {
      content.appendChild(el('button', {
        className: 'btn btn-secondary btn-block mt-12',
        textContent: 'View Device: ' + (device.name || device.id),
        onClick: function () { Router.navigate('#/device/' + device.id); }
      }));
    }
  }

  // If cache is empty (fresh page load / direct deep-link), fetch data first
  if (EventsState.allRecords.length === 0) {
    content.appendChild(el('div', { className: 'loading' }, [
      el('div', { className: 'spinner' }),
      el('span', { textContent: 'Loading event details…' }),
    ]));

    fetchAllWitnessRecords()
      .then(function (data) {
        EventsState.deviceResults = data.results;
        EventsState.allRecords = data.allRecords;
        EventsState.clusters = clusterEvents(data.allRecords);
        EventsState.filteredClusters = applyEventsFilter(EventsState.clusters, EventsState.activeFilter);
        renderDetails();
      })
      .catch(function () {
        renderDetails();
      });
  } else {
    renderDetails();
  }
}

function renderSettingsView() {
  var app = clearApp();
  app.appendChild(renderHeader('Settings', false, false));
  app.appendChild(renderNav('#/settings'));

  var content = el('div', { className: 'content' });

  content.appendChild(el('div', { className: 'card' }, [
    el('div', { className: 'card-title mb-8', textContent: 'About' }),
    el('div', { className: 'card-subtitle' }, [
      'Canary Vision v0.1.0',
    ]),
    el('div', {
      className: 'card-subtitle mt-4',
      textContent: 'Privacy-preserving camera fleet management by ERRERlabs',
    }),
  ]));

  var devices = CanaryStorage.getDevices();
  content.appendChild(el('div', { className: 'card' }, [
    el('div', { className: 'card-title mb-8', textContent: 'Devices' }),
    el('div', { className: 'card-subtitle', textContent: devices.length + ' device(s) configured' }),
  ]));

  // Notification preferences
  var prefs = getNotifPrefs();
  var notifCard = el('div', { className: 'card' });
  notifCard.appendChild(el('div', { className: 'card-title mb-8', textContent: 'Notifications' }));

  function makeToggle(label, key, prefs, onChange) {
    var row = el('div', { className: 'toggle-row' });
    row.appendChild(el('span', { className: 'toggle-label', textContent: label }));
    var toggle = el('label', { className: 'toggle' });
    var checkbox = el('input', { type: 'checkbox' });
    checkbox.checked = prefs[key];
    checkbox.addEventListener('change', function () {
      prefs[key] = checkbox.checked;
      saveNotifPrefs(prefs);
      if (onChange) onChange(checkbox.checked);
    });
    toggle.appendChild(checkbox);
    toggle.appendChild(el('span', { className: 'toggle-slider' }));
    row.appendChild(toggle);
    return row;
  }

  notifCard.appendChild(makeToggle('Notifications Enabled', 'enabled', prefs));
  notifCard.appendChild(makeToggle('Sound', 'sound', prefs));
  notifCard.appendChild(makeToggle('Vibration', 'vibrate', prefs));

  notifCard.appendChild(el('div', {
    className: 'card-subtitle mt-12 mb-8',
    textContent: 'Notify for event types:',
    style: 'font-weight: 500; color: var(--color-text);',
  }));
  // Derive the per-type toggles from EVENT_TYPE_META (highest priority first) so the settings
  // list stays in sync with the event vocabulary automatically when new types are added.
  Object.keys(EVENT_TYPE_META)
    .sort(function (a, b) { return EVENT_TYPE_META[b].priority - EVENT_TYPE_META[a].priority; })
    .forEach(function (key) {
      var meta = EVENT_TYPE_META[key];
      notifCard.appendChild(makeToggle(meta.icon + ' ' + meta.label, key, prefs));
    });

  notifCard.appendChild(el('div', {
    className: 'card-subtitle mt-12 mb-8',
    textContent: 'Quiet Hours:',
    style: 'font-weight: 500; color: var(--color-text);',
  }));
  notifCard.appendChild(makeToggle('Enable Quiet Hours', 'quiet_hours_enabled', prefs));

  var quietRow = el('div', { className: 'form-group', style: 'display: flex; gap: 12px; margin-top: 8px;' });
  var startGroup = el('div', { style: 'flex: 1;' });
  startGroup.appendChild(el('label', { className: 'form-label', textContent: 'Start' }));
  var startInput = el('input', {
    className: 'form-input',
    type: 'time',
    value: prefs.quiet_hours_start,
    onChange: function () {
      prefs.quiet_hours_start = this.value;
      saveNotifPrefs(prefs);
    }
  });
  startGroup.appendChild(startInput);
  quietRow.appendChild(startGroup);

  var endGroup = el('div', { style: 'flex: 1;' });
  endGroup.appendChild(el('label', { className: 'form-label', textContent: 'End' }));
  var endInput = el('input', {
    className: 'form-input',
    type: 'time',
    value: prefs.quiet_hours_end,
    onChange: function () {
      prefs.quiet_hours_end = this.value;
      saveNotifPrefs(prefs);
    }
  });
  endGroup.appendChild(endInput);
  quietRow.appendChild(endGroup);
  notifCard.appendChild(quietRow);

  content.appendChild(notifCard);

  content.appendChild(el('button', {
    className: 'btn btn-danger btn-block mt-20',
    textContent: 'Clear All Data',
    onClick: function () {
      if (confirm('Remove all devices and clear local data?')) {
        localStorage.removeItem(CanaryStorage.KEY);
        Router.navigate('#/canaries');
      }
    },
  }));

  app.appendChild(content);
}

function renderNotFound() {
  var app = clearApp();
  app.appendChild(renderHeader('Not Found', true, false));
  app.appendChild(el('div', { className: 'content' }, [
    el('div', { className: 'empty-state' }, [
      el('h2', { textContent: 'Page Not Found' }),
      el('p', { textContent: 'The page you are looking for does not exist.' }),
      el('button', {
        className: 'btn btn-primary',
        textContent: 'Go Home',
        onClick: function () { Router.navigate('#/canaries'); },
      }),
    ]),
  ]));
}

// --------------- Register Routes ---------------

Router.register('/canaries', renderCanariesView);
Router.register('/canaries/add', renderAddCanaryView);
Router.register('/device/:id', renderDeviceView);
Router.register('/device/:id/config/:section', renderConfigView);
Router.register('/device/:id/logs', renderLogsView);
Router.register('/device/:id/witness', renderWitnessView);
Router.register('/events', renderEventsView);
Router.register('/events/:id', renderEventDetailView);
Router.register('/settings', renderSettingsView);

// --------------- Init ---------------

var _lastKnownSeqs = {};

function backgroundEventCheck() {
  if ((window.location.hash || '') === '#/events') return;
  var devices = CanaryStorage.getDevices();
  if (devices.length === 0) return;

  devices.forEach(function (device) {
    if (!device.token || !isPrivateUrl(device.base_url)) return;
    CanaryAPI.request(device.base_url, '/api/v1/witness?last=1')
      .then(function (data) {
        var records = data.records || [];
        if (records.length === 0) return;
        var latest = records[records.length - 1];
        var key = device.id;
        if (_lastKnownSeqs[key] !== undefined && latest.seq > _lastKnownSeqs[key]) {
          var newCount = latest.seq - _lastKnownSeqs[key];
          for (var n = 0; n < newCount; n++) {
            if (shouldNotify(latest.event_type)) {
              incrementUnseenEvents();
            }
          }
          if (newCount > 0 && shouldNotify(latest.event_type)) {
            playNotificationFeedback(latest.event_type);
          }
        }
        _lastKnownSeqs[key] = latest.seq;
      })
      .catch(function () { /* device offline */ });
  });
}

window.addEventListener('hashchange', function () { Router.resolve(); });
window.addEventListener('DOMContentLoaded', function () {
  // Rehydrate expiry timers for existing sessions
  var devices = CanaryStorage.getDevices();
  for (var i = 0; i < devices.length; i++) {
    if (devices[i].token) {
      CanarySession.scheduleExpiry(devices[i].id);
    }
  }

  // Initialize last known seqs for badge tracking
  devices.forEach(function (device) {
    if (!device.token || !isPrivateUrl(device.base_url)) return;
    CanaryAPI.request(device.base_url, '/api/v1/witness?last=1')
      .then(function (data) {
        var records = data.records || [];
        if (records.length > 0) {
          _lastKnownSeqs[device.id] = records[records.length - 1].seq;
        }
      })
      .catch(function () { /* ignore */ });
  });

  // Background poll for badge updates every 30s
  setInterval(backgroundEventCheck, 30000);

  Router.resolve();
});
