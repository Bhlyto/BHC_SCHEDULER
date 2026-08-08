/* ── app.js — Core app logic ──────────────────────────────────────── */
const BASE = window.location.origin;
let API_KEY = '';
let USER_ID = '';
let USER_ROLE = '';
let APPS_CACHE = [];
let _currentTab = 'dashboard';
let _refreshTimer = null;
let _eventAbort = null;
let _eventReconnectTimer = null;
let _liveRefreshTimer = null;

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

function stopEventStream() {
  const controller = _eventAbort;
  _eventAbort = null;
  if (controller) controller.abort();
  if (_eventReconnectTimer) clearTimeout(_eventReconnectTimer);
  if (_liveRefreshTimer) clearTimeout(_liveRefreshTimer);
  _eventReconnectTimer = null;
  _liveRefreshTimer = null;
}

function scheduleLiveRefresh() {
  if (_liveRefreshTimer) return;
  _liveRefreshTimer = setTimeout(() => {
    _liveRefreshTimer = null;
    if (API_KEY && (_currentTab === 'dashboard' || _currentTab === 'jobs')) refreshCurrentTab();
  }, 250);
}

async function startEventStream() {
  stopEventStream();
  if (!API_KEY) return;
  const controller = new AbortController();
  _eventAbort = controller;
  try {
    const response = await fetch(BASE + '/jobs/events', {
      headers: { 'X-API-Key': API_KEY },
      signal: controller.signal
    });
    if (!response.ok || !response.body) throw new Error('SSE unavailable');
    const reader = response.body.getReader();
    const decoder = new TextDecoder();
    let buffer = '';
    while (true) {
      const chunk = await reader.read();
      if (chunk.done) break;
      buffer += decoder.decode(chunk.value, { stream: true });
      const lines = buffer.split(/\r?\n/);
      buffer = lines.pop() || '';
      lines.forEach(line => {
        if (line.startsWith('data:')) scheduleLiveRefresh();
      });
    }
  } catch (error) {
    if (error.name !== 'AbortError') console.warn('Event stream disconnected');
  } finally {
    if (_eventAbort === controller && API_KEY) {
      _eventAbort = null;
      _eventReconnectTimer = setTimeout(startEventStream, 2000);
    }
  }
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
  return '<span class="badge badge-' + cls + '">' + esc(s) + '</span>';
}
function shortId(id) { return id ? id.substring(0, 8) : '—'; }
function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, ch => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  })[ch]);
}
function fmtSize(bytes) {
  if (!bytes || bytes <= 0) return '0 B';
  const u = ['B','KB','MB','GB'];
  let i = 0;
  while (bytes >= 1024 && i < u.length - 1) { bytes /= 1024; i++; }
  return bytes.toFixed(i ? 1 : 0) + ' ' + u[i];
}

