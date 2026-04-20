// ════════════════════════════════════════════════════
//  GardenNode Dashboard — app.js
//  Supabase real-time + Chart.js + Demo mode
// ════════════════════════════════════════════════════

// ── USER CONFIG (filled via modal or localStorage) ──
let SUPABASE_URL   = localStorage.getItem('gn_url')   || '';
let SUPABASE_KEY   = localStorage.getItem('gn_key')   || '';
let SUPABASE_TABLE = localStorage.getItem('gn_table') || 'garden_data';
const SOIL_THRESHOLD = 30.0;

// ── STATE ──
let supabaseClient = null;
let chart         = null;
let packetCount   = 0;
let alertCount    = 0;
let demoInterval  = null;
let isDemoMode    = false;

// ── PUMP STATE ──
let pumpMode      = 'AUTO'; // 'AUTO' | 'MANUAL'
let pumpForcedOn  = false;
let soilThreshold = SOIL_THRESHOLD;

// ════════════════ BOOT ════════════════
document.addEventListener('DOMContentLoaded', () => {
  initChart();
  bindUI();

  if (SUPABASE_URL && SUPABASE_KEY) {
    hideModal();
    connectSupabase(SUPABASE_URL, SUPABASE_KEY);
  } else {
    // No credentials saved — boot straight into demo mode.
    // User can click "Settings" in the sidebar to connect to Supabase.
    hideModal();
    startDemoMode();
  }
});

// ════════════════ CHART ════════════════
function initChart() {
  const ctx = document.getElementById('historyChart').getContext('2d');

  const soilGrad = ctx.createLinearGradient(0, 0, 0, 240);
  soilGrad.addColorStop(0, 'rgba(74,222,128,0.25)');
  soilGrad.addColorStop(1, 'rgba(74,222,128,0)');

  const tempGrad = ctx.createLinearGradient(0, 0, 0, 240);
  tempGrad.addColorStop(0, 'rgba(251,191,36,0.2)');
  tempGrad.addColorStop(1, 'rgba(251,191,36,0)');

  chart = new Chart(ctx, {
    type: 'line',
    data: {
      labels: [],
      datasets: [
        {
          label: 'Soil Moisture (%)',
          data: [],
          borderColor: '#4ade80',
          backgroundColor: soilGrad,
          borderWidth: 2,
          pointRadius: 0,
          pointHoverRadius: 4,
          pointHoverBackgroundColor: '#4ade80',
          tension: 0.4,
          fill: true,
          yAxisID: 'ySoil',
        },
        {
          label: 'Temperature (°C)',
          data: [],
          borderColor: '#fbbf24',
          backgroundColor: tempGrad,
          borderWidth: 2,
          pointRadius: 0,
          pointHoverRadius: 4,
          pointHoverBackgroundColor: '#fbbf24',
          tension: 0.4,
          fill: true,
          yAxisID: 'yTemp',
        }
      ]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      interaction: { mode: 'index', intersect: false },
      plugins: {
        legend: { display: false },
        tooltip: {
          backgroundColor: '#0d1a14',
          borderColor: 'rgba(255,255,255,0.08)',
          borderWidth: 1,
          titleColor: '#8aab95',
          bodyColor: '#e2f0e8',
          padding: 10,
          callbacks: {
            label: (ctx) => {
              const unit = ctx.datasetIndex === 0 ? '%' : '°C';
              return ` ${ctx.dataset.label.split(' (')[0]}: ${ctx.parsed.y.toFixed(1)}${unit}`;
            }
          }
        }
      },
      scales: {
        x: {
          grid: { color: 'rgba(255,255,255,0.04)' },
          ticks: { color: '#5a7a65', font: { family: 'JetBrains Mono', size: 10 }, maxTicksLimit: 8 }
        },
        ySoil: {
          position: 'left',
          min: 0, max: 100,
          grid: { color: 'rgba(255,255,255,0.04)' },
          ticks: { color: '#4ade80', font: { family: 'JetBrains Mono', size: 10 }, callback: v => v + '%' }
        },
        yTemp: {
          position: 'right',
          min: 0, max: 50,
          grid: { display: false },
          ticks: { color: '#fbbf24', font: { family: 'JetBrains Mono', size: 10 }, callback: v => v + '°' }
        }
      }
    }
  });
}

