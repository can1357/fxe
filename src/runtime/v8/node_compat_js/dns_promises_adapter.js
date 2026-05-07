const native = globalThis.__fxe_native?.dns;

const requireLookup = () => {
  if (!native || typeof native.lookup !== 'function') {
    throw new Error('host-backed node:dns/promises function unavailable: lookup');
  }
  return native.lookup.bind(native);
};

const requireResolveRecord = () => {
  if (!native || typeof native.resolveRecord !== 'function') {
    throw new Error('host-backed node:dns/promises function unavailable: resolveRecord');
  }
  return native.resolveRecord.bind(native);
};


const normalizeOptions = (options) => {
  if (options === undefined || options === null) {
    return {};
  }
  if (typeof options === 'number') {
    return { family: options };
  }
  if (typeof options !== 'object') {
    throw new TypeError('node:dns/promises.lookup options must be an object or number');
  }
  return {
    family: options.family === undefined ? 0 : Number(options.family) || 0,
    all: Boolean(options.all),
    verbatim: options.verbatim === undefined ? true : Boolean(options.verbatim),
  };
};

const makeDnsError = (message, code = 'ENODATA') => {
  const error = new Error(message);
  error.code = code;
  return error;
};

export const lookup = (hostname, options) => new Promise((resolve, reject) => {
  const opts = normalizeOptions(options);
  requireLookup()(String(hostname), opts, (error, result) => {
    if (error) {
      reject(error);
      return;
    }
    if (opts.all) {
      resolve(Array.isArray(result) ? result : (result === undefined || result === null ? [] : [result]));
      return;
    }
    const first = Array.isArray(result) ? result[0] : result;
    resolve({ address: first?.address, family: first?.family });
  });
});

const resolveFamily = async (hostname, family) => {
  const addresses = await lookup(hostname, { family, all: true });
  return addresses.map((entry) => entry.address);
};

export const resolve4 = (hostname) => resolveFamily(hostname, 4);
export const resolve6 = (hostname) => resolveFamily(hostname, 6);

export const resolve = (hostname, rrtype = 'A') => {
  const recordType = String(rrtype ?? 'A').toUpperCase();
  if (recordType === 'A') {
    return resolve4(hostname);
  }
  if (recordType === 'AAAA') {
    return resolve6(hostname);
  }
  if (recordType === 'ANY') {
    return resolveAny(hostname);
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
  return table[recordType] ? table[recordType](hostname) : Promise.reject(makeDnsError('DNS record type has no data', 'ENODATA'));
};

export const resolveAny = async (hostname) => {
  const addresses = await lookup(hostname, { all: true });
  return addresses.map((entry) => ({
    address: entry.address,
    family: entry.family,
    type: entry.family === 6 ? 'AAAA' : 'A',
  }));
};

const resolveRecord = (hostname, rrtype) => new Promise((resolve, reject) => {
  requireResolveRecord()(String(hostname), rrtype, (error, records) => {
    if (error) reject(error);
    else resolve(records);
  });
});

export const resolveTxt = (hostname) => resolveRecord(hostname, 'TXT');
export const resolveMx = (hostname) => resolveRecord(hostname, 'MX');
export const resolveSrv = (hostname) => resolveRecord(hostname, 'SRV');
export const resolveCname = (hostname) => resolveRecord(hostname, 'CNAME');
export const resolveNs = (hostname) => resolveRecord(hostname, 'NS');
export const resolveCaa = (hostname) => resolveRecord(hostname, 'CAA');
export const resolveSoa = (hostname) => resolveRecord(hostname, 'SOA');
export const resolveNaptr = (hostname) => resolveRecord(hostname, 'NAPTR');
export const reverse = (ip) => new Promise((resolve, reject) => {
  lookupService(ip, 0).then(
    ({ hostname }) => resolve([hostname]),
    reject,
  );
});

export const lookupService = (address, port) => new Promise((resolve, reject) => {
  if (!native || typeof native.lookupService !== 'function') {
    reject(new Error('host-backed node:dns/promises function unavailable: lookupService'));
    return;
  }
  native.lookupService(String(address), Number(port), (error, result) => {
    if (error) reject(error);
    else resolve({ hostname: result?.hostname ?? String(address), service: result?.service ?? String(port) });
  });
});

export const getDefaultResultOrder = () => 'verbatim';
export const getServers = () => [];
export const setDefaultResultOrder = () => {};
export const setServers = () => {};

export default {
  lookup,
  resolve,
  resolve4,
  resolve6,
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
