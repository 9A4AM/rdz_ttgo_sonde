// Lightweight modal dialogs replacing native alert()/confirm(), styled via style.css.
//   showAlert(message)    -> Promise            (resolves when dismissed)
//   showConfirm(message)  -> Promise<boolean>   (true = OK, false = Cancel)
//   showProgress(message) -> { update(msg), close() }  (buttonless, non-dismissable)
//   showPrompt(message, opts) -> Promise<boolean>  (opts.confirmWord gates OK)
// A single overlay element is built on first use and reused. Messages are inserted with
// textContent (XSS-safe; they may contain user-supplied names) and CSS white-space:pre-line
// keeps the "\n" line breaks the message text relies on.
(function (global) {
  var overlay, msgEl, okBtn, cancelBtn, current, spinnerEl, titleEl, inputEl;

  function build() {
    overlay = document.createElement('div');
    overlay.className = 'modal-overlay';
    overlay.style.display = 'none';

    var box = document.createElement('div');
    box.className = 'modal-box';
    // Spinner + title are only shown by showProgress(); hidden for alert/confirm.
    spinnerEl = document.createElement('div');
    spinnerEl.className = 'modal-spinner';
    spinnerEl.style.display = 'none';
    titleEl = document.createElement('div');
    titleEl.className = 'modal-title';
    titleEl.style.display = 'none';
    msgEl = document.createElement('div');
    msgEl.className = 'modal-msg';
    inputEl = document.createElement('input');
    inputEl.type = 'text';
    inputEl.className = 'modal-input';
    inputEl.style.display = 'none';   // only shown by showPrompt()

    var actions = document.createElement('div');
    actions.className = 'modal-actions';
    cancelBtn = document.createElement('button');
    cancelBtn.type = 'button';
    cancelBtn.className = 'modal-btn modal-btn-cancel';
    cancelBtn.textContent = 'Cancel';
    okBtn = document.createElement('button');
    okBtn.type = 'button';
    okBtn.className = 'modal-btn modal-btn-primary';
    okBtn.textContent = 'OK';
    actions.appendChild(cancelBtn);
    actions.appendChild(okBtn);

    box.appendChild(spinnerEl);
    box.appendChild(titleEl);
    box.appendChild(msgEl);
    box.appendChild(inputEl);
    box.appendChild(actions);
    overlay.appendChild(box);
    document.body.appendChild(overlay);

    okBtn.addEventListener('click', function () { close(true); });
    cancelBtn.addEventListener('click', function () { close(false); });
    document.addEventListener('keydown', function (e) {
      if (!current) return;
      if (e.key === 'Enter') {
        e.preventDefault();
        if (current.isPrompt && !current.matches()) return;
        close(true);
      }
      else if (e.key === 'Escape') { e.preventDefault(); close(false); }
    });
  }

  function close(result) {
    if (!current) return;
    overlay.style.display = 'none';
    var resolve = current.resolve, isConfirm = current.isConfirm;
    current = null;
    resolve(isConfirm ? !!result : undefined);
  }

  function open(message, isConfirm) {
    if (!overlay) build();
    msgEl.textContent = message == null ? '' : String(message);
    spinnerEl.style.display = 'none';   // alert/confirm: no spinner/title
    titleEl.style.display = 'none';
    okBtn.style.display = '';           // may have been hidden by a previous showProgress()
    inputEl.style.display = 'none';   // not a prompt
    okBtn.disabled = false;           // clear any disable left by a prior showPrompt
    cancelBtn.style.display = isConfirm ? '' : 'none';
    overlay.style.display = 'flex';
    return new Promise(function (resolve) {
      current = { resolve: resolve, isConfirm: isConfirm };
      okBtn.focus();
    });
  }

  global.showAlert = function (message) { return open(message, false); };
  global.showConfirm = function (message) { return open(message, true); };

  // Prompt with a text input. opts: { title, placeholder, confirmWord }. When
  // confirmWord is set, OK stays disabled until the trimmed, case-insensitive input
  // equals it (a deliberate type-to-confirm gate). Resolves true only on a matching
  // OK; false on Cancel/Escape. Mirrors showConfirm's boolean-promise shape.
  global.showPrompt = function (message, opts) {
    opts = opts || {};
    if (!overlay) build();
    spinnerEl.style.display = 'none';
    if (opts.title) { titleEl.textContent = String(opts.title); titleEl.style.display = ''; }
    else titleEl.style.display = 'none';
    msgEl.textContent = message == null ? '' : String(message);
    inputEl.style.display = '';
    inputEl.value = '';
    inputEl.placeholder = opts.placeholder || '';
    okBtn.style.display = '';
    cancelBtn.style.display = '';
    var confirmWord = opts.confirmWord != null ? String(opts.confirmWord).trim().toLowerCase() : null;
    function matches() {
      return confirmWord == null || inputEl.value.trim().toLowerCase() === confirmWord;
    }
    okBtn.disabled = !matches();
    inputEl.oninput = function () { okBtn.disabled = !matches(); };
    overlay.style.display = 'flex';
    return new Promise(function (resolve) {
      current = { resolve: resolve, isConfirm: true, isPrompt: true, matches: matches };
      setTimeout(function () { inputEl.focus(); }, 0);
    });
  };

  // Buttonless, non-dismissable modal with a spinner for long operations (e.g. a
  // firmware update). current stays null so the Enter/Escape handler ignores it.
  // Returns a handle to update the title/message or close it programmatically.
  global.showProgress = function (message, title) {
    if (!overlay) build();
    inputEl.style.display = 'none';
    okBtn.disabled = false;
    current = null;
    spinnerEl.style.display = '';
    if (title) { titleEl.textContent = String(title); titleEl.style.display = ''; }
    else titleEl.style.display = 'none';
    msgEl.textContent = message == null ? '' : String(message);
    okBtn.style.display = 'none';
    cancelBtn.style.display = 'none';
    overlay.style.display = 'flex';
    return {
      update: function (m) { msgEl.textContent = m == null ? '' : String(m); },
      setTitle: function (t) { titleEl.textContent = t == null ? '' : String(t); titleEl.style.display = t ? '' : 'none'; },
      close: function () { overlay.style.display = 'none'; spinnerEl.style.display = 'none'; titleEl.style.display = 'none'; okBtn.style.display = ''; }
    };
  };

  // Show a progress dialog and poll /bootid until it differs from `fromId` (i.e. the
  // device has finished flashing/saving and rebooted), then reload. A changed bootid
  // is the real "done" signal; a max-timeout fallback reloads anyway. Reused by every
  // action that reboots the device (firmware update, config/qrg restore, ...).
  global.waitForRebootAndReload = function (fromId, title, body) {
    body = body || 'The device is restarting.';
    fromId = (fromId || '').trim();
    // Static message (no elapsed counter): while the device is down the /bootid probe
    // can't advance a countdown anyway, so just say it reloads when the device is back.
    var dlg = showProgress(body + '\n\nThis page reloads automatically once the device is back online.',
                           title || 'Please wait');
    var start = Date.now(), MAX_MS = 5 * 60 * 1000, GRACE_MS = 4000, sawDown = false;
    function reload() { (window.top || window).location.reload(); }
    function poll() {
      if (Date.now() - start > MAX_MS) { reload(); return; }
      // Bound each probe so a hung connection (device mid-reboot) doesn't stall polling.
      var ctl = new AbortController();
      var timer = setTimeout(function () { ctl.abort(); }, 2500);
      fetch('/bootid', { cache: 'no-store', signal: ctl.signal })
        .then(function (r) { return r.ok ? r.text() : Promise.reject(); })
        .then(function (id) {
          clearTimeout(timer);
          id = (id || '').trim();
          // With a known baseline, the bootid changing means it rebooted. Without one
          // (capture failed), wait until we've seen it go down and come back instead.
          var back = fromId ? (id && id !== fromId) : (sawDown && id);
          if (back) { dlg.setTitle('Done'); dlg.update('Device is back online — reloading…'); setTimeout(reload, 600); }
          else setTimeout(poll, 2000);
        })
        .catch(function () { clearTimeout(timer); sawDown = true; setTimeout(poll, 2000); });   // down/abort -> retry
    }
    setTimeout(poll, GRACE_MS);
    return dlg;
  };
})(window);
