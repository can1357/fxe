
const native = globalThis.__fxe_native?.dns;

const defer = (callback) => {
  if (typeof queueMicrotask === 'function') {
    queueMicrotask(callback);
  } else {
    setTimeout(callback, 0);
  }
};

const normalizeOptions = (options) => {
  if (options === undefined || options === null || typeof options === 'function') {
    return {};
  }
  if (typeof options === 'number') {
    return { family: options };
  }
  if (typeof options !== 'object') {
    throw new TypeError('node:dns.lookup options must be an object, number, or callback');
  }
  return {
    family: options.family === undefined ? 0 : Number(options.family) || 0,
    all: Boolean(options.all),
    verbatim: options.verbatim === undefined ? true : Boolean(options.verbatim),
  };
};

const requireLookup = () => {
  if (!native || typeof native.lookup !== 'function') {
    throw new Error('host-backed node:dns function unavailable: lookup');
  }
  return native.lookup.bind(native);
};

const requireLookupService = () => {
  if (!native || typeof native.lookupService !== 'function') {
    throw new Error('host-backed node:dns function unavailable: lookupService');
  }
  return native.lookupService.bind(native);
};

const requireResolveRecord = () => {
  if (!native || typeof native.resolveRecord !== 'function') {
    throw new Error('host-backed node:dns function unavailable: resolveRecord');
  }
  return native.resolveRecord.bind(native);
};

const makeDnsError = (message, code = 'ENODATA') => {
  const error = new Error(message);
  error.code = code;
  return error;
};

const noDataRecord = (callback) => {
  defer(() => callback(makeDnsError('DNS record type has no data', 'ENODATA')));
};


export const lookup = (hostname, options, callback) => {
  const cb = typeof options === 'function' ? options : callback;
  if (typeof cb !== 'function') {
    throw new TypeError('node:dns.lookup requires a callback');
  }
  const opts = normalizeOptions(options);
  const lookupNative = requireLookup();
  defer(() => {
    lookupNative(String(hostname), opts, (error, result) => {
      if (error) {
        cb(error);
        return;
      }
      if (opts.all) {
        cb(null, Array.isArray(result) ? result : (result === undefined || result === null ? [] : [result]));
        return;
      }
      const first = Array.isArray(result) ? result[0] : result;
      cb(null, first?.address, first?.family);
    });
  });
};

const resolveFamily = (hostname, family, callback) => {
  if (typeof callback !== 'function') {
    throw new TypeError('node:dns resolve requires a callback');
  }
  lookup(hostname, { family, all: true }, (error, addresses) => {
    if (error) {
      callback(error);
      return;
    }
    callback(null, addresses.map((entry) => entry.address));
  });
};


export const resolve = (hostname, rrtype, callback) => {
  if (typeof rrtype === 'function') {
    callback = rrtype;
    rrtype = 'A';
  }
  if (typeof callback !== 'function') {
    throw new TypeError('node:dns resolve requires a callback');
  }
  const recordType = String(rrtype ?? 'A').toUpperCase();
  if (recordType === 'A') {
    resolve4(hostname, callback);
    return;
  }
  if (recordType === 'AAAA') {
    resolve6(hostname, callback);
    return;
  }
  if (recordType === 'ANY') {
    lookup(hostname, { all: true }, (error, addresses) => {
      if (error) {
        callback(error);
      } else {
        callback(null, addresses.map((entry) => ({
          address: entry.address,
          family: entry.family,
          type: entry.family === 6 ? 'AAAA' : 'A',
        })));
      }
    });
    return;
  }
  const table = {
    TXT: resolveTxt,
    MX: resolveMx,
    SRV: resolveSrv,
    CNAME: resolveCname,
    NS: resolveNs,
    PTR: reverse,
    CAA: resolveCaa,
    SOA: resolveSoa,
    NAPTR: resolveNaptr,
  };
  if (table[recordType]) {
    table[recordType](hostname, callback);
    return;
  }
  noDataRecord(callback);
};


const resolveRecord = (hostname, rrtype, callback) => {
  if (typeof callback !== 'function') throw new TypeError(`node:dns resolve${rrtype} requires a callback`);
  defer(() => {
    requireResolveRecord()(String(hostname), rrtype, (error, records) => {
      if (error) callback(error);
      else callback(null, records);
    });
  });
};

export const resolve4 = (hostname, callback) => resolveFamily(hostname, 4, callback);
export const resolve6 = (hostname, callback) => resolveFamily(hostname, 6, callback);
export const resolveAny = (hostname, callback) => resolve(hostname, 'ANY', callback);
export const resolveTxt = (hostname, callback) => resolveRecord(hostname, 'TXT', callback);
export const resolveMx = (hostname, callback) => resolveRecord(hostname, 'MX', callback);
export const resolveSrv = (hostname, callback) => resolveRecord(hostname, 'SRV', callback);
export const resolveCname = (hostname, callback) => resolveRecord(hostname, 'CNAME', callback);
export const resolveNs = (hostname, callback) => resolveRecord(hostname, 'NS', callback);
export const resolveCaa = (hostname, callback) => resolveRecord(hostname, 'CAA', callback);
export const resolveSoa = (hostname, callback) => resolveRecord(hostname, 'SOA', callback);
export const resolveNaptr = (hostname, callback) => resolveRecord(hostname, 'NAPTR', callback);
export const reverse = (ip, callback) => {
  if (typeof callback !== 'function') throw new TypeError('node:dns reverse requires a callback');
  lookupService(ip, 0, (error, hostname) => {
    if (error) callback(error);
    else callback(null, [hostname]);
  });
};
export const lookupService = (address, port, callback) => {
  if (typeof callback !== 'function') throw new TypeError('node:dns lookupService requires a callback');
  defer(() => {
    requireLookupService()(String(address), Number(port), (error, result) => {
      if (error) {
        callback(error);
        return;
      }
      callback(null, result?.hostname ?? String(address), result?.service ?? String(port));
    });
  });
};
export const getDefaultResultOrder = () => 'verbatim';
export const getServers = () => [];
export const setDefaultResultOrder = () => {};
export const setServers = () => {};

export default {
  lookup,
  resolve4,
  resolve6,
  resolve,
  resolveAny,
  resolveTxt,
  resolveMx,
  resolveSrv,
  resolveCname,
  resolveNs,
  resolveCaa,
  resolveSoa,
  resolveNaptr,
  reverse,
  lookupService,
  getDefaultResultOrder,
  getServers,
  setDefaultResultOrder,
  setServers,
};
