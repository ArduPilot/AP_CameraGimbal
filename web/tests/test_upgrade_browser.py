"""Exercise the served upload JavaScript with file selections in a DOM stub."""
import json
import subprocess


def check_upgrade_browser(script, accepted, rejected):
    harness = r'''
const vm = require('vm');
const assert = require('assert');
const fixture = JSON.parse(require('fs').readFileSync(0, 'utf8'));
for (const [names, allowed] of [[fixture.accepted, true], [fixture.rejected, false]]) {
  for (const name of names) {
    let change, sent;
    const elements = {
      'firmware-upload': {},
      'firmware': {files: [{name, size: 1024}], addEventListener: (type, fn) => {change = fn;}},
      'select-firmware': {addEventListener: () => {}},
      'firmware-progress': {}, 'firmware-status': {},
    };
    const context = {
      document: {currentScript: {dataset: {csrf: 'test-token'}}, getElementById: id => elements[id]},
      confirm: () => true,
      XMLHttpRequest: function() {
        this.upload = {};
        this.open = (method, path) => {assert.equal(method, 'POST'); assert.equal(path, '/upgrade');};
        this.setRequestHeader = () => {};
        this.send = file => {sent = file.name;};
      },
    };
    vm.runInNewContext(fixture.script, context);
    change();
    assert.equal(sent === name, allowed, name);
    if (!allowed) assert.ok(elements['firmware-status'].textContent);
  }
}
'''
    subprocess.run(['node', '-e', harness], input=json.dumps(dict(
        script=script.decode(), accepted=accepted, rejected=rejected)), text=True, check=True)
