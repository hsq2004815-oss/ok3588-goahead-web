(function () {
  var injected = false;

  function postForm(url, data) {
    var form = new FormData();
    Object.keys(data).forEach(function (key) {
      form.append(key, data[key]);
    });
    return fetch(url, { method: "POST", body: form, credentials: "same-origin" })
      .then(function (resp) { return resp.json(); });
  }

  function findLoginButton() {
    var buttons = Array.prototype.slice.call(document.querySelectorAll("button"));
    return buttons.find(function (button) {
      return button.textContent && button.textContent.trim() === "登录";
    });
  }

  function inputValue(type, index) {
    var inputs = Array.prototype.slice.call(document.querySelectorAll("input"));
    var filtered = inputs.filter(function (input) {
      return !type || input.type === type || input.getAttribute("type") === type;
    });
    return filtered[index] ? filtered[index].value.trim() : "";
  }

  function showRegisterDialog() {
    if (document.getElementById("registerPatchMask")) {
      return;
    }
    var mask = document.createElement("div");
    mask.id = "registerPatchMask";
    mask.innerHTML =
      '<div class="register-patch-dialog">' +
      '<h3>注册账号</h3>' +
      '<input id="registerPatchUser" placeholder="用户名，3-32位" autocomplete="username">' +
      '<input id="registerPatchPass" placeholder="密码，6-64位" type="password" autocomplete="new-password">' +
      '<input id="registerPatchPass2" placeholder="确认密码" type="password" autocomplete="new-password">' +
      '<div class="register-patch-error" id="registerPatchError"></div>' +
      '<div class="register-patch-actions">' +
      '<button type="button" id="registerPatchCancel">取消</button>' +
      '<button type="button" id="registerPatchSubmit">注册</button>' +
      '</div>' +
      '</div>';
    document.body.appendChild(mask);

    var style = document.getElementById("registerPatchStyle");
    if (!style) {
      style = document.createElement("style");
      style.id = "registerPatchStyle";
      style.textContent =
        "#registerPatchMask{position:fixed;inset:0;background:rgba(0,0,0,.42);z-index:9999;display:flex;align-items:center;justify-content:center}" +
        ".register-patch-dialog{width:360px;max-width:calc(100vw - 32px);background:#fff;border-radius:6px;padding:24px;box-shadow:0 12px 32px rgba(0,0,0,.2);box-sizing:border-box}" +
        ".register-patch-dialog h3{margin:0 0 18px;font-size:18px;font-weight:600;color:#303133}" +
        ".register-patch-dialog input{display:block;width:100%;height:40px;margin:0 0 12px;padding:0 12px;border:1px solid #dcdfe6;border-radius:4px;box-sizing:border-box;font-size:14px}" +
        ".register-patch-error{min-height:20px;color:#f56c6c;font-size:13px;margin-bottom:8px}" +
        ".register-patch-actions{display:flex;justify-content:flex-end;gap:10px}" +
        ".register-patch-actions button{height:36px;padding:0 18px;border-radius:4px;border:1px solid #dcdfe6;background:#fff;cursor:pointer}" +
        ".register-patch-actions #registerPatchSubmit{border-color:#409eff;background:#409eff;color:#fff}";
      document.head.appendChild(style);
    }

    document.getElementById("registerPatchUser").value = inputValue("text", 0);
    document.getElementById("registerPatchCancel").onclick = function () {
      mask.remove();
    };
    document.getElementById("registerPatchSubmit").onclick = function () {
      var error = document.getElementById("registerPatchError");
      var username = document.getElementById("registerPatchUser").value.trim();
      var password = document.getElementById("registerPatchPass").value;
      var password2 = document.getElementById("registerPatchPass2").value;
      if (!username || !password) {
        error.textContent = "请输入用户名和密码";
        return;
      }
      if (password !== password2) {
        error.textContent = "两次输入密码不一致";
        return;
      }
      postForm("/action/register", { username: username, password: password })
        .then(function (data) {
          if (data && data.resp_code === 0) {
            alert("注册成功，请登录");
            mask.remove();
          } else {
            error.textContent = data && data.resp_msg ? data.resp_msg : "注册失败";
          }
        })
        .catch(function () {
          error.textContent = "网络错误";
        });
    };
  }

  function injectRegisterButton() {
    if (location.hash.indexOf("/login") < 0) {
      return;
    }
    var loginButton = findLoginButton();
    if (!loginButton || document.getElementById("registerPatchButton")) {
      if (!document.getElementById("registerPatchButton") && location.hash.indexOf("/login") >= 0) {
        var fallback = document.createElement("button");
        fallback.id = "registerPatchButton";
        fallback.type = "button";
        fallback.textContent = "注册";
        fallback.style.position = "fixed";
        fallback.style.right = "32px";
        fallback.style.bottom = "32px";
        fallback.style.zIndex = "9998";
        fallback.style.height = "40px";
        fallback.style.padding = "0 26px";
        fallback.style.border = "1px solid #409eff";
        fallback.style.borderRadius = "4px";
        fallback.style.background = "#409eff";
        fallback.style.color = "#fff";
        fallback.style.fontSize = "14px";
        fallback.style.cursor = "pointer";
        fallback.onclick = showRegisterDialog;
        document.body.appendChild(fallback);
        injected = true;
      }
      return;
    }
    var button = document.createElement("button");
    button.id = "registerPatchButton";
    button.type = "button";
    button.textContent = "注册";
    button.className = loginButton.className;
    button.style.marginLeft = "12px";
    button.onclick = showRegisterDialog;
    loginButton.insertAdjacentElement("afterend", button);
    injected = true;
  }

  function tick() {
    var button = document.getElementById("registerPatchButton");
    var mask = document.getElementById("registerPatchMask");
    if (location.hash.indexOf("/login") < 0) {
      if (button) {
        button.remove();
      }
      if (mask) {
        mask.remove();
      }
      injected = false;
      return;
    }
    injectRegisterButton();
  }

  setInterval(tick, 500);
  window.addEventListener("hashchange", tick);
  document.addEventListener("DOMContentLoaded", tick);
})();
