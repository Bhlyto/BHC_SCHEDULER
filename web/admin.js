/* ── admin.js — Users, Keys, Quotas, Apps admin tabs ─────────── */

/* ── Users ─────────────────────────────────────── */
async function loadUsers() {
  try {
    const users = await api('GET', '/admin/users');
    const list = Array.isArray(users) ? users : [];
    document.getElementById('usersBody').innerHTML = list.map(u =>
      '<tr><td>'+esc(u.user_id)+'</td><td>'+esc(u.display_name)+'</td><td>'+esc(u.email)+'</td>'
      +'<td>'+(u.enabled?'<span style="color:var(--green)">Yes</span>':'<span style="color:var(--red)">No</span>')+'</td>'
      +'<td>'+(u.total_jobs||0)+'</td>'
      +'<td><button class="btn btn-sm btn-outline" onclick="showEditUser(\''+esc(u.user_id)+'\')">Edit</button> '
      +'<button class="btn btn-sm btn-danger" onclick="deleteUser(\''+esc(u.user_id)+'\')">Delete</button></td></tr>'
    ).join('') || '<tr><td colspan="6" style="color:var(--text2)">No users</td></tr>';
  } catch(e) { toast('Failed to load users', true); }
}

function showCreateUser() {
  openModal(`<h2>Create User</h2>
    <label>User ID</label><input id="cuUid" placeholder="alice">
    <label>Display Name</label><input id="cuName" placeholder="Alice Smith">
    <label>Email</label><input id="cuEmail" placeholder="alice@example.com">
    <label>Password</label><input id="cuPass" type="password" placeholder="initial password">
    <div class="modal-actions">
      <button class="btn btn-outline" onclick="closeModal()">Cancel</button>
      <button class="btn btn-primary" onclick="createUser()">Create</button>
    </div>`);
}

async function createUser() {
  const body = {
    user_id: document.getElementById('cuUid').value.trim(),
    display_name: document.getElementById('cuName').value.trim(),
    email: document.getElementById('cuEmail').value.trim(),
    password: document.getElementById('cuPass').value
  };
  if (!body.user_id) return toast('User ID is required', true);
  try {
    await api('POST', '/admin/users', body);
    closeModal(); toast('User created'); loadUsers();
  } catch(e) { toast(e.data?.error || 'Create failed', true); }
}

async function showEditUser(uid) {
  try {
    const users = await api('GET', '/admin/users');
    const u = (Array.isArray(users) ? users : []).find(x => x.user_id === uid);
    if (!u) return toast('User not found', true);
    openModal(`<h2>Edit User</h2>
      <label>User ID</label><input value="${esc(u.user_id)}" disabled>
      <label>Display Name</label><input id="euName" value="${esc(u.display_name)}">
      <label>Email</label><input id="euEmail" value="${esc(u.email)}">
      <label>Enabled</label><select id="euEnabled"><option value="1" ${u.enabled?'selected':''}>Yes</option><option value="0" ${!u.enabled?'selected':''}>No</option></select>
      <label>New Password (leave empty to keep)</label><input id="euPass" type="password" placeholder="">
      <div class="modal-actions">
        <button class="btn btn-outline" onclick="closeModal()">Cancel</button>
        <button class="btn btn-primary" onclick="updateUser('${esc(u.user_id)}')">Save</button>
      </div>`);
  } catch(e) { toast('Error', true); }
}

async function updateUser(uid) {
  const body = {
    user_id: uid,
    display_name: document.getElementById('euName').value.trim(),
    email: document.getElementById('euEmail').value.trim(),
    enabled: +document.getElementById('euEnabled').value === 1
  };
  try {
    await api('PUT', '/admin/users', body);
    closeModal(); toast('User updated'); loadUsers();
  } catch(e) { toast(e.data?.error || 'Update failed', true); }
}

