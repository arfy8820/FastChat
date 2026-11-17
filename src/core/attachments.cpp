#include "core/attachments.h"
#include "core/platform.h"
#include "core/util.h"

#include <filesystem>

namespace fs = std::filesystem;

namespace {

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
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool HasUtf16Bom(const std::string& d) {
    return d.size() >= 2 && ((d[0] == '\xFF' && d[1] == '\xFE') || (d[0] == '\xFE' && d[1] == '\xFF'));
}

std::string Utf16ToUtf8(const std::string& d) {
    bool little = d[0] == '\xFF';
    std::string out;
    for (size_t i = 2; i + 1 < d.size(); i += 2) {
        unsigned a = static_cast<unsigned char>(d[i]), b = static_cast<unsigned char>(d[i + 1]);
        unsigned long u = little ? (a | (b << 8)) : ((a << 8) | b);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < d.size()) {
            unsigned c = static_cast<unsigned char>(d[i + 2]), e = static_cast<unsigned char>(d[i + 3]);
            unsigned long low = little ? (c | (e << 8)) : ((c << 8) | e);
            if (low >= 0xDC00 && low < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        AppendUtf8(out, u);
    }
    return out;
}

// Windows-1252 (a superset of Latin-1 for text): what older text files are in.
std::string Cp1252ToUtf8(const std::string& d) {
    static const unsigned short k80[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                           0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
                                           0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                           0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    std::string out;
    for (char ch : d) {
        unsigned c = static_cast<unsigned char>(ch);
        AppendUtf8(out, c >= 0x80 && c < 0xA0 ? k80[c - 0x80] : c);
    }
    return out;
}

// Text in a single-byte encoding: no NUL bytes and hardly any control characters.
bool LooksLikeSingleByteText(const std::string& d) {
    if (d.find('\0') != std::string::npos) return false;
    size_t control = 0;
    for (char ch : d) {
        unsigned c = static_cast<unsigned char>(ch);
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t' && c != '\f') control++;
    }
    return control * 100 <= d.size();
}

std::string GuessType(const std::string& name) {
    std::string ext = ToLower(PathToUtf8(PathFromUtf8(name).extension()));
    static const std::pair<const char*, const char*> kTypes[] = {
        {".mid", "audio/midi"},   {".midi", "audio/midi"},  {".rmi", "audio/midi"},     {".wav", "audio/wav"},
        {".mp3", "audio/mpeg"},   {".ogg", "audio/ogg"},    {".flac", "audio/flac"},    {".m4a", "audio/mp4"},
        {".mp4", "video/mp4"},    {".zip", "application/zip"}, {".7z", "application/x-7z-compressed"},
        {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {".doc", "application/msword"}, {".xls", "application/vnd.ms-excel"}, {".epub", "application/epub+zip"},
        {".exe", "application/vnd.microsoft.portable-executable"}, {".dll", "application/vnd.microsoft.portable-executable"},
        {".bmp", "image/bmp"},    {".tif", "image/tiff"},   {".tiff", "image/tiff"},    {".ico", "image/x-icon"},
        {".sqlite", "application/vnd.sqlite3"}, {".db", "application/vnd.sqlite3"},
    };
    for (const auto& [e, type] : kTypes) {
        if (ext == e) return type;
    }
    return "application/octet-stream";
}

bool IsImageOrPdfType(const std::string& type) {
    return type == "image/png" || type == "image/jpeg" || type == "image/gif" || type == "image/webp" || type == "application/pdf";
}

// Base64 in lines of 76 characters, as mail and most tools write it.
std::string WrappedBase64(const std::string& data) {
    std::string b64 = Base64Encode(data), out;
    for (size_t i = 0; i < b64.size(); i += 76) out += b64.substr(i, 76) + "\n";
    return out;
}

// A name in `dir` no other file has: "song.mid", then "song (2).mid"...
fs::path FreeName(const fs::path& dir, const std::string& name) {
    fs::path wanted = PathFromUtf8(name).filename();
    if (wanted.empty()) wanted = "attachment";
    std::error_code ec;
    if (!fs::exists(dir / wanted, ec)) return dir / wanted;
    std::string stem = PathToUtf8(wanted.stem()), ext = PathToUtf8(wanted.extension());
    for (int n = 2;; n++) {
        fs::path p = dir / PathFromUtf8(stem + " (" + std::to_string(n) + ")" + ext);
        if (!fs::exists(p, ec)) return p;
    }
}

}  // namespace

std::string AttachmentType(const std::string& name, const std::string& data) {
    auto starts = [&](const char* magic, size_t n, size_t at = 0) { return data.size() >= at + n && data.compare(at, n, magic, n) == 0; };
    if (starts("\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (starts("\xFF\xD8\xFF", 3)) return "image/jpeg";
    if (starts("GIF87a", 6) || starts("GIF89a", 6)) return "image/gif";
    if (starts("RIFF", 4) && starts("WEBP", 4, 8)) return "image/webp";
    if (starts("%PDF-", 5)) return "application/pdf";
    // Text: UTF-8 (source, notes, CSV, JSON, logs, REAPER projects...), UTF-16
    // with its byte order mark, or an older single-byte encoding.
    if (data.find('\0') == std::string::npos && IsValidUtf8(data)) return "text/plain";
    if (HasUtf16Bom(data) && data.size() % 2 == 0) return "text/plain";
    if (LooksLikeSingleByteText(data)) return "text/plain";
    return GuessType(name);
}

bool MakeAttachment(const std::string& name, const std::string& data, Attachment& out, std::string& error) {
    std::string type = AttachmentType(name, data);
    bool text = StartsWith(type, "text/");
    size_t limit = IsImageOrPdfType(type) && type != "application/pdf" ? kMaxImageBytes
                   : type == "application/pdf"                       ? kMaxPdfBytes
                   : text                                             ? kMaxTextBytes
                                                                      : kMaxFileBytes;
    if (data.size() > limit) {
        error = name + " is " + FormatBytes(data.size()) + "; FastChat attaches " +
                (IsImageOrPdfType(type) && type != "application/pdf" ? "images"
                 : type == "application/pdf"                           ? "PDFs"
                 : text                                                 ? "text files"
                                                                        : "files") +
                " up to " + FormatBytes(limit) + ".";
        return false;
    }
    out.name = name;
    out.mediaType = type;
    out.data = data;
    if (text) {
        if (HasUtf16Bom(data) && !IsValidUtf8(data)) out.data = Utf16ToUtf8(data);
        else if (!IsValidUtf8(data)) out.data = Cp1252ToUtf8(data);
        // A UTF-8 byte order mark says nothing to the model.
        if (StartsWith(out.data, "\xEF\xBB\xBF")) out.data.erase(0, 3);
    }
    return true;
}

bool LoadAttachment(const std::string& path, Attachment& out, std::string& error) {
    std::string name = PathToUtf8(PathFromUtf8(path).filename());
    std::error_code ec;
    auto size = fs::file_size(PathFromUtf8(path), ec);
    if (ec) {
        error = "Could not read " + name + ".";
        return false;
    }
    if (size > kMaxFileBytes) {
        error = name + " is " + FormatBytes(size) + ", too large to attach (up to " + FormatBytes(kMaxFileBytes) + ").";
        return false;
    }
    std::string data;
    if (!ReadFileText(path, data)) {
        error = "Could not read " + name + ".";
        return false;
    }
    return MakeAttachment(name, data, out, error);
}

std::string AttachmentProblem(const Attachment& a, bool hasFolder) {
    if (!a.IsOtherFile() || hasFolder || a.data.size() <= kMaxInlineFileBytes) return std::string();
    return a.name + " is " + FormatBytes(a.data.size()) + ". In a chat, files other than images, PDFs and text go into " +
           "the message as text (base64), up to " + FormatBytes(kMaxInlineFileBytes) +
           ". A code conversation takes it whole: it is saved in its folder for the model's tools.";
}

std::string AttachedTextBlock(const std::string& name, const std::string& text) {
    return "[Attached file: " + name + "]\n" + text;
}

void AttachToMessage(Message& m, const std::vector<Attachment>& attachments, const std::string& folder) {
    auto add = [&](const std::string& block) {
        if (!m.content.empty()) m.content += "\n\n";
        m.content += block;
    };
    for (const auto& a : attachments) {
        if (a.IsText()) {
            add(AttachedTextBlock(a.name, a.data));
        } else if (a.IsImageOrPdf()) {
            m.images.push_back({a.mediaType, Base64Encode(a.data), a.name});
        } else {
            std::string about = a.name + ", " + FormatBytes(a.data.size()) + ", " + a.mediaType;
            if (!folder.empty()) {
                // Into the project, where the model's tools can open it.
                fs::path dir = PathFromUtf8(folder) / "fastchat-attachments";
                std::error_code ec;
                fs::create_directories(dir, ec);
                fs::path target = FreeName(dir, a.name);
                if (!ec && WriteFileText(PathToUtf8(target), a.data)) {
                    std::string relative = "fastchat-attachments/" + PathToUtf8(target.filename());
                    add("[Attached file: " + about + ". Saved in the project folder as " + relative + "]");
                    continue;
                }
            }
            if (a.data.size() <= kMaxInlineFileBytes) {
                add("[Attached file: " + about + ", in base64]\n" + WrappedBase64(a.data));
            } else {
                add("[Attached file: " + about + ". It could not be included.]");
            }
        }
    }
}

std::string DescribeAttachment(const Attachment& a) {
    std::string kind = a.IsImageOrPdf() && a.mediaType != "application/pdf" ? "image"
                       : a.mediaType == "application/pdf"                     ? "PDF"
                       : a.IsText()                                           ? "text file"
                                                                              : "file";
    return a.name + ", " + FormatBytes(a.data.size()) + " " + kind;
}

std::string AttachmentsSuffix(const Message& m) {
    if (m.images.empty()) return std::string();
    std::vector<std::string> names;
    for (const auto& i : m.images) {
        if (i.name.empty()) return m.images.size() == 1 ? std::string(", with an image")
                                                        : ", with " + std::to_string(m.images.size()) + " images";
        names.push_back(i.name);
    }
    std::string list = names[0];
    for (size_t i = 1; i < names.size(); i++) list += (i + 1 == names.size() ? " and " : ", ") + names[i];
    return ", with " + list;
}
