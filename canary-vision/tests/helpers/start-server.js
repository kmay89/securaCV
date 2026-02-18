'use strict';

const { createApp } = require('../../device-api/server');

const TEST_TOKEN = 'cv_test_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa';

/**
 * Starts the device-api server for testing.
 * @param {Object} options
 * @param {number} [options.port=0] - Port (0 for random)
 * @param {string} [options.token] - Override API token
 * @param {string[]} [options.allowedHosts] - Override allowed hosts
 * @param {Object[]} [options.peers] - Override peer list
 * @param {boolean} [options.devMode=true] - Allow localhost
 * @param {Object} [options.rateLimit] - Rate limit overrides
 * @returns {Promise<{url: string, close: Function, state: Object, rateLimitMiddleware: Object}>}
 */
async function startServer(options = {}) {
  const token = options.token || TEST_TOKEN;
  const deviceOverrides = { api_token: token };
  if (options.allowedHosts) {
    // Custom host handling is done via device identity
  }

  const appOptions = {
    devMode: options.devMode !== undefined ? options.devMode : true,
    peers: options.peers,
    deviceOverrides,
    rateLimit: options.rateLimit || {},
  };

  const { app, state, rateLimitMiddleware } = createApp(appOptions);

  // Ensure token is set
  state.device.api_token = token;

  return new Promise((resolve, reject) => {
    const server = app.listen(options.port || 0, '127.0.0.1', () => {
      const addr = server.address();
      const url = `http://127.0.0.1:${addr.port}`;
      resolve({
        url,
        port: addr.port,
        state,
        rateLimitMiddleware,
        close: () => new Promise((res) => server.close(res)),
      });
    });
    server.on('error', reject);
  });
}

module.exports = { startServer, TEST_TOKEN };
