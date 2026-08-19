const state = { snapshot: null };
const REFRESH_INTERVAL_MS = 3_000;

const $ = (selector) => document.querySelector(selector);
const escapeHtml = (value) => String(value ?? "").replace(/[&<>'"]/g, (character) => ({
  "&": "&amp;", "<": "&lt;", ">": "&gt;", "'": "&#39;", '"': "&quot;",
}[character]));

function formatBytes(bytes) {
  if (bytes == null) return "—";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / 1024 / 1024).toFixed(2)} MiB`;
}

function formatAge(seconds) {
  if (seconds < 60) return `${seconds} 秒前`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)} 分钟前`;
  if (seconds < 86400) return `${Math.floor(seconds / 3600)} 小时前`;
  return `${Math.floor(seconds / 86400)} 天前`;
}

function toast(message) {
  const element = $("#toast");
  element.textContent = message;
  element.classList.add("visible");
  window.setTimeout(() => element.classList.remove("visible"), 2600);
}

async function api(path, options = {}) {
  const response = await fetch(path, { cache: "no-store", ...options });
  if (!response.ok) {
    let message = `请求失败 (${response.status})`;
    try {
      const body = await response.json();
      message = body.detail || message;
    } catch (_) {}
    throw new Error(message);
  }
  return response.status === 204 ? null : response.json();
}

function releaseOptions(device) {
  const releases = state.snapshot.releases.filter((release) => release.model === device.model);
  const target = device.target_version ?? "";
  const options = [`<option value="" ${target === "" ? "selected" : ""}>不指定目标</option>`];
  for (const release of releases) {
    options.push(`<option value="${escapeHtml(release.version)}" ${target === release.version ? "selected" : ""}>${escapeHtml(release.version)}</option>`);
  }
  return options.join("");
}

function renderDevices() {
  const search = $("#device-search").value.trim().toLowerCase();
  const presence = $("#presence-filter").value;
  const devices = state.snapshot.devices.filter((device) => {
    const haystack = `${device.name || ""} ${device.model} ${device.device_id} ${device.wifi_ssid || ""} ${device.local_ip || ""}`.toLowerCase();
    return (!search || haystack.includes(search)) && (presence === "all" || device.presence === presence);
  });

  const container = $("#device-list");
  if (!devices.length) {
    container.innerHTML = '<div class="empty-state">没有符合条件的设备</div>';
    return;
  }
  container.innerHTML = devices.map((device) => `
    <article class="device-row" data-model="${escapeHtml(device.model)}" data-id="${escapeHtml(device.device_id)}">
      <div class="device-name">
        <span class="presence-mark ${escapeHtml(device.presence)}"></span>
        <div class="device-identity">
          <div class="device-alias-heading">
            <strong>${escapeHtml(device.name || device.device_id)}</strong>
            <button
              class="alias-icon-button edit-alias"
              type="button"
              aria-label="${device.name ? "修改设备别名" : "设置设备别名"}"
              title="${device.name ? "修改设备别名" : "设置设备别名"}"
            >
              <svg viewBox="0 0 24 24" aria-hidden="true">
                <path d="M4 20h4.2L19 9.2 14.8 5 4 15.8V20Zm2-3.4 8.8-8.8 1.4 1.4L7.4 18H6v-1.4ZM17.6 2.2a1.4 1.4 0 0 1 2 0l2.2 2.2a1.4 1.4 0 0 1 0 2L20.2 8 16 3.8l1.6-1.6Z" />
              </svg>
            </button>
          </div>
          <small>${escapeHtml(device.model)} · ${escapeHtml(device.device_id)}</small>
          <div class="alias-editor" hidden>
            <input
              class="alias-input"
              type="text"
              maxlength="80"
              value="${escapeHtml(device.name || "")}"
              placeholder="例如：客厅清洗机"
              aria-label="设备别名"
            />
            <button class="button button-primary save-alias" type="button">保存</button>
            <button class="button button-secondary cancel-alias" type="button">取消</button>
          </div>
        </div>
      </div>
      <div class="metric">
        <strong>${escapeHtml(device.current_version)}</strong>
        <small>构建 ${escapeHtml(device.current_build || "未报告")} · Base ${escapeHtml(device.base_version || "未报告")}</small>
      </div>
      <div class="metric">
        <span class="badge ${device.version_matches ? "badge-ok" : "badge-pending"}">${device.version_matches ? "版本一致" : "等待切换"}</span>
        <small>${formatAge(device.last_seen_age_seconds)}</small>
      </div>
      <div class="metric">
        <strong>${device.rssi == null ? "—" : `${device.rssi} dBm`}</strong>
        <small>${escapeHtml(device.wifi_ssid || "未报告 Wi-Fi")} · IP ${escapeHtml(device.local_ip || "未报告")} · ${formatBytes(device.free_heap)} heap · ${escapeHtml(device.status)}</small>
      </div>
      <div class="target-control">
        <select aria-label="目标版本">${releaseOptions(device)}</select>
        <button class="button button-secondary save-target" type="button">应用</button>
      </div>
    </article>
  `).join("");
}

