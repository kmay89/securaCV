/* index-redirect.js — lifted out of index.html so the page can carry a strict Content-Security-Policy
   (script-src 'self', no inline, no hashes to re-pin on every edit; the policy table is
   canary-local/tools/gen_csp.py). Same code, same load order — only the file moved. */
// Device deep-links (index.html#<device-id>) historically opened a card
// sheet on this page; that page is now fleet.html, so forward a simple id
// hash there. Everything else goes to the shell. The hash is validated
// against a strict pattern, so only a bare id is ever appended to a fixed
// same-page path — no untrusted value controls the destination.
(function () {
  var h = location.hash;
  if (h && /^#[\w-]+$/.test(h)) location.replace("fleet.html" + h);
  else location.replace("lab.html");
})();
