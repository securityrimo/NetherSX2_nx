/* griddb.cpp -- SteamGridDB cover fetch. See griddb.h. */
#include "griddb.h"

#include <switch.h>
#include <curl/curl.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// --- HTTP (curl, libnx TLS backend) ---------------------------------------
static const size_t MAX_BODY = 24u * 1024 * 1024; // hard ceiling (covers are ~1 MB)
struct Buf { std::string data; };
static size_t write_cb(void *p, size_t sz, size_t n, void *u) {
  Buf *b = (Buf *)u;
  size_t add = sz * n;
  if (b->data.size() + add > MAX_BODY) return 0; // exceed cap -> abort transfer
  b->data.append((char *)p, add);
  return add;
}

// GET url -> out. Returns true if the request COMPLETED (connected); the HTTP
// status is reported via *code so callers can tell 404/401 from a network fail.
static bool http_get(const std::string &url, const std::string &bearer,
                     std::string &out, long *code) {
  if (code) *code = 0;
  CURL *c = curl_easy_init();
  if (!c) return false;
  Buf b;
  struct curl_slist *hdr = nullptr;
  if (!bearer.empty()) {
    std::string h = "Authorization: Bearer " + bearer;
    hdr = curl_slist_append(hdr, h.c_str());
  }
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  if (hdr) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
  curl_easy_setopt(c, CURLOPT_MAXFILESIZE, (long)MAX_BODY);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "NetherSX2-Launcher/1.0");
  CURLcode rc = curl_easy_perform(c);
  long hc = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &hc);
  if (code) *code = hc;
  if (hdr) curl_slist_free_all(hdr);
  curl_easy_cleanup(c);
  out.swap(b.data);
  return rc == CURLE_OK;
}

// download an image (PNG/JPEG only) to `path`; rejects HTML error pages etc.
static bool http_download(const std::string &url, const std::string &path) {
  std::string data;
  long code = 0;
  if (!http_get(url, "", data, &code) || code < 200 || code >= 300 || data.size() < 64)
    return false;
  const unsigned char *d = (const unsigned char *)data.data();
  bool png = d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
  bool jpg = d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF;
  if (!png && !jpg) return false; // not an image (rate-limit JSON, redirect stub...)
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) return false;
  size_t w = fwrite(data.data(), 1, data.size(), f);
  return fclose(f) == 0 && w == data.size();
}

// --- helpers ---------------------------------------------------------------
static std::string url_encode(const std::string &s) {
  static const char *hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

// Minimal JSON field readers -- the SteamGridDB v2 responses are a fixed shape
// ({"success":true,"data":[{...}]}) and we only need data[0].id and data[0].url,
// which are the first "id":/"url":" occurrences. Not a general parser.
static bool json_first_number(const std::string &j, const char *key, long &out) {
  size_t p = j.find(key);
  if (p == std::string::npos) return false;
  p += strlen(key);
  while (p < j.size() && (j[p] == ' ' || j[p] == ':')) p++;
  char *end = nullptr;
  long v = strtol(j.c_str() + p, &end, 10);
  if (end == j.c_str() + p) return false;
  out = v;
  return true;
}
static bool json_first_string(const std::string &j, const char *key, std::string &out) {
  size_t p = j.find(key); // key includes the opening quote, e.g.  "url":"
  if (p == std::string::npos) return false;
  p += strlen(key);
  std::string s;
  while (p < j.size() && j[p] != '"') {
    if (j[p] == '\\' && p + 1 < j.size()) { // JSON escapes; SteamGridDB emits \/ in URLs
      p++;
      s += (j[p] == '/' || j[p] == '\\') ? j[p] : (j[p] == 'n' ? '\n' : j[p]);
    } else {
      s += j[p];
    }
    p++;
  }
  out.swap(s);
  return !out.empty();
}

// --- public ----------------------------------------------------------------
void griddb_global_init(void) {
  socketInitializeDefault();
  curl_global_init(CURL_GLOBAL_ALL);
}
void griddb_global_exit(void) {
  curl_global_cleanup();
  socketExit();
}

// map an HTTP status to a result code (0 = ok, else a GRIDDB_* error)
static int status_err(long code) {
  if (code == 401 || code == 403) return GRIDDB_NO_KEY;   // bad/missing API key
  if (code < 200 || code >= 300)  return GRIDDB_NOT_FOUND; // 404 etc.
  return GRIDDB_OK;
}

int griddb_fetch_cover(const std::string &key, const std::string &title, const std::string &outPath) {
  if (key.empty()) return GRIDDB_NO_KEY;
  std::string resp;
  long code = 0;

  // 1) title -> game id
  std::string search = "https://www.steamgriddb.com/api/v2/search/autocomplete/" + url_encode(title);
  if (!http_get(search, key, resp, &code)) return GRIDDB_NO_NET; // couldn't connect
  int e = status_err(code);
  if (e) return e;
  long gameId = 0;
  if (!json_first_number(resp, "\"id\":", gameId)) return GRIDDB_NOT_FOUND;

  // 2) id -> first 600x900 static grid url
  char grids[256];
  snprintf(grids, sizeof(grids),
           "https://www.steamgriddb.com/api/v2/grids/game/%ld?dimensions=600x900&types=static", gameId);
  if (!http_get(grids, key, resp, &code)) return GRIDDB_NO_NET;
  e = status_err(code);
  if (e) return e;
  std::string coverUrl;
  if (!json_first_string(resp, "\"url\":\"", coverUrl)) return GRIDDB_NOT_FOUND;

  // 3) download the cover (image-sniffed)
  return http_download(coverUrl, outPath) ? GRIDDB_OK : GRIDDB_ERROR;
}
