(function () {
  'use strict';
  document.documentElement.classList.add('js');
  try {
    var savedTheme = localStorage.getItem('acecode-help-theme');
    if (savedTheme === 'light' || savedTheme === 'dark') {
      document.documentElement.dataset.theme = savedTheme;
    }
  } catch (_) {
    // Reading remains available when file-origin storage is restricted.
  }
}());
