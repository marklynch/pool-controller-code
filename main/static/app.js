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

// File name for a download: <prefix>-YYYYMMDD-HHMMSS.<ext>, in local time
// at `d` (default now).
function stampedName(prefix, ext, d) {
  d = d || new Date();
  const p2 = n => String(n).padStart(2, '0');
  return prefix + '-' + d.getFullYear() + p2(d.getMonth() + 1) + p2(d.getDate()) + '-' +
    p2(d.getHours()) + p2(d.getMinutes()) + p2(d.getSeconds()) + '.' + ext;
}

// Make the link `id` save its same-origin JSON target as a timestamped file
// rather than opening it. The name is set at click time, so it's current.
function bindJsonDownload(id, prefix) {
  const a = document.getElementById(id);
  a.addEventListener('click', function () { a.download = stampedName(prefix, 'json'); });
}
