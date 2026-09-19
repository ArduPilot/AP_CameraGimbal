(() => {
  const nav = document.getElementById('lang-nav');
  if (nav && nav.form) {
    nav.form.querySelectorAll('.lang-apply').forEach(button => { button.hidden = true; });
    nav.addEventListener('change', () => nav.form.submit());
  }
  const login = document.getElementById('lang-login');
  if (login) login.addEventListener('change', () => { window.location.replace('/login?lang=' + encodeURIComponent(login.value)); });
})();
