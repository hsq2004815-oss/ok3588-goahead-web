(function () {
  "use strict";

  var originalFetch = typeof window.fetch === "function" ? window.fetch.bind(window) : null;
  var overlayStyleId = "codex-video-progress-style";
  var panelId = "codex-video-progress-panel";
  var state = {
    active: false,
    requestCompleted: false,
    requestSucceeded: false,
    filename: "",
    percent: 0,
    total: 0,
    current: 0,
    status: "idle",
    pollTimer: null,
    hideTimer: null,
    recoverTimer: null,
    failureCount: 0,
  };

  function injectStyles() {
    if (document.getElementById(overlayStyleId)) {
      return;
    }

    var style = document.createElement("style");
    style.id = overlayStyleId;
    style.textContent = [
      ".content>.processing-overlay{display:none!important;}",
      ".codex-video-progress{display:none;width:100%;margin-top:8px;padding:14px 16px;border-radius:10px;background:linear-gradient(135deg,#fff7ea 0%,#fff 100%);border:1px solid rgba(230,162,60,.25);box-shadow:0 10px 24px rgba(230,162,60,.08);box-sizing:border-box;}",
      ".codex-video-progress.is-active{display:block;}",
      ".codex-video-progress__top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:10px;}",
      ".codex-video-progress__title{font-size:15px;font-weight:700;color:#303133;}",
      ".codex-video-progress__file{font-size:12px;color:#909399;word-break:break-all;}",
      ".codex-video-progress__percent{font-size:18px;font-weight:700;color:#e6a23c;white-space:nowrap;}",
      ".codex-video-progress__bar{width:100%;height:10px;border-radius:999px;background:#f3e3c6;overflow:hidden;}",
      ".codex-video-progress__fill{height:100%;width:0;background:linear-gradient(90deg,#f0a63a 0%,#e6a23c 55%,#d48806 100%);transition:width .35s ease;}",
      ".codex-video-progress__meta{display:flex;justify-content:space-between;gap:12px;margin-top:10px;font-size:12px;color:#8c6b2d;}",
      ".codex-video-progress__hint{margin-top:10px;font-size:12px;color:#a46d12;}",
      "@media (max-width:768px){.codex-video-progress__top,.codex-video-progress__meta{flex-direction:column;align-items:flex-start}.codex-video-progress__percent{font-size:16px;}}",
    ].join("");
    document.head.appendChild(style);
  }

  function mountPanel(panel) {
    if (panel.parentNode) {
      return;
    }

    var header = document.querySelector(".content .compact-header");
    if (header) {
      header.appendChild(panel);
    }
  }

  function ensurePanel() {
    injectStyles();

    var existing = document.getElementById(panelId);
    if (existing) {
      mountPanel(existing);
      return existing;
    }

    var panel = document.createElement("div");
    panel.id = panelId;
    panel.className = "codex-video-progress";
    panel.innerHTML = [
      '<div class="codex-video-progress__top">',
      "  <div>",
      '    <div class="codex-video-progress__title">视频处理中</div>',
      '    <div class="codex-video-progress__file"></div>',
      "  </div>",
      '  <div class="codex-video-progress__percent">0%</div>',
      "</div>",
      '<div class="codex-video-progress__bar" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow="0">',
      '  <div class="codex-video-progress__fill"></div>',
      "</div>",
      '<div class="codex-video-progress__meta">',
      '  <span class="codex-video-progress__status">正在准备处理任务...</span>',
      '  <span class="codex-video-progress__count">0 / 0</span>',
      "</div>",
      '<div class="codex-video-progress__hint">处理过程不可中断，请勿刷新或关闭页面</div>',
    ].join("");

    var observer = new MutationObserver(function () {
      mountPanel(panel);
    });
    observer.observe(document.documentElement, { childList: true, subtree: true });
    mountPanel(panel);
    return panel;
  }

  function setVisible(visible) {
    var panel = ensurePanel();
    if (visible) {
      panel.classList.add("is-active");
    } else {
      panel.classList.remove("is-active");
    }
  }

  function updatePanel() {
    var panel = ensurePanel();
    var percent = Math.max(0, Math.min(100, Number(state.percent) || 0));
    var fileEl = panel.querySelector(".codex-video-progress__file");
    var percentEl = panel.querySelector(".codex-video-progress__percent");
    var fillEl = panel.querySelector(".codex-video-progress__fill");
    var barEl = panel.querySelector(".codex-video-progress__bar");
    var statusEl = panel.querySelector(".codex-video-progress__status");
    var countEl = panel.querySelector(".codex-video-progress__count");

    fileEl.textContent = state.filename ? "文件: " + state.filename : "文件: 当前视频";
    percentEl.textContent = percent.toFixed(1).replace(/\.0$/, "") + "%";
    fillEl.style.width = percent + "%";
    barEl.setAttribute("aria-valuenow", String(percent));

    if (state.status === "done" || percent >= 100) {
      statusEl.textContent = "处理完成，正在刷新结果...";
    } else if (state.status === "processing") {
      statusEl.textContent = "正在处理并绘制检测结果...";
    } else {
      statusEl.textContent = "正在准备处理任务...";
    }

    countEl.textContent = (state.current || 0) + " / " + (state.total || 0);
  }

  function stopPolling() {
    if (state.pollTimer) {
      clearInterval(state.pollTimer);
      state.pollTimer = null;
    }
  }

  function isVideoManagementPage() {
    var title = document.querySelector(".section-title");
    return !!(title && title.textContent && title.textContent.indexOf("视频列表") !== -1);
  }

  function hasStaleProcessingTag() {
    var tags = document.querySelectorAll(".el-tag");
    for (var i = 0; i < tags.length; i += 1) {
      if ((tags[i].textContent || "").indexOf("处理中") !== -1) {
        return true;
      }
    }
    return false;
  }

  function scheduleVideoPageRecovery() {
    if (state.recoverTimer) {
      clearTimeout(state.recoverTimer);
      state.recoverTimer = null;
    }

    state.recoverTimer = setTimeout(function () {
      if (!state.requestSucceeded || !isVideoManagementPage()) {
        return;
      }

      if (hasStaleProcessingTag()) {
        window.location.reload();
      }
    }, 1600);
  }

  function parseProcessSuccess(payload, fallbackStatus) {
    if (payload && typeof payload === "object" && typeof payload.resp_code === "number") {
      return payload.resp_code === 0;
    }
    return !!fallbackStatus;
  }

  function resetState(filename) {
    state.active = true;
    state.requestCompleted = false;
    state.requestSucceeded = false;
    state.filename = filename || "";
    state.percent = 0;
    state.total = 0;
    state.current = 0;
    state.status = "processing";
    state.failureCount = 0;
    if (state.hideTimer) {
      clearTimeout(state.hideTimer);
      state.hideTimer = null;
    }
    if (state.recoverTimer) {
      clearTimeout(state.recoverTimer);
      state.recoverTimer = null;
    }
    updatePanel();
    setVisible(true);
  }

  function finishProgress(forcePercent) {
    if (typeof forcePercent === "number") {
      state.percent = forcePercent;
    } else if (state.percent < 100) {
      state.percent = 100;
    }
    state.status = "done";
    updatePanel();
    state.active = false;
    stopPolling();
    if (state.hideTimer) {
      clearTimeout(state.hideTimer);
    }
    state.hideTimer = setTimeout(function () {
      setVisible(false);
    }, 1200);
  }

  function extractFilename(url, body) {
    var target = "";

    if (typeof url === "string" && url.indexOf("?") !== -1) {
      var query = url.split("?")[1];
      var pairs = query.split("&");
      for (var i = 0; i < pairs.length; i++) {
        var parts = pairs[i].split("=");
        if (parts[0] === "filename" && parts[1]) {
          try {
            return decodeURIComponent(parts[1]);
          } catch (err) {
            return parts[1];
          }
        }
      }
    }

    if (typeof body === "string") {
      try {
        target = JSON.parse(body).filename || "";
      } catch (err2) {
        target = "";
      }
    }

    return target;
  }

  function isProcessRequest(url) {
    return (
      typeof url === "string" &&
      url.indexOf("/action/processVideo") !== -1 &&
      url.indexOf("/action/processVideoFrame") === -1
    );
  }

  function isProgressRequest(url) {
    return typeof url === "string" && url.indexOf("/action/getVideoProgress") !== -1;
  }

  function pollProgress() {
    if (!originalFetch || !state.active) {
      return;
    }

    originalFetch("/action/getVideoProgress?_t=" + Date.now(), {
      method: "GET",
      cache: "no-store",
      credentials: "same-origin",
    })
      .then(function (response) {
        return response.json();
      })
      .then(function (payload) {
        var data = payload && payload.datas ? payload.datas : {};
        state.failureCount = 0;
        if (typeof data.percent === "number") {
          state.percent = data.percent;
        }
        if (typeof data.current === "number") {
          state.current = data.current;
        }
        if (typeof data.total === "number") {
          state.total = data.total;
        }
        if (typeof data.status === "string" && data.status) {
          state.status = data.status;
        }
        updatePanel();

        if (state.requestCompleted && (state.status === "done" || state.percent >= 100)) {
          finishProgress(100);
        }
      })
      .catch(function () {
        state.failureCount += 1;
        if (state.requestCompleted && state.failureCount >= 2) {
          finishProgress();
        }
      });
  }

  function startTracking(filename) {
    resetState(filename);
    stopPolling();
    pollProgress();
    state.pollTimer = setInterval(pollProgress, 1000);
  }

  function markRequestCompleted() {
    state.requestCompleted = true;
    if (state.status === "done" || state.percent >= 100) {
      finishProgress(100);
    }
    if (state.requestSucceeded) {
      scheduleVideoPageRecovery();
    }
  }

  function markRequestResult(success) {
    state.requestSucceeded = !!success;
  }

  function patchXHR() {
    if (!window.XMLHttpRequest) {
      return;
    }

    var originalOpen = XMLHttpRequest.prototype.open;
    var originalSend = XMLHttpRequest.prototype.send;

    XMLHttpRequest.prototype.open = function (method, url) {
      this.__codexUrl = url;
      return originalOpen.apply(this, arguments);
    };

    XMLHttpRequest.prototype.send = function (body) {
      var url = this.__codexUrl || "";
      if (isProcessRequest(url)) {
        var xhr = this;
        startTracking(extractFilename(url, body));
        this.addEventListener(
          "loadend",
          function () {
            var success = xhr.status >= 200 && xhr.status < 300;
            try {
              markRequestResult(parseProcessSuccess(JSON.parse(xhr.responseText || "{}"), success));
            } catch (err) {
              markRequestResult(success);
            }
            markRequestCompleted();
          },
          { once: true }
        );
      }
      return originalSend.apply(this, arguments);
    };
  }

  function patchFetch() {
    if (!originalFetch) {
      return;
    }

    window.fetch = function (resource, init) {
      var url = typeof resource === "string" ? resource : resource && resource.url;
      if (isProgressRequest(url)) {
        return originalFetch(resource, init);
      }

      if (isProcessRequest(url)) {
        startTracking(extractFilename(url, init && init.body));
        return originalFetch(resource, init).then(
          function (response) {
            return response
              .clone()
              .json()
              .then(
                function (payload) {
                  markRequestResult(parseProcessSuccess(payload, response.ok));
                  markRequestCompleted();
                  return response;
                },
                function () {
                  markRequestResult(response.ok);
                  markRequestCompleted();
                  return response;
                }
              );
          },
          function (error) {
            markRequestResult(false);
            markRequestCompleted();
            throw error;
          }
        );
      }

      return originalFetch(resource, init);
    };
  }

  injectStyles();
  ensurePanel();
  patchXHR();
  patchFetch();
})();
