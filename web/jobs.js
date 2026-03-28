/* ── jobs.js — Jobs tab, submission, detail modal ────────────────── */

function jobActions(j) {
  let btns = '';
  if (j.status === 'IN_QUEUE' || j.status === 'RUNNING' || j.status === 'HELD' || j.status === 'STARTING')
    btns += '<button class="btn btn-sm btn-danger" onclick="event.stopPropagation();cancelJob(\''+j.id+'\')">Cancel</button>';
  if (j.status === 'HELD')
    btns += ' <button class="btn btn-sm btn-outline" onclick="event.stopPropagation();releaseJob(\''+j.id+'\')">Release</button>';
  if (j.status === 'RUNNING' || j.status === 'STARTING')
    btns += ' <button class="btn btn-sm btn-outline" style="border-color:var(--red);color:var(--red)" onclick="event.stopPropagation();killJob(\''+j.id+'\')">Kill</button>';
  return btns;
}

function jobRow(j) {
  const appName = appLabel(j.app_id);
  return '<tr style="cursor:pointer" onclick="showJobDetail(\''+j.id+'\')">'
    +'<td title="'+esc(j.id)+'">'+shortId(j.id)+'</td>'
    +'<td>'+esc(j.command)+'</td>'
    +'<td>'+statusBadge(j.status)+'</td>'
    +'<td>'+esc(appName)+'</td>'
    +'<td>'+esc(j.user_id)+'</td>'
    +'<td>'+fmtDate(j.submitted_at)+'</td>'
    +'<td style="white-space:nowrap">'+jobActions(j)+'</td></tr>';
}

function workflowStatusSummary(jobs) {
  const counts = {};
  jobs.forEach(j => { counts[j.status] = (counts[j.status]||0) + 1; });
  return Object.entries(counts).map(([s,n]) => statusBadge(s)+' '+n).join(' &nbsp; ');
}

async function loadJobs() {
  try {
    const jobs = await api('GET', '/jobs');
    const list = Array.isArray(jobs) ? jobs : [];

    /* Group by workflow_id */
    const workflows = {};
    const standalone = [];
    list.forEach(j => {
      if (j.workflow_id) {
        if (!workflows[j.workflow_id]) workflows[j.workflow_id] = [];
        workflows[j.workflow_id].push(j);
      } else standalone.push(j);
    });

    let html = '';

    /* Render workflow groups (sorted by earliest submitted_at) */
    const wfEntries = Object.entries(workflows)
      .sort((a,b) => Math.min(...b[1].map(j=>j.submitted_at)) - Math.min(...a[1].map(j=>j.submitted_at)));

    wfEntries.forEach(([wfId, wfJobs]) => {
      const collapseId = 'wf_' + wfId.replace(/[^a-zA-Z0-9]/g,'');
      html += '<tr class="wf-group-header" onclick="toggleWorkflowGroup(\''+collapseId+'\')" style="cursor:pointer;background:var(--surface)">'
        +'<td colspan="7" style="padding:10px 12px;border-left:3px solid var(--primary)">'
        +'<div style="display:flex;align-items:center;justify-content:space-between">'
        +'<div style="display:flex;align-items:center;gap:8px">'
        +'<span class="wf-toggle" id="toggle_'+collapseId+'" style="font-size:12px;transition:transform .2s;transform:rotate(90deg)">&#9654;</span>'
        +'<b style="font-size:13px">Workflow</b>'
        +'<span style="font-family:monospace;font-size:11px;color:var(--text2)" title="'+esc(wfId)+'">'+shortId(wfId)+'</span>'
        +'<span style="font-size:12px;color:var(--text2)">('+wfJobs.length+' jobs)</span>'
        +'</div>'
        +'<div style="font-size:12px">'+workflowStatusSummary(wfJobs)+'</div>'
        +'</div></td></tr>';
      wfJobs.forEach(j => {
        const appName = appLabel(j.app_id);
        html += '<tr class="wf-child '+collapseId+'" style="cursor:pointer;background:var(--bg)" onclick="showJobDetail(\''+j.id+'\')">'
          +'<td style="padding-left:28px" title="'+esc(j.id)+'">'+shortId(j.id)+'</td>'
          +'<td>'+esc(j.command)+'</td>'
          +'<td>'+statusBadge(j.status)+'</td>'
          +'<td>'+esc(appName)+'</td>'
          +'<td>'+esc(j.user_id)+'</td>'
          +'<td>'+fmtDate(j.submitted_at)+'</td>'
          +'<td style="white-space:nowrap">'+jobActions(j)+'</td></tr>';
      });
    });

    /* Render standalone jobs */
    standalone.forEach(j => { html += jobRow(j); });

    document.getElementById('jobsBody').innerHTML = html
      || '<tr><td colspan="7" style="color:var(--text2)">No jobs</td></tr>';
  } catch(e) { toast('Failed to load jobs', true); }
}

