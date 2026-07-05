// ==UserScript==
// @name         Homebox → HA bin locator webhook
// @namespace    https://github.com/
// @version      1.0
// @description  Posts the current Homebox item/location id to a Home Assistant webhook so it can light up the matching physical drawer LED.
// @match        https://homebox.url/*
// @grant        GM_xmlhttpRequest
// @connect      homeassistant.url
// ==/UserScript==
//
// Watches which Homebox item/location page is open and POSTs the raw
// {type, id} pulled straight from the URL to a Home Assistant webhook.
// This script never calls the Homebox API and holds no credentials — HA
// resolves the id to a unit/drawer code and does the LED index math
// server-side, using a token that lives only in HA's secrets.yaml.

(function () {
  const HA_WEBHOOK_URL = 'https://homeassistant.url/api/webhook/homebox-highlight';
  let lastPath = '';

  function notify(payload) {
    GM_xmlhttpRequest({
      method: 'POST',
      url: HA_WEBHOOK_URL,
      headers: { 'Content-Type': 'application/json' },
      data: JSON.stringify(payload || { type: null, id: null }),
      onerror: (err) => console.error('[homebox-wled] webhook request failed', err)
    });
  }

  function checkUrl() {
    if (location.pathname === lastPath) return;
    lastPath = location.pathname;
    const locMatch = location.pathname.match(/\/location\/([a-f0-9-]+)/i);
    const itemMatch = location.pathname.match(/\/item\/([a-f0-9-]+)/i);
    if (locMatch) notify({ type: 'location', id: locMatch[1] });
    else if (itemMatch) notify({ type: 'item', id: itemMatch[1] });
    else notify(null);
  }

  setInterval(checkUrl, 500);
  window.addEventListener('beforeunload', () => notify(null));
})();
