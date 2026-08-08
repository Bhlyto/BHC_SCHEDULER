/* ── app.js — Core app logic ──────────────────────────────────────── */
const BASE = window.location.origin;
let API_KEY = '';
let USER_ID = '';
let USER_ROLE = '';
let APPS_CACHE = [];
let _currentTab = 'dashboard';
let _refreshTimer = null;

/* ── API helpers ─────────────────────────────── */
async function api(method, path, body) {
  const opts = { method, headers: { 'Content-Type': 'application/json' } };
  if (API_KEY) opts.headers['X-API-Key'] = API_KEY;
  if (body) opts.body = JSON.stringify(body);
  const r = await fetch(BASE + path, opts);
  const text = await r.text();
  let data;
  try { data = JSON.parse(text); } catch { data = text; }
  if (!r.ok) throw { status: r.status, data };
  return data;
}

async function apiRaw(method, path, rawBody, contentType) {
  const opts = { method, headers: {} };
  if (API_KEY) opts.headers['X-API-Key'] = API_KEY;
  if (contentType) opts.headers['Content-Type'] = contentType;
  if (rawBody !== undefined) opts.body = rawBody;
  const r = await fetch(BASE + path, opts);
  if (!r.ok) { const t = await r.text(); let d; try { d = JSON.parse(t); } catch { d = t; } throw { status: r.status, data: d }; }
  return r;
}

/* ── Toast ────────────────────────────────────── */
function toast(msg, error) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'toast show' + (error ? ' error' : '');
  setTimeout(() => el.className = 'toast', 3000);
}

/* ── Modal ────────────────────────────────────── */
function openModal(html) {
  document.getElementById('modalContent').innerHTML = html;
  document.getElementById('modal').classList.add('show');
}
function closeModal() { document.getElementById('modal').classList.remove('show'); }
document.getElementById('modal').addEventListener('click', e => { if (e.target === e.currentTarget) closeModal(); });

/* ── Helpers ───────────────────────────────────── */
function fmtDate(ts) {
  if (!ts) return '—';
  return new Date(ts * 1000).toLocaleString();
}
function statusBadge(s) {
  const cls = { RUNNING:'running', QUEUED:'queued', SUCCEEDED:'finished', FAILED:'failed',
                CREATED:'held', CANCELLED:'cancelled' }[s] || 'queued';
  return '<span class="badge badge-' + cls + '">' + s + '</span>';
}
function shortId(id) { return id ? id.substring(0, 8) : '—'; }
function esc(s) {
  const d = document.createElement('div'); d.textContent = s || ''; return d.innerHTML;
}
function fmtSize(bytes) {
  if (!bytes || bytes <= 0) return '0 B';
  const u = ['B','KB','MB','GB'];
  let i = 0;
  while (bytes >= 1024 && i < u.length - 1) { bytes /= 1024; i++; }
  return bytes.toFixed(i ? 1 : 0) + ' ' + u[i];
}

/* ── Auth ─────────────────────────────────────── */
async function doLogin() {
  const uid = document.getElementById('loginUser').value.trim();
  const pwd = document.getElementById('loginPass').value;
  if (!uid || !pwd) return showLoginError('Enter user ID and password');
  try {
    const data = await api('POST', '/auth/login', { user_id: uid, password: pwd });
    API_KEY = data.api_key;
    USER_ID = data.user_id;
    USER_ROLE = data.role;
    sessionStorage.setItem('api_key', API_KEY);
    sessionStorage.setItem('user_id', USER_ID);
    sessionStorage.setItem('user_role', USER_ROLE);
    enterApp();
  } catch (e) {
    showLoginError(e.data?.error || 'Login failed');
  }
}

async function doApiKeyLogin() {
  const key = document.getElementById('loginApiKey').value.trim();
  if (!key) return showLoginError('Enter an API key');
  API_KEY = key;
  try {
    const data = await api('GET', '/stats');
    USER_ID = 'api-key-user';
    USER_ROLE = 'admin';
    sessionStorage.setItem('api_key', API_KEY);
    sessionStorage.setItem('user_id', USER_ID);
    sessionStorage.setItem('user_role', USER_ROLE);
    enterApp();
  } catch(e) {
    API_KEY = '';
    showLoginError('Invalid API key');
  }
}