async function deleteUser(uid) {
  if (!confirm('Delete user "' + uid + '"?')) return;
  try { await api('DELETE', '/admin/users', { user_id: uid }); toast('User deleted'); loadUsers(); }
  catch(e) { toast(e.data?.error || 'Delete failed', true); }
}

/* ── API Keys ──────────────────────────────────── */
async function loadKeys() {
  try {
    const keys = await api('GET', '/admin/keys');
    const list = Array.isArray(keys) ? keys : [];
    document.getElementById('keysBody').innerHTML = list.map(k =>
      '<tr><td>'+esc(k.label)+'</td><td>'+esc(k.role)+'</td><td>'+esc(k.user_id)+'</td>'
      +'<td>'+fmtDate(k.created_at)+'</td><td>'+(k.expires_at?fmtDate(k.expires_at):'Never')+'</td>'
      +'<td>'+(k.revoked?'<span style="color:var(--red)">Revoked</span>':'<span style="color:var(--green)">Active</span>')+'</td>'
      +'<td>'+(k.revoked?'':'<button class="btn btn-sm btn-danger" onclick="revokeKey(\''+esc(k.key_hash)+'\')">Revoke</button>')+'</td></tr>'
    ).join('') || '<tr><td colspan="7" style="color:var(--text2)">No keys</td></tr>';
  } catch(e) { toast('Failed to load keys', true); }
}

function showCreateKey() {
  openModal(`<h2>Create API Key</h2>
    <label>Label</label><input id="ckLabel" placeholder="my-key">
    <label>Role</label><select id="ckRole"><option value="user">user</option><option value="admin">admin</option></select>
    <label>User ID (bind to user)</label><input id="ckUser" placeholder="alice">
    <div class="modal-actions">
      <button class="btn btn-outline" onclick="closeModal()">Cancel</button>
      <button class="btn btn-primary" onclick="createKey()">Create</button>
    </div>`);
}

async function createKey() {
  const body = {
    label: document.getElementById('ckLabel').value.trim() || 'default',
    role: document.getElementById('ckRole').value,
    user_id: document.getElementById('ckUser').value.trim()
  };
  try {
    const data = await api('POST', '/admin/keys', body);
    closeModal();
    openModal(`<h2>Key Created</h2>
      <p style="margin-bottom:12px;color:var(--text2)">Copy this key now — it won't be shown again.</p>
      <input value="${esc(data.api_key)}" onclick="this.select()" readonly style="font-family:monospace;font-size:13px">
      <div class="modal-actions"><button class="btn btn-primary" onclick="closeModal()">Done</button></div>`);
    loadKeys();
  } catch(e) { toast(e.data?.error || 'Create failed', true); }
}

async function revokeKey(hash) {
  if (!confirm('Revoke this API key?')) return;
  try { await api('DELETE', '/admin/keys', { key_hash: hash }); toast('Key revoked'); loadKeys(); }
  catch(e) { toast(e.data?.error || 'Revoke failed', true); }
}

/* ── Quotas ────────────────────────────────────── */
async function loadQuotas() {
  try {
    const quotas = await api('GET', '/admin/quotas');
    const list = Array.isArray(quotas) ? quotas : [];
    document.getElementById('quotasBody').innerHTML = list.map(q =>
      '<tr><td>'+esc(q.user_id||'*')+'</td><td>'+esc(q.app_id||'*')+'</td>'
      +'<td>'+(q.max_jobs||'&#8734;')+'</td><td>'+(q.max_ram_mb?q.max_ram_mb+' MB':'&#8734;')+'</td>'
      +'<td>'+(q.max_cores||'&#8734;')+'</td><td>'+(q.max_concurrent||'&#8734;')+'</td>'
      +'<td><button class="btn btn-sm btn-danger" onclick="deleteQuota(\''+esc(q.user_id)+'\',\''+esc(q.app_id)+'\')">Delete</button></td></tr>'
    ).join('') || '<tr><td colspan="7" style="color:var(--text2)">No quotas</td></tr>';
  } catch(e) { toast('Failed to load quotas', true); }
}

