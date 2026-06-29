#include "memo_store.h"

#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr const char* kNamespace = "core_s3_memo";
constexpr int kMaxMemos = 8;
constexpr size_t kMaxTitleBytes = 48;
constexpr size_t kMaxContentBytes = 260;

struct MemoRecord {
    int id = 0;
    std::string title;
    std::string content;
};

std::string SlotKey(const char* prefix, int slot) {
    char key[8];
    std::snprintf(key, sizeof(key), "%s%d", prefix, slot);
    return key;
}

std::string TruncateUtf8(const std::string& value, size_t max_bytes) {
    if (value.size() <= max_bytes) {
        return value;
    }

    size_t pos = 0;
    size_t last = 0;
    while (pos < value.size()) {
        const unsigned char ch = static_cast<unsigned char>(value[pos]);
        size_t len = 1;
        if ((ch & 0x80) == 0) {
            len = 1;
        } else if ((ch & 0xE0) == 0xC0) {
            len = 2;
        } else if ((ch & 0xF0) == 0xE0) {
            len = 3;
        } else if ((ch & 0xF8) == 0xF0) {
            len = 4;
        }
        if (pos + len > max_bytes) {
            break;
        }
        last = pos + len;
        pos += len;
    }
    return value.substr(0, last);
}

std::string NormalizeAction(const std::string& action) {
    std::string normalized;
    normalized.reserve(action.size());
    for (unsigned char ch : action) {
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (normalized == "view" || normalized == "get") {
        return "list";
    }
    if (normalized == "add") {
        return "create";
    }
    if (normalized == "edit") {
        return "update";
    }
    if (normalized == "remove") {
        return "delete";
    }
    if (normalized == "delete_all" || normalized == "remove_all" || normalized == "deleteall") {
        return "clear";
    }
    return normalized.empty() ? "list" : normalized;
}

std::vector<MemoRecord> LoadMemos(Settings& settings) {
    std::vector<MemoRecord> memos;
    int count = settings.GetInt("count", 0);
    count = std::max(0, std::min(count, kMaxMemos));
    memos.reserve(count);

    for (int slot = 0; slot < count; ++slot) {
        MemoRecord memo;
        memo.id = settings.GetInt(SlotKey("id", slot), 0);
        memo.title = settings.GetString(SlotKey("t", slot), "");
        memo.content = settings.GetString(SlotKey("c", slot), "");
        if (memo.id > 0) {
            memos.push_back(std::move(memo));
        }
    }
    return memos;
}

void SaveMemos(Settings& settings, const std::vector<MemoRecord>& memos, int next_id) {
    const int count = std::min(static_cast<int>(memos.size()), kMaxMemos);
    for (int slot = 0; slot < count; ++slot) {
        settings.SetInt(SlotKey("id", slot), memos[slot].id);
        settings.SetString(SlotKey("t", slot), memos[slot].title);
        settings.SetString(SlotKey("c", slot), memos[slot].content);
    }
    for (int slot = count; slot < kMaxMemos; ++slot) {
        settings.EraseKey(SlotKey("id", slot));
        settings.EraseKey(SlotKey("t", slot));
        settings.EraseKey(SlotKey("c", slot));
    }
    settings.SetInt("count", count);
    settings.SetInt("next", std::max(1, next_id));
}

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", ch);
                out += buf;
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return out;
}

std::string ErrorJson(const char* error) {
    std::string out = "{\"ok\":false,\"error\":\"";
    out += JsonEscape(error);
    out += "\"}";
    return out;
}

std::string MemosJson(const std::vector<MemoRecord>& memos,
                      const std::string& action,
                      int changed_id = 0) {
    std::string out = "{\"ok\":true,\"action\":\"";
    out += JsonEscape(action);
    out += "\",\"count\":";
    out += std::to_string(memos.size());
    if (changed_id > 0) {
        out += ",\"id\":";
        out += std::to_string(changed_id);
    }
    out += ",\"memos\":[";
    for (size_t i = 0; i < memos.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        out += "{\"id\":";
        out += std::to_string(memos[i].id);
        out += ",\"title\":\"";
        out += JsonEscape(memos[i].title);
        out += "\",\"content\":\"";
        out += JsonEscape(memos[i].content);
        out += "\"}";
    }
    out += "]}";
    return out;
}
}  // namespace

std::string CoreS3MemoStore::HandleMcpAction(const std::string& requested_action,
                                             int id,
                                             const std::string& title,
                                             const std::string& content) {
    const std::string action = NormalizeAction(requested_action);

    if (action == "list") {
        Settings settings(kNamespace);
        return MemosJson(LoadMemos(settings), action);
    }

    Settings settings(kNamespace, true);
    auto memos = LoadMemos(settings);
    int next_id = settings.GetInt("next", 1);
    if (next_id < 1) {
        next_id = 1;
    }

    if (action == "create") {
        const std::string clipped_title = TruncateUtf8(title, kMaxTitleBytes);
        const std::string clipped_content = TruncateUtf8(content, kMaxContentBytes);
        if (clipped_title.empty() && clipped_content.empty()) {
            return ErrorJson("memo_empty");
        }
        if (static_cast<int>(memos.size()) >= kMaxMemos) {
            return ErrorJson("memo_full");
        }

        MemoRecord memo;
        memo.id = next_id++;
        memo.title = clipped_title;
        memo.content = clipped_content;
        memos.push_back(memo);
        SaveMemos(settings, memos, next_id);
        return MemosJson(memos, action, memo.id);
    }

    if (action == "update") {
        if (id <= 0) {
            return ErrorJson("id_required");
        }
        auto it = std::find_if(memos.begin(), memos.end(), [id](const MemoRecord& memo) {
            return memo.id == id;
        });
        if (it == memos.end()) {
            return ErrorJson("memo_not_found");
        }
        if (title.empty() && content.empty()) {
            return ErrorJson("memo_empty");
        }
        if (!title.empty()) {
            it->title = TruncateUtf8(title, kMaxTitleBytes);
        }
        if (!content.empty()) {
            it->content = TruncateUtf8(content, kMaxContentBytes);
        }
        SaveMemos(settings, memos, next_id);
        return MemosJson(memos, action, id);
    }

    if (action == "delete") {
        if (id <= 0) {
            return ErrorJson("id_required");
        }
        const auto original_size = memos.size();
        memos.erase(std::remove_if(memos.begin(), memos.end(), [id](const MemoRecord& memo) {
            return memo.id == id;
        }), memos.end());
        if (memos.size() == original_size) {
            return ErrorJson("memo_not_found");
        }
        SaveMemos(settings, memos, next_id);
        return MemosJson(memos, action, id);
    }

    if (action == "clear") {
        memos.clear();
        SaveMemos(settings, memos, 1);
        return MemosJson(memos, action);
    }

    return ErrorJson("unknown_action");
}

std::string CoreS3MemoStore::DisplayText() {
    Settings settings(kNamespace);
    auto memos = LoadMemos(settings);
    if (memos.empty()) {
        return "暂无备忘录";
    }

    std::string text;
    for (size_t i = 0; i < memos.size(); ++i) {
        if (i > 0) {
            text += "\n\n";
        }
        text += "#";
        text += std::to_string(memos[i].id);
        text += " ";
        text += memos[i].title.empty() ? "未命名" : memos[i].title;
        if (!memos[i].content.empty()) {
            text += "\n";
            text += memos[i].content;
        }
    }
    return text;
}