function handleClickAction(action, element, event) {
  switch (action) {
    case 'login': doLogin(); break;
    case 'api-key-login': doApiKeyLogin(); break;
    case 'show-change-password': showChangePassword(); break;
    case 'logout': doLogout(); break;
    case 'switch-tab': switchTab(element.dataset.tab); break;
    case 'show-submit-job': showSubmitJob(); break;
    case 'load-jobs': loadJobs(); break;
    case 'purge-jobs': purgeJobs(); break;
    case 'show-create-user': showCreateUser(); break;
    case 'show-create-key': showCreateKey(); break;
    case 'show-create-quota': showCreateQuota(); break;
    case 'show-create-app': showCreateApp(); break;
    case 'show-create-workflow': showCreateWorkflow(); break;
    case 'load-machines': if (window.loadMachineStatus) window.loadMachineStatus(); break;
    case 'close-modal': closeModal(); break;
    case 'change-password': changePassword(); break;
    case 'submit-job': submitJob(); break;
    case 'job-cancel': event.stopPropagation(); cancelJob(element.dataset.jobId); break;
    case 'job-release': event.stopPropagation(); releaseJob(element.dataset.jobId); break;
    case 'job-kill': event.stopPropagation(); killJob(element.dataset.jobId); break;
    case 'job-detail': showJobDetail(element.dataset.jobId); break;
    case 'workflow-toggle-group': toggleWorkflowGroup(element.dataset.collapseId); break;
    case 'view-log': viewLog(element.dataset.jobId, element.dataset.stderr === '1'); break;
    case 'upload-job': uploadToJob(element.dataset.jobId); break;
    case 'download-file': downloadFile(element.dataset.jobId, element.dataset.filename); break;
    case 'wf-toggle-favorite': wfToggleFavorite(element.dataset.workflowId); break;
    case 'wf-run-saved': wfLoadAndRun(element.dataset.workflowId); break;
    case 'wf-edit-saved': wfLoadForEdit(element.dataset.workflowId); break;
    case 'wf-duplicate': wfDuplicate(element.dataset.workflowId); break;
    case 'wf-delete-saved': wfDeleteSaved(element.dataset.workflowId); break;
    case 'wf-remove-step': wfRemoveStep(Number(element.dataset.step)); break;
    case 'wf-add-step': wfAddStep(); break;
    case 'wf-save': wfSave(false); break;
    case 'wf-submit': wfSubmit(); break;
    case 'wf-hide': hideWorkflowEditor(); break;
    case 'user-edit': showEditUser(element.dataset.userId); break;
    case 'user-delete': deleteUser(element.dataset.userId); break;
    case 'user-create': createUser(); break;
    case 'user-update': updateUser(element.dataset.userId); break;
    case 'key-create': createKey(); break;
    case 'key-revoke': revokeKey(element.dataset.keyHash); break;
    case 'quota-create': createQuota(); break;
    case 'quota-delete': deleteQuota(element.dataset.userId, element.dataset.appId); break;
    case 'app-create': createApp(); break;
    case 'app-edit': showEditApp(element.dataset.appId); break;
    case 'app-delete': deleteApp(element.dataset.appId); break;
    case 'app-update': updateApp(element.dataset.appId); break;
    case 'select-self': element.select(); break;
  }
}

function handleChangeAction(action, element) {
  const step = Number(element.dataset.step);
  switch (action) {
    case 'app-selection': onAppChange(); break;
    case 'wf-field': {
      const type = element.dataset.fieldType;
      const value = type === 'checkbox' ? element.checked : element.value;
      wfFieldChange(step, element.dataset.fieldName, value, type);
      break;
    }
    case 'wf-toggle-dep': wfToggleDep(step, Number(element.dataset.dependency)); break;
    case 'wf-app': wfAppChange(step, element.value); break;
    case 'wf-priority': wfSetPriority(step, element.value); break;
    case 'wf-timeout': wfSetTimeout(step, element.value); break;
    case 'wf-files': wfSetFiles(step, element); break;
    case 'wf-same-machine': _wfSteps[step].same_machine = element.checked; break;
  }
}

document.addEventListener('click', event => {
  const element = event.target.closest('[data-action]');
  if (!element || element.disabled) return;
  handleClickAction(element.dataset.action, element, event);
});

document.addEventListener('change', event => {
  const element = event.target.closest('[data-change-action]');
  if (element) handleChangeAction(element.dataset.changeAction, element);
});

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
    const data = await api('GET', '/auth/me');
    USER_ID = data.user_id || 'api-key-user';
    USER_ROLE = data.role || 'user';
    enterApp();
  } catch(e) {
    API_KEY = '';
    showLoginError('Invalid API key');
  }
}

function doLogout() {
  API_KEY = ''; USER_ID = ''; USER_ROLE = '';
  stopEventStream();
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
  startEventStream();
}

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
      <button class="btn btn-outline" data-action="close-modal">Cancel</button>
      <button class="btn btn-primary" data-action="change-password">Change</button>
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