function showCreateQuota() {
  openModal(`<h2>Create / Update Quota</h2>
    <label>User ID (empty = all users)</label><input id="cqUser" placeholder="">
    <label>App ID (empty = all apps)</label><input id="cqApp" placeholder="">
    <label>Max Jobs (0 = unlimited)</label><input id="cqJobs" type="number" value="0" min="0">
    <label>Max RAM MB (0 = unlimited)</label><input id="cqRam" type="number" value="0" min="0">
    <label>Max Cores (0 = unlimited)</label><input id="cqCores" type="number" value="0" min="0">
    <label>Max Concurrent (0 = unlimited)</label><input id="cqConc" type="number" value="0" min="0">
    <div class="modal-actions">
      <button class="btn btn-outline" onclick="closeModal()">Cancel</button>
      <button class="btn btn-primary" onclick="createQuota()">Save</button>
    </div>`);
}

async function createQuota() {
  const body = {
    user_id: document.getElementById('cqUser').value.trim(),
    app_id: document.getElementById('cqApp').value.trim(),
    max_jobs: +document.getElementById('cqJobs').value,
    max_ram_mb: +document.getElementById('cqRam').value,
    max_cores: +document.getElementById('cqCores').value,
    max_concurrent: +document.getElementById('cqConc').value
  };
  try {
    await api('POST', '/admin/quotas', body);
    closeModal(); toast('Quota saved'); loadQuotas();
  } catch(e) { toast(e.data?.error || 'Failed', true); }
}

async function deleteQuota(uid, appid) {
  if (!confirm('Delete this quota?')) return;
  try { await api('DELETE', '/admin/quotas', { user_id: uid, app_id: appid }); toast('Quota deleted'); loadQuotas(); }
  catch(e) { toast(e.data?.error || 'Failed', true); }
}

/* ── Apps ───────────────────────────────────────── */
async function loadApps() {
  try {
    const apps = await api('GET', '/apps');
    const list = Array.isArray(apps) ? apps : [];
    /* Refresh cache */
    APPS_CACHE.length = 0;
    list.forEach(a => APPS_CACHE.push(a));

    document.getElementById('appsBody').innerHTML = list.map(a =>
      '<tr><td>'+esc(a.app_id)+'</td><td>'+esc(a.name)+'</td>'
      +'<td>'+esc(a.command_template)+'</td>'
      +'<td>'+(a.req_cores||0)+' / '+(a.req_ram_mb||0)+' MB / '+(a.req_disk_mb||0)+' MB / '+(a.req_gpu||0)+' GPU</td>'
      +'<td>'+(a.fields?a.fields.length:0)+' fields</td>'
      +'<td><button class="btn btn-sm btn-outline" onclick="showEditApp(\''+esc(a.app_id)+'\')">Edit</button> '
      +'<button class="btn btn-sm btn-danger" onclick="deleteApp(\''+esc(a.app_id)+'\')">Delete</button></td></tr>'
    ).join('') || '<tr><td colspan="6" style="color:var(--text2)">No apps configured</td></tr>';
  } catch(e) { toast('Failed to load apps', true); }
}

function showCreateApp() {
  const tpl = {
    app_id: '',
    name: '',
    command_template: '',
    req_cores: 1,
    req_ram_mb: 1024,
    req_disk_mb: 256,
    req_gpu: 0,
    fields: []
  };
  openModal(`<h2>Create Application</h2>
    <label>App ID (alphanumeric, dashes, underscores)</label><input id="caId" placeholder="my-app">
    <label>Display Name</label><input id="caName" placeholder="My Application">
    <label>Command Template</label><input id="caCmd" placeholder="my_app.exe">
    <label>Required Cores</label><input id="caCores" type="number" value="1" min="0">
    <label>Required RAM (MB)</label><input id="caRam" type="number" value="1024" min="0">
    <label>Required Disk (MB)</label><input id="caDisk" type="number" value="256" min="0">
    <label>Required GPU</label><input id="caGpu" type="number" value="0" min="0">
    <label>Fields (JSON array)</label>
    <textarea id="caFields" rows="6" placeholder='[{"name":"opt1","type":"checkbox","label":"Enable option","default":false}]'>[]</textarea>
    <div style="font-size:11px;color:var(--text2);margin-top:-8px;margin-bottom:12px">
      Field types: checkbox, select, text, number. Select fields need "options" array.
    </div>
    <div class="modal-actions">
      <button class="btn btn-outline" onclick="closeModal()">Cancel</button>
      <button class="btn btn-primary" onclick="createApp()">Create</button>
    </div>`);
}

