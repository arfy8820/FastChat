#include "core/web_tools.h"
#include "core/platform.h"
#include "core/util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>

namespace {

const size_t kPagePart = 24000;          // characters of a page per call
const size_t kMaxDownload = 4 * 1024 * 1024;

json Schema(json properties, std::vector<std::string> required) {
    return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required}};
}

std::string ArgStr(const json& args, const char* key) {
    auto it = args.find(key);
    return it != args.end() && it->is_string() ? it->get<std::string>() : std::string();
}

ToolOutcome Failed(const std::string& message) {
    return {message, true, message};
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i] == '+' ? ' ' : s[i]);
        }
    }
    return out;
}

void AppendUtf8(std::string& out, unsigned long cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string DecodeEntities(const std::string& s) {
    static const std::pair<const char*, const char*> kNamed[] = {
        {"amp", "&"},      {"lt", "<"},       {"gt", ">"},       {"quot", "\""},    {"apos", "'"},
        {"nbsp", " "},     {"ndash", "\xE2\x80\x93"}, {"mdash", "\xE2\x80\x94"}, {"hellip", "\xE2\x80\xA6"},
        {"rsquo", "\xE2\x80\x99"}, {"lsquo", "\xE2\x80\x98"}, {"rdquo", "\xE2\x80\x9D"}, {"ldquo", "\xE2\x80\x9C"},
        {"deg", "\xC2\xB0"}, {"copy", "\xC2\xA9"}, {"reg", "\xC2\xAE"}, {"middot", "\xC2\xB7"}, {"bull", "\xE2\x80\xA2"},
        {"times", "\xC3\x97"}, {"euro", "\xE2\x82\xAC"}, {"pound", "\xC2\xA3"},
    };
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out.push_back('&');
            continue;
        }
        std::string name = s.substr(i + 1, semi - i - 1);
        bool done = false;
        if (!name.empty() && name[0] == '#') {
            unsigned long cp = name.size() > 1 && (name[1] == 'x' || name[1] == 'X') ? std::strtoul(name.c_str() + 2, nullptr, 16)
                                                                                     : std::strtoul(name.c_str() + 1, nullptr, 10);
            if (cp > 0) {
                AppendUtf8(out, cp == 0xA0 ? 0x20 : cp);
                done = true;
            }
        } else {
            for (const auto& [n, text] : kNamed) {
                if (name == n) {
                    out += text;
                    done = true;
                    break;
                }
            }
        }
        if (done) i = semi;
        else out.push_back('&');
    }
    return out;
}

// Text without tags, entities decoded, white space collapsed to single spaces.
std::string InlineText(const std::string& html) {
    std::string text;
    bool inTag = false;
    for (char c : html) {
        if (c == '<') inTag = true;
        else if (c == '>') inTag = false;
        else if (!inTag) text.push_back(c);
    }
    text = DecodeEntities(text);
    std::string out;
    bool space = false;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            space = !out.empty();
        } else {
            if (space) out.push_back(' ');
            space = false;
            out.push_back(c);
        }
    }
    return out;
}

std::string AttrValue(const std::string& tag, const std::string& attr) {
    std::string lower = ToLower(tag);
    size_t p = lower.find(attr + "=");
    if (p == std::string::npos) return std::string();
    p += attr.size() + 1;
    if (p >= tag.size()) return std::string();
    char quote = tag[p];
    if (quote == '"' || quote == '\'') {
        size_t end = tag.find(quote, p + 1);
        return end == std::string::npos ? std::string() : tag.substr(p + 1, end - p - 1);
    }
    size_t end = tag.find_first_of(" >", p);
    return tag.substr(p, end == std::string::npos ? std::string::npos : end - p);
}

// DuckDuckGo links through itself (//duckduckgo.com/l/?uddg=<the address>).
std::string ResultUrl(std::string href) {
    href = DecodeEntities(href);
    size_t u = href.find("uddg=");
    if (u != std::string::npos) {
        size_t end = href.find('&', u);
        href = UrlDecode(href.substr(u + 5, end == std::string::npos ? std::string::npos : end - u - 5));
    }
    if (StartsWith(href, "//")) href = "https:" + href;
    return href;
}