function toggleWorkflowGroup(collapseId) {
  const rows = document.querySelectorAll('.'+collapseId);
  const toggle = document.getElementById('toggle_'+collapseId);
  const visible = rows.length && rows[0].style.display !== 'none';
  rows.forEach(r => r.style.display = visible ? 'none' : '');
  if (toggle) toggle.style.transform = visible ? '' : 'rotate(90deg)';
}

async function releaseJob(id) {
  try { await api('POST', '/jobs/' + id + '/release'); toast('Job released'); loadJobs(); }
  catch(e) { toast(e.data?.error || 'Release failed', true); }
}

async function killJob(id) {
  if (!confirm('Kill this job? It will be forcefully terminated.')) return;
  try { await api('DELETE', '/jobs/' + id); toast('Job killed'); loadJobs(); }
  catch(e) { toast(e.data?.error || 'Kill failed', true); }
}

async function purgeJobs() {
  if (!confirm('Delete all finished, failed and cancelled jobs?')) return;
  try {
    const r = await api('DELETE', '/jobs');
    toast('Purged ' + (r.deleted || 0) + ' jobs');
    loadJobs();
  } catch(e) { toast(e.data?.error || 'Purge failed', true); }
}

function appLabel(appId) {
  if (!appId) return '—';
  const a = APPS_CACHE.find(x => x.app_id === appId);
  return a ? a.name : appId;
}

/* ── Submit Job ────────────────────────────────── */
function showSubmitJob() {
  const apps = APPS_CACHE;
  if (!apps.length) {
    toast('No applications configured. Ask an admin to add apps.', true);
    return;
  }
  let opts = apps.map(a => '<option value="'+esc(a.app_id)+'">'+esc(a.name || a.app_id)+'</option>').join('');

  openModal(`<h2>Submit Job</h2>
    <label>Application</label>
    <select id="sjApp" onchange="onAppChange()">${opts}</select>
    <div id="sjResources" class="resource-bar"></div>
    <div id="sjAppFields"></div>
    <label>Priority (0-100)</label><input id="sjPri" type="number" value="50" min="0" max="100">
    <label>Timeout (seconds, 0=none)</label><input id="sjTimeout" type="number" value="0" min="0">
    <label>Input Files (optional)</label>
    <input id="sjFiles" type="file" multiple style="padding:8px">
    <div class="modal-actions">
      <button class="btn btn-outline" onclick="closeModal()">Cancel</button>
      <button class="btn btn-primary" onclick="submitJob()">Submit</button>
    </div>`);

  /* Trigger initial app selection */
  onAppChange();
}

function onAppChange() {
  const sel = document.getElementById('sjApp');
  if (!sel) return;
  const appId = sel.value;
  const app = APPS_CACHE.find(a => a.app_id === appId);
  if (!app) return;

  /* Show resources */
  const resEl = document.getElementById('sjResources');
  if (resEl) {
    resEl.innerHTML =
      '<div class="item"><span>Cores:</span> '+(app.req_cores||1)+'</div>'
      +'<div class="item"><span>RAM:</span> '+(app.req_ram_mb||0)+' MB</div>'
      +'<div class="item"><span>Disk:</span> '+(app.req_disk_mb||0)+' MB</div>'
      +'<div class="item"><span>GPU:</span> '+(app.req_gpu||0)+'</div>';
  }

  /* Render dynamic fields */
  renderAppFields(app.fields || []);
}

function renderAppFields(fields) {
  const container = document.getElementById('sjAppFields');
  if (!container) return;
  let html = '';
  fields.forEach(f => {
    const id = 'sjField_' + f.name;
    if (f.type === 'checkbox') {
      html += '<label class="checkbox-label"><input type="checkbox" id="'+id+'" '+(f.default ? 'checked' : '')+'> '+esc(f.label || f.name)+'</label>';
    } else if (f.type === 'select') {
      const options = (f.options || []).map(o =>
        '<option value="'+esc(o)+'" '+(o === f.default ? 'selected' : '')+'>'+esc(o)+'</option>'
      ).join('');
      html += '<label>'+esc(f.label || f.name)+'</label><select id="'+id+'">'+options+'</select>';
    } else if (f.type === 'number') {
      html += '<label>'+esc(f.label || f.name)+'</label><input type="number" id="'+id+'" value="'+(f.default||0)+'">';
    } else {
      html += '<label>'+esc(f.label || f.name)+'</label><input type="text" id="'+id+'" value="'+esc(f.default||'')+'" placeholder="'+esc(f.placeholder||'')+'">';
    }
  });
  container.innerHTML = html;
}

