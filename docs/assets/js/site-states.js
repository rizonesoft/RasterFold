(function () {
  "use strict";

  var ALLOWED_THEMES = {
    "light": "Light",
    "dark": "Dark",
    "hc-light": "High Contrast Light",
    "hc-dark": "High Contrast Dark"
  };

  var ALLOWED_DENSITIES = {
    "comfortable": "Comfortable",
    "compact": "Compact"
  };

  var SESSION_PREFIX = "rf_docs_session:";
  var forceStorageUnavailable = false;
  var forceClipboardFail = false;
  var fallbackSearchData = null;
  var useFallbackSearch = false;

  function announce(message) {
    var region = document.getElementById("rf-a11y-status");
    if (!region) {
      return;
    }
    region.textContent = "";
    window.setTimeout(function () {
      region.textContent = message;
    }, 10);
  }

  function readSessionFallback() {
    try {
      if (typeof window.name === "string" && window.name.indexOf(SESSION_PREFIX) === 0) {
        var parsed = JSON.parse(window.name.slice(SESSION_PREFIX.length));
        if (parsed && typeof parsed === "object") {
          return parsed;
        }
      }
    } catch (e) {
      /* Ignore malformed window.name */
    }
    return {};
  }

  function writeSessionFallback(updates) {
    try {
      var cur = readSessionFallback();
      if (updates && typeof updates === "object") {
        if (updates.theme && ALLOWED_THEMES[updates.theme]) {
          cur.theme = updates.theme;
        }
        if (typeof updates.themeUnpersisted === "boolean") {
          cur.themeUnpersisted = updates.themeUnpersisted;
        }
        if (updates.density && ALLOWED_DENSITIES[updates.density]) {
          cur.density = updates.density;
        }
        if (typeof updates.densityUnpersisted === "boolean") {
          cur.densityUnpersisted = updates.densityUnpersisted;
        }
      }
      window.name = SESSION_PREFIX + JSON.stringify(cur);
    } catch (e) {
      /* Ignore window.name errors */
    }
  }

  function safeStorageGet(key) {
    if (forceStorageUnavailable) {
      throw new Error("storage disabled");
    }
    return window.localStorage.getItem(key);
  }

  function safeStorageSet(key, value) {
    if (forceStorageUnavailable) {
      throw new Error("storage disabled");
    }
    window.localStorage.setItem(key, value);
  }

  function getTheme() {
    var attr = document.documentElement.getAttribute("data-rf-theme");
    return (attr && ALLOWED_THEMES[attr]) ? attr : "light";
  }

  function getDensity() {
    var attr = document.documentElement.getAttribute("data-rf-density");
    return (attr && ALLOWED_DENSITIES[attr]) ? attr : "comfortable";
  }

  function syncToolbarButtons() {
    var theme = getTheme();
    var density = getDensity();
    var themeBtns = document.querySelectorAll(".rf-theme-btn[data-rf-theme-choice]");
    for (var i = 0; i < themeBtns.length; i++) {
      var tChoice = themeBtns[i].getAttribute("data-rf-theme-choice");
      themeBtns[i].setAttribute("aria-pressed", tChoice === theme ? "true" : "false");
    }
    var densityBtns = document.querySelectorAll(".rf-density-btn[data-rf-density-choice]");
    for (var j = 0; j < densityBtns.length; j++) {
      var dChoice = densityBtns[j].getAttribute("data-rf-density-choice");
      densityBtns[j].setAttribute("aria-pressed", dChoice === density ? "true" : "false");
    }
  }

  function setTheme(theme, shouldAnnounce) {
    if (!ALLOWED_THEMES[theme]) {
      return false;
    }
    document.documentElement.setAttribute("data-rf-theme", theme);
    var persisted = true;
    try {
      safeStorageSet("rf_docs_theme", theme);
    } catch (e) {
      persisted = false;
    }
    writeSessionFallback({ theme: theme, themeUnpersisted: !persisted });
    syncToolbarButtons();
    if (shouldAnnounce !== false) {
      announce(
        "Theme set to " + ALLOWED_THEMES[theme] + (persisted ? "" : " (session only)")
      );
    }
    return true;
  }

  function setDensity(density, shouldAnnounce) {
    if (!ALLOWED_DENSITIES[density]) {
      return false;
    }
    document.documentElement.setAttribute("data-rf-density", density);
    var persisted = true;
    try {
      safeStorageSet("rf_docs_density", density);
    } catch (e) {
      persisted = false;
    }
    writeSessionFallback({ density: density, densityUnpersisted: !persisted });
    syncToolbarButtons();
    if (shouldAnnounce !== false) {
      announce(
        "Density set to " + ALLOWED_DENSITIES[density] + (persisted ? "" : " (session only)")
      );
    }
    return true;
  }

  function initToolbarControls() {
    var sess = readSessionFallback();
    var resolvedTheme = null;
    var resolvedDensity = null;
    var themeUnpersisted = Boolean(sess.themeUnpersisted);
    var densityUnpersisted = Boolean(sess.densityUnpersisted);

    if (themeUnpersisted && sess.theme && ALLOWED_THEMES[sess.theme]) {
      resolvedTheme = sess.theme;
    }
    if (densityUnpersisted && sess.density && ALLOWED_DENSITIES[sess.density]) {
      resolvedDensity = sess.density;
    }

    try {
      if (!resolvedTheme) {
        var savedTheme = safeStorageGet("rf_docs_theme");
        if (savedTheme && ALLOWED_THEMES[savedTheme]) {
          resolvedTheme = savedTheme;
          themeUnpersisted = false;
        }
      }
      if (!resolvedDensity) {
        var savedDensity = safeStorageGet("rf_docs_density");
        if (savedDensity && ALLOWED_DENSITIES[savedDensity]) {
          resolvedDensity = savedDensity;
          densityUnpersisted = false;
        }
      }
    } catch (e) {
      /* Storage unavailable; fall back to window.name session state */
    }

    if (!resolvedTheme && sess.theme && ALLOWED_THEMES[sess.theme]) {
      resolvedTheme = sess.theme;
    }
    if (!resolvedDensity && sess.density && ALLOWED_DENSITIES[sess.density]) {
      resolvedDensity = sess.density;
    }
    if (resolvedTheme) {
      document.documentElement.setAttribute("data-rf-theme", resolvedTheme);
    }
    if (resolvedDensity) {
      document.documentElement.setAttribute("data-rf-density", resolvedDensity);
    }
    writeSessionFallback({
      theme: getTheme(),
      themeUnpersisted: themeUnpersisted,
      density: getDensity(),
      densityUnpersisted: densityUnpersisted
    });
    syncToolbarButtons();

    var themeBtns = document.querySelectorAll(".rf-theme-btn[data-rf-theme-choice]");
    for (var i = 0; i < themeBtns.length; i++) {
      themeBtns[i].addEventListener("click", function (ev) {
        var choice = ev.currentTarget.getAttribute("data-rf-theme-choice");
        setTheme(choice, true);
      });
    }

    var densityBtns = document.querySelectorAll(".rf-density-btn[data-rf-density-choice]");
    for (var j = 0; j < densityBtns.length; j++) {
      densityBtns[j].addEventListener("click", function (ev) {
        var choice = ev.currentTarget.getAttribute("data-rf-density-choice");
        setDensity(choice, true);
      });
    }
  }

  function initPageOutline() {
    var main = document.getElementById("main-content");
    if (!main || document.getElementById("page-outline")) {
      return;
    }
    var headings = main.querySelectorAll("h2[id]");
    if (headings.length < 2) {
      return;
    }
    var nav = document.createElement("nav");
    nav.id = "page-outline";
    nav.className = "rf-page-outline";
    nav.setAttribute("aria-label", "On this page");

    var title = document.createElement("p");
    title.className = "rf-page-outline-title";
    title.textContent = "On this page";
    nav.appendChild(title);

    var list = document.createElement("ul");
    list.className = "rf-page-outline-list";
    for (var i = 0; i < headings.length; i++) {
      var h = headings[i];
      var id = h.getAttribute("id");
      var label = (h.textContent || "").replace(/\s+/g, " ").trim();
      if (!id || !label) {
        continue;
      }
      var li = document.createElement("li");
      var a = document.createElement("a");
      a.setAttribute("href", "#" + id);
      a.textContent = label;
      li.appendChild(a);
      list.appendChild(li);
    }
    nav.appendChild(list);

    var h1 = main.querySelector("h1");
    if (h1 && h1.nextSibling) {
      h1.parentNode.insertBefore(nav, h1.nextSibling);
    } else {
      main.insertBefore(nav, main.firstChild);
    }
  }

  function copyTextToClipboard(text) {
    if (forceClipboardFail) {
      return Promise.reject(new Error("Clipboard access denied"));
    }
    if (window.navigator && window.navigator.clipboard && window.isSecureContext) {
      return window.navigator.clipboard.writeText(text);
    }
    return new Promise(function (resolve, reject) {
      try {
        var ta = document.createElement("textarea");
        ta.value = text;
        ta.setAttribute("readonly", "readonly");
        ta.style.position = "fixed";
        ta.style.top = "-9999px";
        document.body.appendChild(ta);
        ta.select();
        var ok = document.execCommand("copy");
        document.body.removeChild(ta);
        if (ok) {
          resolve();
        } else {
          reject(new Error("execCommand copy returned false"));
        }
      } catch (err) {
        reject(err);
      }
    });
  }

  function initCodeCopyButtons() {
    var main = document.getElementById("main-content");
    if (!main) {
      return;
    }
    var blocks = main.querySelectorAll("div.highlighter-rouge, figure.highlight");
    for (var i = 0; i < blocks.length; i++) {
      (function (block) {
        if (block.querySelector(".rf-copy-button")) {
          return;
        }
        var btn = document.createElement("button");
        btn.type = "button";
        btn.className = "rf-copy-button";
        btn.setAttribute("aria-label", "Copy code to clipboard");
        btn.textContent = "Copy";
        block.appendChild(btn);

        btn.addEventListener("click", function () {
          var codeEl = block.querySelector("pre code") || block.querySelector("pre") || block.querySelector("code");
          var codeText = codeEl ? (codeEl.innerText || codeEl.textContent || "") : "";
          copyTextToClipboard(codeText)
            .then(function () {
              btn.setAttribute("data-copy-state", "copied");
              btn.textContent = "Copied";
              announce("Copied code to clipboard");
              window.setTimeout(function () {
                btn.removeAttribute("data-copy-state");
                btn.textContent = "Copy";
              }, 3000);
            })
            .catch(function () {
              btn.setAttribute("data-copy-state", "failed");
              btn.textContent = "Copy failed";
              announce("Unable to copy code to clipboard; select and copy the code block manually");
              window.setTimeout(function () {
                btn.removeAttribute("data-copy-state");
                btn.textContent = "Copy";
              }, 3000);
            });
        });
      })(blocks[i]);
    }
  }

  function isValidSearchIndex(data) {
    if (!data || typeof data !== "object" || Array.isArray(data)) {
      return false;
    }
    var keys = Object.keys(data);
    if (keys.length === 0) {
      return false;
    }
    for (var i = 0; i < keys.length; i++) {
      var entry = data[keys[i]];
      if (entry && typeof entry === "object" && typeof entry.url === "string" && entry.url.length > 0) {
        return true;
      }
    }
    return false;
  }

  function renderFallbackSearchResults(query) {
    var searchInput = document.getElementById("search-input");
    var searchResults = document.getElementById("search-results");
    if (!searchResults || !fallbackSearchData) {
      return;
    }
    var q = (query || "").trim();
    if (!q) {
      searchResults.innerHTML = "";
      document.documentElement.classList.remove("search-active");
      if (searchInput) {
        searchInput.setAttribute("aria-expanded", "false");
      }
      return;
    }
    var terms = q.toLowerCase().split(/\s+/).filter(Boolean);
    var matches = [];
    var seenUrls = {};
    var keys = Object.keys(fallbackSearchData);
    for (var i = 0; i < keys.length; i++) {
      var entry = fallbackSearchData[keys[i]];
      if (!entry || !entry.url) {
        continue;
      }
      var hayTitle = (entry.title || "").toLowerCase();
      var hayDoc = (entry.doc || "").toLowerCase();
      var hayContent = (entry.content || "").toLowerCase();
      var score = 0;
      var allMatched = true;
      for (var t = 0; t < terms.length; t++) {
        var term = terms[t];
        var inTitle = hayTitle.indexOf(term) !== -1;
        var inDoc = hayDoc.indexOf(term) !== -1;
        var inContent = hayContent.indexOf(term) !== -1;
        if (!inTitle && !inDoc && !inContent) {
          allMatched = false;
          break;
        }
        if (inTitle) {
          score += 10;
        }
        if (inDoc) {
          score += 5;
        }
        if (inContent) {
          score += 1;
        }
      }
      if (allMatched && !seenUrls[entry.url]) {
        seenUrls[entry.url] = true;
        matches.push({ entry: entry, score: score });
      }
    }
    matches.sort(function (a, b) {
      return b.score - a.score;
    });

    searchResults.innerHTML = "";
    document.documentElement.classList.add("search-active");
    if (matches.length === 0) {
      var noRes = document.createElement("div");
      noRes.className = "search-no-result";
      noRes.setAttribute("data-rf-fallback", "true");
      noRes.textContent = "No results found";
      searchResults.appendChild(noRes);
      enhanceSearchResults();
      return;
    }

    var ul = document.createElement("ul");
    ul.className = "search-results-list";
    ul.setAttribute("data-rf-fallback", "true");
    var limit = Math.min(matches.length, 20);
    for (var m = 0; m < limit; m++) {
      var item = matches[m].entry;
      var li = document.createElement("li");
      li.className = "search-results-list-item";
      var a = document.createElement("a");
      a.className = "search-result";
      a.setAttribute("href", item.url);
      var titleDiv = document.createElement("div");
      titleDiv.className = "search-result-title";
      titleDiv.textContent = item.title || item.doc || item.url;
      a.appendChild(titleDiv);
      li.appendChild(a);
      ul.appendChild(li);
    }
    searchResults.appendChild(ul);
    enhanceSearchResults();
  }

  function triggerSearchOffline() {
    useFallbackSearch = true;
    var banner = document.getElementById("rf-search-offline");
    var searchInput = document.getElementById("search-input");
    if (banner) {
      banner.removeAttribute("hidden");
    }
    if (searchInput) {
      searchInput.setAttribute("aria-invalid", "true");
    }
    announce("Search index is unavailable offline");
  }

  function loadSearchIndex(isRetry) {
    var banner = document.getElementById("rf-search-offline");
    var searchInput = document.getElementById("search-input");
    var fetchOpts = isRetry ? { cache: "no-store" } : {};
    return window.fetch("/RasterFold/assets/js/search-data.json", fetchOpts)
      .then(function (resp) {
        if (!resp.ok) {
          throw new Error("HTTP " + resp.status);
        }
        return resp.json();
      })
      .then(function (data) {
        if (!isValidSearchIndex(data)) {
          throw new Error("Invalid search-data.json payload");
        }
        fallbackSearchData = data;
        useFallbackSearch = true;
        window.__rfSearchIndexState = { status: "loaded", data: data };
        if (banner) {
          banner.setAttribute("hidden", "hidden");
        }
        if (searchInput) {
          searchInput.removeAttribute("aria-invalid");
          if (isRetry) {
            ensureSearchInputVisibleAndFocus(searchInput);
            if (searchInput.value && searchInput.value.trim()) {
              renderFallbackSearchResults(searchInput.value);
            }
          }
        }
        if (isRetry) {
          announce("Search index loaded");
        }
        return true;
      })
      .catch(function () {
        fallbackSearchData = null;
        triggerSearchOffline();
        return false;
      });
  }

  function retrySearchIndex() {
    return loadSearchIndex(true);
  }

  function ensureSearchInputVisibleAndFocus(searchInput) {
    if (!searchInput) {
      return;
    }
    var mainHeader = document.getElementById("main-header");
    var siteNav = document.getElementById("site-nav");
    var menuButton = document.getElementById("menu-button");
    if (mainHeader && window.getComputedStyle(mainHeader).display === "none") {
      mainHeader.classList.add("nav-open");
      if (siteNav) {
        siteNav.classList.add("nav-open");
      }
      if (menuButton) {
        menuButton.classList.add("nav-open");
        menuButton.setAttribute("aria-expanded", "true");
      }
    }
    searchInput.focus();
    searchInput.select();
  }

  function clearSearchAndFocus() {
    var searchInput = document.getElementById("search-input");
    var searchResults = document.getElementById("search-results");
    document.documentElement.classList.remove("search-active");
    if (searchResults) {
      searchResults.innerHTML = "";
    }
    if (searchInput) {
      searchInput.value = "";
      searchInput.setAttribute("aria-expanded", "false");
      searchInput.dispatchEvent(new Event("keyup", { bubbles: true }));
      ensureSearchInputVisibleAndFocus(searchInput);
    }
    announce("Search cleared");
  }

  function enhanceSearchResults() {
    var searchInput = document.getElementById("search-input");
    var searchResults = document.getElementById("search-results");
    if (!searchResults) {
      return;
    }
    var noResult = searchResults.querySelector(".search-no-result");
    if (noResult && !noResult.getAttribute("data-rf-enhanced")) {
      noResult.setAttribute("data-rf-enhanced", "true");
      var query = searchInput ? searchInput.value.trim() : "";
      noResult.textContent = "";
      var msg = document.createElement("div");
      msg.className = "rf-search-empty-message";
      msg.textContent = query
        ? 'No matching documentation found for "' + query + '".'
        : "No matching documentation found.";
      noResult.appendChild(msg);

      var actions = document.createElement("div");
      actions.className = "rf-search-empty-actions";

      var clearBtn = document.createElement("button");
      clearBtn.type = "button";
      clearBtn.id = "rf-search-clear";
      clearBtn.className = "rf-inline-btn search-result";
      clearBtn.textContent = "Clear search";
      clearBtn.addEventListener("click", function (ev) {
        ev.preventDefault();
        ev.stopPropagation();
        clearSearchAndFocus();
      });
      actions.appendChild(clearBtn);

      var linksSpan = document.createElement("span");
      linksSpan.innerHTML =
        'Browse <a class="search-result" href="/RasterFold/">Overview</a>, ' +
        '<a class="search-result" href="/RasterFold/usage.html">CLI Usage</a>, ' +
        '<a class="search-result" href="/RasterFold/c-api.html">C API Guide</a>, or ' +
        '<a class="search-result" href="/RasterFold/troubleshooting.html">Troubleshooting</a>.';
      actions.appendChild(linksSpan);
      noResult.appendChild(actions);

      if (searchInput) {
        searchInput.setAttribute("aria-expanded", "true");
      }
      announce(msg.textContent);
      return;
    }

    var items = searchResults.querySelectorAll(".search-results-list-item");
    if (items.length > 0) {
      if (searchInput) {
        searchInput.setAttribute("aria-expanded", "true");
      }
      announce(items.length + " search result" + (items.length === 1 ? "" : "s") + " available");
    } else if (!noResult && searchInput) {
      searchInput.setAttribute("aria-expanded", "false");
    }
  }

  function maybeRunFallbackSearch() {
    var searchInput = document.getElementById("search-input");
    var searchResults = document.getElementById("search-results");
    if (!searchInput || !searchResults) {
      return;
    }
    if (!fallbackSearchData && window.__rfSearchIndexState && window.__rfSearchIndexState.data) {
      fallbackSearchData = window.__rfSearchIndexState.data;
    }
    if (!useFallbackSearch || !fallbackSearchData) {
      return;
    }
    renderFallbackSearchResults(searchInput.value || "");
  }

  function initSearchEnhancements() {
    var searchInput = document.getElementById("search-input");
    var searchResults = document.getElementById("search-results");
    var retryBtn = document.getElementById("rf-search-retry");

    if (retryBtn) {
      retryBtn.addEventListener("click", function () {
        retrySearchIndex();
      });
    }

    if (searchInput) {
      searchInput.setAttribute("aria-label", "Search RasterFold documentation (press / or Ctrl+K)");
      searchInput.setAttribute("aria-controls", "search-results");
      searchInput.setAttribute("aria-expanded", "false");
      searchInput.addEventListener("input", function () {
        maybeRunFallbackSearch();
      });
      searchInput.addEventListener("keyup", function (ev) {
        if (ev.key === "Escape" || ev.key === "ArrowDown" || ev.key === "ArrowUp" || ev.key === "Enter") {
          return;
        }
        maybeRunFallbackSearch();
      });
      searchInput.addEventListener(
        "keydown",
        function (ev) {
          if (!searchResults) {
            return;
          }
          if (ev.key === "ArrowDown" || ev.key === "ArrowUp") {
            var results = searchResults.querySelectorAll(".search-result");
            if (results.length > 0) {
              ev.preventDefault();
              ev.stopImmediatePropagation();
              var curIdx = -1;
              for (var r = 0; r < results.length; r++) {
                if (results[r].classList.contains("active")) {
                  curIdx = r;
                  results[r].classList.remove("active");
                }
              }
              var nextIdx = ev.key === "ArrowDown"
                ? (curIdx + 1) % results.length
                : (curIdx <= 0 ? results.length - 1 : curIdx - 1);
              results[nextIdx].classList.add("active");
            }
            return;
          }
          if (ev.key === "Enter") {
            var activeEl =
              searchResults.querySelector(".search-result.active") ||
              searchResults.querySelector(".search-result");
            if (!activeEl) {
              return;
            }
            ev.preventDefault();
            ev.stopImmediatePropagation();
            var href = activeEl.getAttribute("href");
            if (href) {
              window.location.assign(href);
            } else if (typeof activeEl.click === "function") {
              activeEl.click();
            }
          }
        },
        true
      );
    }
    if (searchResults) {
      searchResults.setAttribute("role", "region");
      searchResults.setAttribute("aria-label", "Search results");
      var observer = new MutationObserver(function () {
        enhanceSearchResults();
      });
      observer.observe(searchResults, { childList: true, subtree: true });
    }

    document.addEventListener("keydown", function (ev) {
      if (!searchInput) {
        return;
      }
      var tag = ev.target && ev.target.tagName ? ev.target.tagName.toLowerCase() : "";
      var inEditable = tag === "input" || tag === "textarea" || tag === "select" || (ev.target && ev.target.isContentEditable);

      if ((ev.ctrlKey || ev.metaKey) && (ev.key === "k" || ev.key === "K")) {
        ev.preventDefault();
        ensureSearchInputVisibleAndFocus(searchInput);
        announce("Search input focused");
        return;
      }
      if (ev.key === "/" && !inEditable && !ev.ctrlKey && !ev.metaKey && !ev.altKey) {
        ev.preventDefault();
        ensureSearchInputVisibleAndFocus(searchInput);
        announce("Search input focused");
        return;
      }
      if (ev.key === "Escape") {
        var isSearchOpen = document.documentElement.classList.contains("search-active");
        var inSearch = ev.target === searchInput || (searchResults && searchResults.contains(ev.target));
        if (isSearchOpen || inSearch) {
          ev.preventDefault();
          clearSearchAndFocus();
        }
      }
    });

    if (window.__rfSearchIndexState) {
      if (window.__rfSearchIndexState.status === "failed") {
        triggerSearchOffline();
      } else if (window.__rfSearchIndexState.status === "loaded" && window.__rfSearchIndexState.data) {
        fallbackSearchData = window.__rfSearchIndexState.data;
      }
    } else if (window.fetch) {
      loadSearchIndex(false);
    }
  }

  function initMobileNavAccessibility() {
    var siteNav = document.getElementById("site-nav");
    if (siteNav) {
      var activeLink = siteNav.querySelector("a.nav-list-link.active");
      if (activeLink) {
        if (!activeLink.getAttribute("href")) {
          activeLink.setAttribute("href", window.location.pathname);
        }
        activeLink.setAttribute("aria-current", "page");
      }
    }
    var main = document.getElementById("main-content");
    if (main && !main.hasAttribute("tabindex")) {
      main.setAttribute("tabindex", "-1");
    }
    var skipLink = document.querySelector("a.skip-to-main");
    if (skipLink) {
      skipLink.addEventListener("click", function () {
        if (main) {
          main.focus();
        }
      });
    }
    var menuButton = document.getElementById("menu-button");
    if (!menuButton || !siteNav) {
      return;
    }
    menuButton.setAttribute("aria-controls", "site-nav");
    if (!menuButton.hasAttribute("aria-expanded")) {
      menuButton.setAttribute("aria-expanded", "false");
    }
    menuButton.addEventListener("click", function () {
      window.setTimeout(function () {
        var expanded = menuButton.classList.contains("nav-open") ? "true" : "false";
        menuButton.setAttribute("aria-expanded", expanded);
        announce(expanded === "true" ? "Navigation menu expanded" : "Navigation menu collapsed");
      }, 0);
    });
  }

  function onReady(fn) {
    if (document.readyState !== "loading") {
      fn();
    } else {
      document.addEventListener("DOMContentLoaded", fn);
    }
  }

  onReady(function () {
    window.setTimeout(function () {
      initToolbarControls();
      initPageOutline();
      initCodeCopyButtons();
      initSearchEnhancements();
      initMobileNavAccessibility();
    }, 0);
  });

  window.rfDocsState = {
    getTheme: getTheme,
    setTheme: setTheme,
    getDensity: getDensity,
    setDensity: setDensity,
    clearSearchAndFocus: clearSearchAndFocus,
    triggerSearchOffline: triggerSearchOffline,
    retrySearchIndex: retrySearchIndex,
    setStorageUnavailableForTest: function (flag) {
      forceStorageUnavailable = Boolean(flag);
    },
    setClipboardForceFail: function (flag) {
      forceClipboardFail = Boolean(flag);
    }
  };
})();
