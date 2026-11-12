#include "http.h"

#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#endif

namespace plumnin {

#ifdef _WIN32

namespace {

HINTERNET openInternet(const std::string& userAgent) {
    const std::wstring agent(userAgent.begin(), userAgent.end());
    HINTERNET net = InternetOpenW(agent.c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!net) return nullptr;
    DWORD timeout = 15000;
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof timeout);
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof timeout);
    return net;
}

// The status of the answer to `request`, and its body: only a 200's with `onlyOk`.
int readAnswer(HINTERNET request, std::vector<uint8_t>& body, bool onlyOk) {
    DWORD code = 0, size = sizeof code;
    int status = 0;
    if (HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &size, nullptr))
        status = static_cast<int>(code);
    char buffer[16384];
    DWORD read = 0;
    while ((status == 200 || (!onlyOk && status > 0)) && InternetReadFile(request, buffer, sizeof buffer, &read) &&
           read > 0)
        body.insert(body.end(), buffer, buffer + read);
    return status;
}

int get(const std::string& url, const std::string& userAgent, std::vector<uint8_t>& body, bool onlyOk) {
    body.clear();
    HINTERNET net = openInternet(userAgent);
    if (!net) return 0;
    const std::wstring wide(url.begin(), url.end());  // percent-encoded, so plain ASCII
    HINTERNET request = InternetOpenUrlW(net, wide.c_str(), nullptr, 0,
                                         INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_RELOAD, 0);
    int status = 0;
    if (request) {
        status = readAnswer(request, body, onlyOk);
        InternetCloseHandle(request);
    }
    InternetCloseHandle(net);
    return status;
}

int post(const std::string& url, const std::string& data, const std::string& userAgent, std::vector<uint8_t>& body) {
    body.clear();
    const std::wstring wide(url.begin(), url.end());
    wchar_t host[256] = {}, path[2048] = {};
    URL_COMPONENTSW parts{};
    parts.dwStructSize = sizeof parts;
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!InternetCrackUrlW(wide.c_str(), 0, 0, &parts)) return 0;
    HINTERNET net = openInternet(userAgent);
    if (!net) return 0;
    int status = 0;
    if (HINTERNET connection = InternetConnectW(net, host, parts.nPort, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0)) {
        DWORD flags = INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES | INTERNET_FLAG_RELOAD;
        if (parts.nScheme == INTERNET_SCHEME_HTTPS) flags |= INTERNET_FLAG_SECURE;
        if (HINTERNET request = HttpOpenRequestW(connection, L"POST", path, nullptr, nullptr, nullptr, flags, 0)) {
            static const wchar_t kType[] = L"Content-Type: application/x-www-form-urlencoded\r\n";
            if (HttpSendRequestW(request, kType, static_cast<DWORD>(-1), const_cast<char*>(data.data()),
                                 static_cast<DWORD>(data.size())))
                status = readAnswer(request, body, false);
            InternetCloseHandle(request);
        }
        InternetCloseHandle(connection);
    }
    InternetCloseHandle(net);
    return status;
}

}  // namespace

int httpGet(const std::string& url, std::vector<uint8_t>& body) { return get(url, "plumnin", body, true); }

int httpRequest(const std::string& url, const std::string& data, const std::string& userAgent,
                std::vector<uint8_t>& body) {
    return data.empty() ? get(url, userAgent, body, false) : post(url, data, userAgent, body);
}

#else

namespace {

int curl(const std::string& url, const std::string& data, const std::string& userAgent, std::vector<uint8_t>& body,
         bool onlyOk) {
    body.clear();
    // The URL and the form data are percent-encoded and the User-Agent is plumnin's, so
    // none has a quote that could end the shell string.
    for (const std::string* s : {&url, &data, &userAgent})
        if (s->find('\'') != std::string::npos) return 0;
    std::string command = "curl -sSL --max-time 20 -A '" + userAgent + "' -w '\\n%{http_code}' ";
    if (!data.empty()) command += "--data-binary '" + data + "' ";
    command += "'" + url + "' 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return 0;
    char buffer[16384];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) body.insert(body.end(), buffer, buffer + n);
    pclose(pipe);
    // The status code is on the last line, after the body.
    size_t line = body.size();
    while (line > 0 && body[line - 1] != '\n') --line;
    int status = 0;
    for (size_t i = line; i < body.size(); ++i)
        if (body[i] >= '0' && body[i] <= '9') status = status * 10 + (body[i] - '0');
    body.resize(line > 0 ? line - 1 : 0);
    if (status != 200 && onlyOk) body.clear();
    return status;
}

}  // namespace

int httpGet(const std::string& url, std::vector<uint8_t>& body) { return curl(url, {}, "plumnin", body, true); }

int httpRequest(const std::string& url, const std::string& data, const std::string& userAgent,
                std::vector<uint8_t>& body) {
    return curl(url, data, userAgent, body, false);
}

#endif

}  // namespace plumnin