function doLogout() {
  API_KEY = ''; USER_ID = ''; USER_ROLE = '';
  sessionStorage.clear();
  stopAutoRefresh();
  document.getElementById('app').style.display = 'none';
  document.getElementById('loginScreen').style.display = 'flex';
}

function showLoginError(msg) {
  const el = document.getElementById('loginError');
  el.textContent = msg; el.style.display = 'block';
}

async function enterApp() {
  document.getElementById('loginScreen').style.display = 'none';
  document.getElementById('app').style.display = 'block';
  document.getElementById('userInfo').textContent = USER_ID + ' (' + USER_ROLE + ')';
  document.querySelectorAll('.admin-only').forEach(el => {
    el.style.display = USER_ROLE === 'admin' ? '' : 'none';
  });
  /* Pre-load apps list */
  try { APPS_CACHE = await api('GET', '/apps'); } catch { APPS_CACHE = []; }
  /* Initialize reports module with API key */
  if (window.reportsSetApiKey) reportsSetApiKey(API_KEY);
  if (window.initReports) initReports();
  switchTab('dashboard');
  startAutoRefresh();
}

/* Auto-login from session */
(function() {
  const k = sessionStorage.getItem('api_key');
  if (k) {
    API_KEY = k;
    USER_ID = sessionStorage.getItem('user_id') || '';
    USER_ROLE = sessionStorage.getItem('user_role') || 'user';
    enterApp();
  }
})();

/* Enter key on login */
document.getElementById('loginPass').addEventListener('keydown', e => { if (e.key === 'Enter') doLogin(); });
document.getElementById('loginApiKey').addEventListener('keydown', e => { if (e.key === 'Enter') doApiKeyLogin(); });

/* ── Navigation ───────────────────────────────── */
function switchTab(tab) {
  _currentTab = tab;
  document.querySelectorAll('.tab-content').forEach(el => el.style.display = 'none');
  document.querySelectorAll('.nav-btn').forEach(el => el.classList.remove('active'));
  const tabEl = document.getElementById('tab-' + tab);
  if (tabEl) tabEl.style.display = 'block';
  const navBtn = document.querySelector('[data-tab="' + tab + '"]');
  if (navBtn) navBtn.classList.add('active');
  refreshCurrentTab();
}

function refreshCurrentTab() {
  if (_currentTab === 'dashboard') loadDashboard();
  if (_currentTab === 'jobs') loadJobs();
  if (_currentTab === 'users') loadUsers();
  if (_currentTab === 'keys') loadKeys();
  if (_currentTab === 'quotas') loadQuotas();
  if (_currentTab === 'apps') loadApps();
  if (_currentTab === 'workflows') loadWorkflowTab();
  if (_currentTab === 'reports' && window.loadAllReports) loadAllReports();
  if (_currentTab === 'machines' && window.loadMachineStatus) loadMachineStatus();
}

/* ── Auto-refresh ─────────────────────────────── */
function startAutoRefresh() {
  stopAutoRefresh();
  _refreshTimer = setInterval(() => refreshCurrentTab(), 60000);
}
function stopAutoRefresh() {
  if (_refreshTimer) { clearInterval(_refreshTimer); _refreshTimer = null; }
}

/* ── Change Password ──────────────────────────── */
function showChangePassword() {
  openModal(`<h2>Change Password</h2>
    <label>Old Password</label><input id="cpOld" type="password">
    <label>New Password</label><input id="cpNew" type="password">
    <label>Confirm New Password</label><input id="cpConfirm" type="password">
    <div class="modal-actions">
      <button class="btn btn-outline" onclick="closeModal()">Cancel</button>
      <button class="btn btn-primary" onclick="changePassword()">Change</button>
    </div>`);
}

async function changePassword() {
  const oldp = document.getElementById('cpOld').value;
  const newp = document.getElementById('cpNew').value;
  const conf = document.getElementById('cpConfirm').value;
  if (newp !== conf) return toast('Passwords do not match', true);
  if (!newp) return toast('New password is required', true);
  try {
    await api('POST', '/auth/change-password', { old_password: oldp, new_password: newp });
    closeModal(); toast('Password changed');
  } catch(e) { toast(e.data?.error || 'Failed', true); }
}
