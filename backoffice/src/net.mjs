// Every outbound call the backoffice makes (Stripe, USPS, the backup bucket, the site's catalog) goes
// through here, so none of them can hang a job or a request forever: a call that gives no signal of its
// own is abandoned after 20 s. A caller that knows its call is long (a backup upload) passes a longer
// signal. Tests pass their own fetchImpl and never reach the network.
export const timedFetch = (url, init = {}) => fetch(url, { ...init, signal: init.signal ?? AbortSignal.timeout(20_000) });
