#include "tiny_http/mime_types.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace tiny_http {

std::string mime_type_for_path(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return "application/octet-stream";
    }

    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    static const std::unordered_map<std::string, std::string> types{
        {".html", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "application/javascript; charset=utf-8"},
        {".txt", "text/plain; charset=utf-8"},
        {".json", "application/json; charset=utf-8"},
        {".png", "image/png"},
        {".jpg", "image/jpeg"}
    };

    const std::unordered_map<std::string, std::string>::const_iterator it = types.find(ext);
    return it == types.end() ? "application/octet-stream" : it->second;
}

} // namespace tiny_http