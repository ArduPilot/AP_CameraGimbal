(() => {
  const form = document.querySelector('form[action="/parameters"]');
  if (!form) return;
  const tablist = document.getElementById('parameter-tabs');
  const tabs = [...tablist.querySelectorAll('[role=tab]')];
  const panels = tabs.map(tab => document.getElementById(tab.getAttribute('aria-controls')));
  function selectTab(tab, focus = false) {
    if (!tabs.includes(tab)) tab = tabs[0];
    tabs.forEach((item, i) => {
      const selected = item === tab;
      item.setAttribute('aria-selected', String(selected));
      item.tabIndex = selected ? 0 : -1;
      panels[i].hidden = !selected;
    });
    const name = tab.id.slice(4);
    form.setAttribute('action', '/parameters#' + name);
    try { history.replaceState(null, '', '#' + name); } catch (_) {}
    if (focus) tab.focus();
  }
  function reveal(field) {
    const panel = field.closest('.parameter-panel');
    selectTab(tabs[panels.indexOf(panel)]);
  }
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => selectTab(tab));
    tab.addEventListener('keydown', event => {
      let next;
      if (event.key === 'ArrowRight') next = (index + 1) % tabs.length;
      else if (event.key === 'ArrowLeft') next = (index + tabs.length - 1) % tabs.length;
      else if (event.key === 'Home') next = 0;
      else if (event.key === 'End') next = tabs.length - 1;
      else return;
      event.preventDefault(); selectTab(tabs[next], true);
    });
  });
  panels.forEach(panel => panel.setAttribute('role', 'tabpanel'));
  tablist.hidden = false;
  const fromHash = () => selectTab(tabs.find(tab => tab.id === 'tab-' + location.hash.slice(1)));
  fromHash();
  window.addEventListener('hashchange', fromHash);
  const fields = [...form.querySelectorAll('.field input, .field select')];
  const touched = new Set();
  let attempted = Boolean(document.querySelector('.notice.error'));
  const get = name => form.elements.namedItem(name);
  const value = name => get(name)?.value || '';
  for (const field of fields) {
    const error = document.createElement('div');
    error.id = field.id + '-error';
    error.className = 'field-error';
    error.hidden = true;
    field.setAttribute('aria-describedby', error.id);
    field.closest('.field').append(error);
  }
  const ipv4 = text => {
    const parts = text.split('.');
    return parts.length === 4 && parts.every(p => /^(0|[1-9][0-9]{0,2})$/.test(p) && Number(p) <= 255);
  };
  const number = text => text.split('.').reduce((v, n) => ((v << 8) | Number(n)) >>> 0, 0);
  const host = text => ipv4(text) && Number(text.split('.')[0]) > 0 && Number(text.split('.')[0]) !== 127 && Number(text.split('.')[0]) < 224;
  function invalid(field, message) {
    if (field) field.setCustomValidity(message || L.invalid.replace('%s', field.closest('.field').querySelector('label').textContent));
  }
  function validate() {
    for (const field of fields) {
      field.setCustomValidity('');
      if (field.name.startsWith('proxy_') && ['text', 'password'].includes(field.type)) {
        if (/[^\x20-\x7e]|"/.test(field.value) || field.value.length > field.maxLength) invalid(field);
      }
      if (field.name === 'timezone' && /[\s\x00-\x1f\x7f]/.test(field.value)) invalid(field);
    }
    for (const name of ['proxy_host', 'network_interface', 'main_alias', 'sub_alias']) {
      if (!/^[a-zA-Z0-9_.-]*$/.test(value(name))) invalid(get(name));
    }
    const parseAddress = text => {
      const parts = text.split('/');
      if (parts.length !== 2 || !host(parts[0]) || !/^([1-9]|[12][0-9]|3[0-2])$/.test(parts[1])) return null;
      const ip = number(parts[0]), prefix = Number(parts[1]), mask = (0xffffffff << (32 - prefix)) >>> 0;
      if (prefix < 31 && ((ip & ~mask) === 0 || (ip & ~mask) === (~mask >>> 0))) return null;
      return {ip, mask, prefix};
    };
    for (const name of ['network_primary_address', 'network_secondary_address']) {
      if (value(name) && !parseAddress(value(name))) invalid(get(name), L.address);
    }
    const primary = parseAddress(value('network_primary_address'));
    const secondary = parseAddress(value('network_secondary_address'));
    const gateway = value('network_gateway');
    if (primary && secondary && primary.ip === secondary.ip) invalid(get('network_secondary_address'), L.network);
    if (gateway) {
      if (!host(gateway)) invalid(get('network_gateway'));
      else {
        const g = number(gateway);
        const reachable = address => address && !((g ^ address.ip) & address.mask) &&
          (address.prefix >= 31 || ((g & ~address.mask) !== 0 && (g & ~address.mask) !== (~address.mask >>> 0)));
        if ((primary && g === primary.ip) || (secondary && g === secondary.ip) ||
            (primary && !reachable(primary) && !reachable(secondary)))
          invalid(get('network_gateway'), L.network);
      }
    }
    const reconnect = document.getElementById('network-reconnect');
    reconnect.hidden = !primary || reconnect.dataset.sitl === 'true';
    if (primary) {
      const link = document.getElementById('network-link');
      const url = new URL(location.href);
      url.hostname = value('network_primary_address').split('/')[0];
      url.pathname = '/parameters'; url.search = ''; url.hash = 'network';
      link.href = url.href; link.textContent = url.href;
    }
    if (value('proxy_enabled') === 'true') {
      if (!value('proxy_host')) invalid(get('proxy_host'));
      if (value('proxy_signing') === 'true' && !value('proxy_signing_passphrase')) invalid(get('proxy_signing_passphrase'));
      for (const n of [1, 2]) {
        if (Number(value('proxy_video' + n + '_port')) && !value('proxy_video' + n + '_name')) invalid(get('proxy_video' + n + '_name'));
      }
      if (Number(value('proxy_video1_port')) && Number(value('proxy_video1_port')) === Number(value('proxy_video2_port')))
        invalid(get('proxy_video2_port'), L.ports);
    }
    for (const field of fields) {
      const bad = !field.validity.valid && (attempted || touched.has(field));
      const error = document.getElementById(field.id + '-error');
      field.setAttribute('aria-invalid', String(bad));
      error.hidden = !bad;
      error.textContent = bad ? field.validationMessage : '';
    }
    tabs.forEach((tab, i) => tab.classList.toggle('has-error', Boolean(panels[i].querySelector('[aria-invalid=true]'))));
    return fields.find(field => !field.validity.valid);
  }
  for (const name of ['input', 'change']) form.addEventListener(name, event => {
    touched.add(event.target);
    validate();
  });
  form.addEventListener('submit', event => {
    attempted = true;
    const bad = validate();
    if (bad) {
      event.preventDefault();
      reveal(bad); bad.focus();
      bad.reportValidity();
      return;
    }
    const reconnect = document.getElementById('network-reconnect');
    const link = document.getElementById('network-link');
    if (event.submitter && event.submitter.value === 'save_restart' && !reconnect.hidden &&
        new URL(link.href).hostname !== location.hostname) {
      // Keep the reconnect link visible if removing our address breaks the response.
      event.preventDefault();
      reveal(get('network_primary_address'));
      const body = new URLSearchParams(new FormData(form));
      body.set('action', 'save_restart');
      const buttons = [...form.querySelectorAll('button[type=submit]')];
      buttons.forEach(button => { button.disabled = true; });
      // Named submit buttons shadow form.action; read the HTML attribute.
      const endpoint = form.getAttribute('action').split('#')[0];
      fetch(endpoint, {method: 'POST', body, signal: AbortSignal.timeout(45000)})
        .then(response => response.text()).then(page => {
          document.open(); document.write(page); document.close();
        }).catch(() => {
          const notice = document.createElement('p');
          notice.className = 'notice error'; notice.textContent = L.connectionLost;
          reconnect.before(notice);
          buttons.forEach(button => { button.disabled = false; });
        });
    }
  });
  // Run our checks before native submission so dependent fields are checked too.
  form.noValidate = true;
  const bad = validate();
  if (attempted && bad) { reveal(bad); bad.focus(); }
})();
