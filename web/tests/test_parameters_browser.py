"""Check the rendered parameter form in an isolated headless Chrome profile."""
import html
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


CHECKS = r"""
(() => {
  const result = document.createElement('pre');
  result.id = 'browser-test-result';
  document.body.append(result);
  try {
    const form = document.querySelector('form[action="/parameters"]');
    const field = name => form.elements.namedItem(name);
    const original = Object.fromEntries(new FormData(form));
    const assert = (ok, message) => { if (!ok) throw new Error(message); };
    let blocked;
    // Observe whether the application blocked submission, then prevent actual
    // navigation for valid cases too so this test never writes configuration.
    form.addEventListener('submit', event => {
      blocked = event.defaultPrevented;
      event.preventDefault();
    });
    function submit() {
      blocked = undefined;
      form.requestSubmit(form.querySelector('button[value=save]'));
      assert(blocked !== undefined, 'submit handler did not run');
    }
    function reset() {
      for (const [name, value] of Object.entries(original)) field(name).value = value;
      form.dispatchEvent(new Event('input', {bubbles: true}));
    }
    function rejected(changes, badName) {
      reset();
      for (const [name, value] of Object.entries(changes)) field(name).value = value;
      const before = JSON.stringify([...new FormData(form)]);
      submit();
      assert(blocked, 'invalid form submitted: ' + badName);
      assert(document.activeElement === field(badName), 'wrong focused field: ' + badName);
      assert(field(badName).getAttribute('aria-invalid') === 'true', 'missing highlight: ' + badName);
      const error = document.getElementById(badName + '-error');
      assert(!error.hidden && error.textContent, 'missing inline error: ' + badName);
      assert(JSON.stringify([...new FormData(form)]) === before, 'entered values lost');
    }
    submit();
    assert(blocked === false, 'valid configuration rejected');
    rejected({proxy_network_address: '192.168.2.97'}, 'proxy_network_address');
    assert(document.getElementById('proxy_network_address-error').textContent.includes('/24'), 'missing prefix guidance');
    field('proxy_network_address').value = '192.168.2.97/24';
    field('proxy_network_address').dispatchEvent(new Event('input', {bubbles: true}));
    assert(field('proxy_network_address').getAttribute('aria-invalid') === 'false', 'correction still highlighted');
    submit();
    assert(blocked === false, 'corrected prefix rejected');
    rejected({proxy_network_address: '192.168.2.999/24'}, 'proxy_network_address');
    rejected({proxy_network_address: '192.168.2.97/33'}, 'proxy_network_address');
    rejected({proxy_network_gateway: '192.168.2.999'}, 'proxy_network_gateway');
    rejected({proxy_host: ''}, 'proxy_host');
    rejected({proxy_host: 'rtsp://localhost'}, 'proxy_host');
    rejected({proxy_signing_passphrase: ''}, 'proxy_signing_passphrase');
    rejected({proxy_video2_port: original.proxy_video1_port}, 'proxy_video2_port');
    rejected({proxy_video1_name: ''}, 'proxy_video1_name');
    rejected({proxy_publish_password: 'bad"password'}, 'proxy_publish_password');
    rejected({mavlink_system_id: '256'}, 'mavlink_system_id');
    rejected({mavlink_system_id: '1.5'}, 'mavlink_system_id');
    rejected({brightness: '101'}, 'brightness');
    reset();
    field('proxy_enabled').value = 'false';
    field('proxy_host').value = '';
    field('proxy_signing_passphrase').value = '';
    field('proxy_network_address').value = '';
    field('proxy_network_gateway').value = '';
    submit();
    assert(blocked === false, 'disabled proxy requires optional fields');
    result.textContent = 'PASS browser validation, focus, highlighting, corrections and input preservation';
  } catch (error) {
    result.textContent = 'FAIL ' + error.stack;
  }
})();
"""


def check_parameters_browser(page, script):
    browser = shutil.which('google-chrome') or shutil.which('chromium')
    if browser is None:
        print('SKIP parameter browser checks: Chrome/Chromium not installed')
        return
    # Chrome helpers can briefly retain profile files after --dump-dom exits.
    with tempfile.TemporaryDirectory(prefix='camera-parameters-browser-', ignore_cleanup_errors=True) as temp:
        root = Path(temp)
        # Use the actual server-rendered form and translated validation script,
        # but remove unrelated network scripts and never submit to the camera.
        document = re.sub(r'<script\b[^>]*>.*?</script>', '', page.decode(), flags=re.S)
        document = document.replace('</body>', '<script>' + script.decode() +
                                    '</script><script>' + CHECKS + '</script></body>')
        fixture = root / 'form.html'
        fixture.write_text(document)
        run = subprocess.run([browser, '--headless', '--no-sandbox', '--disable-gpu',
                              '--disable-dev-shm-usage', '--no-first-run',
                              '--disable-background-networking', '--disable-component-update',
                              '--disable-extensions', '--timeout=10000',
                              '--user-data-dir=' + str(root / 'profile'),
                              '--dump-dom', fixture.as_uri()],
                             capture_output=True, timeout=30)
        match = re.search(r'<pre id="browser-test-result">(.*?)</pre>',
                          run.stdout.decode(), re.S)
        assert run.returncode == 0 and match, run.stderr.decode()[-2000:]
        result = html.unescape(match.group(1))
        assert result.startswith('PASS '), result
        print(result)
