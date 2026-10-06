// Caches the app shell so the app opens instantly and works offline.
// API calls (/api, /health) always go to the network.

const CACHE = "cbio-app-v18";

const SHELL = [
  "./",
  "index.html",
  "styles.css",
  "app.js",
  "android-bridge.js",
  "manifest.webmanifest",
  "icons/icon.svg",
  "icons/icon-192.png",
  "icons/icon-512.png",
];

self.addEventListener("install", (event) => {
  event.waitUntil(caches.open(CACHE).then((cache) => cache.addAll(SHELL)));
  self.skipWaiting();
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((key) => key !== CACHE).map((key) => caches.delete(key))))
  );
  self.clients.claim();
});

self.addEventListener("fetch", (event) => {
  const url = new URL(event.request.url);

  if (event.request.method !== "GET" ||
      url.origin !== location.origin ||
      url.pathname.startsWith("/api/") ||
      url.pathname.startsWith("/health")) {
    return;
  }

  // Network first so updates show up; fall back to the cache offline
  event.respondWith(
    fetch(event.request)
      .then((response) => {
        const copy = response.clone();
        caches.open(CACHE).then((cache) => cache.put(event.request, copy));
        return response;
      })
      .catch(() => caches.match(event.request, { ignoreSearch: true }))
  );
});
