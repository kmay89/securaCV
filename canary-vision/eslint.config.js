'use strict';

// ESLint flat config for canary-vision (device-api = Node/CommonJS,
// spa = browser, tests = Node). Conservative high-signal rules: catch real
// bugs (undefined names, unused vars, accidental redeclaration) without a
// stylistic reformat. See docs/review/03-feature-flags-and-hygiene.md (HY-03).
const js = require('@eslint/js');
const globals = require('globals');

module.exports = [
  {
    ignores: ['node_modules/**', '**/vendor/**'],
  },
  js.configs.recommended,
  {
    // Rules apply to every file; kept separate from the env blocks below so a
    // single broad `globals` block can't leak Node globals into the browser SPA.
    files: ['**/*.js'],
    rules: {
      // Unused vars are real rot; allow a leading-underscore opt-out for
      // intentionally-ignored args/catch bindings.
      'no-unused-vars': ['error', { argsIgnorePattern: '^_', varsIgnorePattern: '^_', caughtErrors: 'none' }],
    },
  },
  {
    // Node/CommonJS: device-api + tests (everything except the browser SPA).
    files: ['**/*.js'],
    ignores: ['spa/**/*.js'],
    languageOptions: {
      sourceType: 'commonjs',
      ecmaVersion: 2023,
      globals: { ...globals.node },
    },
  },
  {
    // Browser SPA — browser globals only, so accidental Node API use is caught.
    files: ['spa/**/*.js'],
    languageOptions: {
      sourceType: 'script',
      ecmaVersion: 2023,
      globals: { ...globals.browser },
    },
  },
];