function renderReleases() {
  const body = $("#release-list");
  if (!state.snapshot.releases.length) {
    body.innerHTML = '<tr><td colspan="7" class="empty-state">尚未发布固件</td></tr>';
    return;
  }
  body.innerHTML = state.snapshot.releases.map((release) => `
    <tr>
      <td>${escapeHtml(release.model)}</td>
      <td><strong>${escapeHtml(release.version)}</strong></td>
      <td>${escapeHtml(release.build || "—")}</td>
      <td>${formatBytes(release.file_size)}</td>
      <td><code title="${escapeHtml(release.sha256)}">${escapeHtml(release.sha256.slice(0, 12))}…</code></td>
      <td>${new Date(release.created_at).toLocaleString("zh-CN")}</td>
      <td class="notes">${escapeHtml(release.notes || "—")}</td>
    </tr>
  `).join("");
}

function render() {
  const { counts, generated_at: generatedAt } = state.snapshot;
  for (const key of ["total", "online", "delayed", "offline", "pending"]) {
    $(`#count-${key}`).textContent = counts[key];
  }
  $("#last-refresh").textContent = `更新于 ${new Date(generatedAt).toLocaleTimeString("zh-CN")}`;
  renderDevices();
  renderReleases();
}

async function refresh() {
  try {
    state.snapshot = await api("/api/manage/snapshot");
    render();
  } catch (error) {
    $("#last-refresh").textContent = error.message;
    toast(error.message);
  }
}

$("#refresh-button").addEventListener("click", refresh);
$("#device-search").addEventListener("input", () => state.snapshot && renderDevices());
$("#presence-filter").addEventListener("change", () => state.snapshot && renderDevices());

$("#device-list").addEventListener("click", async (event) => {
  const row = event.target.closest(".device-row");
  if (!row) return;

  const editAliasButton = event.target.closest(".edit-alias");
  if (editAliasButton) {
    const editor = row.querySelector(".alias-editor");
    editor.hidden = false;
    editAliasButton.hidden = true;
    const input = editor.querySelector(".alias-input");
    input.focus();
    input.select();
    return;
  }

  const cancelAliasButton = event.target.closest(".cancel-alias");
  if (cancelAliasButton) {
    row.querySelector(".alias-editor").hidden = true;
    row.querySelector(".edit-alias").hidden = false;
    return;
  }

  const saveAliasButton = event.target.closest(".save-alias");
  if (saveAliasButton) {
    const name = row.querySelector(".alias-input").value.trim() || null;
    saveAliasButton.disabled = true;
    try {
      await api(`/api/manage/devices/${row.dataset.model}/${row.dataset.id}`, {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name }),
      });
      toast(name ? `设备别名已设为 ${name}` : "设备别名已清除");
      await refresh();
    } catch (error) {
      toast(error.message);
      saveAliasButton.disabled = false;
    }
    return;
  }

  const saveTargetButton = event.target.closest(".save-target");
  if (saveTargetButton) {
    const targetVersion = row.querySelector(".target-control select").value || null;
    saveTargetButton.disabled = true;
    try {
      await api(`/api/manage/devices/${row.dataset.model}/${row.dataset.id}/target`, {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ target_version: targetVersion }),
      });
      toast(targetVersion ? `目标版本已设为 ${targetVersion}` : "已取消目标版本");
      await refresh();
    } catch (error) {
      toast(error.message);
    } finally {
      saveTargetButton.disabled = false;
    }
  }
});

$("#device-list").addEventListener("keydown", (event) => {
  if (!event.target.matches(".alias-input")) return;
  const row = event.target.closest(".device-row");
  if (event.key === "Enter") {
    event.preventDefault();
    row.querySelector(".save-alias").click();
  } else if (event.key === "Escape") {
    row.querySelector(".cancel-alias").click();
  }
});

const dialog = $("#release-dialog");
$("#open-release-button").addEventListener("click", () => dialog.showModal());
$("#close-release-button").addEventListener("click", () => dialog.close());
$("[data-close-dialog]").addEventListener("click", () => dialog.close());

$("#release-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const form = event.currentTarget;
  const submit = form.querySelector('[type="submit"]');
  const errorElement = $("#release-error");
  submit.disabled = true;
  errorElement.textContent = "";
  try {
    await api("/api/manage/releases", { method: "POST", body: new FormData(form) });
    form.reset();
    dialog.close();
    toast("固件已经发布");
    await refresh();
  } catch (error) {
    errorElement.textContent = error.message;
  } finally {
    submit.disabled = false;
  }
});

refresh();
window.setInterval(() => {
  if (!document.activeElement?.matches(".alias-input")) refresh();
}, REFRESH_INTERVAL_MS);
