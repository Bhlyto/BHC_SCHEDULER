/* ── dashboard.js — Dashboard tab ─────────────────────────────────── */

async function loadDashboard() {
  try {
    const data = await api('GET', '/stats');
    const stats = data.jobs || {};
    const grid = document.getElementById('statsGrid');
    grid.innerHTML = [
      { label: 'Total', value: stats.total || 0, color: 'var(--text)' },
      { label: 'Running', value: stats.running || 0, color: 'var(--accent)' },
      { label: 'Queued', value: stats.in_queue || stats.queued || 0, color: 'var(--yellow)' },
      { label: 'Held', value: stats.held || 0, color: 'var(--orange)' },
      { label: 'Finished', value: stats.finished || 0, color: 'var(--green)' },
      { label: 'Failed', value: stats.failed || 0, color: 'var(--red)' },
    ].map(s => '<div class="stat"><div class="value" style="color:'+s.color+'">'+s.value+'</div><div class="label">'+s.label+'</div></div>').join('');

    const jobs = await api('GET', '/jobs');
    const recent = (Array.isArray(jobs) ? jobs : []).slice(0, 10);
    document.getElementById('recentJobsBody').innerHTML = recent.map(j =>
      '<tr style="cursor:pointer" data-action="job-detail" data-job-id="'+esc(j.id)+'"><td title="'+esc(j.id)+'">'+esc(shortId(j.id))+'</td><td>'+esc(j.command)+'</td><td>'+statusBadge(j.status)+'</td><td>'+esc(j.user_id)+'</td><td>'+fmtDate(j.submitted_at)+'</td></tr>'
    ).join('') || '<tr><td colspan="5" style="color:var(--text2)">No jobs</td></tr>';
  } catch(e) { toast('Failed to load dashboard', true); }
}