function collectAppFields(app) {
  const result = {};
  (app.fields || []).forEach(f => {
    const el = document.getElementById('sjField_' + f.name);
    if (!el) return;
    if (f.type === 'checkbox') result[f.name] = el.checked;
    else if (f.type === 'number') result[f.name] = +el.value;
    else result[f.name] = el.value;
  });
  return result;
}

async function submitJob() {
  const appId = document.getElementById('sjApp').value;
  const app = APPS_CACHE.find(a => a.app_id === appId);
  if (!app) return toast('Select an application', true);

  const fileInput = document.getElementById('sjFiles');
  const files = fileInput ? fileInput.files : [];
  const inputFileNames = [];
  for (let i = 0; i < files.length; i++) inputFileNames.push(files[i].name);

  /* Collect field values as parameters for server-side command resolution */
  const appFields = collectAppFields(app);
  const parameters = {};
  Object.entries(appFields).forEach(([k, v]) => {
    parameters[k] = String(v);
  });

  const body = {
    app_id: appId,
    parameters: parameters,
    req_cores: app.req_cores || 1,
    req_ram_mb: app.req_ram_mb || 0,
    req_gpu: app.req_gpu || 0,
    req_disk_mb: app.req_disk_mb || 0,
    priority: +document.getElementById('sjPri').value,
    timeout_seconds: +document.getElementById('sjTimeout').value
  };
  if (inputFileNames.length) body.input_files = inputFileNames;

  try {
    const job = await api('POST', '/jobs', body);
    if (files.length) {
      toast('Uploading ' + files.length + ' file(s)...');
      for (let i = 0; i < files.length; i++) {
        const buf = await files[i].arrayBuffer();
        await apiRaw('POST', '/jobs/' + job.id + '/input/' + encodeURIComponent(files[i].name),
                     buf, 'application/octet-stream');
      }
      toast('Job submitted with ' + files.length + ' file(s)');
    } else {
      toast('Job submitted');
    }
    closeModal(); loadJobs();
  } catch(e) { toast(e.data?.error || 'Submit failed', true); }
}

async function cancelJob(id) {
  try { await api('DELETE', '/jobs/' + id); toast('Job cancelled'); loadJobs(); }
  catch(e) { toast(e.data?.error || 'Cancel failed', true); }
}

