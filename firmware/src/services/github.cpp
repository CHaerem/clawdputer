#include "github.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "sealed.h"
#include "wifi.h"
#include "ui/canvas.h"

#ifndef CLAWD_GITHUB_REPO
#define CLAWD_GITHUB_REPO "CHaerem/clawdputer"
#endif

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// Parse `"number": 123` and `"html_url": "..."` out of a tiny GitHub
// response. Avoids pulling in a JSON dependency for two fields.
void parseIssueResponse(const String& body, int& number, std::string& url) {
    int n = body.indexOf("\"number\":");
    if (n >= 0) {
        int end = n + 9;
        while (end < (int)body.length() && (body[end] == ' ')) end++;
        int start = end;
        while (end < (int)body.length() && isdigit(body[end])) end++;
        if (end > start) number = body.substring(start, end).toInt();
    }
    int u = body.indexOf("\"html_url\":");
    if (u >= 0) {
        int q1 = body.indexOf('"', u + 11);
        int q2 = q1 >= 0 ? body.indexOf('"', q1 + 1) : -1;
        if (q1 >= 0 && q2 > q1) url = body.substring(q1 + 1, q2).c_str();
    }
}

}  // namespace

namespace github {

bool hasToken() {
    return !sealed::unsealGithubPat().empty();
}

SubmitResult submitIssue(const std::string& title,
                         const std::string& body,
                         const std::string& label) {
    SubmitResult r;

    std::string pat = sealed::unsealGithubPat();
    if (pat.empty()) {
        r.error = "no sealed PAT (run tools/seal-pat.py)";
        return r;
    }

    if (!wifi::isConnected()) { r.error = "wifi offline"; return r; }

    bool hadCanvas = ui::canvasActive();
    if (hadCanvas) ui::releaseCanvas();

    std::string payload = "{\"title\":\"" + jsonEscape(title)
                        + "\",\"body\":\""  + jsonEscape(body)
                        + "\",\"labels\":[\"" + jsonEscape(label) + "\"]}";

    String url = String("https://api.github.com/repos/")
               + CLAWD_GITHUB_REPO + "/issues";

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    if (!http.begin(client, url)) {
        if (hadCanvas) ui::tryAcquireCanvas();
        r.error = "http begin failed";
        return r;
    }
    http.addHeader("Authorization", String("Bearer ") + pat.c_str());
    http.addHeader("Accept",       "application/vnd.github+json");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent",   "clawdputer");

    int code = http.POST((uint8_t*)payload.data(), payload.size());
    String resp = http.getString();
    http.end();

    if (hadCanvas) ui::tryAcquireCanvas();

    if (code == 201) {
        r.ok = true;
        parseIssueResponse(resp, r.issueNumber, r.issueUrl);
        Serial.printf("[github] issue #%d submitted: %s\n",
                      r.issueNumber, r.issueUrl.c_str());
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "http %d", code);
        r.error = buf;
        Serial.printf("[github] submit failed: %s\nresp: %s\n",
                      r.error.c_str(), resp.c_str());
    }
    return r;
}

}  // namespace github
