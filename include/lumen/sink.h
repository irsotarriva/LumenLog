#ifndef LUMEN_SINK_H
#define LUMEN_SINK_H

#include <memory>
#include <string_view>
#include <vector>

#include "lumen/record.h"

namespace lumen {

class Sink {
public:
    virtual void on_log(const LogRecord&) {}
    virtual void on_metric(const MetricRecord&) {}
    virtual void on_progress(const ProgressRecord&) {}
    virtual void flush() = 0;
    virtual ~Sink() = default;
};

enum class AnsiColor {
    Default,
    Black,
    Red,
    Green,
    Yellow,
    Blue,
    Magenta,
    Cyan,
    White,
};

enum class FileFormat {
    PlainText,
    KeyValue,
    Json,
};

class TerminalSink : public Sink {
public:
    struct Config {
        std::string_view                  time_format = "%H:%M:%S.%ms";
        std::vector<std::string_view>     inline_tags;
        std::vector<std::string_view>     dashboard_metrics;
        int                               dashboard_height = 5;
        bool                              enable_dashboard = false;
        std::array<AnsiColor, 6>          colors = {};
    };

    static Config default_config();
    explicit TerminalSink(Config cfg);
    ~TerminalSink() override;
    void on_log(const LogRecord& record) override;
    void on_metric(const MetricRecord& record) override;
    void on_progress(const ProgressRecord& record) override;
    void flush() override;

private:
    Config __cfg;
    struct DashboardState;
    std::unique_ptr<DashboardState> __dashboard;
};

class FileSink : public Sink {
public:
    struct Config {
        std::string_view  path = "run_{date}.log";
        int               rotate_mb = 256;
        int               max_files = 5;
        FileFormat        format = FileFormat::KeyValue;
    };

    explicit FileSink(Config cfg);
    ~FileSink() override;
    void on_log(const LogRecord& record) override;
    void on_metric(const MetricRecord& record) override;
    void on_progress(const ProgressRecord& record) override;
    void flush() override;

private:
    Config __cfg;
    struct Impl;
    std::unique_ptr<Impl> __impl;
};

class JsonSink : public Sink {
public:
    explicit JsonSink(std::string_view path);
    ~JsonSink() override;
    void on_log(const LogRecord& record) override;
    void on_metric(const MetricRecord& record) override;
    void on_progress(const ProgressRecord& record) override;
    void flush() override;

private:
    struct Impl;
    std::unique_ptr<Impl> __impl;
};

class NullSink : public Sink {
public:
    void on_log(const LogRecord&) override {}
    void on_metric(const MetricRecord&) override {}
    void on_progress(const ProgressRecord&) override {}
    void flush() override {}
};

}  // namespace lumen

#endif