/* ── Job Detail Modal ──────────────────────────── */
async function showJobDetail(id) {
  try {
    const j = await api('GET', '/jobs/' + id);
    let files = { input: [], output: [], logs: {} };
    try { files = await api('GET', '/jobs/' + id + '/files'); } catch {}

    const dur = j.started_at && j.ended_at ? Math.round(j.ended_at - j.started_at) + 's'
              : j.started_at ? 'running...' : '—';

    let html = '<h2>Job Details</h2>'
      + '<div style="display:grid;grid-template-columns:1fr 1fr;gap:8px 20px;font-size:13px;margin-bottom:16px">'
      + '<div><span style="color:var(--text2)">ID</span><br><span style="font-family:monospace;font-size:12px">' + esc(j.id) + '</span></div>'
      + '<div><span style="color:var(--text2)">Status</span><br>' + statusBadge(j.status) + '</div>'
      + '<div><span style="color:var(--text2)">Application</span><br>' + esc(appLabel(j.app_id)) + '</div>'
      + '<div><span style="color:var(--text2)">Command</span><br><code style="font-size:12px;word-break:break-all">' + esc(j.command) + '</code></div>'
      + '<div><span style="color:var(--text2)">User</span><br>' + esc(j.user_id) + '</div>'
      + '<div><span style="color:var(--text2)">Machine</span><br>' + (esc(j.machine_id) || '—') + '</div>'
      + '<div><span style="color:var(--text2)">Exit Code</span><br>' + (j.exit_code !== undefined ? j.exit_code : '—') + '</div>'
      + '<div><span style="color:var(--text2)">Submitted</span><br>' + fmtDate(j.submitted_at) + '</div>'
      + '<div><span style="color:var(--text2)">Started</span><br>' + fmtDate(j.started_at) + '</div>'
      + '<div><span style="color:var(--text2)">Ended</span><br>' + fmtDate(j.ended_at) + '</div>'
      + '<div><span style="color:var(--text2)">Duration</span><br>' + dur + '</div>'
      + '<div><span style="color:var(--text2)">Resources</span><br>' + j.req_cores + ' cores, ' + j.req_ram_mb + ' MB RAM, ' + j.req_gpu + ' GPU</div>'
      + '<div><span style="color:var(--text2)">Timeout</span><br>' + (j.timeout_seconds ? j.timeout_seconds + 's' : 'none') + '</div>'
      + '</div>';

    if (j.status_reason) {
      html += '<div style="font-size:13px;color:var(--text2);margin-bottom:16px"><b>Reason:</b> ' + esc(j.status_reason) + '</div>';
    }

    /* Logs section */
    html += '<div class="card" style="margin-bottom:12px;padding:12px">'
      + '<div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:8px"><b style="font-size:14px">Logs</b>'
      + '<div style="display:flex;gap:6px">'
      + (files.logs.has_stdout ? '<button class="btn btn-sm btn-outline" onclick="viewLog(\''+id+'\',false)">stdout (' + fmtSize(files.logs.stdout_size) + ')</button>' : '<span style="font-size:12px;color:var(--text2)">no stdout</span>')
      + (files.logs.has_stderr ? '<button class="btn btn-sm btn-outline" onclick="viewLog(\''+id+'\',true)">stderr (' + fmtSize(files.logs.stderr_size) + ')</button>' : '<span style="font-size:12px;color:var(--text2)">no stderr</span>')
      + '</div></div>'
      + '<pre id="logContent" style="background:var(--bg);border:1px solid var(--border);border-radius:var(--radius);padding:10px;font-size:12px;max-height:200px;overflow:auto;white-space:pre-wrap;display:none"></pre>'
      + '</div>';

    /* Input files */
    if (files.input.length || (j.input_files && j.input_files.length)) {
      html += '<div class="card" style="margin-bottom:12px;padding:12px">'
        + '<b style="font-size:14px">Input Files</b>';
      if (j.input_files) html += '<div style="font-size:12px;color:var(--text2);margin-bottom:6px">Expected: ' + esc(j.input_files) + '</div>';
      if (files.input.length) {
        html += '<table style="font-size:12px"><thead><tr><th>Name</th><th>Size</th></tr></thead><tbody>';
        files.input.forEach(f => { html += '<tr><td>' + esc(f.name) + '</td><td>' + fmtSize(f.size) + '</td></tr>'; });
        html += '</tbody></table>';
      } else {
        html += '<div style="font-size:12px;color:var(--text2);margin-top:6px">No files uploaded yet</div>';
      }
      if (j.status === 'HELD') {
        html += '<div style="margin-top:8px">'
          + '<label style="font-size:12px">Upload files</label>'
          + '<input id="jdUpload" type="file" multiple style="padding:6px;font-size:12px">'
          + '<button class="btn btn-sm btn-primary" style="margin-top:4px" onclick="uploadToJob(\''+id+'\')">Upload</button>'
          + '</div>';
      }
      html += '</div>';
    }

    /* Output files */
    if (files.output.length) {
      html += '<div class="card" style="padding:12px">'
        + '<b style="font-size:14px">Output Files</b>'
        + '<table style="font-size:12px;margin-top:6px"><thead><tr><th>Name</th><th>Size</th><th></th></tr></thead><tbody>';
      files.output.forEach(f => {
        html += '<tr><td>' + esc(f.name) + '</td><td>' + fmtSize(f.size) + '</td>'
          + '<td><button class="btn btn-sm btn-outline" onclick="downloadFile(\''+id+'\',\''+esc(f.name)+'\')">Download</button></td></tr>';
      });
      html += '</tbody></table></div>';
    }

    html += '<div class="modal-actions"><button class="btn btn-outline" onclick="closeModal()">Close</button></div>';
    openModal(html);
  } catch(e) { toast('Failed to load job', true); }
}

async function viewLog(jobId, isStderr) {
  const el = document.getElementById('logContent');
  el.style.display = 'block';
  el.textContent = 'Loading...';
  try {
    const r = await apiRaw('GET', '/jobs/' + jobId + '/log' + (isStderr ? '/stderr' : ''));
    el.textContent = await r.text();
  } catch(e) { el.textContent = 'Error loading log'; }
}

async function uploadToJob(jobId) {
  const inp = document.getElementById('jdUpload');
  if (!inp || !inp.files.length) return toast('Select files first', true);
  try {
    for (let i = 0; i < inp.files.length; i++) {
      const buf = await inp.files[i].arrayBuffer();
      await apiRaw('POST', '/jobs/' + jobId + '/input/' + encodeURIComponent(inp.files[i].name),
                   buf, 'application/octet-stream');
    }
    toast(inp.files.length + ' file(s) uploaded');
    showJobDetail(jobId);
  } catch(e) { toast(e.data?.error || 'Upload failed', true); }
}

async function downloadFile(jobId, filename) {
  try {
    const r = await apiRaw('GET', '/jobs/' + jobId + '/output/' + encodeURIComponent(filename));
    const blob = await r.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url; a.download = filename;
    document.body.appendChild(a); a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  } catch(e) { toast('Download failed', true); }
}

/* ── Workflows ─────────────────────────────────── */

let _wfSteps = [];
let _wfEditId = null;   /* null = new, string = editing existing */
let _savedWorkflows = [];

