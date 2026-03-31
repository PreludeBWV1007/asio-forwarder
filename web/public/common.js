function qs(sel) { return document.querySelector(sel); }
function qsa(sel) { return Array.from(document.querySelectorAll(sel)); }

function setActiveNav() {
  const p = location.pathname.replace(/\/+$/, '') || '/';
  for (const a of qsa('.nav a')) {
    const href = a.getAttribute('href');
    a.classList.toggle('active', href === p || (p === '/' && href === '/index.html'));
  }
}

function fmtBytes(n) {
  if (!Number.isFinite(n)) return '-';
  const u = ['B', 'KB', 'MB', 'GB'];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return `${n.toFixed(i === 0 ? 0 : 2)} ${u[i]}`;
}

function fmtRate(nPerSec) {
  if (!Number.isFinite(nPerSec)) return '-';
  if (nPerSec >= 1000) return `${(nPerSec/1000).toFixed(2)}k/s`;
  return `${nPerSec.toFixed(2)}/s`;
}

function b64ToBytes(b64) {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return bytes;
}

function bytesToHex(bytes, maxLen) {
  const len = Math.min(bytes.length, maxLen || bytes.length);
  let s = '';
  for (let i = 0; i < len; i++) s += bytes[i].toString(16).padStart(2, '0');
  return s + (bytes.length > len ? '…' : '');
}

function bytesToUtf8(bytes, maxLen) {
  const len = Math.min(bytes.length, maxLen || bytes.length);
  try {
    const dec = new TextDecoder('utf-8', { fatal: false });
    const text = dec.decode(bytes.slice(0, len));
    return text + (bytes.length > len ? '…' : '');
  } catch (e) {
    return '(decode failed)';
  }
}

window.__common = { qs, qsa, setActiveNav, fmtBytes, fmtRate, b64ToBytes, bytesToHex, bytesToUtf8 };