// ════════════════ SUPABASE ════════════════
function connectSupabase(url, key) {
  try {
    supabaseClient = window.supabase.createClient(url, key);
    setConnectionStatus('connecting');

    // Load historical data
    fetchHistory();

    // Subscribe to real-time inserts
    supabaseClient
      .channel('garden_realtime')
      .on('postgres_changes', {
        event: 'INSERT',
        schema: 'public',
        table: SUPABASE_TABLE
      }, (payload) => {
        handleNewReading(payload.new);
      })
      .subscribe((status) => {
        if (status === 'SUBSCRIBED') setConnectionStatus('connected');
        if (status === 'CHANNEL_ERROR') setConnectionStatus('disconnected');
      });

  } catch (e) {
    console.error('Supabase error:', e);
    setConnectionStatus('disconnected');
  }
}

async function fetchHistory() {
  const { data, error } = await supabaseClient
    .from(SUPABASE_TABLE)
    .select('*')
    .order('created_at', { ascending: false })
    .limit(50);

  if (error) { console.error('Fetch error:', error); return; }
  if (!data || !data.length) return;

  // Reverse so oldest first for chart
  const rows = [...data].reverse();
  rows.forEach(row => pushToChart(row));
  updateCards(data[0]); // Latest reading for stat cards
  populateTable(data.slice(0, 10));
}

function handleNewReading(row) {
  packetCount++;
  document.getElementById('packet-count').textContent = packetCount;
  updateCards(row);
  pushToChart(row);
  prependTableRow(row);
  setLastUpdated();
}

// ════════════════ CARDS ════════════════
function updateCards(row) {
  const soil  = parseFloat(row.soil_moisture);
  const temp  = parseFloat(row.temperature);
  const relay = row.relay_status === true || row.relay_status === 'true';

  updateGauge(soil);
  updateTemp(temp);
  updatePump(relay);
}

function updateGauge(val) {
  const pct    = Math.min(Math.max(val, 0), 100);
  // 270° arc: total stroke of gauge-track (dasharray 377 out of 502)
  const total  = 377;
  const filled = (pct / 100) * total;

  document.getElementById('gauge-fill').setAttribute('stroke-dasharray', `${filled} 502`);
  document.getElementById('gauge-value').textContent = pct.toFixed(0);

  let state = 'CRITICAL';
  if      (pct >= 60)  state = 'OPTIMAL';
  else if (pct >= 30)  state = 'MODERATE';
  else if (pct >= 15)  state = 'LOW';

  const stateEl = document.getElementById('gauge-state');
  stateEl.textContent = state;
  stateEl.style.fill =
    pct >= 60 ? '#4ade80' :
    pct >= 30 ? '#fbbf24' : '#f87171';
}

function updateTemp(val) {
  document.getElementById('temp-value').textContent = val.toFixed(1);
  const pct = Math.min((val / 50) * 100, 100);
  document.getElementById('temp-bar').style.width = pct + '%';

  let status = 'Normal';
  if (val > 38) status = 'High — risk of heat stress';
  else if (val > 30) status = 'Warm';
  else if (val < 10) status = 'Cold';

  document.getElementById('temp-status').textContent = status;
}

function updatePump(active) {
  const card = document.getElementById('pump-card');
  const text = document.getElementById('pump-text');
  const sub  = document.getElementById('pump-sub');

  card.classList.toggle('pump-on',  active);
  card.classList.toggle('pump-off', !active);
  text.textContent = active ? 'ACTIVE' : 'IDLE';
  sub.textContent  = active ? 'Irrigation running' : 'Relay open — pump off';
}

// ════════════════ CHART HELPERS ════════════════
function pushToChart(row) {
  const label = formatTime(row.created_at);
  chart.data.labels.push(label);
  chart.data.datasets[0].data.push(parseFloat(row.soil_moisture));
  chart.data.datasets[1].data.push(parseFloat(row.temperature));

  // Cap at 50 points
  if (chart.data.labels.length > 50) {
    chart.data.labels.shift();
    chart.data.datasets.forEach(ds => ds.data.shift());
  }
  chart.update('none');
}