async function loadWorkflowTab() {
  try {
    _savedWorkflows = await api('GET', '/workflows');
    if (!Array.isArray(_savedWorkflows)) _savedWorkflows = [];
  } catch { _savedWorkflows = []; }
  renderSavedWorkflows();
}

function renderSavedWorkflows() {
  const body = document.getElementById('savedWorkflowsBody');
  if (!body) return;
  if (!_savedWorkflows.length) {
    body.innerHTML = '<tr><td colspan="7" style="color:var(--text2)">No saved workflows</td></tr>';
    return;
  }
  /* Sort: favorites first, then by updated_at desc */
  const sorted = [..._savedWorkflows].sort((a, b) => {
    if (a.is_favorite !== b.is_favorite) return b.is_favorite ? 1 : -1;
    return (b.updated_at || 0) - (a.updated_at || 0);
  });
  body.innerHTML = sorted.map(w => {
    const stepCount = Array.isArray(w.steps) ? w.steps.length : 0;
    const isOwner = w.owner_id === USER_ID || USER_ROLE === 'admin';
    const favIcon = w.is_favorite ? '\u2605' : '\u2606';
    return '<tr>'
      + '<td style="cursor:pointer;font-size:18px;width:30px" onclick="wfToggleFavorite(\''+esc(w.id)+'\')" title="Toggle favorite">'+favIcon+'</td>'
      + '<td><b>'+esc(w.name)+'</b></td>'
      + '<td>'+stepCount+' step'+(stepCount!==1?'s':'')+'</td>'
      + '<td>'+esc(w.owner_id)+'</td>'
      + '<td>'+(w.is_global ? '<span style="color:var(--accent)">Global</span>' : 'Private')+'</td>'
      + '<td>'+fmtDate(w.updated_at)+'</td>'
      + '<td style="display:flex;gap:4px;flex-wrap:wrap">'
      + '<button class="btn btn-sm btn-primary" onclick="wfLoadAndRun(\''+esc(w.id)+'\')">Run</button>'
      + (isOwner ? '<button class="btn btn-sm btn-outline" onclick="wfLoadForEdit(\''+esc(w.id)+'\')">Edit</button>' : '')
      + '<button class="btn btn-sm btn-outline" onclick="wfDuplicate(\''+esc(w.id)+'\')">Duplicate</button>'
      + (isOwner ? '<button class="btn btn-sm btn-danger" onclick="wfDeleteSaved(\''+esc(w.id)+'\')">Delete</button>' : '')
      + '</td></tr>';
  }).join('');
}

async function wfToggleFavorite(wfId) {
  try {
    const res = await api('POST', '/workflows/' + wfId + '/favorite');
    const wf = _savedWorkflows.find(w => w.id === wfId);
    if (wf) wf.is_favorite = res.is_favorite;
    renderSavedWorkflows();
  } catch(e) { toast(e.data?.error || 'Failed to toggle favorite', true); }
}

async function wfDeleteSaved(wfId) {
  if (!confirm('Delete this workflow?')) return;
  try {
    await api('DELETE', '/workflows/' + wfId);
    toast('Workflow deleted');
    loadWorkflowTab();
  } catch(e) { toast(e.data?.error || 'Delete failed', true); }
}

function wfLoadForEdit(wfId) {
  const wf = _savedWorkflows.find(w => w.id === wfId);
  if (!wf) return;
  _wfEditId = wf.id;
  _wfSteps = (wf.steps || []).map(s => ({
    app_id: s.app_id || '',
    parameters: s.parameters || {},
    depends_on_steps: s.depends_on_steps || [],
    priority: s.priority || 50,
    timeout_seconds: s.timeout_seconds || 0,
    same_machine: !!s.same_machine,
    _files: []
  }));
  showWorkflowEditor(wf.name, wf.is_global);
}

function wfLoadAndRun(wfId) {
  const wf = _savedWorkflows.find(w => w.id === wfId);
  if (!wf) return;
  _wfEditId = null; /* not editing, just running */
  _wfSteps = (wf.steps || []).map(s => ({
    app_id: s.app_id || '',
    parameters: s.parameters || {},
    depends_on_steps: s.depends_on_steps || [],
    priority: s.priority || 50,
    timeout_seconds: s.timeout_seconds || 0,
    same_machine: !!s.same_machine,
    _files: []
  }));
  showWorkflowEditor(wf.name, false, true);
}

function wfDuplicate(wfId) {
  const wf = _savedWorkflows.find(w => w.id === wfId);
  if (!wf) return;
  _wfEditId = null;
  _wfSteps = (wf.steps || []).map(s => ({
    app_id: s.app_id || '',
    parameters: { ...s.parameters },
    depends_on_steps: [...(s.depends_on_steps || [])],
    priority: s.priority || 50,
    timeout_seconds: s.timeout_seconds || 0,
    same_machine: !!s.same_machine,
    _files: []
  }));
  showWorkflowEditor(wf.name + ' (copy)', false);
}