async function createApp() {
  let fields;
  try { fields = JSON.parse(document.getElementById('caFields').value); }
  catch { return toast('Fields must be valid JSON', true); }

  const body = {
    app_id: document.getElementById('caId').value.trim(),
    name: document.getElementById('caName').value.trim(),
    command_template: document.getElementById('caCmd').value.trim(),
    req_cores: +document.getElementById('caCores').value,
    req_ram_mb: +document.getElementById('caRam').value,
    req_disk_mb: +document.getElementById('caDisk').value,
    req_gpu: +document.getElementById('caGpu').value,
    fields: fields
  };
  if (!body.app_id) return toast('App ID is required', true);
  try {
    await api('POST', '/admin/apps', body);
    closeModal(); toast('App created'); loadApps();
  } catch(e) { toast(e.data?.error || 'Create failed', true); }
}

async function showEditApp(appId) {
  try {
    const app = await api('GET', '/apps/' + encodeURIComponent(appId));
    if (!app) return toast('App not found', true);
    const fieldsJson = JSON.stringify(app.fields || [], null, 2);
    openModal(`<h2>Edit Application</h2>
      <label>App ID</label><input value="${esc(app.app_id)}" disabled>
      <label>Display Name</label><input id="eaName" value="${esc(app.name)}">
      <label>Command Template</label><input id="eaCmd" value="${esc(app.command_template)}">
      <label>Required Cores</label><input id="eaCores" type="number" value="${app.req_cores||0}" min="0">
      <label>Required RAM (MB)</label><input id="eaRam" type="number" value="${app.req_ram_mb||0}" min="0">
      <label>Required Disk (MB)</label><input id="eaDisk" type="number" value="${app.req_disk_mb||0}" min="0">
      <label>Required GPU</label><input id="eaGpu" type="number" value="${app.req_gpu||0}" min="0">
      <label>Fields (JSON array)</label>
      <textarea id="eaFields" rows="8">${esc(fieldsJson)}</textarea>
      <div class="modal-actions">
        <button class="btn btn-outline" onclick="closeModal()">Cancel</button>
        <button class="btn btn-primary" onclick="updateApp('${esc(app.app_id)}')">Save</button>
      </div>`);
  } catch(e) { toast('Error loading app', true); }
}

async function updateApp(appId) {
  let fields;
  try { fields = JSON.parse(document.getElementById('eaFields').value); }
  catch { return toast('Fields must be valid JSON', true); }

  const body = {
    app_id: appId,
    name: document.getElementById('eaName').value.trim(),
    command_template: document.getElementById('eaCmd').value.trim(),
    req_cores: +document.getElementById('eaCores').value,
    req_ram_mb: +document.getElementById('eaRam').value,
    req_disk_mb: +document.getElementById('eaDisk').value,
    req_gpu: +document.getElementById('eaGpu').value,
    fields: fields
  };
  try {
    await api('PUT', '/admin/apps', body);
    closeModal(); toast('App updated'); loadApps();
  } catch(e) { toast(e.data?.error || 'Update failed', true); }
}

async function deleteApp(appId) {
  if (!confirm('Delete app "' + appId + '"?')) return;
  try {
    await api('DELETE', '/admin/apps/' + encodeURIComponent(appId));
    toast('App deleted'); loadApps();
  } catch(e) { toast(e.data?.error || 'Delete failed', true); }
}