std::string SearchUrl() {
    if (const char* env = std::getenv("FASTCHAT_SEARCH_URL"); env && *env) return env;  // the tests' stand-in
    return "https://html.duckduckgo.com/html/";
}

const char* kBrowserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) FastChat";

ToolOutcome WebSearch(const json& args, const std::atomic<bool>* cancel) {
    std::string query = Trim(ArgStr(args, "query"));
    if (query.empty()) return Failed("No query given.");
    HttpRequest req;
    req.method = "POST";
    req.url = SearchUrl();
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}, {"User-Agent", kBrowserAgent}};
    req.body = "q=" + UrlEncode(query);
    req.cancel = cancel;
    req.receiveTimeoutMs = 30000;
    HttpResponse r = HttpSend(req);
    if (!r.Ok()) {
        return Failed("The search failed: " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
    }
    std::vector<SearchResult> results = ParseSearchResults(r.body);
    if (results.empty()) {
        ToolOutcome none{"No results for \"" + query + "\".", false, "No results for " + OneLine(query, 80)};
        if (r.body.find("anomaly") != std::string::npos || r.body.find("captcha") != std::string::npos) {
            none.output = "The search engine refused this search (it asked to prove a person is searching). Try again later, "
                          "or read a page directly with fetch_page.";
            none.error = true;
        }
        return none;
    }
    std::string out = "Results for \"" + query + "\":\n";
    int n = 0;
    for (const auto& res : results) {
        if (++n > 8) break;
        out += "\n" + std::to_string(n) + ". " + res.title + "\n   " + res.url + "\n";
        if (!res.snippet.empty()) out += "   " + res.snippet + "\n";
    }
    return {out, false, std::to_string(std::min<size_t>(results.size(), 8)) + " results for " + OneLine(query, 80)};
}

ToolOutcome FetchPage(const json& args, const std::atomic<bool>* cancel) {
    std::string url = Trim(ArgStr(args, "url"));
    if (url.empty()) return Failed("No address given.");
    if (!StartsWith(ToLower(url), "http://") && !StartsWith(ToLower(url), "https://")) url = "https://" + url;
    size_t start = 0;
    if (auto s = args.find("start"); s != args.end() && s->is_number_integer() && s->get<int64_t>() > 0) {
        start = static_cast<size_t>(s->get<int64_t>());
    }
    HttpRequest req;
    req.url = url;
    req.headers = {{"User-Agent", kBrowserAgent},
                   {"Accept", "text/html,application/xhtml+xml,text/plain,application/json;q=0.9,*/*;q=0.5"}};
    req.cancel = cancel;
    req.receiveTimeoutMs = 30000;
    std::string body;
    bool tooBig = false;
    req.onData = [&](const char* d, size_t n) {
        body.append(d, n);
        if (body.size() > kMaxDownload) {
            tooBig = true;
            return false;
        }
        return true;
    };
    HttpResponse r = HttpSend(req);
    if (r.cancelled) return Failed("Stopped.");
    if (!r.completed && !tooBig) return Failed("Could not read " + url + ": " + r.error);
    if (r.status >= 400) return Failed(url + " answered HTTP " + std::to_string(r.status) + ".");
    if (body.empty()) body = r.body;
    std::string type = ToLower(r.Header("content-type"));
    std::string text;
    if (type.find("html") != std::string::npos || (type.empty() && ToLower(body.substr(0, 512)).find("<html") != std::string::npos)) {
        text = HtmlToText(body);
    } else if (type.empty() || StartsWith(type, "text/") || type.find("json") != std::string::npos ||
               type.find("xml") != std::string::npos || type.find("javascript") != std::string::npos) {
        text = body;
    } else {
        return Failed(url + " is " + type + " (" + FormatBytes(body.size()) + "), not a page that can be read as text.");
    }
    text = SanitizeUtf8(text);
    if (start >= text.size() && start > 0) return Failed("The page has only " + std::to_string(text.size()) + " characters.");
    std::string part = text.substr(start, kPagePart);
    // Do not cut a character in two.
    while (!part.empty() && (static_cast<unsigned char>(part.back()) & 0xC0) == 0x80) part.pop_back();
    if (!part.empty() && (static_cast<unsigned char>(part.back()) & 0xC0) == 0xC0) part.pop_back();
    std::string out = "Page " + url + (tooBig ? " (only its first 4 MB were read)" : "") + ":\n\n" + part;
    size_t next = start + part.size();
    if (next < text.size()) {
        out += "\n\n[The page goes on: " + std::to_string(text.size() - next) + " more characters. Call fetch_page with start " +
               std::to_string(next) + " to read on.]";
    }
    return {out, false, "Read " + OneLine(url, 100)};
}

