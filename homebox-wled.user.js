// ==UserScript==
// @name         Homebox → HA bin locator webhook
// @match        https://homebox.url/*
// @grant        GM_xmlhttpRequest
// @connect      homeassistant.url
// @connect      homebox.url
// ==/UserScript==
//
// Watches which Homebox item/location page is open, resolves it to a
// {unit, code} pair (e.g. {"unit":"A2","code":"C-03"}), and POSTs that to
// a Home Assistant webhook. HA does the LED index math — this script's
// only job is figuring out what's on screen.
//
// Requires a Homebox API token below. Generate one from your Homebox
// profile page. Note this token is visible to anyone who can view this
// script's source (browser dev tools) — worth using a scoped/read-only
// token if Homebox supports one.

(function () {
  const HA_WEBHOOK_URL = 'https://homeassistant.url/api/webhook/homebox-highlight';
  const HOMEBOX_API_TOKEN = 'your-api-token';
  let lastPath = '';
  const cache = {};

  function notify(payload) {
    GM_xmlhttpRequest({
      method: 'POST',
      url: HA_WEBHOOK_URL,
      headers: { 'Content-Type': 'application/json' },
      data: JSON.stringify(payload || { unit: null, code: null })
    });
  }

  function fetchEntity(id, cb) {
    if (cache[id]) return cb(cache[id]);
    GM_xmlhttpRequest({
      method: 'GET',
      url: `${location.origin}/api/v1/entities/${id}`,
      headers: { Authorization: `Bearer ${HOMEBOX_API_TOKEN}` },
      onload: (res) => {
        try {
          const data = JSON.parse(res.responseText);
          cache[id] = data;
          cb(data);
        } catch (e) {}
      }
    });
  }

  function resolveUnitAndCode(locationId, cb) {
    fetchEntity(locationId, (loc) => {
      const name = (loc.name || '').toUpperCase();
      if (!/^[A-H]-\d{2}$/.test(name)) return cb(null); // not a drawer page
      if (loc.parent?.name) return cb({ unit: loc.parent.name.toUpperCase(), code: name });
      if (loc.parent?.id) {
        return fetchEntity(loc.parent.id, (parent) =>
          cb({ unit: (parent.name || '').toUpperCase(), code: name })
        );
      }
      cb(null);
    });
  }

  function resolveFromItem(itemId, cb) {
    fetchEntity(itemId, (item) => {
      const parentId = item.parent?.id;
      if (!parentId) return cb(null);
      resolveUnitAndCode(parentId, cb);
    });
  }

  function checkUrl() {
    if (location.pathname === lastPath) return;
    lastPath = location.pathname;
    const locMatch = location.pathname.match(/\/location\/([a-f0-9-]+)/i);
    const itemMatch = location.pathname.match(/\/item\/([a-f0-9-]+)/i);
    if (locMatch) resolveUnitAndCode(locMatch[1], notify);
    else if (itemMatch) resolveFromItem(itemMatch[1], notify);
    else notify(null);
  }

  setInterval(checkUrl, 500);
  window.addEventListener('beforeunload', () => notify(null));
})();
