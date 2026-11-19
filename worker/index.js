// Serves the static flasher site from the project's R2 bucket "site" (binding R2_SITE).
const TYPES = {
  html: 'text/html; charset=utf-8',
  js: 'text/javascript; charset=utf-8',
  json: 'application/json',
  css: 'text/css',
  svg: 'image/svg+xml',
  png: 'image/png',
  bin: 'application/octet-stream',
};

export default {
  async fetch(request, env) {
    if (request.method !== 'GET' && request.method !== 'HEAD') {
      return new Response('Method Not Allowed', { status: 405, headers: { Allow: 'GET, HEAD' } });
    }
    let key = decodeURIComponent(new URL(request.url).pathname).replace(/^\/+/, '');
    if (key === '' || key.endsWith('/')) key += 'index.html';

    const obj = await env.R2_SITE.get(key);
    if (!obj) return new Response('Not Found', { status: 404 });

    const ext = key.split('.').pop();
    const headers = new Headers();
    headers.set('Content-Type', TYPES[ext] || obj.httpMetadata?.contentType || 'application/octet-stream');
    headers.set('ETag', obj.httpEtag);
    // esp-web-tools chunks carry a content hash; everything else (pages, catalog, images) must stay fresh.
    headers.set('Cache-Control', key.startsWith('esp-web-tools/') ? 'public, max-age=31536000, immutable' : 'no-cache');
    return new Response(request.method === 'HEAD' ? null : obj.body, { headers });
  },
};