ToolOutcome CurrentTime() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buf[128];
    std::strftime(buf, sizeof(buf), "%A, %d %B %Y, %H:%M:%S (UTC%z)", &local);
    std::string text = std::string("It is ") + buf + ".";
    return {text, false, text};
}

}  // namespace

std::vector<ToolDef> WebToolDefinitions() {
    return {
        {"web_search",
         "Search the web. Gives the top results with their addresses and a snippet each; read a result in full with "
         "fetch_page. Use it for anything current or that you are unsure of (news, weather, prices, documentation).",
         Schema({{"query", {{"type", "string"}, {"description", "What to search for"}}}}, {"query"})},
        {"fetch_page",
         "Read a web page (or any text address: JSON, plain text) as text, without its markup. Long pages come in "
         "parts; the end of each part says where to start the next.",
         Schema({{"url", {{"type", "string"}, {"description", "The page's address"}}},
                 {"start", {{"type", "integer"}, {"description", "Character to start at, to read on in a long page"}}}},
                {"url"})},
        {"current_time", "The current date and time, and the time zone, on the user's computer.", Schema(json::object(), {})},
    };
}

bool IsWebTool(const std::string& name) {
    return name == "web_search" || name == "fetch_page" || name == "current_time";
}

ToolOutcome RunWebTool(const ToolCall& call, const std::atomic<bool>* cancel) {
    json args = ArgumentsToObject(call.arguments);
    ToolOutcome outcome;
    if (call.name == "web_search") outcome = WebSearch(args, cancel);
    else if (call.name == "fetch_page") outcome = FetchPage(args, cancel);
    else if (call.name == "current_time") outcome = CurrentTime();
    else outcome = Failed("No such tool: " + call.name);
    outcome.output = SanitizeUtf8(outcome.output);
    outcome.summary = SanitizeUtf8(outcome.summary);
    return outcome;
}

std::string WebToolsPrompt() {
    return "You can look things up: web_search searches the web, fetch_page reads a page, and current_time gives the "
           "date and time. Use them for anything current or that you are not sure of, rather than saying you cannot "
           "know; say where what you found came from.";
}

std::vector<SearchResult> ParseSearchResults(const std::string& html) {
    std::vector<SearchResult> results;
    size_t pos = 0;
    for (;;) {
        size_t a = html.find("result__a", pos);
        if (a == std::string::npos) break;
        size_t tagStart = html.rfind('<', a);
        size_t tagEnd = html.find('>', a);
        size_t close = html.find("</a>", tagEnd);
        if (tagStart == std::string::npos || tagEnd == std::string::npos || close == std::string::npos) break;
        std::string tag = html.substr(tagStart, tagEnd - tagStart + 1);
        SearchResult r;
        r.url = ResultUrl(AttrValue(tag, "href"));
        r.title = InlineText(html.substr(tagEnd + 1, close - tagEnd - 1));
        pos = close;
        // The snippet, before the next result.
        size_t next = html.find("result__a", close);
        size_t snip = html.find("result__snippet", close);
        if (snip != std::string::npos && (next == std::string::npos || snip < next)) {
            size_t sTagEnd = html.find('>', snip);
            // Snippets carry <b> for the words searched for: end at their own closing tag.
            size_t endA = html.find("</a>", sTagEnd), endTd = html.find("</td>", sTagEnd), endDiv = html.find("</div>", sTagEnd);
            size_t sClose = std::min({endA, endTd, endDiv});
            if (sTagEnd != std::string::npos && sClose != std::string::npos) r.snippet = InlineText(html.substr(sTagEnd + 1, sClose - sTagEnd - 1));
        }
        // Ads link through DuckDuckGo's ad redirect.
        if (r.url.find("duckduckgo.com/y.js") != std::string::npos || r.url.empty() || r.title.empty()) continue;
        results.push_back(r);
    }
    return results;
}

