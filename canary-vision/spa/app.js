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
    // Strip tokens before persisting to localStorage
    var sanitized = devices.map(function (d) {
      var copy = Object.assign({}, d);
      if (copy.token) {
        CanaryStorage._setToken(copy.id, copy.token);
        delete copy.token;
      }
      return copy;
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
  }
};

// --------------- URL Validation ---------------

function isPrivateUrl(url) {
  try {
    var parsed = new URL(url);
    var hostname = parsed.hostname;
    if (hostname.endsWith('.local')) return true;
    if (hostname.match(/^192\.168\./)) return true;
    if (hostname.match(/^10\./)) return true;
    if (hostname.match(/^172\.(1[6-9]|2[0-9]|3[01])\./)) return true;
    if (hostname === 'localhost' || hostname === '127.0.0.1') return true;
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
      headers: {
        'X-Canary-Token': token,
      },
      signal: controller.signal,
    };

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
  while (app.firstChild) app.removeChild(app.firstChild);
  return app;
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

    // Try exact match first
    if (Router.routes[path]) {
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
    var link = el('a', {
      href: tab.hash,
      textContent: tab.label,
    });
    if (tab.hash === active) link.className = 'active';
    return link;
  }));
}

function renderCanariesView() {
  var app = clearApp();
  app.appendChild(renderHeader('Canary Vision', false, true));
  app.appendChild(renderNav('#/canaries'));

  var content = el('div', { className: 'content' });
  var devices = CanaryStorage.getDevices();

  if (devices.length === 0) {
    content.appendChild(el('div', { className: 'empty-state' }, [
      el('h2', { textContent: 'No Canaries Yet' }),
      el('p', { textContent: 'Add your first Canary device to get started.' }),
      el('button', {
        className: 'btn btn-primary',
        textContent: 'Add Canary',
        onClick: function () { Router.navigate('#/canaries/add'); }
      }),
    ]));
  } else {
    devices.forEach(function (device) {
      var card = el('div', { className: 'card cursor-pointer' });
      card.addEventListener('click', function () {
        Router.navigate('#/device/' + device.id);
      });

      var header = el('div', { className: 'card-header' }, [
        el('div', {}, [
          el('div', { className: 'card-title', textContent: device.name || device.id }),
          el('div', { className: 'card-subtitle', textContent: device.base_url }),
        ]),
        el('span', { className: 'status-dot online' }),
      ]);
      card.appendChild(header);

      var tokenRow = el('div', { className: 'token-display', textContent: maskToken(device.token) });
      card.appendChild(tokenRow);

      content.appendChild(card);
    });

    content.appendChild(el('button', {
      className: 'btn btn-secondary btn-block mt-12',
      textContent: '+ Add Canary',
      onClick: function () { Router.navigate('#/canaries/add'); },
    }));

    // Discovered-but-not-yet-added peers slot in below the fleet list.
    // The container is populated asynchronously so the view paints fast.
    content.appendChild(el('div', { id: 'discovered-peers' }));
  }

  app.appendChild(content);

  if (devices.length > 0) {
    refreshDiscoveredPeers(devices);
  }
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

function refreshDiscoveredPeers(devices) {
  var slot = document.getElementById('discovered-peers');
  if (!slot) return;

  // Query each known device's peer list in parallel. Failures are silent —
  // a device being offline shouldn't break the discovery UI.
  var requests = devices.map(function (d) {
    return CanaryAPI.request(d.base_url, '/api/v1/peers')
      .then(function (data) { return (data && data.peers) || []; })
      .catch(function () { return []; });
  });

  Promise.all(requests).then(function (results) {
    var slotNow = document.getElementById('discovered-peers');
    if (!slotNow) return; // user navigated away

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
    var row = el('div', { className: 'card' }, [
      el('div', { className: 'card-header' }, [
        el('div', {}, [
          el('div', { className: 'card-title', textContent: peer.name || peer.device_id }),
          el('div', { className: 'card-subtitle', textContent: baseUrl || peer.device_id }),
        ]),
        el('span', { className: 'status-dot' }),
      ]),
      el('button', {
        className: 'btn btn-primary btn-block mt-12',
        textContent: 'Pair this Canary',
        onClick: function () {
          // Pre-fill the Add form with what we already know so the user
          // only needs to paste this device's token.
          try {
            sessionStorage.setItem('canary_prefill_host', baseUrl);
            sessionStorage.setItem('canary_prefill_id', peer.device_id);
          } catch (e) { /* ignore */ }
          Router.navigate('#/canaries/add');
        },
      }),
    ]);
    section.appendChild(row);
  });

  return section;
}

function renderAddCanaryView() {
  var app = clearApp();
  app.appendChild(renderHeader('Add Canary', true, false));

  var content = el('div', { className: 'content' });
  var alert = el('div', { id: 'add-alert' });
  content.appendChild(alert);

  // If the user got here from the "Discovered" list, the host is already
  // known — we just need their token. One-shot consume so a manual
  // re-entry on the same view starts blank.
  var prefillHost = '';
  try {
    prefillHost = sessionStorage.getItem('canary_prefill_host') || '';
    sessionStorage.removeItem('canary_prefill_host');
    sessionStorage.removeItem('canary_prefill_id');
  } catch (e) { /* ignore */ }

  if (prefillHost) {
    content.appendChild(el('div', {
      className: 'alert alert-info',
      textContent: 'Pairing ' + prefillHost + ' — paste its API token below.',
    }));
  }

  var form = el('div', {}, [
    el('div', { className: 'form-group' }, [
      el('label', { className: 'form-label', textContent: 'Device Address' }),
      el('input', {
        className: 'form-input',
        id: 'add-host',
        type: 'text',
        placeholder: 'e.g. canary-a3f7.local or 192.168.1.47',
        value: prefillHost,
      }),
    ]),
    el('div', { className: 'form-group' }, [
      el('label', { className: 'form-label', textContent: 'API Token' }),
      el('input', {
        className: 'form-input',
        id: 'add-token',
        type: 'text',
        placeholder: 'cv_xxxx_...',
      }),
    ]),
    el('div', { className: 'form-group' }, [
      el('label', { className: 'form-label', textContent: 'Or paste provisioning receipt JSON' }),
      el('textarea', {
        className: 'form-input',
        id: 'add-receipt',
        rows: '4',
        placeholder: '{"device_id": "...", "base_url": "...", "token": "..."}',
      }),
    ]),
    el('button', {
      className: 'btn btn-primary btn-block',
      textContent: 'Add Device',
      onClick: handleAddCanary,
    }),
  ]);
  content.appendChild(form);
  app.appendChild(content);
}

function handleAddCanary() {
  var alertEl = document.getElementById('add-alert');
  while (alertEl.firstChild) alertEl.removeChild(alertEl.firstChild);

  var receiptText = document.getElementById('add-receipt').value.trim();
  var host, token, deviceId;

  if (receiptText) {
    try {
      var receipt = JSON.parse(receiptText);
      host = receipt.base_url || receipt.host;
      token = receipt.token || receipt.api_token;
      deviceId = receipt.device_id;
    } catch (e) {
      alertEl.appendChild(el('div', { className: 'alert alert-error', textContent: 'Invalid receipt JSON' }));
      return;
    }
  } else {
    host = document.getElementById('add-host').value.trim();
    token = document.getElementById('add-token').value.trim();
  }

  if (!host) {
    alertEl.appendChild(el('div', { className: 'alert alert-error', textContent: 'Device address is required' }));
    return;
  }

  if (!token) {
    alertEl.appendChild(el('div', { className: 'alert alert-error', textContent: 'API token is required' }));
    return;
  }

  // Normalize base_url
  // SECURITY NOTE: HTTP is used for private-network device communication.
  // The isPrivateUrl() check below ensures only RFC 1918 / link-local
  // addresses are accepted. Public addresses are rejected before any
  // request is made. The device API requires X-Canary-Token auth and
  // CORS/PNA middleware provides additional browser-level isolation.
  var baseUrl = host;
  if (baseUrl.indexOf('http') !== 0) {
    baseUrl = 'http://' + baseUrl;
  }
  // Remove trailing slash
  baseUrl = baseUrl.replace(/\/+$/, '');

  if (!isPrivateUrl(baseUrl)) {
    alertEl.appendChild(el('div', { className: 'alert alert-error', textContent: 'Only private network addresses are allowed' }));
    return;
  }

  // Test connection
  alertEl.appendChild(el('div', { className: 'loading' }, [
    el('div', { className: 'spinner' }),
    'Connecting...',
  ]));

  CanaryAPI.request(baseUrl, '/api/v1/info', { token: token })
    .then(function (info) {
      var id = deviceId || info.device_id;
      var device = {
        id: id,
        name: info.name || id,
        base_url: baseUrl,
        token: token,
        last_info: info,
        added_at: new Date().toISOString(),
      };
      CanaryStorage.addDevice(device);
      Router.navigate('#/device/' + id);
    })
    .catch(function (err) {
      while (alertEl.firstChild) alertEl.removeChild(alertEl.firstChild);
      var msg = 'Connection failed';
      if (err.status === 401) msg = 'Invalid token. Check your API token and try again.';
      else if (err.status === 429) msg = 'Rate limited. Try again in ' + (err.data ? err.data.retry_after : 60) + ' seconds.';
      else if (err.status === 0) msg = 'Device offline or unreachable.';
      alertEl.appendChild(el('div', { className: 'alert alert-error', textContent: msg }));
    });
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

  // Logs link
  content.appendChild(el('div', { className: 'card cursor-pointer' }, [
    el('a', { href: '#/device/' + deviceId + '/logs', className: 'card-link' }, [
      el('span', { textContent: 'View Logs' }),
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
      CanaryStorage.updateDevice(deviceId, { last_info: info });
      var grid = document.getElementById('device-stats');
      if (grid) {
        var uptimeHrs = Math.floor(info.uptime_s / 3600);
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
    'Loading configuration...',
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
      row.appendChild(el('span', { className: 'toggle-label', textContent: key }));
      var toggle = el('label', { className: 'toggle' });
      var checkbox = el('input', { type: 'checkbox' });
      checkbox.checked = value;
      toggle.appendChild(checkbox);
      toggle.appendChild(el('span', { className: 'toggle-slider' }));
      row.appendChild(toggle);
      group.appendChild(row);
      inputs[key] = { type: 'boolean', el: checkbox };
    } else if (typeof value === 'number') {
      group.appendChild(el('label', { className: 'form-label', textContent: key }));
      var numInput = el('input', { className: 'form-input', type: 'number', value: String(value) });
      group.appendChild(numInput);
      inputs[key] = { type: 'number', el: numInput };
    } else if (typeof value === 'string') {
      group.appendChild(el('label', { className: 'form-label', textContent: key }));
      var textInput = el('input', { className: 'form-input', type: 'text', value: value });
      group.appendChild(textInput);
      inputs[key] = { type: 'string', el: textInput };
    } else {
      // Arrays/objects — show as read-only JSON
      group.appendChild(el('label', { className: 'form-label', textContent: key }));
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
        else if (inp.type === 'number') body[key] = parseInt(inp.el.value, 10);
        else body[key] = inp.el.value;
      });

      var alertArea = document.getElementById('config-alert');
      while (alertArea.firstChild) alertArea.removeChild(alertArea.firstChild);

      CanaryAPI.request(device.base_url, '/api/v1/config/' + section, {
        method: 'PUT',
        body: body,
      })
        .then(function (res) {
          var msg = 'Configuration saved.';
          if (res.pending_physical_confirm && res.pending_physical_confirm.length > 0) {
            msg += ' Some settings require physical confirmation: ' + res.pending_physical_confirm.join(', ');
          }
          alertArea.appendChild(el('div', { className: 'alert alert-success', textContent: msg }));
        })
        .catch(function (err) {
          alertArea.appendChild(el('div', { className: 'alert alert-error', textContent: err.message }));
        });
    },
  }));
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
    'Loading logs...',
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
          el('span', { className: 'log-level ' + log.level, textContent: log.level }),
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

function renderEventsView() {
  var app = clearApp();
  app.appendChild(renderHeader('Events', false, false));
  app.appendChild(renderNav('#/events'));
  app.appendChild(el('div', { className: 'content' }, [
    el('div', { className: 'empty-state' }, [
      el('h2', { textContent: 'Coming Soon' }),
      el('p', { textContent: 'Event aggregation across your Canary fleet is under development.' }),
    ]),
  ]));
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
Router.register('/events', renderEventsView);
Router.register('/settings', renderSettingsView);

// --------------- Init ---------------

window.addEventListener('hashchange', function () { Router.resolve(); });
window.addEventListener('DOMContentLoaded', function () {
  // Rehydrate expiry timers for existing sessions
  var devices = CanaryStorage.getDevices();
  for (var i = 0; i < devices.length; i++) {
    if (devices[i].token) {
      CanarySession.scheduleExpiry(devices[i].id);
    }
  }
  Router.resolve();
});
