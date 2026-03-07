/* ── jobs.js — Jobs tab, submission, detail modal ────────────────── */

async function loadJobs() {
  try {
    const jobs = await api('GET', '/jobs');
    const list = Array.isArray(jobs) ? jobs : [];
    document.getElementById('jobsBody').innerHTML = list.map(j => {
      const appName = appLabel(j.app_id);
      return '<tr style="cursor:pointer" onclick="showJobDetail(\''+j.id+'\')">'
        +'<td title="'+esc(j.id)+'">'+shortId(j.id)+'</td>'
        +'<td>'+esc(j.command)+'</td>'
        +'<td>'+statusBadge(j.status)+'</td>'
        +'<td>'+esc(appName)+'</td>'
        +'<td>'+esc(j.user_id)+'</td>'
        +'<td>'+fmtDate(j.submitted_at)+'</td>'
        +'<td>'+(j.status==='QUEUED'||j.status==='RUNNING'||j.status==='HELD'
          ?'<button class="btn btn-sm btn-danger" onclick="event.stopPropagation();cancelJob(\''+j.id+'\')">Cancel</button>':'')
        +'</td></tr>';
    }).join('') || '<tr><td colspan="7" style="color:var(--text2)">No jobs</td></tr>';
  } catch(e) { toast('Failed to load jobs', true); }
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
    <label>Command</label><input id="sjCmd" placeholder="auto-filled from app">
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

  /* Set command template */
  const cmdEl = document.getElementById('sjCmd');
  if (cmdEl && app.command_template) cmdEl.value = app.command_template;

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

  /* Build command: template + app field values as args */
  let cmd = document.getElementById('sjCmd').value;
  const appFields = collectAppFields(app);
  /* Append field values as command arguments */
  Object.entries(appFields).forEach(([k, v]) => {
    if (typeof v === 'boolean') { if (v) cmd += ' --' + k; }
    else cmd += ' --' + k + ' ' + v;
  });

  const body = {
    command: cmd,
    app_id: appId,
    req_cores: app.req_cores || 1,
    req_ram_mb: app.req_ram_mb || 0,
    req_gpu: app.req_gpu || 0,
    req_disk_mb: app.req_disk_mb || 0,
    priority: +document.getElementById('sjPri').value,
    timeout_seconds: +document.getElementById('sjTimeout').value
  };
  if (inputFileNames.length) body.input_files = inputFileNames;
  if (!body.command) return toast('Command is required', true);

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