function showCreateWorkflow() {
  _wfEditId = null;
  _wfSteps = [makeEmptyStep()];
  showWorkflowEditor('', false);
}

function makeEmptyStep() {
  const step = { app_id: '', parameters: {}, depends_on_steps: [], priority: 50, timeout_seconds: 0, same_machine: false, _files: [] };
  if (APPS_CACHE.length) step.app_id = APPS_CACHE[0].app_id;
  return step;
}

function showWorkflowEditor(name, isGlobal, runMode) {
  const editor = document.getElementById('workflowEditor');
  if (editor) editor.style.display = 'block';
  const title = document.getElementById('wfEditorTitle');
  if (title) title.textContent = _wfEditId ? 'Edit Workflow' : (runMode ? 'Run Workflow' : 'New Workflow');
  renderWorkflowSteps(name, isGlobal, runMode);
}

function hideWorkflowEditor() {
  const editor = document.getElementById('workflowEditor');
  if (editor) editor.style.display = 'none';
  _wfSteps = [];
  _wfEditId = null;
}

/* Render a single field with the right widget type */
function renderWfField(stepIdx, field, value) {
  const id = 'wfF_' + stepIdx + '_' + field.name;
  if (field.type === 'checkbox') {
    const checked = value === true || value === 'true' ? 'checked' : '';
    return '<label class="checkbox-label" style="font-size:12px;margin-right:14px">'
      + '<input type="checkbox" id="'+id+'" '+checked
      + ' onchange="wfFieldChange('+stepIdx+',\''+esc(field.name)+'\',this.checked,\'checkbox\')"> '
      + esc(field.label || field.name) + '</label>';
  }
  if (field.type === 'select') {
    const options = (field.options || []).map(o =>
      '<option value="'+esc(o)+'" '+(String(value)===String(o)?'selected':'')+'>'+esc(o)+'</option>'
    ).join('');
    return '<div style="display:inline-block;margin-right:12px;margin-bottom:6px">'
      + '<label style="font-size:12px">'+esc(field.label || field.name)+'</label>'
      + '<select id="'+id+'" onchange="wfFieldChange('+stepIdx+',\''+esc(field.name)+'\',this.value,\'select\')" style="min-width:120px">'
      + options + '</select></div>';
  }
  if (field.type === 'number') {
    const numVal = value !== undefined && value !== '' ? value : (field.default || 0);
    return '<div style="display:inline-block;margin-right:12px;margin-bottom:6px">'
      + '<label style="font-size:12px">'+esc(field.label || field.name)+'</label>'
      + '<input type="number" id="'+id+'" value="'+esc(String(numVal))+'" style="width:100px"'
      + ' onchange="wfFieldChange('+stepIdx+',\''+esc(field.name)+'\',this.value,\'number\')"></div>';
  }
  /* default: text */
  const txtVal = value !== undefined ? value : (field.default || '');
  return '<div style="display:inline-block;margin-right:12px;margin-bottom:6px">'
    + '<label style="font-size:12px">'+esc(field.label || field.name)+'</label>'
    + '<input type="text" id="'+id+'" value="'+esc(String(txtVal))+'" style="max-width:180px"'
    + ' placeholder="'+esc(field.placeholder || '')+'"'
    + ' onchange="wfFieldChange('+stepIdx+',\''+esc(field.name)+'\',this.value,\'text\')"></div>';
}

function wfFieldChange(stepIdx, key, value, fieldType) {
  if (fieldType === 'checkbox') _wfSteps[stepIdx].parameters[key] = !!value;
  else if (fieldType === 'number') _wfSteps[stepIdx].parameters[key] = +value;
  else _wfSteps[stepIdx].parameters[key] = value;
}

function getAppFields(app) {
  /* Unify fields + parameters into a standard fields array */
  if (app.fields && app.fields.length) return app.fields;
  if (app.parameters) {
    return Object.entries(app.parameters).map(([k, def]) => ({
      name: k,
      type: def.type || 'text',
      label: k,
      default: def.default || '',
      options: def.options || [],
      placeholder: def.placeholder || ''
    }));
  }
  return [];
}

