(function () {
  'use strict';
  var root = document.documentElement;
  var themeButton = document.querySelector('.theme-toggle');
  var toast = document.querySelector('.toast');
  var toastTimer;
  function announce(message) {
    window.clearTimeout(toastTimer);
    toast.textContent = message;
    toast.hidden = false;
    toastTimer = window.setTimeout(function () { toast.hidden = true; }, 3600);
  }
  function syncThemeLabel() {
    var nextTheme = root.dataset.theme === 'dark' ? '浅色' : '深色';
    themeButton.setAttribute('aria-label', '切换到' + nextTheme + '主题');
    themeButton.title = '切换到' + nextTheme + '主题';
  }
  syncThemeLabel();
  themeButton.addEventListener('click', function () {
    root.dataset.theme = root.dataset.theme === 'dark' ? 'light' : 'dark';
    try { localStorage.setItem('acecode-help-theme', root.dataset.theme); } catch (_) {}
    syncThemeLabel();
  });

  var sidebar = document.querySelector('.sidebar');
  var menuButton = document.querySelector('.menu-toggle');
  var menuClose = document.querySelector('.drawer-close');
  var backdrop = document.querySelector('.drawer-backdrop');
  var mainLayout = document.querySelector('.document-layout');
  var headerSearch = document.querySelector('.search-trigger');
  var headerActions = document.querySelector('.header-actions');
  var drawerOpen = false;
  var mobileQuery = window.matchMedia('(max-width: 760px)');

  function setDrawer(open, restoreFocus) {
    drawerOpen = open;
    sidebar.classList.toggle('is-open', open);
    menuButton.setAttribute('aria-expanded', String(open));
    menuButton.setAttribute('aria-label', open ? '关闭文档目录' : '打开文档目录');
    backdrop.hidden = !open;
    document.body.classList.toggle('modal-open', open || searchDialog.open);
    mainLayout.inert = open;
    headerSearch.inert = open;
    headerActions.inert = open;
    if (open) {
      sidebar.setAttribute('role', 'dialog');
      sidebar.setAttribute('aria-modal', 'true');
      menuClose.focus();
    } else {
      sidebar.removeAttribute('role');
      sidebar.removeAttribute('aria-modal');
      if (restoreFocus) menuButton.focus();
    }
  }
  menuButton.addEventListener('click', function () { setDrawer(!drawerOpen, true); });
  menuClose.addEventListener('click', function () { setDrawer(false, true); });
  backdrop.addEventListener('click', function () { setDrawer(false, true); });
  sidebar.addEventListener('click', function (event) {
    if (event.target.closest('a') && drawerOpen) setDrawer(false, false);
  });
  mobileQuery.addEventListener('change', function (event) {
    if (!event.matches && drawerOpen) setDrawer(false, false);
  });

  document.querySelectorAll('.copy-button').forEach(function (button) {
    button.addEventListener('click', async function () {
      var block = button.closest('.code-block');
      var code = block.querySelector('code');
      var text = code.textContent;
      var success = false;
      button.disabled = true;
      try {
        if (navigator.clipboard && window.isSecureContext) {
          await navigator.clipboard.writeText(text);
          success = true;
        }
      } catch (_) {}
      if (!success) {
        var textarea = document.createElement('textarea');
        textarea.value = text;
        textarea.setAttribute('readonly', '');
        textarea.style.position = 'fixed';
        textarea.style.opacity = '0';
        document.body.appendChild(textarea);
        textarea.select();
        try { success = document.execCommand('copy'); } catch (_) {}
        textarea.remove();
        button.focus({ preventScroll: true });
      }
      if (success) {
        button.querySelector('span').textContent = '已复制';
        announce('已复制到剪贴板');
        window.setTimeout(function () { button.querySelector('span').textContent = '复制'; }, 1800);
      } else {
        var range = document.createRange();
        range.selectNodeContents(code);
        var selection = window.getSelection();
        if (selection) { selection.removeAllRanges(); selection.addRange(range); }
        announce('浏览器未允许复制，已选中代码，请手动复制。');
      }
      button.disabled = false;
    });
  });

  var searchDialog = document.querySelector('.search-dialog');
  var searchInput = document.querySelector('#docs-search');
  var resultsElement = document.querySelector('.search-results');
  var searchStatus = document.querySelector('#search-status');
  var searchIndex = Array.isArray(window.ACECODE_HELP_INDEX) ? window.ACECODE_HELP_INDEX : [];
  var activeIndex = 0;
  var resultLinks = [];
  var previousFocus;
  var searchTerms = function (query) {
    return query.toLocaleLowerCase().trim().split(/\s+/).filter(Boolean);
  };
  function score(item, terms) {
    var title = item.title.toLocaleLowerCase();
    var page = item.page.toLocaleLowerCase();
    var text = item.text.toLocaleLowerCase();
    if (!terms.every(function (term) { return title.includes(term) || page.includes(term) || text.includes(term); })) return 0;
    return terms.reduce(function (value, term) {
      return value + (title.includes(term) ? 10 : 0) + (page.includes(term) ? 4 : 0) + (text.includes(term) ? 1 : 0);
    }, 0);
  }
  function selectResult(index, moveFocus) {
    if (!resultLinks.length) return;
    activeIndex = (index + resultLinks.length) % resultLinks.length;
    resultLinks.forEach(function (link, i) { link.classList.toggle('is-selected', i === activeIndex); });
    if (moveFocus) resultLinks[activeIndex].focus({ preventScroll: true });
    resultLinks[activeIndex].scrollIntoView({ block: 'nearest' });
  }
  function renderResults() {
    var terms = searchTerms(searchInput.value);
    var matches = terms.length
      ? searchIndex.map(function (item) { return { item: item, score: score(item, terms) }; })
          .filter(function (match) { return match.score > 0; })
          .sort(function (a, b) { return b.score - a.score; })
          .map(function (match) { return match.item; })
      : searchIndex.filter(function (item) { return !item.url.includes('#'); });
    resultsElement.replaceChildren();
    searchStatus.textContent = terms.length
      ? (matches.length ? '找到 ' + matches.length + ' 条结果' : '没有找到相关内容')
      : '全部文档 · ' + searchIndex.filter(function (item) { return !item.url.includes('#'); }).length + ' 篇';
    resultLinks = matches.map(function (item, index) {
      var link = document.createElement('a');
      link.className = 'search-result';
      link.href = item.url;
      var path = document.createElement('span');
      path.textContent = item.group + ' / ' + item.page;
      var title = document.createElement('strong');
      title.textContent = item.title;
      var description = document.createElement('p');
      description.textContent = item.description;
      link.append(path, title, description);
      link.addEventListener('focus', function () { selectResult(index, false); });
      link.addEventListener('click', function () { closeSearch(false); });
      resultsElement.appendChild(link);
      return link;
    });
    if (!matches.length) {
      var empty = document.createElement('p');
      empty.className = 'search-empty';
      empty.textContent = '试试“安装”“模型”“工作区”或具体命令名称。';
      resultsElement.appendChild(empty);
    }
    selectResult(0, false);
  }
  function openSearch() {
    if (drawerOpen) setDrawer(false, false);
    if (searchDialog.open) return;
    previousFocus = document.activeElement;
    searchInput.value = '';
    searchDialog.showModal();
    document.body.classList.add('modal-open');
    renderResults();
    searchInput.focus();
  }
  function closeSearch(restoreFocus) {
    searchDialog.close();
    document.body.classList.toggle('modal-open', drawerOpen);
    if (restoreFocus !== false && previousFocus && previousFocus.isConnected) previousFocus.focus();
  }
  headerSearch.addEventListener('click', openSearch);
  document.querySelector('.search-close').addEventListener('click', function () { closeSearch(); });
  searchInput.addEventListener('input', renderResults);
  searchDialog.addEventListener('cancel', function (event) { event.preventDefault(); closeSearch(); });
  searchDialog.addEventListener('click', function (event) {
    if (event.target !== searchDialog) return;
    var bounds = searchDialog.getBoundingClientRect();
    if (event.clientX < bounds.left || event.clientX > bounds.right || event.clientY < bounds.top || event.clientY > bounds.bottom) closeSearch();
  });
  searchDialog.addEventListener('keydown', function (event) {
    if (event.isComposing) return;
    if (event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      closeSearch();
      return;
    }
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      var isInput = event.target === searchInput;
      var change = event.key === 'ArrowDown' ? 1 : -1;
      selectResult(isInput && event.key === 'ArrowDown' ? activeIndex : activeIndex + change, true);
    } else if (event.key === 'Enter' && event.target === searchInput && resultLinks.length) {
      event.preventDefault();
      resultLinks[activeIndex].click();
    }
  });
  document.addEventListener('keydown', function (event) {
    if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'k') {
      event.preventDefault();
      if (searchDialog.open) closeSearch(); else openSearch();
    }
    if (drawerOpen && event.key === 'Escape') {
      event.preventDefault();
      setDrawer(false, true);
    }
    if (drawerOpen && event.key === 'Tab') {
      var focusable = [menuButton].concat(Array.from(sidebar.querySelectorAll('button, a[href], summary'))
        .filter(function (item) { return item.getClientRects().length > 0; }));
      var first = focusable[0];
      var last = focusable[focusable.length - 1];
      if (event.shiftKey && (document.activeElement === first || !focusable.includes(document.activeElement))) {
        event.preventDefault(); last.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault(); first.focus();
      }
    }
  });

  var headingList = Array.from(document.querySelectorAll('.article h2[id]'));
  var tocLinks = Array.from(document.querySelectorAll('[data-toc-link]'));
  var pendingFrame = false;
  function updateContents() {
    pendingFrame = false;
    var cutoff = parseInt(getComputedStyle(root).getPropertyValue('--header-height'), 10) + 75;
    var active = headingList[0];
    headingList.forEach(function (heading) {
      if (heading.getBoundingClientRect().top <= cutoff) active = heading;
    });
    if (!active) return;
    if (window.scrollY + window.innerHeight >= document.documentElement.scrollHeight - 5) active = headingList[headingList.length - 1];
    tocLinks.forEach(function (link) {
      if (link.hash === '#' + active.id) link.setAttribute('aria-current', 'location');
      else link.removeAttribute('aria-current');
    });
    document.querySelectorAll('.platform-links a').forEach(function (link) {
      if (link.hash === '#' + active.id) link.setAttribute('aria-current', 'location');
      else link.removeAttribute('aria-current');
    });
  }
  function scheduleContents() {
    if (!pendingFrame) { pendingFrame = true; window.requestAnimationFrame(updateContents); }
  }
  window.addEventListener('scroll', scheduleContents, { passive: true });
  window.addEventListener('resize', scheduleContents, { passive: true });
  window.addEventListener('hashchange', scheduleContents);
  document.querySelectorAll('.mobile-contents a').forEach(function (link) {
    link.addEventListener('click', function (event) {
      var target = document.getElementById(link.hash.slice(1));
      link.closest('details').open = false;
      if (target) {
        event.preventDefault();
        window.location.hash = link.hash;
        window.requestAnimationFrame(function () { target.scrollIntoView(); });
      }
    });
  });
  updateContents();
}());
