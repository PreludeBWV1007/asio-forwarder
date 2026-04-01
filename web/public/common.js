/*
本文件结构（前端公共工具）：
- DOM 工具：qs/qsa/setActiveNav（导航高亮）
- 数值格式化：fmtBytes/fmtRate（监控大屏用）
- 二进制展示：
  - b64ToBytes：把 body 的 base64 还原为 Uint8Array
  - bytesToHex：以十六进制预览 bytes（可截断）
  - bytesToUtf8Safe：UTF-8 安全解码（按字节截断时回退到合法边界，避免中文乱码）
说明：body 可能是任意二进制；utf8 预览仅用于“尽力展示”，需要精确内容可切换 hex。
*/

(() => {
  if (window.__common) return; // 幂等：避免重复加载导致重复声明/执行

  const qs = (sel) => document.querySelector(sel);
  const qsa = (sel) => Array.from(document.querySelectorAll(sel));

  const setActiveNav = () => {
    const p = location.pathname.replace(/\/+$/, '') || '/';
    for (const a of qsa('.nav a')) {
      const href = a.getAttribute('href');
      a.classList.toggle('active', href === p || (p === '/' && href === '/index.html'));
    }
  };

  const fmtBytes = (n) => {
    if (!Number.isFinite(n)) return '-';
    const u = ['B', 'KB', 'MB', 'GB'];
    let i = 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return `${n.toFixed(i === 0 ? 0 : 2)} ${u[i]}`;
  };

  const fmtRate = (nPerSec) => {
    if (!Number.isFinite(nPerSec)) return '-';
    if (nPerSec >= 1000) return `${(nPerSec / 1000).toFixed(2)}k/s`;
    return `${nPerSec.toFixed(2)}/s`;
  };

  const b64ToBytes = (b64) => {
    const bin = atob(b64);
    const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    return bytes;
  };

  const bytesToHex = (bytes, maxLen) => {
    const len = Math.min(bytes.length, maxLen || bytes.length);
    let s = '';
    for (let i = 0; i < len; i++) s += bytes[i].toString(16).padStart(2, '0');
    return s + (bytes.length > len ? '…' : '');
  };

  const bytesToUtf8 = (bytes, maxLen) => {
    const len = Math.min(bytes.length, maxLen || bytes.length);
    try {
      const dec = new TextDecoder('utf-8', { fatal: false });
      const text = dec.decode(bytes.slice(0, len));
      return text + (bytes.length > len ? '…' : '');
    } catch (e) {
      return '(decode failed)';
    }
  };

  // UTF-8 按“字节”截断时的安全解码。
  //
  // 说明：UTF-8 是变长编码，直接按固定字节数 slice 可能把一个字符切成半截，
  // 导致出现替换字符（�）或“乱码”。这里会把末尾回退到合法的 UTF-8 边界再解码。
  const bytesToUtf8Safe = (bytes, maxBytes) => {
    const cap = maxBytes == null ? bytes.length : Math.min(bytes.length, Math.max(0, maxBytes));
    let end = cap;
    const decStrict = new TextDecoder('utf-8', { fatal: true });
    while (end > 0) {
      try {
        const text = decStrict.decode(bytes.slice(0, end));
        return text + (bytes.length > end ? '…' : '');
      } catch (e) {
        // 逐步回退，直到落在合法边界。
        end -= 1;
      }
    }
    return bytes.length ? '(invalid utf-8)' : '';
  };

  window.__common = { qs, qsa, setActiveNav, fmtBytes, fmtRate, b64ToBytes, bytesToHex, bytesToUtf8, bytesToUtf8Safe };
})();

