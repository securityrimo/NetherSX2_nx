/* griddb.h -- SteamGridDB 600x900 cover downloader for the NetherSX2 launcher.
 * HTTPS via devkitPro switch-curl (libnx TLS backend -- uses the console's own
 * CA store, no cacert bundle). The user supplies their own free API key.
 */
#pragma once
#include <string>

// Result codes for griddb_fetch_cover.
enum {
  GRIDDB_OK = 0,
  GRIDDB_NO_KEY,     // no API key file
  GRIDDB_NO_NET,     // network/DNS/connect failure
  GRIDDB_NOT_FOUND,  // no matching game or no 600x900 grid
  GRIDDB_ERROR,      // other (write failed, bad response)
};

// Search SteamGridDB for `title`, download its first 600x900 static grid to
// `outPath` (PNG). Blocking -- run with a "downloading" indicator. curl/socket
// must already be initialized (griddb_global_init).
int griddb_fetch_cover(const std::string &apiKey, const std::string &title,
                       const std::string &outPath);

// Download up to maxCount PNG icons for `title` into outDir (gicon_<i>.png). Returns the count.
int griddb_fetch_icons(const std::string &key, const std::string &title,
                       const std::string &outDir, int maxCount);

void griddb_global_init(void);  // socketInitializeDefault + curl_global_init
void griddb_global_exit(void);
