#pragma once

// Tools for looking things up, given to models in chat and code conversations
// (FastChat runs them; nothing to approve, as they only read): searching the
// web, reading a page, and the current time.

#include "core/code_tools.h"

#include <atomic>
#include <string>
#include <vector>

std::vector<ToolDef> WebToolDefinitions();
bool IsWebTool(const std::string& name);
ToolOutcome RunWebTool(const ToolCall& call, const std::atomic<bool>* cancel);

// What the instructions say about them.
std::string WebToolsPrompt();

struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};
// The results on a DuckDuckGo HTML results page.
std::vector<SearchResult> ParseSearchResults(const std::string& html);
// A web page as readable text: no scripts, styles or tags; paragraphs, list
// items and headings on lines of their own; entities decoded.
std::string HtmlToText(const std::string& html);
