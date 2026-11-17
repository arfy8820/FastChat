#pragma once

// Files the user attaches to a message. Images and PDFs go with it as
// themselves (Message::images); text files of any kind go into its text; any
// other file is saved in a code conversation's folder (where the model's tools
// can work on it), or goes into a chat's text in base64 when it is small.

#include "core/conversation.h"

#include <string>
#include <vector>

struct Attachment {
    std::string name;       // the file's name
    std::string mediaType;  // "image/png", "application/pdf", "text/plain", "application/octet-stream"...
    std::string data;       // its bytes (text: UTF-8)
    bool IsText() const { return mediaType.rfind("text/", 0) == 0; }
    // The images models take (PNG, JPEG, GIF, WebP), and PDFs.
    bool IsImageOrPdf() const {
        return mediaType == "image/png" || mediaType == "image/jpeg" || mediaType == "image/gif" || mediaType == "image/webp" ||
               mediaType == "application/pdf";
    }
    bool IsOtherFile() const { return !IsText() && !IsImageOrPdf(); }
};

// The largest files that are attached (the services' own limits, roughly).
constexpr size_t kMaxImageBytes = 5 * 1024 * 1024;
constexpr size_t kMaxPdfBytes = 30 * 1024 * 1024;
constexpr size_t kMaxTextBytes = 1024 * 1024;
constexpr size_t kMaxFileBytes = 50 * 1024 * 1024;     // other files, saved in a code conversation's folder
constexpr size_t kMaxInlineFileBytes = 100 * 1024;     // other files in a chat, as base64 in the text

// The kind of file these bytes are, from their first bytes: "image/png",
// "image/jpeg", "image/gif", "image/webp", "application/pdf", "text/plain"
// (UTF-8, UTF-16 with a byte order mark, or a single-byte encoding), or for
// anything else a type guessed from the name ("audio/midi"...), else
// "application/octet-stream".
std::string AttachmentType(const std::string& name, const std::string& data);

// A file read to be attached. `error` says why not (too large, unreadable).
bool LoadAttachment(const std::string& path, Attachment& out, std::string& error);
// The same for bytes already in memory (a pasted image). Text in other
// encodings becomes UTF-8.
bool MakeAttachment(const std::string& name, const std::string& data, Attachment& out, std::string& error);

// Why it cannot go with a message of this conversation ("" when it can): an
// other file too large for a chat, which has no folder to save it in.
std::string AttachmentProblem(const Attachment& a, bool hasFolder);

// Puts them on the message: images and PDFs with it, text files at the end of
// its text ("[Attached file: notes.txt]" and the file). Other files are saved
// in `folder` (a code conversation's; in fastchat-attachments) and named in
// the text with where they are, or without a folder go in as base64.
void AttachToMessage(Message& m, const std::vector<Attachment>& attachments, const std::string& folder = std::string());

// How a text file reads in a message's text.
std::string AttachedTextBlock(const std::string& name, const std::string& text);

// "pic.png, 120 KB image" for lists.
std::string DescribeAttachment(const Attachment& a);

// What a message carries besides its text, for its row: ", with an image",
// ", with pic.png and notes.pdf". Empty when nothing.
std::string AttachmentsSuffix(const Message& m);