// ════════════════ TABLE ════════════════
function populateTable(rows) {
  const tbody = document.getElementById('readings-body');
  tbody.innerHTML = '';
  rows.forEach(row => tbody.appendChild(buildRow(row)));
}

function prependTableRow(row) {
  const tbody = document.getElementById('readings-body');
  const empty = tbody.querySelector('.empty-row');
  if (empty) tbody.innerHTML = '';

  tbody.insertBefore(buildRow(row), tbody.firstChild);
  // Keep max 10 rows
  while (tbody.children.length > 10) tbody.removeChild(tbody.lastChild);
}

function buildRow(row) {
  const tr = document.createElement('tr');
  const relay = row.relay_status === true || row.relay_status === 'true';
  tr.innerHTML = `
    <td>${formatDateTime(row.created_at)}</td>
    <td>${parseFloat(row.soil_moisture).toFixed(1)}%</td>
    <td>${parseFloat(row.temperature).toFixed(1)} °C</td>
    <td><span class="pill ${relay ? 'pill-on' : 'pill-off'}">${relay ? 'ON' : 'OFF'}</span></td>
  `;
  return tr;
}

// ════════════════ DEMO MODE ════════════════
function startDemoMode() {
  isDemoMode = true;
  setConnectionStatus('demo');

  let tick = 0;
  const base = { soil: 55, temp: 24 };

  function generateReading() {
    tick++;
    base.soil += (Math.random() - 0.48) * 4;
    base.temp += (Math.random() - 0.5) * 0.8;
    base.soil = Math.min(Math.max(base.soil, 5), 95);
    base.temp = Math.min(Math.max(base.temp, 15), 40);

    const row = {
      soil_moisture: base.soil,
      temperature:   base.temp,
      relay_status:  base.soil < SOIL_THRESHOLD,
      created_at:    new Date().toISOString()
    };
    handleNewReading(row);
  }

  // Seed 20 historical points immediately
  const now = Date.now();
  for (let i = 20; i >= 1; i--) {
    base.soil += (Math.random() - 0.5) * 6;
    base.temp += (Math.random() - 0.5) * 2;
    base.soil = Math.min(Math.max(base.soil, 5), 95);
    base.temp = Math.min(Math.max(base.temp, 15), 40);
    pushToChart({
      soil_moisture: base.soil,
      temperature:   base.temp,
      relay_status:  base.soil < SOIL_THRESHOLD,
      created_at:    new Date(now - i * 10000).toISOString()
    });
  }
  updateCards({ soil_moisture: base.soil, temperature: base.temp, relay_status: base.soil < SOIL_THRESHOLD });
  setLastUpdated();

  demoInterval = setInterval(generateReading, 5000);
}

// ════════════════ UI HELPERS ════════════════
function setConnectionStatus(state) {
  const dot   = document.getElementById('conn-dot');
  const label = document.getElementById('conn-label');
  dot.className = 'status-dot ' + state;
  label.textContent =
    state === 'connected'    ? 'Live — Supabase' :
    state === 'connecting'   ? 'Connecting…'     :
    state === 'demo'         ? 'Demo Mode'        :
    'Disconnected';
}

