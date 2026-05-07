const plusToSpace = (value) => String(value).replace(/\+/g, ' ');

export const unescape = (value) => {
  try {
    return decodeURIComponent(plusToSpace(value));
  } catch {
    return plusToSpace(value);
  }
};

export const escape = (value) => encodeURIComponent(String(value));

export const parse = (str, sep = '&', eq = '=', options = undefined) => {
  const out = Object.create(null);
  if (str == null || str === '') return out;
  const maxKeys = options && options.maxKeys === 0 ? Infinity : Number(options?.maxKeys ?? 1000);
  const decode = typeof options?.decodeURIComponent === 'function' ? options.decodeURIComponent : unescape;
  const parts = String(str).split(String(sep));
  const limit = Number.isFinite(maxKeys) ? Math.min(parts.length, maxKeys) : parts.length;
  for (let i = 0; i < limit; ++i) {
    const part = parts[i];
    const idx = part.indexOf(String(eq));
    const rawKey = idx < 0 ? part : part.slice(0, idx);
    const rawValue = idx < 0 ? '' : part.slice(idx + String(eq).length);
    const key = decode(rawKey);
    const value = decode(rawValue);
    if (Object.prototype.hasOwnProperty.call(out, key)) {
      const current = out[key];
      if (Array.isArray(current)) current.push(value);
      else out[key] = [current, value];
    } else {
      out[key] = value;
    }
  }
  return out;
};

export const stringify = (obj, sep = '&', eq = '=', options = undefined) => {
  if (obj == null) return '';
  const encode = typeof options?.encodeURIComponent === 'function' ? options.encodeURIComponent : escape;
  const fields = [];
  for (const key of Object.keys(obj)) {
    const value = obj[key];
    const values = Array.isArray(value) ? value : [value];
    for (const item of values) {
      const scalar = item == null ? '' : typeof item === 'boolean' || typeof item === 'number' || typeof item === 'bigint' ? String(item) : String(item);
      fields.push(`${encode(key)}${eq}${encode(scalar)}`);
    }
  }
  return fields.join(String(sep));
};

export const decode = parse;
export const encode = stringify;
export default { parse, stringify, decode, encode, escape, unescape };
