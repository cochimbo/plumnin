#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plumnin {

// Downloads `url` (http or https) into `body`. Returns the HTTP status (200, 404...)
// or 0 if the server could not be reached. Blocks: call it from a worker thread.
// Windows uses WinINet, so it follows the system's proxy settings; elsewhere it runs
// curl. `url` must already be percent-encoded.
int httpGet(const std::string& url, std::vector<uint8_t>& body);

// The same with a User-Agent, and as a POST of `post` (form data, percent-encoded) when
// it is not empty. Unlike httpGet, `body` keeps what the server answered with any status.
int httpRequest(const std::string& url, const std::string& post, const std::string& userAgent,
                std::vector<uint8_t>& body);

}  // namespace plumnin