function setLastUpdated() {
  document.getElementById('last-updated-time').textContent =
    new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

function showModal() {
  document.getElementById('config-modal').classList.remove('hidden');
}
function hideModal() {
  document.getElementById('config-modal').classList.add('hidden');
}

// ════════════════ BIND UI ════════════════
function bindUI() {
  document.getElementById('cfg-save').addEventListener('click', () => {
    const url   = document.getElementById('cfg-url').value.trim();
    const key   = document.getElementById('cfg-key').value.trim();
    const table = document.getElementById('cfg-table').value.trim() || 'garden_data';

    if (!url || !key) { alert('Please fill in URL and API key.'); return; }

    localStorage.setItem('gn_url',   url);
    localStorage.setItem('gn_key',   key);
    localStorage.setItem('gn_table', table);
    SUPABASE_URL   = url;
    SUPABASE_KEY   = key;
    SUPABASE_TABLE = table;

    hideModal();
    connectSupabase(url, key);
  });

  document.getElementById('cfg-demo').addEventListener('click', () => {
    hideModal();
    startDemoMode();
  });

  document.getElementById('refresh-btn').addEventListener('click', () => {
    const btn = document.getElementById('refresh-btn');
    btn.classList.add('spinning');
    setTimeout(() => btn.classList.remove('spinning'), 700);
    if (!isDemoMode && supabaseClient) fetchHistory();
  });

  // Mode Toggle
  document.getElementById('mode-auto').addEventListener('click', () => setPumpMode('AUTO'));
  document.getElementById('mode-manual').addEventListener('click', () => setPumpMode('MANUAL'));

  // Force Buttons
  document.getElementById('pump-force-on').addEventListener('click', () => forcePump(true));
  document.getElementById('pump-force-off').addEventListener('click', () => forcePump(false));

  // Threshold Stepper
  document.getElementById('thresh-minus').addEventListener('click', () => updateThreshold(-5));
  document.getElementById('thresh-plus').addEventListener('click', () => updateThreshold(5));

  // Nav highlighting (simple SPA stub)
  document.querySelectorAll('.nav-item').forEach(item => {
    item.addEventListener('click', (e) => {
      e.preventDefault();
      document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));
      item.classList.add('active');
      if (item.id === 'nav-settings') showModal();
    });
  });
}

// ════════════════ PUMP LOGIC ════════════════
function setPumpMode(mode) {
  pumpMode = mode;
  const isAuto = (mode === 'AUTO');

  document.getElementById('mode-auto').classList.toggle('active', isAuto);
  document.getElementById('mode-manual').classList.toggle('active', !isAuto);
  
  const manualControls = document.getElementById('manual-controls');
  isAuto ? manualControls.classList.remove('visible') : manualControls.classList.add('visible');

  // If returning to AUTO, sync pump logic with current gauge reading
  if (isAuto) {
    const gaugeVal = parseInt(document.getElementById('gauge-value').textContent || 0, 10);
    const active = (gaugeVal < soilThreshold);
    updatePump(active);
    pushPumpStateConfig(mode, null, soilThreshold);
  }
}

function forcePump(on) {
  if (pumpMode !== 'MANUAL') return;
  pumpForcedOn = on;

  document.getElementById('pump-force-on').classList.toggle('active-btn', on);
  document.getElementById('pump-force-off').classList.toggle('active-btn', !on);

  updatePump(on);
  pushPumpStateConfig('MANUAL', on, soilThreshold);
}

function updateThreshold(delta) {
  let newT = soilThreshold + delta;
  if (newT < 5) newT = 5;
  if (newT > 95) newT = 95;
  soilThreshold = newT;

  document.getElementById('thresh-val').textContent = soilThreshold + '%';

  // If in AUTO, instantly evaluate new threshold rule
  if (pumpMode === 'AUTO') {
    const gaugeVal = parseInt(document.getElementById('gauge-value').textContent || 0, 10);
    const active = (gaugeVal < soilThreshold);
    updatePump(active);
  }
  pushPumpStateConfig(pumpMode, pumpMode === 'MANUAL' ? pumpForcedOn : null, soilThreshold);
}

async function pushPumpStateConfig(mode, forcedState, threshold) {
  if (isDemoMode) {
    console.log('[Demo] Config Updated:', { mode, forcedState, threshold });
    return;
  }
  
  if (!supabaseClient) return;

  // Supabase update to a separate control table or row so the ESP32 can poll it.
  // We assume a table named 'garden_config' exists with a single row id=1
  console.log('[Supabase] Pushing new config...');
  const { error } = await supabaseClient
    .from('garden_config')
    .update({ 
      pump_mode: mode, 
      manual_override: forcedState, 
      soil_threshold: threshold 
    })
    .eq('id', 1);

  if (error) console.error('Failed to update garden_config:', error);
}

// ════════════════ FORMAT HELPERS ════════════════
function formatTime(iso) {
  const d = new Date(iso);
  return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
}

function formatDateTime(iso) {
  const d = new Date(iso);
  return d.toLocaleString([], {
    month: 'short', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit'
  });
}
