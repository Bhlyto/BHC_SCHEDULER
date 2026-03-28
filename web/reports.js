/* reports.js — Reporting & Analytics UI */

(function () {
    'use strict';

    const API = window.API_BASE || '';
    let currentApiKey = '';

    function setApiKey(key) { currentApiKey = key; }
    window.reportsSetApiKey = setApiKey;

    function headers() {
        return { 'X-API-Key': currentApiKey, 'Content-Type': 'application/json' };
    }

    async function apiFetch(url) {
        const res = await fetch(API + url, { headers: headers() });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return res.json();
    }

    async function apiPost(url, body) {
        const res = await fetch(API + url, {
            method: 'POST', headers: headers(), body: JSON.stringify(body)
        });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return res.json();
    }

    /* ── Simple bar chart using CSS ──────────────────────────────── */
    function renderBarChart(container, labels, values, color) {
        container.innerHTML = '';
        if (!values.length) { container.textContent = 'No data'; return; }
        const max = Math.max(...values, 1);
        const chart = document.createElement('div');
        chart.className = 'report-bar-chart';
        labels.forEach((label, i) => {
            const row = document.createElement('div');
            row.className = 'bar-row';
            const lbl = document.createElement('span');
            lbl.className = 'bar-label';
            lbl.textContent = label;
            const barWrap = document.createElement('div');
            barWrap.className = 'bar-wrap';
            const bar = document.createElement('div');
            bar.className = 'bar-fill';
            bar.style.width = (values[i] / max * 100) + '%';
            bar.style.background = color || 'var(--accent)';
            const val = document.createElement('span');
            val.className = 'bar-value';
            val.textContent = values[i];
            barWrap.appendChild(bar);
            row.appendChild(lbl);
            row.appendChild(barWrap);
            row.appendChild(val);
            chart.appendChild(row);
        });
        container.appendChild(chart);
    }

    /* ── Date range helpers ──────────────────────────────────────── */
    function getTimeRange() {
        const range = document.getElementById('report-range');
        if (!range) return { from: 0, to: 0 };
        const val = range.value;
        const now = Math.floor(Date.now() / 1000);
        if (val === '24h') return { from: now - 86400, to: 0 };
        if (val === '7d')  return { from: now - 86400 * 7, to: 0 };
        if (val === '30d') return { from: now - 86400 * 30, to: 0 };
        if (val === '90d') return { from: now - 86400 * 90, to: 0 };
        return { from: 0, to: 0 }; /* all time */
    }

    /* ── Jobs over time ──────────────────────────────────────────── */
    async function loadJobsReport() {
        const { from, to } = getTimeRange();
        const gran = document.getElementById('report-granularity')?.value || 'day';
        const data = await apiFetch(`/admin/reports/jobs?granularity=${gran}&from=${from}&to=${to}`);

        const container = document.getElementById('report-jobs-chart');
        const labels = data.map(d => d.period);
        const values = data.map(d => d.total);
        renderBarChart(container, labels, values, '#4fc3f7');

        /* Summary table */
        const tbody = document.getElementById('report-jobs-table');
        tbody.innerHTML = '';
        data.forEach(d => {
            const tr = document.createElement('tr');
            tr.innerHTML = `<td>${d.period}</td><td>${d.total}</td><td>${d.finished}</td>` +
                `<td>${d.failed}</td><td>${d.avg_duration_s ? d.avg_duration_s.toFixed(1) + 's' : '-'}</td>`;
            tbody.appendChild(tr);
        });
    }

    /* ── Users report ────────────────────────────────────────────── */
    async function loadUsersReport() {
        const { from, to } = getTimeRange();
        const data = await apiFetch(`/admin/reports/users?from=${from}&to=${to}`);
        const container = document.getElementById('report-users-chart');
        renderBarChart(container, data.map(d => d.user_id), data.map(d => d.total_jobs), '#81c784');

        const tbody = document.getElementById('report-users-table');
        tbody.innerHTML = '';
        data.forEach(d => {
            const tr = document.createElement('tr');
            tr.innerHTML = `<td>${d.user_id}</td><td>${d.total_jobs}</td><td>${d.finished}</td>` +
                `<td>${d.failed}</td><td>${d.avg_duration_s ? d.avg_duration_s.toFixed(1) + 's' : '-'}</td>` +
                `<td>${d.total_cores_used}</td><td>${d.total_ram_mb_used}</td>`;
            tbody.appendChild(tr);
        });
    }

    /* ── Apps report ─────────────────────────────────────────────── */
    async function loadAppsReport() {
        const { from, to } = getTimeRange();
        const data = await apiFetch(`/admin/reports/apps?from=${from}&to=${to}`);
        const container = document.getElementById('report-apps-chart');
        renderBarChart(container, data.map(d => d.app_id), data.map(d => d.total_jobs), '#ffb74d');

        const tbody = document.getElementById('report-apps-table');
        tbody.innerHTML = '';
        data.forEach(d => {
            const tr = document.createElement('tr');
            tr.innerHTML = `<td>${d.app_id}</td><td>${d.total_jobs}</td><td>${d.finished}</td>` +
                `<td>${d.failed}</td><td>${d.avg_duration_s ? d.avg_duration_s.toFixed(1) + 's' : '-'}</td>`;
            tbody.appendChild(tr);
        });
    }

    /* ── Machines report ─────────────────────────────────────────── */
    async function loadMachinesReport() {
        const { from, to } = getTimeRange();
        const data = await apiFetch(`/admin/reports/machines?from=${from}&to=${to}`);
        const container = document.getElementById('report-machines-chart');
        renderBarChart(container, data.map(d => d.machine_id), data.map(d => d.total_allocations), '#e57373');

        const tbody = document.getElementById('report-machines-table');
        tbody.innerHTML = '';
        data.forEach(d => {
            const tr = document.createElement('tr');
            tr.innerHTML = `<td>${d.machine_id}</td><td>${d.total_allocations}</td>` +
                `<td>${d.total_cores_reserved}</td><td>${d.total_ram_mb_reserved}</td>` +
                `<td>${d.avg_utilization_pct ? d.avg_utilization_pct.toFixed(1) + '%' : '-'}</td>`;
            tbody.appendChild(tr);
        });
    }

    /* ── Machine status (live probe) ─────────────────────────────── */
    async function loadMachineStatus() {
        const data = await apiFetch('/admin/machines/status');
        const container = document.getElementById('machines-status-grid');
        container.innerHTML = '';
        data.forEach(m => {
            const card = document.createElement('div');
            card.className = `machine-card status-${m.status}`;
            card.innerHTML = `
                <div class="machine-name">${esc(m.id)}</div>
                <div class="machine-meta">${esc(m.hostname || m.ip)}</div>
                <div class="machine-status-badge">${m.status.toUpperCase()}</div>
                <div class="machine-detail">
                    Type: ${m.type} ${m.cloud_provider ? '(' + esc(m.cloud_provider) + ')' : ''}<br>
                    Cores: ${m.cores_reserved}/${m.cores_total} | RAM: ${m.ram_mb_reserved}/${m.ram_mb_total} MB
                    ${m.mac_address ? '<br>MAC: ' + esc(m.mac_address) : ''}
                </div>
                ${m.mac_address && m.status === 'offline' ? '<button class="btn-wol" data-id="' + esc(m.id) + '">Wake (WoL)</button>' : ''}
            `;
            container.appendChild(card);
        });

        /* Bind WoL buttons */
        container.querySelectorAll('.btn-wol').forEach(btn => {
            btn.onclick = async () => {
                try {
                    await apiPost('/admin/wol', { machine_id: btn.dataset.id });
                    btn.textContent = 'Sent!';
                    btn.disabled = true;
                } catch (e) { btn.textContent = 'Failed'; }
            };
        });
    }

    /* ── Events log ──────────────────────────────────────────────── */
    async function loadEventsLog() {
        const { from, to } = getTimeRange();
        const cat = document.getElementById('event-category-filter')?.value || '';
        let url = `/admin/events?limit=200&from=${from}&to=${to}`;
        if (cat) url += `&category=${encodeURIComponent(cat)}`;
        const data = await apiFetch(url);

        const tbody = document.getElementById('events-table-body');
        tbody.innerHTML = '';
        data.forEach(ev => {
            const tr = document.createElement('tr');
            const dt = new Date(ev.created_at * 1000).toLocaleString();
            tr.innerHTML = `<td>${dt}</td><td>${esc(ev.category)}</td><td>${esc(ev.event_type)}</td>` +
                `<td>${esc(ev.detail)}</td><td>${esc(ev.user_id)}</td>` +
                `<td>${esc(ev.job_id)}</td><td>${esc(ev.machine_id)}</td>`;
            tbody.appendChild(tr);
        });
    }

    /* ── Cloud Provisioning UI ───────────────────────────────────── */
    async function cloudProvision() {
        const form = document.getElementById('cloud-provision-form');
        if (!form) return;
        const spec = {
            provider:      form.querySelector('[name=provider]').value,
            instance_type: form.querySelector('[name=instance_type]').value,
            region:        form.querySelector('[name=region]').value,
            image_id:      form.querySelector('[name=image_id]').value,
            cores:         parseInt(form.querySelector('[name=cores]').value) || 0,
            ram_mb:        parseInt(form.querySelector('[name=ram_mb]').value) || 0,
            disk_mb:       parseInt(form.querySelector('[name=disk_mb]').value) || 0,
            cores_min:     parseInt(form.querySelector('[name=cores_min]').value) || 0,
            ram_mb_min:    parseInt(form.querySelector('[name=ram_mb_min]').value) || 0,
            disk_mb_min:   parseInt(form.querySelector('[name=disk_mb_min]').value) || 0
        };
        try {
            const result = await apiPost('/admin/cloud/provision', spec);
            alert('Provisioned: ' + result.machine_id);
            loadMachineStatus();
        } catch (e) {
            alert('Provisioning failed: ' + e.message);
        }
    }

    function esc(s) {
        if (!s) return '';
        const d = document.createElement('div');
        d.textContent = s;
        return d.innerHTML;
    }

    /* ── Initialize ──────────────────────────────────────────────── */
    function initReports() {
        const refreshBtn = document.getElementById('reports-refresh');
        if (refreshBtn) refreshBtn.onclick = loadAllReports;

        const range = document.getElementById('report-range');
        if (range) range.onchange = loadAllReports;

        const gran = document.getElementById('report-granularity');
        if (gran) gran.onchange = loadJobsReport;

        const catFilter = document.getElementById('event-category-filter');
        if (catFilter) catFilter.onchange = loadEventsLog;

        const cloudBtn = document.getElementById('cloud-provision-btn');
        if (cloudBtn) cloudBtn.onclick = cloudProvision;
    }

    async function loadAllReports() {
        try {
            await Promise.all([
                loadJobsReport(),
                loadUsersReport(),
                loadAppsReport(),
                loadMachinesReport(),
                loadMachineStatus(),
                loadEventsLog()
            ]);
        } catch (e) {
            console.error('Reports load error:', e);
        }
    }

    window.initReports = initReports;
    window.loadAllReports = loadAllReports;
    window.loadMachineStatus = loadMachineStatus;
})();
