// Pool Controller web UI: helpers shared by several pages. Loaded without
// defer in <head>, so the inline page scripts can call these as they parse.

// Show `msg` in the alert box `el`; variant is 'success', 'warning', 'danger'
// or omitted for a neutral box.
function showAlert(el, msg, variant) {
  el.textContent = msg;
  if (variant) el.setAttribute('data-variant', variant);
  else el.removeAttribute('data-variant');
  el.hidden = false;
}

// Shared by the GitHub, manual-upload, reboot and entity-reset flows: count
// down while the device reboots, then reload the page.
function rebootReload(el, msg) {
  msg = msg || 'Update successful!';
  let countdown = 15;
  const show = () => showAlert(el, msg + ' Rebooting... reload in ' + countdown + ' seconds', 'success');
  show();
  const timer = setInterval(function () {
    countdown--;
    if (countdown > 0) {
      show();
    } else {
      clearInterval(timer);
      showAlert(el, msg + ' Rebooting... reloading now...', 'success');
      window.location.reload();
    }
  }, 1000);
}

// Shared by the settings switches: POST <url>?enabled=0|1 on change, then
// show the setting the device reports, or revert on failure. The status box
// is the element with id <id>Status.
function bindToggle(id, url) {
  const c = document.getElementById(id);
  const s = document.getElementById(id + 'Status');
  c.addEventListener('change', function () {
    const want = c.checked;
    c.disabled = true;
    s.hidden = true;
    fetch(url + '?enabled=' + (want ? 1 : 0), { method: 'POST' })
      .then(r => r.json())
      .then(function (d) {
        if (!d.success) throw new Error();
        c.checked = d.enabled;
      })
      .catch(function () {
        c.checked = !want;
        showAlert(s, 'Could not save the setting.', 'danger');
      })
      .finally(function () { c.disabled = false; });
  });
}

function pad2(n) { return String(n).padStart(2, '0'); }

// A date and time for display, in local time with the zone, e.g.
// "22/11/2026, 13:00:54 GMT+11". Every page shows times this way.
function fmtDateTime(d) {
  return d.toLocaleString('en-GB', {
    day: '2-digit', month: '2-digit', year: 'numeric',
    hour: '2-digit', minute: '2-digit', second: '2-digit',
    hour12: false, timeZoneName: 'short'
  });
}

// File name for a download: <prefix>-YYYYMMDD-HHMMSS.<ext>, in local time
// at `d` (default now).
function stampedName(prefix, ext, d) {
  d = d || new Date();
  return prefix + '-' + d.getFullYear() + pad2(d.getMonth() + 1) + pad2(d.getDate()) + '-' +
    pad2(d.getHours()) + pad2(d.getMinutes()) + pad2(d.getSeconds()) + '.' + ext;
}

// Make the link `id` save its same-origin JSON target as a timestamped file
// rather than opening it. The name is set at click time, so it's current.
function bindJsonDownload(id, prefix) {
  const a = document.getElementById(id);
  a.addEventListener('click', function () { a.download = stampedName(prefix, 'json'); });
}

// /status message_counts as text, e.g.
// "118,439 decoded, 4,136 unknown (96.6% decoded)".
function messageSummary(mc) {
  const tot = mc.decoded + mc.unknown;
  return mc.decoded.toLocaleString() + ' decoded, ' + mc.unknown.toLocaleString() + ' unknown' +
    (tot > 0 ? ' (' + (mc.decoded / tot * 100).toFixed(1) + '% decoded)' : '');
}

// /status resyncs as text: the total, then each non-zero reason, e.g.
// "44 (6 no-start, 38 header-checksum)".
function resyncSummary(rs) {
  const parts = Object.keys(rs).filter(k => k !== 'total' && rs[k])
    .map(k => rs[k] + ' ' + k.replace(/_/g, '-'));
  return (rs.total || 0) + (parts.length ? ' (' + parts.join(', ') + ')' : '');
}

// Opens and closes the sidebar on narrow screens. This was the only part of
// oat.min.js the pages used; Oat's CSS still lays out the sidebar from the
// data-sidebar-open attribute.
document.addEventListener('click', function (e) {
  const t = e.target.closest('[data-sidebar-toggle]');
  if (t) t.closest('[data-sidebar-layout]')?.toggleAttribute('data-sidebar-open');
});
