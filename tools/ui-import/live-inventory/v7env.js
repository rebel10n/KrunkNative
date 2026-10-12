// Minimal browser globals so v7.1.4 data modules can be require()d in node (read-only).
const stub = () => new Proxy(function () {}, { get: (t, k) => k === Symbol.toPrimitive ? () => '' : (k === 'length' ? 0 : stub()), apply: () => stub(), construct: () => stub() });
global.window = global;
global.location = { hostname: 'localhost', href: 'http://localhost/', search: '', pathname: '/', host: 'localhost', protocol: 'http:' };
global.document = stub(); global.navigator = { userAgent: 'node', platform: 'Linux' };
global.localStorage = { getItem: () => null, setItem: () => {}, removeItem: () => {} };
global.Image = function () {}; global.Audio = function () {}; global.self = global;
global.requestAnimationFrame = () => 0; global.addEventListener = () => {};