function renderWorkflowSteps(name, isGlobal, runMode) {
  const container = document.getElementById('workflowSteps');
  if (!container) return;
  const apps = APPS_CACHE;

  let html = '<div style="display:flex;gap:12px;margin-bottom:12px;flex-wrap:wrap;align-items:flex-end">'
    + '<div><label>Workflow Name</label>'
    + '<input id="wfName" value="'+esc(name||'')+'" placeholder="e.g. training-pipeline" style="max-width:300px"></div>';

  if (USER_ROLE === 'admin') {
    html += '<div><label class="checkbox-label" style="font-size:13px">'
      + '<input type="checkbox" id="wfGlobal" '+(isGlobal?'checked':'')+' > Share with all users (global)'
      + '</label></div>';
  }
  html += '</div>';

  _wfSteps.forEach((step, i) => {
    const appOpts = apps.map(a =>
      '<option value="'+esc(a.app_id)+'" '+(a.app_id===step.app_id?'selected':'')+'>'+esc(a.name||a.app_id)+'</option>'
    ).join('');

    /* depends_on checkboxes for previous steps */
    let depChecks = '';
    for (let d = 0; d < i; d++) {
      const checked = step.depends_on_steps.includes(d) ? 'checked' : '';
      const depApp = APPS_CACHE.find(a => a.app_id === _wfSteps[d].app_id);
      const depLabel = 'Step '+(d+1)+(depApp ? ' ('+esc(depApp.name||depApp.app_id)+')' : '');
      depChecks += '<label class="checkbox-label" style="font-size:12px;margin-right:10px">'
        + '<input type="checkbox" onchange="wfToggleDep('+i+','+d+')" '+checked+'> '+depLabel
        + '</label>';
    }

    /* Render typed fields */
    const app = apps.find(a => a.app_id === step.app_id);
    let paramFields = '';
    if (app) {
      const fields = getAppFields(app);
      fields.forEach(f => {
        const val = step.parameters[f.name] !== undefined ? step.parameters[f.name] : f.default;
        paramFields += renderWfField(i, f, val);
      });
    }

    html += '<div class="card" style="padding:12px;margin-bottom:10px;border-left:3px solid var(--accent)">'
      + '<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:8px">'
      + '<b>Step '+(i+1)+(app?' — '+esc(app.name||app.app_id):'')+'</b>'
      + '<button class="btn btn-sm btn-danger" onclick="wfRemoveStep('+i+')">Remove</button>'
      + '</div>'
      + '<div style="display:flex;gap:12px;flex-wrap:wrap;margin-bottom:8px">'
      + '<div><label style="font-size:12px">Application</label>'
      + '<select onchange="wfAppChange('+i+',this.value)" style="min-width:180px">'+appOpts+'</select></div>'
      + '<div><label style="font-size:12px">Priority</label>'
      + '<input type="number" value="'+step.priority+'" min="0" max="100" style="width:70px" onchange="wfSetPriority('+i+',this.value)"></div>'
      + '<div><label style="font-size:12px">Timeout (s)</label>'
      + '<input type="number" value="'+step.timeout_seconds+'" min="0" style="width:80px" onchange="wfSetTimeout('+i+',this.value)"></div>'
      + '</div>'
      + (app ? '<div class="resource-bar" style="margin-bottom:8px;font-size:12px;display:flex;gap:12px;color:var(--text2)">'
        + '<span>Cores: '+(app.req_cores||1)+'</span>'
        + '<span>RAM: '+(app.req_ram_mb||0)+' MB</span>'
        + '<span>Disk: '+(app.req_disk_mb||0)+' MB</span>'
        + '<span>GPU: '+(app.req_gpu||0)+'</span>'
        + '</div>' : '')
      + (paramFields ? '<div style="margin-bottom:8px;display:flex;flex-wrap:wrap;align-items:flex-end;gap:4px">'+paramFields+'</div>' : '')
      + '<div style="margin-bottom:8px"><label style="font-size:12px">Input Files</label>'
      + '<input type="file" multiple style="padding:6px;font-size:12px" onchange="wfSetFiles('+i+',this)">'
      + (step._files && step._files.length ? '<span style="font-size:11px;color:var(--text2);margin-left:8px">'+step._files.length+' file(s) selected</span>' : '')
      + '</div>'
      + (i > 0 ? '<div><label style="font-size:12px">Depends on:</label> '+depChecks
        + (step.depends_on_steps.length ? '<label class="checkbox-label" style="font-size:12px;margin-left:14px">'
          + '<input type="checkbox" onchange="_wfSteps['+i+'].same_machine=this.checked" '+(step.same_machine?'checked':'')
          + '> Use same machine</label>' : '')
        + '</div>' : '')
      + '</div>';
  });

  html += '<div style="display:flex;gap:8px;margin-top:12px;flex-wrap:wrap">'
    + '<button class="btn btn-sm btn-outline" onclick="wfAddStep()">+ Add Step</button>';
  if (!runMode) {
    html += '<button class="btn btn-sm btn-outline" onclick="wfSave(false)">'+(_wfEditId?'Update':'Save')+' Workflow</button>';
  }
  html += '<button class="btn btn-sm btn-primary" onclick="wfSubmit()">Run Workflow</button>'
    + '<button class="btn btn-sm btn-outline" onclick="hideWorkflowEditor()">Cancel</button>'
    + '</div>';

  container.innerHTML = html;
}

