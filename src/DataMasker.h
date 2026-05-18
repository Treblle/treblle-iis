#pragma once
#include "precomp.h"

// Mask values for keys matching any entry in 'keywords' (case-insensitive).
// String values: each character replaced with '*' (same count).
// Non-string values: replaced with same-length '*' string (wrapped in quotes).
// Operates on any nesting depth. Returns the original unchanged if
// json.size() > 500 KB or keywords is empty.
std::string MaskJson(const std::string& json,
                     const std::vector<std::string>& keywords);

// Mask values in a headers map for matching keys (case-insensitive).
// Each character of the matching header value is replaced with '*'.
void MaskHeaders(std::map<std::string, std::string>& headers,
                 const std::vector<std::string>& keywords);
