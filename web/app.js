(() => {
  'use strict';

  const TOKEN_KEY = 'mcastpilot.token';
  const $ = (id) => document.getElementById(id);

  const state = {
    token: localStorage.getItem(TOKEN_KEY) || '',
    authEnabled: false,
    tasks: new Map(),
    rowNodes: new Map(),
    tasksLoaded: false,
    health: null,
    version: '0.2.0-m2',
    nics: [],
    roots: [],
    ws: null,
    wsOpen: false,
    wsRetry: 1000,
    wsTimer: 0,
    editingId: null,
    logsTaskId: null,
    mediaPath: '',
    mediaParent: '',
    monitorTimer: 0,
    monitorRunning: false,
    tokenPromptOpen: false,
  };

  const STATE_LABELS = {
    idle: '已停止',
    starting: '启动中',
    running: '运行中',
    stopping: '停止中',
    error: '错误',
  };

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined && text !== null) node.textContent = String(text);
    return node;
  }

  function showModal(id) {
    const modal = $(id);
    if (modal) modal.classList.remove('hidden');
  }

  function hideModal(id) {
    const modal = $(id);
    if (modal) modal.classList.add('hidden');
  }

  function isModalOpen(id) {
    const modal = $(id);
    return modal ? !modal.classList.contains('hidden') : false;
  }

  let toastTimer = 0;
  function toast(message, kind = '') {
    const node = $('toast');
    node.textContent = message;
    node.className = 'toast' + (kind ? ' ' + kind : '');
    node.classList.remove('hidden');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => node.classList.add('hidden'), 3600);
  }

  function fmtBytes(bytes) {
    const value = Number(bytes) || 0;
    if (value < 1024) return value + ' B';
    if (value < 1024 * 1024) return (value / 1024).toFixed(1) + ' KB';
    if (value < 1024 * 1024 * 1024) return (value / 1024 / 1024).toFixed(2) + ' MB';
    return (value / 1024 / 1024 / 1024).toFixed(2) + ' GB';
  }

  function fmtDuration(seconds) {
    const value = Math.max(0, Number(seconds) || 0);
    if (value < 60) return value.toFixed(0) + 's';
    const m = Math.floor(value / 60);
    const s = Math.floor(value % 60);
    if (m < 60) return `${m}m ${s}s`;
    const h = Math.floor(m / 60);
    return `${h}h ${m % 60}m`;
  }

  function baseName(path) {
    if (!path) return '';
    const parts = String(path).split(/[\\/]/);
    return parts[parts.length - 1] || path;
  }

  function isUrl(value) {
    return /^[a-z][a-z0-9+.-]*:\/\//i.test(String(value || ''));
  }

  function num(id, fallback = 0) {
    const value = Number($(id).value);
    return Number.isFinite(value) ? value : fallback;
  }

  async function api(path, options = {}) {
    const opts = { ...options, headers: { ...(options.headers || {}) } };
    if (state.authEnabled && state.token) {
      opts.headers.Authorization = 'Bearer ' + state.token;
    }
    if (opts.body !== undefined && typeof opts.body !== 'string') {
      opts.headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(opts.body);
    }
    const response = await fetch(path, opts);
    const text = await response.text();
    let data = {};
    if (text) {
      try {
        data = JSON.parse(text);
      } catch {
        data = { error: text };
      }
    }
    if (response.status === 401) {
      if (!state.tokenPromptOpen) showTokenDialog('Token 无效或已过期，请重新输入。');
      throw new Error('unauthorized');
    }
    if (!response.ok) {
      throw new Error(data.error || `HTTP ${response.status}`);
    }
    return data;
  }

  function showTokenDialog(message) {
    state.tokenPromptOpen = true;
    $('btnCloseToken').classList.toggle('hidden', !state.token);
    $('tokenInput').value = state.token || '';
    const error = $('tokenError');
    if (message) {
      error.textContent = message;
      error.classList.remove('hidden');
    } else {
      error.classList.add('hidden');
    }
    showModal('tokenModal');
    setTimeout(() => $('tokenInput').focus(), 30);
  }

  function setConnection(open, text) {
    const dot = $('connDot');
    dot.classList.toggle('online', open);
    dot.classList.toggle('offline', !open);
    $('connText').textContent = text;
  }

  function updateHeader() {
    const total = state.tasksLoaded
      ? state.tasks.size
      : state.health
        ? state.health.tasks
        : 0;
    const running = state.tasksLoaded
      ? [...state.tasks.values()].filter((task) => task.state === 'running').length
      : state.health
        ? state.health.running
        : 0;
    $('healthText').textContent =
      `v${state.version} · ${total} 个任务 · ${running} 运行中`;
  }

  function updateHealth(health) {
    if (!health) return;
    state.health = health;
    if (health.version) state.version = health.version;
    state.authEnabled = health.auth === true;
    $('btnToken').classList.toggle('hidden', !state.authEnabled);
    if (!state.authEnabled && state.tokenPromptOpen) {
      hideModal('tokenModal');
      state.tokenPromptOpen = false;
    }
    updateHeader();
  }

  async function refreshHealth() {
    try {
      const health = await api('/api/v1/health');
      updateHealth(health);
    } catch {
      /* health is optional */
    }
  }

  function applyTasks(list) {
    const next = new Map();
    for (const task of list || []) {
      if (task && task.id) next.set(task.id, task);
    }
    state.tasks = next;
    state.tasksLoaded = true;
    renderTasks();
  }

  function applyTask(task) {
    if (!task || !task.id) return;
    state.tasks.set(task.id, task);
    renderTasks();
    if (state.logsTaskId === task.id) {
      const title = $('logsTitle');
      if (title) title.textContent = `任务日志 · ${task.name || task.id}`;
    }
  }

  async function loadTasks() {
    const data = await api('/api/v1/tasks');
    applyTasks(data.tasks || []);
  }

  async function loadNics() {
    const data = await api('/api/v1/nics');
    state.nics = data.nics || [];
    populateNicSelect($('fNic').value);
  }

  async function loadRoots() {
    const data = await api('/api/v1/media/roots');
    state.roots = data.roots || [];
  }

  async function refreshAll() {
    await Promise.allSettled([refreshHealth(), loadNics(), loadTasks(), loadRoots()]);
  }

  function populateNicSelect(selected) {
    const select = $('fNic');
    select.replaceChildren(new Option('系统默认路由', ''));
    for (const nic of state.nics) {
      const ip = nic.ipv4 && nic.ipv4.length ? nic.ipv4[0].addr : '';
      const value = ip || nic.id || nic.name;
      let label = nic.name || value;
      if (ip) label += ' — ' + ip;
      if (!nic.up) label += '（未连接）';
      if (nic.virtual) label += '（虚拟）';
      select.add(new Option(label, value));
    }
    if (selected) {
      let found = false;
      for (const option of select.options) {
        if (option.value === selected) {
          found = true;
          break;
        }
      }
      if (!found) select.add(new Option(selected, selected));
      select.value = selected;
    } else {
      select.value = '';
    }
  }

  function nicLabel(value) {
    if (!value) return '默认路由';
    const nic = state.nics.find((item) => {
      const ip = item.ipv4 && item.ipv4.length ? item.ipv4[0].addr : '';
      return value === ip || value === item.id || value === item.name;
    });
    if (!nic) return value;
    const ip = nic.ipv4 && nic.ipv4.length ? nic.ipv4[0].addr : '';
    return ip ? `${nic.name} (${ip})` : nic.name;
  }

  function renderTasks() {
    const tbody = $('taskRows');
    const tasks = [...state.tasks.values()].sort((a, b) =>
      String(a.name || '').localeCompare(String(b.name || ''), 'zh-CN')
    );
    $('emptyState').classList.toggle('hidden', tasks.length > 0);

    const seen = new Set();
    let previous = null;
    for (const task of tasks) {
      seen.add(task.id);
      let row = state.rowNodes.get(task.id);
      if (!row) {
        row = createTaskRow(task);
        state.rowNodes.set(task.id, row);
      } else {
        updateTaskRow(row, task);
      }
      const expected = previous ? previous.nextSibling : tbody.firstChild;
      if (row !== expected) tbody.insertBefore(row, expected || null);
      previous = row;
    }

    for (const [id, row] of [...state.rowNodes.entries()]) {
      if (seen.has(id)) continue;
      row.remove();
      state.rowNodes.delete(id);
    }
    updateSummary(tasks);
  }

  function updateSummary(tasks = [...state.tasks.values()]) {
    const running = tasks.filter((task) => task.state === 'running').length;
    $('taskSummary').textContent = tasks.length
      ? `${tasks.length} 个任务，${running} 个运行中`
      : '暂无任务';
    updateHeader();
  }

  function createTaskRow(task) {
    const row = el('tr');
    row.dataset.id = task.id;

    const parts = {};
    const nameCell = el('td');
    parts.name = el('div', 'task-name');
    parts.id = el('div', 'task-id');
    parts.error = el('div', 'error-text hidden');
    nameCell.append(parts.name, parts.id, parts.error);

    const stateCell = el('td');
    parts.badge = el('span', 'badge');
    parts.retry = el('div', 'cell-sub hidden');
    stateCell.append(parts.badge, parts.retry);

    const sourceCell = el('td');
    parts.source = el('div');
    parts.sourceMeta = el('div', 'cell-sub');
    sourceCell.append(parts.source, parts.sourceMeta);

    const destCell = el('td');
    parts.dest = el('div', 'mono');
    parts.nic = el('div', 'cell-sub');
    destCell.append(parts.dest, parts.nic);

    const bitrateCell = el('td');
    parts.bitrate = el('div');
    parts.mux = el('div', 'cell-sub');
    bitrateCell.append(parts.bitrate, parts.mux);

    const bufferCell = el('td');
    parts.buffer = el('div');
    parts.elapsed = el('div', 'cell-sub');
    bufferCell.append(parts.buffer, parts.elapsed);

    const sendCell = el('td');
    parts.send = el('div');
    parts.sendMeta = el('div', 'cell-sub');
    sendCell.append(parts.send, parts.sendMeta);

    const actionCell = el('td');
    const actions = el('div', 'row-actions');
    parts.start = actionButton('启动', 'start', task.id, false);
    parts.vlc = actionButton('VLC', 'vlc', task.id, true);
    parts.vlc.title = '仅在服务器本机（A 电脑）可用';
    parts.logs = actionButton('日志', 'logs', task.id, false);
    parts.edit = actionButton('编辑', 'edit', task.id, false);
    parts.remove = actionButton('删除', 'delete', task.id, false, 'danger');
    actions.append(parts.start, parts.vlc, parts.logs, parts.edit, parts.remove);
    actionCell.appendChild(actions);

    row.append(nameCell, stateCell, sourceCell, destCell, bitrateCell, bufferCell, sendCell, actionCell);
    row._parts = parts;
    updateTaskRow(row, task);
    return row;
  }

  function updateTaskRow(row, task) {
    const parts = row._parts;
    if (!parts) return;
    row.dataset.id = task.id;

    parts.name.textContent = task.name || task.id;
    parts.id.textContent = task.id;
    const lastError = task.last_error || (task.stats && task.stats.last_error) || '';
    parts.error.textContent = lastError;
    parts.error.title = lastError;
    parts.error.classList.toggle('hidden', !lastError);

    const state = task.state || 'idle';
    parts.badge.className = 'badge state-' + state;
    parts.badge.textContent = STATE_LABELS[state] || state;
    const showRetry = task.retries > 0 && (state === 'starting' || state === 'error');
    parts.retry.classList.toggle('hidden', !showRetry);
    if (showRetry) {
      const max = task.restart ? task.restart.max_retries : '';
      parts.retry.textContent = `重试 ${task.retries}/${max}`;
    }

    const source = task.source || {};
    const sourceName = isUrl(source.uri) ? source.uri : baseName(source.uri);
    parts.source.textContent = sourceName || '—';
    parts.source.title = source.uri || '';
    const sourceMeta = [];
    if (source.loop) sourceMeta.push('循环');
    if (task.realtime === false) sourceMeta.push('不限速');
    if (Number(task.speed) && Number(task.speed) !== 1) sourceMeta.push(task.speed + 'x');
    const encode = task.encode || {};
    if (encode.mode === 'transcode') sourceMeta.push('转码 H.264');
    parts.sourceMeta.textContent = sourceMeta.join(' · ');
    parts.sourceMeta.classList.toggle('hidden', !sourceMeta.length);

    const output = task.output || {};
    const extras = Array.isArray(output.extra) ? output.extra : [];
    parts.dest.textContent =
      `${output.group || '—'}:${output.port || ''}` +
      (extras.length ? ` +${extras.length}` : '');
    parts.dest.title = [
      `${output.group || '—'}:${output.port || ''}`,
      ...extras.map((item) => `${item.group}:${item.port}`),
    ].join('\n');
    parts.nic.textContent = nicLabel(output.nic) + (task.record ? ' · 录制' : '');

    const stats = task.stats || {};
    const live = state === 'running';
    parts.bitrate.textContent = live
      ? `${(Number(stats.bitrate_mbps) || 0).toFixed(2)} Mbps`
      : '—';
    const mux = task.mux || {};
    const muxrate = Number(mux.muxrate) || 0;
    parts.mux.textContent = muxrate > 0
      ? `CBR ${(muxrate / 1e6).toFixed(2)} Mbps`
      : 'CBR 自动';

    const fill = Number(stats.buffer_fill) || 0;
    parts.buffer.textContent = live ? `${fill.toFixed(0)}%` : '—';
    parts.elapsed.textContent = live ? fmtDuration(stats.elapsed_sec) : '';

    parts.send.textContent =
      `${Number(stats.packets_sent) || 0} / ${Number(stats.packets_dropped) || 0}`;
    const lost = Number(stats.buffer_drop_bytes) || 0;
    const errors = Number(stats.send_errors) || 0;
    const recorded = Number(stats.recorded_bytes) || 0;
    parts.sendMeta.textContent =
      `丢包 ${fmtBytes(lost)} · 错误 ${errors}` +
      (recorded > 0 ? ` · 录制 ${fmtBytes(recorded)}` : '');

    const canStart = state === 'idle' || state === 'error';
    const canStop = state === 'running' || state === 'starting';
    const canEdit = canStart;
    parts.start.textContent = canStop ? '停止' : '启动';
    parts.start.dataset.action = canStop ? 'stop' : 'start';
    parts.start.dataset.id = task.id;
    parts.start.disabled = canStop ? false : !canStart;
    parts.vlc.dataset.id = task.id;
    parts.vlc.disabled = state !== 'running';
    parts.logs.dataset.id = task.id;
    parts.edit.dataset.id = task.id;
    parts.remove.dataset.id = task.id;
    parts.edit.disabled = !canEdit;
    parts.remove.disabled = !canEdit;
  }

  function actionButton(label, action, id, disabled, kind = '') {
    const button = el('button', `small-btn ${kind}`.trim(), label);
    button.type = 'button';
    button.dataset.action = action;
    button.dataset.id = id;
    button.disabled = !!disabled;
    return button;
  }

  async function handleTaskAction(action, id) {
    const task = state.tasks.get(id);
    if (!task) return;
    try {
      if (action === 'start' || action === 'stop') {
        const data = await api(`/api/v1/tasks/${encodeURIComponent(id)}/${action}`, {
          method: 'POST',
        });
        if (data.task) applyTask(data.task);
        toast(action === 'start' ? '任务已启动' : '任务已停止', 'ok');
      } else if (action === 'logs') {
        openLogs(id);
      } else if (action === 'vlc') {
        const data = await api(`/api/v1/tasks/${encodeURIComponent(id)}/open-vlc`, {
          method: 'POST',
        });
        toast('已在服务器桌面打开 VLC：' + (data.url || ''), 'ok');
      } else if (action === 'edit') {
        openTaskModal(task);
      } else if (action === 'delete') {
        if (!confirm(`删除任务“${task.name || task.id}”？`)) return;
        await api(`/api/v1/tasks/${encodeURIComponent(id)}`, { method: 'DELETE' });
        state.tasks.delete(id);
        renderTasks();
        toast('任务已删除', 'ok');
      }
    } catch (error) {
      if (action === 'vlc' &&
          String(error.message).includes('only available on the server machine')) {
        const output = task.output || {};
        const url = `udp://@${output.group || '<group>'}:${output.port || '<port>'}`;
        window.prompt(
          '“VLC”按钮只能在服务器本机（A 电脑）上使用。\n\n' +
          '请在当前电脑的 VLC 中手动打开下面地址：\n' +
          '（如果是单播任务，只有目标电脑能收到）',
          url
        );
        return;
      }
      toast(error.message, 'error');
    }
  }

  function openTaskModal(task) {
    state.editingId = task ? task.id : null;
    $('taskModalTitle').textContent = task ? '编辑任务' : '新建任务';
    $('taskFormError').classList.add('hidden');
    $('probeResult').textContent = '尚未探测';
    $('btnSaveTask').textContent = task ? '保存' : '创建';

    const editAllowed = !task || task.state === 'idle' || task.state === 'error';
    $('btnSaveTask').disabled = !editAllowed;
    if (!editAllowed) {
      showFormError('taskFormError', '任务运行中，请先停止再编辑。');
    }

    populateNicSelect(task && task.output ? task.output.nic : '');
    $('fName').value = task ? task.name || '' : '';
    $('fInput').value = task && task.source ? task.source.uri || '' : '';
    $('fLoop').checked = task && task.source ? !!task.source.loop : true;
    $('fRealtime').checked = task ? task.realtime !== false : true;
    $('fGroup').value = task && task.output ? task.output.group : '239.1.1.1';
    $('fPort').value = task && task.output ? task.output.port : 5000;
    $('fTtl').value = task && task.output ? task.output.ttl : 16;
    $('fDscp').value = task && task.output ? task.output.dscp : 0;
    $('fMuxrate').value = task && task.mux ? task.mux.muxrate : 0;
    $('fPktSize').value = task && task.output ? task.output.pkt_size : 1316;
    $('fBuffer').value = task ? task.buffer_kb || 4096 : 4096;
    $('fSpeed').value = task ? task.speed || 1 : 1;
    $('fLoopback').checked = task && task.output ? !!task.output.loopback : false;
    $('fExtras').value = task && task.output && Array.isArray(task.output.extra)
      ? task.output.extra.map((item) => `${item.group}:${item.port}`).join('\n')
      : '';
    $('fRecord').checked = !!(task && task.record);
    const encode = task && task.encode ? task.encode : {};
    $('fEncodeMode').value = encode.mode || 'copy';
    $('fVideoBitrate').value = encode.video_bitrate || 4000000;
    $('fAudioBitrate').value = encode.audio_bitrate || 128000;
    $('fPreset').value = encode.preset || 'veryfast';
    $('fGop').value = encode.gop || 0;
    syncEncodeFields();
    $('fAutoRestart').checked = task && task.restart ? task.restart.auto !== false : true;
    $('fMaxRetries').value = task && task.restart ? task.restart.max_retries : 10;
    $('fBackoff').value = task && task.restart ? task.restart.backoff_ms : 1000;

    showModal('taskModal');
  }

  function showFormError(id, message) {
    const node = $(id);
    if (!node) return;
    node.textContent = message;
    node.classList.remove('hidden');
  }

  function syncEncodeFields() {
    const transcode = $('fEncodeMode').value === 'transcode';
    document.querySelectorAll('.encode-only').forEach((node) => {
      node.classList.toggle('hidden', !transcode);
    });
  }

  function collectTaskForm() {
    const input = $('fInput').value.trim();
    if (!input) throw new Error('请填写输入路径或 URL');
    const group = $('fGroup').value.trim();
    if (!group) throw new Error('请填写目的地址');
    const port = num('fPort', 0);
    if (port < 1 || port > 65535) throw new Error('端口必须是 1-65535');
    const pktSize = num('fPktSize', 0);
    if (pktSize < 188 || pktSize % 188 !== 0) throw new Error('UDP 包大小必须是 188 的倍数');

    const extras = [];
    for (const line of $('fExtras').value.split(/\r?\n/)) {
      const text = line.trim();
      if (!text) continue;
      const colon = text.lastIndexOf(':');
      if (colon <= 0) throw new Error('额外目的地格式必须是 group:port：' + text);
      const group = text.slice(0, colon).trim();
      const port = Number(text.slice(colon + 1));
      if (!group || !(port >= 1 && port <= 65535)) {
        throw new Error('额外目的地格式必须是 group:port：' + text);
      }
      extras.push({ group, port });
    }
    if (extras.length > 4) throw new Error('最多 4 个额外目的地');

    return {
      name: $('fName').value.trim(),
      source: {
        uri: input,
        loop: $('fLoop').checked,
      },
      output: {
        group,
        port,
        nic: $('fNic').value,
        ttl: num('fTtl', 16),
        dscp: num('fDscp', 0),
        loopback: $('fLoopback').checked,
        pkt_size: pktSize,
        extra: extras,
      },
      mux: {
        format: 'mpegts',
        muxrate: num('fMuxrate', 0),
        cbr: true,
      },
      encode: {
        mode: $('fEncodeMode').value,
        video_bitrate: num('fVideoBitrate', 4000000),
        audio_bitrate: num('fAudioBitrate', 128000),
        preset: $('fPreset').value,
        gop: num('fGop', 0),
      },
      restart: {
        auto: $('fAutoRestart').checked,
        max_retries: num('fMaxRetries', 10),
        backoff_ms: num('fBackoff', 1000),
      },
      realtime: $('fRealtime').checked,
      speed: num('fSpeed', 1),
      buffer_kb: num('fBuffer', 4096),
      record: $('fRecord').checked,
    };
  }

  async function submitTask(event) {
    event.preventDefault();
    const errorNode = $('taskFormError');
    errorNode.classList.add('hidden');
    try {
      const body = collectTaskForm();
      const data = state.editingId
        ? await api(`/api/v1/tasks/${encodeURIComponent(state.editingId)}`, {
            method: 'PATCH',
            body,
          })
        : await api('/api/v1/tasks', { method: 'POST', body });
      if (data.task) applyTask(data.task);
      hideModal('taskModal');
      toast(state.editingId ? '任务已保存' : '任务已创建', 'ok');
      state.editingId = null;
    } catch (error) {
      showFormError('taskFormError', error.message);
    }
  }

  async function probeInput() {
    const input = $('fInput').value.trim();
    if (!input) {
      toast('请先填写输入路径或 URL', 'error');
      return;
    }
    const node = $('probeResult');
    node.textContent = '探测中...';
    try {
      const data = await api('/api/v1/probe', { method: 'POST', body: { input } });
      const probe = data.probe || {};
      const lines = [];
      lines.push(
        `${probe.format || '?'} · ${Number(probe.duration_sec || 0).toFixed(2)}s · ` +
          `${(Number(probe.bitrate || 0) / 1e6).toFixed(3)} Mbps`
      );
      for (const stream of probe.streams || []) {
        if (stream.type === 'video') {
          lines.push(
            `#${stream.index} video ${stream.codec} ${stream.width}x${stream.height} ` +
              `${Number(stream.fps || 0).toFixed(3)} fps ${stream.bitrate} bps`
          );
        } else if (stream.type === 'audio') {
          lines.push(
            `#${stream.index} audio ${stream.codec} ${stream.sample_rate} Hz ` +
              `${stream.channels} ch ${stream.bitrate} bps`
          );
        } else {
          lines.push(`#${stream.index} ${stream.type || 'other'} ${stream.codec}`);
        }
      }
      node.textContent = lines.join('\n');
    } catch (error) {
      node.textContent = '探测失败：' + error.message;
    }
  }

  async function openMedia(path) {
    showModal('mediaModal');
    await loadMedia(path || '');
  }

  async function loadMedia(path) {
    const list = $('mediaList');
    list.replaceChildren(el('div', 'muted', '加载中...'));
    try {
      const query = path ? '?path=' + encodeURIComponent(path) : '';
      const data = await api('/api/v1/media/list' + query);
      state.mediaPath = data.path || '';
      state.mediaParent = data.parent || '';
      $('mediaPath').textContent = state.mediaPath || '媒体库根目录';
      $('btnMediaUp').disabled = !state.mediaParent;
      list.replaceChildren();

      const entries = data.entries || [];
      if (!entries.length) {
        list.appendChild(el('div', 'muted', '这个目录里没有可用的媒体文件。'));
        return;
      }
      for (const entry of entries) {
        const button = el('button', 'media-item ' + (entry.type === 'dir' ? 'dir' : 'file'));
        button.type = 'button';
        button.appendChild(el('span', 'media-icon', entry.type === 'dir' ? '▸' : '≡'));
        const info = el('span', '');
        info.appendChild(el('div', '', entry.name));
        if (entry.root) info.appendChild(el('div', 'cell-sub', '媒体库根目录'));
        button.appendChild(info);
        button.appendChild(
          el('span', 'media-meta', entry.type === 'dir' ? '目录' : fmtBytes(entry.size))
        );
        button.addEventListener('click', () => {
          if (entry.type === 'dir') {
            loadMedia(entry.path);
          } else {
            $('fInput').value = entry.path;
            if (!$('fName').value.trim()) $('fName').value = entry.name;
            hideModal('mediaModal');
            toast('已选择 ' + entry.name, 'ok');
          }
        });
        list.appendChild(button);
      }
    } catch (error) {
      list.replaceChildren(el('div', 'muted', '加载失败：' + error.message));
    }
  }

  function openLogs(id) {
    const task = state.tasks.get(id);
    if (!task) return;
    state.logsTaskId = id;
    $('logsTitle').textContent = '任务日志 · ' + (task.name || id);
    $('logsBody').textContent = '加载中...';
    showModal('logsModal');
    refreshLogs();
  }

  async function refreshLogs() {
    if (!state.logsTaskId) return;
    try {
      const data = await api(
        `/api/v1/tasks/${encodeURIComponent(state.logsTaskId)}/logs?tail=500`
      );
      renderLogs(data.lines || []);
    } catch (error) {
      $('logsBody').textContent = '日志加载失败：' + error.message;
    }
  }

  function renderLogs(lines) {
    const body = $('logsBody');
    body.textContent = lines.length ? lines.join('\n') : '暂无日志';
    body.scrollTop = body.scrollHeight;
  }

  function appendLogLine(line) {
    const body = $('logsBody');
    let lines = body.textContent && body.textContent !== '暂无日志' ? body.textContent.split('\n') : [];
    lines.push(line);
    if (lines.length > 1000) lines = lines.slice(lines.length - 1000);
    body.textContent = lines.join('\n');
    body.scrollTop = body.scrollHeight;
  }

  async function openSettings() {
    const errorNode = $('settingsError');
    errorNode.classList.add('hidden');
    try {
      const data = await api('/api/v1/settings');
      const settings = data.settings || {};
      $('settingsListen').textContent = `${settings.listen || '?'}:${settings.port || '?'}`;
      $('sRoots').value = (settings.media_roots || []).join('\n');
      $('sWebEnabled').checked = settings.web_enabled !== false;
      $('sAuthEnabled').checked = settings.auth_enabled === true;
      $('sToken').value = '';
      $('sToken').disabled = settings.auth_enabled !== true;
      $('sToken').placeholder = settings.token_set
        ? `当前：${settings.token_hint || '已设置'}（留空保留）`
        : '留空则自动生成';
      $('sVlcPath').value = settings.vlc_path || '';
      $('sLogLevel').value = settings.log_level || 'info';
      showModal('settingsModal');
    } catch (error) {
      toast(error.message, 'error');
    }
  }

  function syncAuthFields() {
    const enabled = $('sAuthEnabled').checked;
    $('sToken').disabled = !enabled;
  }

  function populateMonitorIfaces() {
    const select = $('mIface');
    const current = select.value;
    select.replaceChildren(new Option('自动 / 任意', ''));
    const seen = new Set();
    for (const nic of state.nics) {
      const ip = nic.ipv4 && nic.ipv4.length ? nic.ipv4[0].addr : '';
      if (!ip || seen.has(ip)) continue;
      seen.add(ip);
      let label = `${nic.name} — ${ip}`;
      if (!nic.up) label += '（未连接）';
      if (nic.virtual) label += '（虚拟）';
      select.add(new Option(label, ip));
    }
    select.value = current || '';
    if (!select.value) {
      const preferred =
        state.nics.find((nic) => nic.up && !nic.loopback && nic.ipv4 && nic.ipv4.length) ||
        state.nics.find((nic) => nic.ipv4 && nic.ipv4.length);
      if (preferred) select.value = preferred.ipv4[0].addr;
    }
  }

  async function openMonitor() {
    populateMonitorIfaces();
    showModal('monitorModal');
    await refreshMonitor();
    if (state.monitorRunning) startMonitorPolling();
  }

  function startMonitorPolling() {
    stopMonitorPolling();
    const tick = async () => {
      state.monitorTimer = 0;
      await refreshMonitor();
      if (isModalOpen('monitorModal') && state.monitorRunning) {
        state.monitorTimer = setTimeout(tick, 1000);
      }
    };
    state.monitorTimer = setTimeout(tick, 0);
  }

  function stopMonitorPolling() {
    if (state.monitorTimer) clearTimeout(state.monitorTimer);
    state.monitorTimer = 0;
  }

  async function refreshMonitor() {
    try {
      const data = await api('/api/v1/monitor');
      updateMonitor(data.monitor || {});
    } catch (error) {
      $('monitorStatus').textContent = '读取失败：' + error.message;
    }
  }

  function updateMonitor(monitor) {
    state.monitorRunning = !!monitor.running;
    const hasData = monitor.running || Number(monitor.packets) > 0;
    $('mBitrate').textContent = hasData
      ? `${(Number(monitor.bitrate_mbps) || 0).toFixed(2)} Mbps`
      : '—';
    $('mPackets').textContent = String(Number(monitor.packets) || 0);
    $('mBytes').textContent = fmtBytes(monitor.bytes);
    $('mElapsed').textContent = fmtDuration(monitor.elapsed_sec);
    $('mSync').textContent = String(Number(monitor.sync_errors) || 0);
    $('mCc').textContent = String(Number(monitor.cc_errors) || 0);
    $('btnMonitorStart').disabled = state.monitorRunning;
    $('btnMonitorStop').disabled = !state.monitorRunning;

    if (state.monitorRunning) {
      $('monitorStatus').textContent = '监测中...';
    } else if (monitor.last_error) {
      $('monitorStatus').textContent = '已停止：' + monitor.last_error;
    } else if (hasData) {
      $('monitorStatus').textContent =
        `已停止 · 共接收 ${Number(monitor.packets) || 0} 包，` +
        `同步错误 ${Number(monitor.sync_errors) || 0}，CC 错误 ${Number(monitor.cc_errors) || 0}`;
    } else {
      $('monitorStatus').textContent = '未运行';
    }
  }

  async function startMonitor() {
    const group = $('mGroup').value.trim();
    const port = Number($('mPort').value);
    const iface = $('mIface').value;
    const duration = Number($('mDuration').value) || 0;
    if (!group) {
      toast('请填写组播或目的地址', 'error');
      return;
    }
    if (!(port >= 1 && port <= 65535)) {
      toast('端口必须是 1-65535', 'error');
      return;
    }
    try {
      const data = await api('/api/v1/monitor/start', {
        method: 'POST',
        body: { group, port, iface, duration_sec: duration },
      });
      updateMonitor(data.monitor || {});
      startMonitorPolling();
      toast('监测已启动', 'ok');
    } catch (error) {
      toast(error.message, 'error');
      refreshMonitor();
    }
  }

  async function stopMonitor() {
    try {
      const data = await api('/api/v1/monitor/stop', { method: 'POST' });
      updateMonitor(data.monitor || {});
      stopMonitorPolling();
      toast('监测已停止', 'ok');
    } catch (error) {
      toast(error.message, 'error');
    }
  }

  async function saveSettings() {
    const errorNode = $('settingsError');
    errorNode.classList.add('hidden');
    const roots = $('sRoots')
      .value.split(/\r?\n/)
      .map((line) => line.trim())
      .filter(Boolean);
    try {
      const data = await api('/api/v1/settings', {
        method: 'PATCH',
        body: {
          media_roots: roots,
          web_enabled: $('sWebEnabled').checked,
          auth_enabled: $('sAuthEnabled').checked,
          token: $('sToken').value.trim(),
          vlc_path: $('sVlcPath').value.trim(),
          log_level: $('sLogLevel').value,
        },
      });
      const settings = data.settings || {};
      state.roots = settings.media_roots || roots;
      state.authEnabled = settings.auth_enabled === true;
      if (state.authEnabled && data.token) {
        state.token = data.token;
        localStorage.setItem(TOKEN_KEY, data.token);
        state.tokenPromptOpen = false;
      } else if (!state.authEnabled) {
        state.token = '';
        localStorage.removeItem(TOKEN_KEY);
      }
      hideModal('settingsModal');
      toast('设置已保存', 'ok');
      refreshHealth();
      refreshAll();
      connectWs();
    } catch (error) {
      showFormError('settingsError', error.message);
    }
  }

  function connectWs() {
    if (state.authEnabled && !state.token) return;
    if (state.ws) {
      try {
        state.ws.close();
      } catch {
        /* ignore */
      }
      state.ws = null;
    }
    clearTimeout(state.wsTimer);

    const protocol = location.protocol === 'https:' ? 'wss' : 'ws';
    const tokenQuery = state.authEnabled
      ? '?token=' + encodeURIComponent(state.token)
      : '';
    const url = `${protocol}://${location.host}/api/v1/events${tokenQuery}`;
    const ws = new WebSocket(url);
    state.ws = ws;

    ws.onopen = () => {
      state.wsOpen = true;
      state.wsRetry = 1000;
      setConnection(true, '实时连接');
    };
    ws.onmessage = (event) => {
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        return;
      }
      handleEvent(message);
    };
    ws.onerror = () => {
      try {
        ws.close();
      } catch {
        /* ignore */
      }
    };
    ws.onclose = () => {
      state.wsOpen = false;
      setConnection(false, '轮询模式');
      if (state.ws === ws) state.ws = null;
      if ((state.authEnabled && !state.token) || state.tokenPromptOpen) return;
      const delay = state.wsRetry;
      state.wsRetry = Math.min(state.wsRetry * 2, 15000);
      state.wsTimer = setTimeout(() => {
        if (!state.wsOpen) connectWs();
      }, delay);
    };
  }

  function handleEvent(message) {
    if (!message || !message.type) return;
    if (message.type === 'snapshot') {
      if (message.health) updateHealth(message.health);
      applyTasks(message.tasks || []);
    } else if (message.type === 'task') {
      applyTask(message.task);
    } else if (message.type === 'deleted') {
      state.tasks.delete(message.task_id);
      renderTasks();
      if (state.logsTaskId === message.task_id) {
        state.logsTaskId = null;
        hideModal('logsModal');
      }
    } else if (message.type === 'log') {
      if (state.logsTaskId === message.task_id) appendLogLine(message.line || '');
    }
  }

  async function saveToken(event) {
    event.preventDefault();
    const token = $('tokenInput').value.trim();
    const errorNode = $('tokenError');
    errorNode.classList.add('hidden');
    if (!token) {
      errorNode.textContent = 'Token 不能为空';
      errorNode.classList.remove('hidden');
      return;
    }

    const previous = state.token;
    state.token = token;
    try {
      const data = await api('/api/v1/tasks');
      localStorage.setItem(TOKEN_KEY, token);
      state.tokenPromptOpen = false;
      hideModal('tokenModal');
      $('tokenInput').value = '';
      applyTasks(data.tasks || []);
      connectWs();
      refreshAll();
      toast('已连接', 'ok');
    } catch (error) {
      state.token = previous;
      errorNode.textContent = error.message === 'unauthorized' ? 'Token 不正确' : error.message;
      errorNode.classList.remove('hidden');
    }
  }

  function bindUi() {
    $('btnNew').addEventListener('click', () => openTaskModal(null));
    $('btnRefresh').addEventListener('click', () => {
      refreshAll();
      toast('已刷新');
    });
    $('btnSettings').addEventListener('click', openSettings);
    $('btnMonitor').addEventListener('click', openMonitor);
    $('btnMonitorStart').addEventListener('click', startMonitor);
    $('btnMonitorStop').addEventListener('click', stopMonitor);
    $('btnToken').addEventListener('click', () => showTokenDialog(''));
    $('btnSaveSettings').addEventListener('click', saveSettings);
    $('sAuthEnabled').addEventListener('change', syncAuthFields);
    $('fEncodeMode').addEventListener('change', syncEncodeFields);
    $('btnSaveTask').addEventListener('click', (event) => {
      if ($('btnSaveTask').disabled) event.preventDefault();
    });
    $('taskForm').addEventListener('submit', submitTask);
    $('tokenForm').addEventListener('submit', saveToken);
    $('btnBrowse').addEventListener('click', () => openMedia(''));
    $('btnProbe').addEventListener('click', probeInput);
    $('btnMediaUp').addEventListener('click', () => {
      if (state.mediaParent) loadMedia(state.mediaParent);
    });
    $('btnLogsRefresh').addEventListener('click', refreshLogs);
    $('btnLogsScroll').addEventListener('click', () => {
      const body = $('logsBody');
      body.scrollTop = body.scrollHeight;
    });

    $('taskRows').addEventListener('click', (event) => {
      const button = event.target.closest('button[data-action]');
      if (!button) return;
      handleTaskAction(button.dataset.action, button.dataset.id);
    });

    document.querySelectorAll('[data-close]').forEach((button) => {
      button.addEventListener('click', () => {
        const id = button.dataset.close;
        if (id === 'tokenModal' && !state.token) return;
        hideModal(id);
        if (id === 'tokenModal') state.tokenPromptOpen = false;
        if (id === 'logsModal') state.logsTaskId = null;
        if (id === 'taskModal') state.editingId = null;
        if (id === 'monitorModal') stopMonitorPolling();
      });
    });

    document.querySelectorAll('.modal').forEach((modal) => {
      modal.addEventListener('click', (event) => {
        if (event.target !== modal) return;
        if (modal.id === 'tokenModal' && !state.token) return;
        modal.classList.add('hidden');
        if (modal.id === 'tokenModal') state.tokenPromptOpen = false;
        if (modal.id === 'logsModal') state.logsTaskId = null;
        if (modal.id === 'taskModal') state.editingId = null;
        if (modal.id === 'monitorModal') stopMonitorPolling();
      });
    });

    document.addEventListener('keydown', (event) => {
      if (event.key !== 'Escape') return;
      if (!isModalOpen('tokenModal') || state.token) {
        document.querySelectorAll('.modal').forEach((modal) => modal.classList.add('hidden'));
        state.tokenPromptOpen = false;
        state.logsTaskId = null;
        state.editingId = null;
        stopMonitorPolling();
      }
    });
  }

  document.addEventListener('DOMContentLoaded', async () => {
    bindUi();
    await refreshHealth();
    setInterval(refreshHealth, 5000);
    setInterval(() => {
      if (!state.wsOpen && (!state.authEnabled || state.token)) {
        loadTasks().catch(() => {});
      }
    }, 4000);
    if (state.authEnabled && !state.token) {
      showTokenDialog();
      return;
    }
    refreshAll().then(() => connectWs());
  });
})();