function wfAddStep() {
  _wfSteps.push(makeEmptyStep());
  wfRerender();
}

function wfRemoveStep(idx) {
  _wfSteps.splice(idx, 1);
  _wfSteps.forEach(s => {
    s.depends_on_steps = s.depends_on_steps
      .filter(d => d !== idx)
      .map(d => d > idx ? d - 1 : d);
  });
  wfRerender();
}

function wfAppChange(idx, appId) {
  _wfSteps[idx].app_id = appId;
  _wfSteps[idx].parameters = {};
  wfRerender();
}

function wfToggleDep(stepIdx, depIdx) {
  const deps = _wfSteps[stepIdx].depends_on_steps;
  const pos = deps.indexOf(depIdx);
  if (pos >= 0) deps.splice(pos, 1);
  else deps.push(depIdx);
}

function wfSetPriority(idx, val) { _wfSteps[idx].priority = +val; }
function wfSetTimeout(idx, val) { _wfSteps[idx].timeout_seconds = +val; }
function wfSetFiles(idx, input) {
  _wfSteps[idx]._files = input.files ? Array.from(input.files) : [];
}

/* Re-render preserving name/global inputs */
function wfRerender() {
  const nameEl = document.getElementById('wfName');
  const globalEl = document.getElementById('wfGlobal');
  const name = nameEl ? nameEl.value : '';
  const isGlobal = globalEl ? globalEl.checked : false;
  renderWorkflowSteps(name, isGlobal);
}

function collectWfSteps() {
  return _wfSteps.map(s => {
    const params = {};
    Object.entries(s.parameters).forEach(([k, v]) => { params[k] = String(v); });
    const obj = { app_id: s.app_id, parameters: params, priority: s.priority };
    if (s.timeout_seconds > 0) obj.timeout_seconds = s.timeout_seconds;
    if (s.depends_on_steps.length) obj.depends_on_steps = [...s.depends_on_steps];
    if (s.same_machine && s.depends_on_steps.length) obj.same_machine = true;
    if (s._files && s._files.length) obj.input_files = s._files.map(f => f.name);
    const app = APPS_CACHE.find(a => a.app_id === s.app_id);
    if (app) {
      if (app.req_cores) obj.req_cores = app.req_cores;
      if (app.req_ram_mb) obj.req_ram_mb = app.req_ram_mb;
      if (app.req_gpu) obj.req_gpu = app.req_gpu;
      if (app.req_disk_mb) obj.req_disk_mb = app.req_disk_mb;
    }
    return obj;
  });
}

async function wfSave() {
  if (!_wfSteps.length) return toast('Add at least one step', true);
  const nameEl = document.getElementById('wfName');
  const name = nameEl ? nameEl.value.trim() : '';
  if (!name) return toast('Workflow name is required', true);

  const globalEl = document.getElementById('wfGlobal');
  const isGlobal = globalEl ? globalEl.checked : false;

  const body = { name, steps: collectWfSteps(), is_global: isGlobal };
  if (_wfEditId) body.id = _wfEditId;

  try {
    await api('POST', '/workflows', body);
    toast(_wfEditId ? 'Workflow updated' : 'Workflow saved');
    hideWorkflowEditor();
    loadWorkflowTab();
  } catch(e) { toast(e.data?.error || 'Save failed', true); }
}

async function wfSubmit() {
  if (!_wfSteps.length) return toast('Add at least one step', true);

  const body = { steps: collectWfSteps() };
  const nameEl = document.getElementById('wfName');
  if (nameEl && nameEl.value.trim()) body.name = nameEl.value.trim();

  try {
    const result = await api('POST', '/workflows/run', body);
    const jobs = result.jobs || [];

    /* Upload files for each step that has them */
    let totalUploads = 0;
    for (let i = 0; i < _wfSteps.length && i < jobs.length; i++) {
      const files = _wfSteps[i]._files || [];
      if (!files.length) continue;
      const jobId = jobs[i].id;
      for (let f = 0; f < files.length; f++) {
        const buf = await files[f].arrayBuffer();
        await apiRaw('POST', '/jobs/' + jobId + '/input/' + encodeURIComponent(files[f].name),
                     buf, 'application/octet-stream');
        totalUploads++;
      }
    }

    const msg = 'Workflow submitted: ' + jobs.length + ' jobs created'
      + (totalUploads ? ', ' + totalUploads + ' file(s) uploaded' : '');
    toast(msg);
    hideWorkflowEditor();
    loadJobs();
  } catch(e) { toast(e.data?.error || 'Workflow submission failed', true); }
}
