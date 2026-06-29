#ifndef M5STACK_CORE_S3_MEMO_STORE_H
#define M5STACK_CORE_S3_MEMO_STORE_H

#include <string>

class CoreS3MemoStore {
public:
    static std::string HandleMcpAction(const std::string& action,
                                       int id,
                                       const std::string& title,
                                       const std::string& content);
    static std::string DisplayText();
};

#endif  // M5STACK_CORE_S3_MEMO_STORE_H