std::string HtmlToText(const std::string& html) {
    std::string lower = ToLower(html);
    std::string out;
    out.reserve(html.size() / 3);
    size_t i = 0;
    auto newline = [&](int count) {
        while (!out.empty() && out.back() == ' ') out.pop_back();
        int have = 0;
        for (size_t k = out.size(); k > 0 && out[k - 1] == '\n'; k--) have++;
        for (; have < count && !out.empty(); have++) out.push_back('\n');
    };
    while (i < html.size()) {
        if (html[i] != '<') {
            size_t next = html.find('<', i);
            std::string piece = DecodeEntities(html.substr(i, next == std::string::npos ? std::string::npos : next - i));
            for (char c : piece) {
                if (c == '\r' || c == '\n' || c == '\t') c = ' ';
                if (c == ' ' && (out.empty() || out.back() == ' ' || out.back() == '\n')) continue;
                out.push_back(c);
            }
            if (next == std::string::npos) break;
            i = next;
            continue;
        }
        if (lower.compare(i, 4, "<!--") == 0) {
            size_t end = html.find("-->", i);
            i = end == std::string::npos ? html.size() : end + 3;
            continue;
        }
        size_t end = html.find('>', i);
        if (end == std::string::npos) break;
        std::string tag = lower.substr(i + 1, end - i - 1);
        bool closing = !tag.empty() && tag[0] == '/';
        std::string name;
        for (size_t k = closing ? 1 : 0; k < tag.size() && (std::isalnum(static_cast<unsigned char>(tag[k])) != 0); k++) name.push_back(tag[k]);
        // Whole elements that are not text.
        if (!closing && (name == "script" || name == "style" || name == "noscript" || name == "svg" || name == "template" ||
                         name == "iframe" || name == "head")) {
            size_t close = lower.find("</" + name, end);
            if (name == "head") {
                // Keep the title.
                size_t t = lower.find("<title", end);
                if (t != std::string::npos && (close == std::string::npos || t < close)) {
                    size_t te = html.find('>', t), tc = lower.find("</title", t);
                    if (te != std::string::npos && tc != std::string::npos) {
                        out += "Title: " + InlineText(html.substr(te + 1, tc - te - 1));
                        newline(2);
                    }
                }
            }
            size_t closeEnd = close == std::string::npos ? std::string::npos : html.find('>', close);
            i = closeEnd == std::string::npos ? html.size() : closeEnd + 1;
            continue;
        }
        if (name == "br") newline(1);
        else if (name == "p" || name == "div" || name == "section" || name == "article" || name == "table" ||
                 name == "ul" || name == "ol" || name == "blockquote" || name == "pre" || name == "header" || name == "footer" ||
                 name == "nav" || name == "main" || name == "form") newline(closing ? 2 : 1);
        else if (name == "tr" || name == "dt" || name == "dd") newline(1);
        else if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') newline(2);
        else if (name == "li" && !closing) {
            newline(1);
            out += "- ";
        } else if ((name == "td" || name == "th") && closing) out += " | ";
        i = end + 1;
    }
    // At most one blank line in a row; no spaces at line ends.
    std::string clean;
    int blank = 0;
    for (const auto& line : SplitLines(out)) {
        std::string t = Trim(line);
        if (t.empty()) {
            if (++blank > 1 || clean.empty()) continue;
        } else {
            blank = 0;
        }
        clean += t + "\n";
    }
    return Trim(clean);
}
