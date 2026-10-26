// People: who may do what. An owner invites by link; the person sets their
// own password, scans their own authenticator, and the account opens only
// once a code from it has been seen. Nobody is ever handed a password.
// Everyone has /me for their own password, authenticator and sessions.
import { html, redirect, HttpError, escapeHtml as e } from './http.mjs';
import { layout, table, dt, field, form, button, statusPill, cards } from './render.mjs';
import { ROLES, ROLE_TEXT, listUsers, userById, inviteUser, inviteFor, acceptInvite, confirmTotp, pendingTotp, beginTotpRotation, changePassword, setRole, setStatus, removeUser, sessionsFor, endOtherSessions, totpUri } from './auth.mjs';
import { qrSvg } from './qr.mjs';
import { enqueue } from './modules/mail.mjs';

export function registerUsers(app) {
  const { db, cfg, router } = app;
  const page = (c, title, body) => html(c.res, layout({ title, body, path: c.url.pathname, company: cfg.company, csrf: c.csrf, demo: cfg.demo, demoResetMinutes: cfg.demoResetMinutes, user: c.user, flash: c.query.get('ok') ? { text: c.query.get('ok') } : c.query.get('err') ? { kind: 'err', text: c.query.get('err') } : null }));
  const back = (c, to, ok) => redirect(c.res, `${to}${to.includes('?') ? '&' : '?'}ok=${encodeURIComponent(ok)}`);
  const inviteUrl = (tok) => `${cfg.baseUrl}/invite/${tok}`;
  const issuer = `${cfg.company || 'Errer Labs'} backoffice`;
  const authenticatorBlock = (email, secret) => `<div class="two"><div>${qrSvg(totpUri(secret, email, issuer), { size: 220, label: 'authenticator setup' })}</div><div><p>Open your authenticator app (any TOTP app: Aegis, Google Authenticator, 1Password, Apple Passwords…) and scan the code, or type the secret:</p><pre style="word-break:break-all">${e(secret)}</pre><p class="muted">The secret is shown once. It lives on your phone and in this database, nowhere else.</p></div></div>`;

  // ---------- the people page (owner)
  router.get('/users', (c) => {
    const rows = listUsers(db); const now = new Date().toISOString(); const me = c.user.id;
    const roleForm = (u) => `<form method="post" action="/users/${u.id}/role" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><select name="role" aria-label="role of ${e(u.name)}">${ROLES.map(r => `<option value="${r}" ${r === u.role ? 'selected' : ''}>${r}</option>`).join('')}</select> <button class="quiet">set</button></form>`;
    const acts = (u) => [
      u.status === 'invited' ? `<form method="post" action="/users/${u.id}/reinvite" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><button class="quiet">new link</button></form>` : '',
      u.status === 'active' && u.id !== me ? `<form method="post" action="/users/${u.id}/status" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="status" value="disabled"><button class="quiet">disable</button></form>` : '',
      u.status === 'disabled' ? `<form method="post" action="/users/${u.id}/status" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="status" value="active"><button class="quiet">enable</button></form>` : '',
      u.id !== me ? `<form method="post" action="/users/${u.id}/remove" class="inline"><input type="hidden" name="_csrf" value="${e(c.csrf)}"><input type="hidden" name="sure" value="1"><button class="quiet">remove</button></form>` : '<span class="muted">you</span>',
    ].filter(Boolean).join(' ');
    const body = `<p class="muted">Each person logs in with their own password and authenticator, under their own name in the audit log. An invitation is a link that works once, for 48 hours; the person sets their own password and scans their own authenticator — nobody is handed a password. Roles: ${link('/users/roles', 'what each may do')}.</p>
${table(rows, [{ label: 'Name', html: u => `<b>${e(u.name)}</b><br><span class="muted">${e(u.email)}</span>` }, { label: 'Role', html: u => u.id === me ? `<span class="pill">${e(u.role)}</span>` : roleForm(u) }, { label: 'Status', html: u => u.locked_until && u.locked_until > now ? '<span class="pill bad">locked out</span>' : statusPill(u.status === 'invited' ? (u.invite_expires_at > now ? 'invited' : 'invite expired') : u.status) }, { label: 'Last login', get: u => dt(u.last_login_at) }, { label: 'Invited by', get: u => u.invited_by || '' }, { label: '', html: acts }], { empty: 'Nobody yet.' })}
${form('/users/invite', c.csrf, `<b>Invite someone</b><div class="row">${field('name', 'Name', { required: true })}${field('email', 'Email', { type: 'email', required: true })}${field('role', 'Role', { options: ROLES.map(r => [r, r]), value: 'ops' })}</div><p class="muted">You get a link to hand over however you like; if mail is configured it is also sent to them.</p>`, { submit: 'Create the invitation' })}
${c.query.get('invite') ? `<div class="card"><div class="k">The invitation link</div><pre style="word-break:break-all">${e(inviteUrl(c.query.get('invite')))}</pre><p class="muted">Works once, for 48 hours. Hand it over in person, by chat, or any way you trust; it is shown here only this once.</p></div>` : ''}`;
    page(c, 'People', body);
  });
  router.get('/users/roles', (c) => page(c, 'What each role may do', `${cards(Object.entries(ROLE_TEXT).map(([r, t]) => [r, `<span style="font-size:14px;font-weight:400">${e(t)}</span>`]))}<p class="muted">The rule is checked on every request by path and method, not by hiding buttons: a role that may not do something gets a clear refusal, and the sidebar shows only what it can open. API tokens are owner-made and not bound to a role; their scope (read-only or read-write) is the limit.</p><p>${link('/users', 'People')}</p>`));
  router.post('/users/invite', (c) => {
    const u = inviteUser(db, { email: c.body.email, name: c.body.name, role: c.body.role, actor: c.actor });
    if (cfg.smtpUrl || cfg.demo) enqueue(db, { to: u.email, subject: `You are invited to the ${cfg.company || 'Errer Labs'} backoffice`, kind: 'transactional', ref: `invite:${u.email}`, text: `Hi ${u.name.split(' ')[0]},\n\n${c.user.name} invited you to the ${cfg.company || 'Errer Labs'} backoffice as ${u.role}. Open this link within 48 hours, choose a password, and scan the authenticator code it shows:\n\n${inviteUrl(u.invite_token)}\n\nThe link works once. If it has expired, ask ${c.user.name} for a new one.\n\n— ${cfg.company || 'Errer Labs'}` });
    redirect(c.res, `/users?invite=${encodeURIComponent(u.invite_token)}&ok=${encodeURIComponent(`invited ${u.email} as ${u.role}`)}`);
  });
  router.post('/users/:id/reinvite', (c) => { const u = userById(db, Number(c.params.id)); if (!u) throw new HttpError(404, 'no such person'); const r = inviteUser(db, { email: u.email, name: u.name, role: u.role, actor: c.actor }); redirect(c.res, `/users?invite=${encodeURIComponent(r.invite_token)}&ok=${encodeURIComponent(`new link for ${u.email}`)}`); });
  router.post('/users/:id/role', (c) => { if (Number(c.params.id) === c.user.id) throw new HttpError(400, 'ask another owner to change your role'); const u = setRole(db, Number(c.params.id), c.body.role, { actor: c.actor }); back(c, '/users', `${u.email} is now ${u.role}`); });
  router.post('/users/:id/status', (c) => { if (Number(c.params.id) === c.user.id) throw new HttpError(400, 'you cannot disable yourself'); const u = setStatus(db, Number(c.params.id), c.body.status, { actor: c.actor }); back(c, '/users', `${u.email} ${u.status}`); });
  router.post('/users/:id/remove', (c) => { if (Number(c.params.id) === c.user.id) throw new HttpError(400, 'you cannot remove yourself'); const u = removeUser(db, Number(c.params.id), { actor: c.actor }); back(c, '/users', `${u.email} removed — their past actions stay in the audit log`); });
  router.get('/api/users', (c) => { if (c.user && c.user.role !== 'owner') throw new HttpError(403, 'owners only'); c.res.setHeader('Content-Type', 'application/json; charset=utf-8'); c.res.end(JSON.stringify(listUsers(db), null, 2)); });

  // ---------- your own account
  router.get('/me', (c) => {
    const u = userById(db, c.user.id); const pending = pendingTotp(db, u.id); const sessions = sessionsFor(db, u.id);
    page(c, 'Your account', `<p><b>${e(u.name)}</b> · ${e(u.email)} · <span class="pill">${e(u.role)}</span> · ${e(ROLE_TEXT[u.role])}</p>
<div class="two"><div>${form('/me/password', c.csrf, `<b>Change your password</b>${field('current', 'Current password', { type: 'password', required: true })}${field('next', 'New password (12 characters or more)', { type: 'password', required: true })}`, { submit: 'Change password' })}
<p class="muted">Password set ${dt(u.password_set_at)} · last login ${dt(u.last_login_at)}</p></div>
<div>${pending ? `<div class="box card"><b>New authenticator — scan it, then confirm with a code</b>${authenticatorBlock(u.email, pending)}${form('/me/totp/confirm', c.csrf, field('code', 'A code from the new authenticator', { required: true }), { submit: 'Confirm and switch', cls: '' })}<p class="muted">Your old authenticator keeps working until you confirm.</p></div>` : `<div class="card"><div class="k">Authenticator</div><p>Set up. Lost the phone? ${button('/me/totp', c.csrf, 'Set up a new one', { quiet: true })} — the old one keeps working until the new one is confirmed with a code.</p></div>`}
<div class="card" style="margin-top:.8rem"><div class="k">Sessions</div><p>${sessions.length} open (this one included). ${sessions.length > 1 ? button('/me/sessions/end', c.csrf, 'Log out everywhere else', { quiet: true }) : ''}</p></div></div></div>`);
  });
  router.post('/me/password', (c) => { changePassword(db, c.user.id, c.body.current, c.body.next); back(c, '/me', 'password changed'); });
  router.post('/me/totp', (c) => { beginTotpRotation(db, c.user.id); back(c, '/me', 'scan the new code, then confirm it'); });
  router.post('/me/totp/confirm', (c) => { if (!confirmTotp(db, c.user.id, c.body.code)) throw new HttpError(400, 'that code did not match the new authenticator — try the next one'); back(c, '/me', 'authenticator switched'); });
  router.post('/me/sessions/end', (c) => { const n = endOtherSessions(db, c.user.id, c.session.id); back(c, '/me', `${n} other session${n === 1 ? '' : 's'} ended`); });

  // ---------- accepting an invitation (public, token-gated; no session until the account is open)
  const invitePage = (title, body) => layout({ title, body, company: cfg.company, nav: false, demo: cfg.demo });
  router.get('/invite/:token', (c) => {
    const u = inviteFor(db, c.params.token); if (!u) return html(c.res, invitePage('Invitation', '<p>This invitation is not valid any more: it was used, it expired after 48 hours, or it was withdrawn. Ask the person who invited you for a new link.</p>'), 404);
    const pending = pendingTotp(db, u.id);
    if (pending) return html(c.res, invitePage('Almost there', `<p>Hi ${e(u.name.split(' ')[0])}. Your password is set. Now the authenticator: it is the second thing every login asks for.</p>${authenticatorBlock(u.email, pending)}<form method="post" action="/invite/${e(c.params.token)}/confirm" class="box">${field('code', 'Enter a code from the app to prove it is set up', { required: true })}<div style="margin-top:.8rem"><button>Open my account</button></div></form>`));
    html(c.res, invitePage(`Welcome, ${u.name.split(' ')[0]}`, `<p>${e(u.invited_by || 'An owner')} invited you to the ${e(cfg.company || 'Errer Labs')} backoffice as <b>${e(u.role)}</b> (${e(ROLE_TEXT[u.role])}).</p><p>You sign in as <b>${e(u.email)}</b>. Choose a password nobody else knows; the next step sets up your authenticator app.</p>${c.query.get('err') ? `<div class="flash err">${e(c.query.get('err'))}</div>` : ''}<form method="post" action="/invite/${e(c.params.token)}" class="box">${field('password', 'Password (12 characters or more)', { type: 'password', required: true })}${field('again', 'The same password again', { type: 'password', required: true })}<div style="margin-top:.8rem"><button>Set password and continue</button></div></form>`));
  });
  router.post('/invite/:token', (c) => {
    if (!inviteFor(db, c.params.token)) throw new HttpError(404, 'this invitation is not valid any more');
    if (c.body.password !== c.body.again) return redirect(c.res, `/invite/${encodeURIComponent(c.params.token)}?err=${encodeURIComponent('the two passwords differ')}`);
    try { acceptInvite(db, c.params.token, c.body.password); } catch (err) { return redirect(c.res, `/invite/${encodeURIComponent(c.params.token)}?err=${encodeURIComponent(err.message)}`); }
    redirect(c.res, `/invite/${encodeURIComponent(c.params.token)}`);
  }, { public: true, csrf: false });
  router.post('/invite/:token/confirm', (c) => {
    const u = inviteFor(db, c.params.token); if (!u) throw new HttpError(404, 'this invitation is not valid any more');
    if (!confirmTotp(db, u.id, c.body.code)) return html(c.res, invitePage('Almost there', `<div class="flash err">That code did not match. Wait for the next one and try again.</div><p><a href="/invite/${e(c.params.token)}">Back to the code</a></p>`), 400);
    // Open the door for them straight away: the password and the code they just proved.
    redirect(c.res, `/login?ok=${encodeURIComponent('your account is open — log in with your email, password and a code')}`);
  }, { public: true, csrf: false });
}
const link = (href, label) => `<a href="${e(href)}">${e(label)}</a>`;
