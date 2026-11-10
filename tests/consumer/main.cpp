// Logs one record through a sink and checks that it arrived. Exit status 0
// means the record reached the sink with the message that was logged.
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lumen/lumen.hpp"

namespace {

// Copies each message: a record's views are only valid during the callback.
class RecordingSink : public lumen::Sink {
public:
    explicit RecordingSink(std::shared_ptr<std::vector<std::string>> messages)
        : messages_(std::move(messages)) {}

    void on_log(const lumen::LogRecord& record) override {
        messages_->emplace_back(record.message);
    }
    void flush() override {}

private:
    std::shared_ptr<std::vector<std::string>> messages_;
};

}  // namespace

int main() {
    auto messages = std::make_shared<std::vector<std::string>>();
    [[maybe_unused]] const auto sink_id =
        lumen::core().add_sink(std::make_unique<RecordingSink>(messages), lumen::always());

    LUMEN_LOG_INFO("consumer record");
    lumen::core().flush();

    if (messages->size() != 1 || messages->front() != "consumer record") {
        std::cerr << "expected exactly one record \"consumer record\", got " << messages->size()
                  << '\n';
        return 1;
    }
    std::cout << "consumer received: " << messages->front() << '\n';
    return 0;
}
