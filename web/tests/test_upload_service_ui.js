// The Cloud Upload tab shows the credentials of the selected cloud service and hides the other.
// This runs the page script's switching code against a small fake page.
const fs = require('fs');
const src = fs.readFileSync(process.argv[2], 'utf8');
const start = src.indexOf('// Show the credentials of the selected cloud service');
const end = src.indexOf('// Saving or removing the credentials reloads the page', start);
if (start < 0 || end < 0) { console.log('FAIL upload service UI: switching code not found'); process.exit(1); }
const code = src.slice(start, end);

function page(selected, services) {
  const listeners = [];
  const select = { value: selected, addEventListener: (type, fn) => { if (type === 'change') listeners.push(fn); } };
  const blocks = services.map(name => ({ dataset: { service: name }, hidden: false }));
  const document = {
    getElementById: id => (id === 'upload_service' ? select : null),
    querySelectorAll: selector => (selector === '.upload-credentials[data-service]' ? blocks : []),
  };
  new Function('document', code)(document);
  return { select, blocks, change: value => { select.value = value; listeners.forEach(fn => fn()); } };
}
const shown = p => p.blocks.filter(b => !b.hidden).map(b => b.dataset.service).join(',');
let bad = 0;
const check = (ok, message) => { if (!ok) { console.log('FAIL:', message); bad++; } };

const p = page('dropbox', ['gdrive', 'dropbox']);
check(shown(p) === 'dropbox', 'starts with the saved service: ' + shown(p));
p.change('gdrive');
check(shown(p) === 'gdrive', 'switches to Google Drive: ' + shown(p));
p.change('dropbox');
check(shown(p) === 'dropbox', 'switches back to Dropbox: ' + shown(p));
check(shown(page('gdrive', ['gdrive', 'dropbox'])) === 'gdrive', 'Google Drive selected at the start');
// A page without the setting or the blocks is left alone.
new Function('document', code)({ getElementById: () => null, querySelectorAll: () => [] });
console.log(bad ? 'FAIL upload service UI: ' + bad + ' problems' : 'PASS upload service dropdown shows only the selected service\'s credentials');
process.exit(bad ? 1 : 0);
