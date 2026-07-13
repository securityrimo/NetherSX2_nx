#pragma once
#include <string>

// Build and install a HOME-menu forwarder that boots the launcher straight into `gameKey`.
// name/author show on HOME; iconImgPath is any PNG/JPG (resized to the 256x256 NACP icon).
// Keyless (keys derived on-console via SPL); needs sigpatches. Writes a message to err on failure.
bool forwarder_create(const std::string &gameKey, const std::string &name, const std::string &author,
                      const std::string &iconImgPath, char *err, std::size_t errSize);
