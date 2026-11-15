export const INVITE_PREFIX = 'acecode://connect/';
export const MAX_INVITE_LENGTH = 1024;

function unescape(text) {
  return text.replace(/%([0-9a-f]{2})/gi, (_, hex) => String.fromCharCode(parseInt(hex, 16)));
}

export function extractInviteLink(text) {
  const match = unescape(String(text || '').slice(0, 8192)).match(/acecode:\/\/connect\/[A-Za-z0-9_-]*/);
  return match?.[0] || '';
}

export function validInviteAddress(address) {
  if (typeof address !== 'string' || !address || address.length > 253) return false;
  if (address.includes(':')) {
    const parts = address.split('%');
    if (parts.length > 2 || (parts.length === 2 && !/^[A-Za-z0-9_-]+$/.test(parts[1]))) return false;
    try { return new URL(`http://[${parts[0]}]/`).hostname.startsWith('['); } catch { return false; }
  }
  if (/^[0-9.]+$/.test(address)) {
    const octets = address.split('.');
    return octets.length === 4 && octets.every((part) => /^(0|[1-9][0-9]{0,2})$/.test(part) && Number(part) <= 255);
  }
  return address.split('.').every((part) => part.length <= 63 && /^[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?$/.test(part));
}

function checksum(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return (~crc) >>> 0;
}

export function decodeInviteLink(text, now = Math.floor(Date.now() / 1000)) {
  const link = extractInviteLink(text);
  if (!link) return { error: 'not_invite' };
  if (link.length > MAX_INVITE_LENGTH) return { error: 'too_long' };
  const encoded = link.slice(INVITE_PREFIX.length);
  let bytes;
  try {
    const raw = atob(encoded.replace(/-/g, '+').replace(/_/g, '/'));
    if (btoa(raw).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '') !== encoded) throw new Error();
    bytes = Uint8Array.from(raw, (c) => c.charCodeAt(0));
  } catch { return { error: 'checksum' }; }
  if (bytes.length < 5) return { error: 'checksum' };
  const body = bytes.subarray(0, -4);
  const expected = new DataView(bytes.buffer).getUint32(bytes.length - 4, false);
  if (checksum(body) !== expected) return { error: 'checksum' };
  let payload;
  try { payload = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(body)); } catch { return { error: 'invalid_fields' }; }
  const p = payload;
  if (!p || !Number.isInteger(p.v) || typeof p.n !== 'string' || typeof p.t !== 'string' ||
      !Array.isArray(p.a) || !Number.isInteger(p.p) || typeof p.c !== 'string' || !Number.isSafeInteger(p.e)) {
    return { error: 'invalid_fields' };
  }
  if (p.v !== 1) return { error: 'unsupported_version' };
  const utf8Length = (value) => new TextEncoder().encode(value).length;
  if (!p.n || utf8Length(p.n) > 256 || utf8Length(p.t) > 512 ||
      !p.a.length || p.a.length > 32 || p.p < 1 || p.p > 65535 ||
      !/^[0-9a-f]{16}\.[A-Za-z0-9_-]{32}$/.test(p.c) || p.e <= 0) return { error: 'invalid_fields' };
  if (p.a.some((address) => typeof address !== 'string')) return { error: 'invalid_fields' };
  if (!p.a.every(validInviteAddress)) return { error: 'invalid_address' };
  if (p.e <= now) return { error: 'expired' };
  return { value: { owner: p.n, title: p.t, addresses: p.a, port: p.p, credential: p.c, expires_at: p.e }, link };
}

export function inviteErrorMessage(error) {
  return {
    not_invite: '未找到邀请链接',
    too_long: '邀请链接过长',
    checksum: '邀请链接不完整，请重新复制',
    invalid_fields: '邀请链接格式无效',
    invalid_address: '邀请地址无效',
    unsupported_version: '邀请版本不兼容，请升级 ACECode',
    expired: '邀请链接已过期，请对方重新生成',
  }[error] || '连接失败，请确认双方在同一网络或请对方重新生成链接';
}
