(function () {
  'use strict';

  const REFRESH_MS = 5000;
  const DAY_NAMES = ['M', 'T', 'W', 'T', 'F', 'S', 'S'];
  let loading = false;

  function byId(id) { return document.getElementById(id); }

  function node(tag, className, text) {
    const item = document.createElement(tag);
    if (className) item.className = className;
    if (text !== undefined && text !== null) item.textContent = String(text);
    return item;
  }

  function replace(id, content) {
    const target = byId(id);
    target.replaceChildren(content);
  }

  function setText(id, value) {
    byId(id).textContent = value === null || value === undefined || value === '' ? '--' : String(value);
  }

  function badge(text, tone) {
    return node('span', 'badge ' + (tone || 'secondary'), text);
  }

  function isActive(item) {
    if (typeof item.active === 'boolean') return item.active;
    return String(item.state || '').toLowerCase() === 'on';
  }

  function formatNumber(value) {
    return typeof value === 'number' ? value.toLocaleString() : '--';
  }

  function ageSeconds(data) {
    if (!data.last_update_ms || typeof data.current_ms !== 'number') return null;
    return Math.max(0, Math.floor((data.current_ms - data.last_update_ms) / 1000));
  }

  function formatAge(seconds) {
    if (seconds === null) return 'Never';
    if (seconds < 60) return seconds + 's ago';
    if (seconds < 3600) return Math.floor(seconds / 60) + 'm ' + (seconds % 60) + 's ago';
    return Math.floor(seconds / 3600) + 'h ' + Math.floor((seconds % 3600) / 60) + 'm ago';
  }

  function renderSummary(data) {
    setText('summary-mode', data.mode || 'Unknown');
    setText('summary-service', data.service_mode === true ? 'Service mode active' : 'Normal operation');

    const age = ageSeconds(data);
    setText('summary-update', age !== null && age < 30 ? 'Live' : (age === null ? 'Waiting' : 'Stale'));
    setText('summary-clock', 'Last bus update ' + formatAge(age));

    const messages = data.message_counts || {};
    const resyncs = data.resyncs || {};
    setText('summary-bus', (resyncs.total || 0) === 0 ? 'Healthy' : formatNumber(resyncs.total) + ' resyncs');
    setText('summary-messages', formatNumber(messages.decoded || 0) + ' decoded / ' + formatNumber(messages.unknown || 0) + ' unknown');

    const pump = data.pump || {};
    setText('summary-pump', pump.speed_rpm === null || pump.speed_rpm === undefined ? 'No reading' : formatNumber(pump.speed_rpm) + ' RPM');
    setText('summary-power', pump.power_watts === null || pump.power_watts === undefined ? 'Power unavailable' : formatNumber(pump.power_watts) + ' W');
  }

  function equipmentItems(data) {
    const items = [];
    (data.channels || []).forEach(function (item) {
      items.push({name: item.name || item.type || ('Output ' + item.id), kind: item.type || 'Output', state: item.state, active: isActive(item)});
    });
    (data.lighting || []).forEach(function (item) {
      const detail = item.active && item.color ? item.state + ' / ' + item.color : item.state;
      items.push({name: item.name || ('Lighting zone ' + item.zone), kind: 'Lighting', state: detail, active: isActive(item)});
    });
    (data.valves || []).forEach(function (item) {
      items.push({name: item.name || ('Valve ' + item.id), kind: 'Valve', state: item.state, active: isActive(item)});
    });
    (data.heaters || []).forEach(function (item) {
      const gas = item.gas_status || {};
      const running = String(item.state || '').toLowerCase() === 'on' || String(gas.burner || '').toLowerCase() === 'on';
      items.push({name: 'Heater ' + (item.index + 1), kind: 'Heater', state: gas.status || item.state || 'Unknown', active: running});
    });
    return items;
  }

  function renderEquipment(data) {
    const items = equipmentItems(data);
    const grid = document.createDocumentFragment();
    let activeCount = 0;

    items.forEach(function (item) {
      if (item.active) activeCount++;
      const card = node('article', 'card equipment-card');
      const title = node('div', 'equipment-title');
      title.appendChild(node('span', 'status-dot' + (item.active ? ' active' : '')));
      title.appendChild(node('strong', '', item.name));
      title.appendChild(badge(item.active ? 'Active' : 'Inactive', item.active ? 'success' : 'secondary'));
      card.appendChild(title);
      card.appendChild(node('div', 'equipment-meta', item.kind + (item.state ? ' / ' + item.state : '')));
      grid.appendChild(card);
    });

    if (!items.length) grid.appendChild(node('div', 'empty-state', 'No configured equipment has been reported yet.'));
    setText('equipment-summary', items.length ? activeCount + ' active / ' + (items.length - activeCount) + ' inactive' : 'No equipment reported');
    replace('equipment-grid', grid);
  }

  function renderTemperatures(data) {
    const values = [];
    const suffix = '\u00b0' + ((data.temperature || {}).scale === 'Fahrenheit' ? 'F' : 'C');
    (data.devices || []).forEach(function (device) {
      if (device.temperature1 === undefined || device.temperature1 === null) return;
      const hasSecond = Object.prototype.hasOwnProperty.call(device, 'temperature2');
      values.push({name: device.name + (hasSecond ? ' sensor 1' : ''), value: device.temperature1});
      if (device.temperature2 !== undefined && device.temperature2 !== null) values.push({name: device.name + ' sensor 2', value: device.temperature2});
    });

    const section = byId('temperature-section');
    section.hidden = values.length === 0;
    const grid = document.createDocumentFragment();
    values.forEach(function (reading) {
      const card = node('article', 'card temperature-card');
      card.appendChild(node('span', 'text-lighter', reading.name));
      card.appendChild(node('span', 'temperature-value', reading.value + suffix));
      grid.appendChild(card);
    });
    replace('temperature-grid', grid);
  }

  function timerMask(timer) {
    if (typeof timer.days_mask === 'number') return timer.days_mask;
    return String(timer.days || '').split('').reduce(function (mask, day, index) {
      return day === '-' ? mask : mask | (1 << index);
    }, 0);
  }

  function minutes(value) {
    const parts = String(value || '00:00').split(':');
    return (Number(parts[0]) || 0) * 60 + (Number(parts[1]) || 0);
  }

  function timerRunning(timer, now) {
    const mask = timerMask(timer);
    const today = (now.getDay() + 6) % 7;
    const previous = (today + 6) % 7;
    const current = now.getHours() * 60 + now.getMinutes();
    const start = minutes(timer.start);
    const stop = minutes(timer.stop);
    if (!mask) return false;
    if (start === stop) return Boolean(mask & (1 << today));
    if (start < stop) return Boolean(mask & (1 << today)) && current >= start && current < stop;
    return (Boolean(mask & (1 << today)) && current >= start) ||
      (Boolean(mask & (1 << previous)) && current < stop);
  }

  function renderTimers(data) {
    const timers = data.timers || [];
    const grid = document.createDocumentFragment();
    const now = new Date();

    timers.forEach(function (timer) {
      const running = timerRunning(timer, now);
      const mask = timerMask(timer);
      const card = node('article', 'card timer-card' + (running ? ' running' : ''));
      const top = node('div', 'timer-top');
      const heading = node('div');
      heading.appendChild(node('div', 'timer-caption', 'Timer ' + timer.num));
      const time = node('div', 'timer-time');
      time.appendChild(node('span', '', timer.start));
      time.appendChild(node('span', 'timer-arrow', '\u2192'));
      time.appendChild(node('span', '', timer.stop));
      heading.appendChild(time);
      top.appendChild(heading);
      top.appendChild(badge(running ? 'Running now' : 'Not running', running ? 'success' : 'secondary'));
      card.appendChild(top);

      const days = node('div', 'day-row');
      DAY_NAMES.forEach(function (day, index) {
        const enabled = Boolean(mask & (1 << index));
        const chip = node('span', 'day' + (enabled ? ' on' : ''), day);
        chip.title = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday'][index];
        days.appendChild(chip);
      });
      card.appendChild(days);
      grid.appendChild(card);
    });

    if (!timers.length) grid.appendChild(node('div', 'empty-state', 'No configured timers have been reported yet.'));
    replace('timer-grid', grid);
  }

  function table(rows) {
    const tableNode = node('table', 'detail-table');
    const body = node('tbody');
    rows.forEach(function (row) {
      if (row[1] === null || row[1] === undefined || row[1] === '') return;
      const tr = node('tr');
      tr.appendChild(node('th', '', row[0]));
      tr.appendChild(node('td', '', row[1]));
      body.appendChild(tr);
    });
    tableNode.appendChild(body);
    return tableNode;
  }

  function renderDetails(data) {
    const firmware = data.firmware || {};
    const gateway = data.internet_gateway || {};
    const memory = data.memory || {};
    const resyncs = data.resyncs || {};
    replace('controller-details', table([
      ['Firmware', firmware.version], ['Project', firmware.project], ['Built', firmware.compile_time],
      ['ESP-IDF', firmware.idf_version], ['Controller time', data.current_time],
      ['Free heap', memory.free_heap], ['Minimum free heap', memory.min_free_heap],
      ['Gateway IP', gateway.ip], ['Gateway signal', gateway.signal_level === null || gateway.signal_level === undefined ? null : gateway.signal_level + ' dBm'],
      ['Gateway comms', gateway.comms_status_text], ['Total resyncs', resyncs.total]
    ]));

    const devices = data.devices || [];
    const deviceWrap = node('div');
    if (!devices.length) {
      deviceWrap.appendChild(node('p', 'text-lighter', 'No bus devices reported.'));
    } else {
      deviceWrap.appendChild(table(devices.map(function (device) {
        const counts = device.message_counts || {};
        return [device.name + ' (' + device.address + ')',
          (device.firmware_version ? 'firmware ' + device.firmware_version + ', ' : '') +
          formatNumber(counts.decoded || 0) + ' decoded / ' + formatNumber(counts.unknown || 0) + ' unknown'];
      })));
    }
    replace('device-details', deviceWrap);

    const chemistry = data.chlorinator || {};
    const waterWrap = node('div');
    const waterRows = [
      ['pH', chemistry.ph_reading === null || chemistry.ph_reading === undefined ? null : chemistry.ph_reading + ' (setpoint ' + chemistry.ph_setpoint + ')'],
      ['ORP', chemistry.orp_reading === null || chemistry.orp_reading === undefined ? null : chemistry.orp_reading + ' mV (setpoint ' + chemistry.orp_setpoint + ' mV)'],
      ['Chlorine output', chemistry.chlor_output_level]
    ];
    (data.heaters || []).forEach(function (heater) {
      const gas = heater.gas_status || {};
      waterRows.push(['Heater ' + (heater.index + 1), gas.status || heater.state || 'Unknown']);
      if (heater.pool_setpoint !== undefined) waterRows.push(['Pool setpoint', heater.pool_setpoint]);
      if (heater.spa_setpoint !== undefined) waterRows.push(['Spa setpoint', heater.spa_setpoint]);
    });
    waterWrap.appendChild(table(waterRows));
    replace('water-details', waterWrap);
  }

  function render(data) {
    renderSummary(data);
    renderEquipment(data);
    renderTemperatures(data);
    renderTimers(data);
    renderDetails(data);
  }

  async function loadStatus() {
    if (loading) return;
    loading = true;
    const state = byId('refresh-state');
    const button = byId('refresh-status');
    const error = byId('status-error');
    button.disabled = true;
    state.textContent = 'Refreshing...';
    try {
      const response = await fetch('/status', {cache: 'no-store'});
      if (!response.ok) throw new Error('HTTP ' + response.status);
      const data = await response.json();
      render(data);
      error.hidden = true;
      state.textContent = 'Updated ' + new Date().toLocaleTimeString();
    } catch (problem) {
      error.textContent = 'Unable to load controller status: ' + problem.message;
      error.hidden = false;
      state.textContent = 'Refresh failed';
    } finally {
      button.disabled = false;
      loading = false;
    }
  }

  byId('refresh-status').addEventListener('click', loadStatus);
  loadStatus();
  window.setInterval(loadStatus, REFRESH_MS);
}());
