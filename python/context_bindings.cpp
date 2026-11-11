#include <pybind11/pybind11.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "errors.hpp"
#include "lumen/context.hpp"
#include "registration.hpp"
#include "tag_values.hpp"

namespace py = pybind11;

namespace lumen::python {
namespace {

// The scopes entered from Python on one thread, innermost last. The guards
// must be destroyed on their own thread in reverse order of creation; Python
// does not guarantee either for the objects that `with` uses (a scope can be
// left on another thread, out of order by a generator or a coroutine, or
// never). So the guards live here and a Scope only holds a token: leaving a
// scope closes it and every scope entered after it on the same thread, and a
// scope still open when its thread ends is closed then.
class ThreadScopes {
public:
    ThreadScopes() {
        // Makes sure the thread's context (src/context.cpp) is constructed
        // before this object, so that it is destroyed after it and still
        // there when the guards below are popped at thread exit. The result
        // does not matter.
        static_cast<void>(remove_thread_tag({}));
    }
    ~ThreadScopes() { close_from_(0); }
    ThreadScopes(const ThreadScopes&) = delete;
    ThreadScopes& operator=(const ThreadScopes&) = delete;
    ThreadScopes(ThreadScopes&&) = delete;
    ThreadScopes& operator=(ThreadScopes&&) = delete;

    [[nodiscard]] uint64_t enter(std::string_view key, std::string_view value) {
        const uint64_t token = next_token_++;
        entries_.push_back(Entry{token, std::make_unique<ScopedTag>(key, value)});
        return token;
    }

    // Closes the scope with this token and every scope entered after it. A
    // token that is not open (closed by leaving an enclosing scope) is ignored.
    void exit(uint64_t token) {
        const auto it = std::ranges::find(entries_, token, &Entry::token);
        if (it != entries_.end()) {
            close_from_(static_cast<std::size_t>(it - entries_.begin()));
        }
    }

private:
    struct Entry {
        uint64_t token;
        std::unique_ptr<ScopedTag> guard;
    };

    void close_from_(std::size_t first) {
        while (entries_.size() > first) {
            entries_.pop_back();
        }
    }

    std::vector<Entry> entries_;
    uint64_t next_token_ = 1;
};

// Python scopes of the calling thread; this thread_local is their single owner.
// cert-err58-cpp: constructing a ThreadScopes allocates nothing, so its
// initialisation cannot throw although no constructor says noexcept.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,cert-err58-cpp)
thread_local ThreadScopes tls_scopes;

// The object a `with lumen.scope(key, value):` statement uses.
class Scope {
public:
    Scope(std::string key, std::string value) : key_(std::move(key)), value_(std::move(value)) {}

    void enter() {
        if (token_ != 0) {
            throw py::value_error("this scope is already entered");
        }
        token_ = tls_scopes.enter(key_, value_);
        thread_ = std::this_thread::get_id();
    }

    void exit() {
        if (token_ == 0) {
            throw py::value_error("this scope is not entered");
        }
        if (thread_ != std::this_thread::get_id()) {
            PyErr_SetString(PyExc_RuntimeError,
                            "a scope must be left on the thread that entered it");
            throw py::error_already_set();
        }
        tls_scopes.exit(token_);
        token_ = 0;
    }

private:
    std::string key_;
    std::string value_;
    uint64_t token_ = 0;  // 0 while not entered
    std::thread::id thread_;
};

[[nodiscard]] std::string value_text(py::handle value) {
    return tag_text(tag_value(value));
}

using SetTag = std::expected<void, std::error_code> (*)(std::string_view, std::string_view);

[[nodiscard]] auto setter(SetTag set) {
    return [set](std::string_view key, py::handle value) {
        value_or_raise(set(key, value_text(value)));
    };
}

void register_tag_layers(py::module_& m) {
    // None of these waits for anything the dispatch thread holds, so they keep the GIL.
    m.def("set_thread_tag", setter(&set_thread_tag), py::arg("key"), py::arg("value"),
          "Set a tag on every record the calling thread produces from now on (below explicit "
          "and scope tags). The value is a str, bool, int or float. Raises TagTableFullError "
          "when the key is new and the thread already has its maximum number of tags.");
    m.def("remove_thread_tag", &remove_thread_tag, py::arg("key"),
          "Remove a tag of the calling thread. Returns False if it was not set.");
    m.def("clear_thread_tags", &clear_thread_tags, "Remove every tag of the calling thread.");
    m.def("set_process_tag", setter(&set_process_tag), py::arg("key"), py::arg("value"),
          "Set a tag on every record of every thread from now on (the lowest layer). The "
          "value is a str, bool, int or float. Raises TagTableFullError when the key is new "
          "and the table is full.");
    m.def("remove_process_tag", &remove_process_tag, py::arg("key"),
          "Remove a process tag. Returns False if it was not set.");
    m.def("clear_process_tags", &clear_process_tags, "Remove every process tag.");
}

}  // namespace

void register_context(py::module_& m) {
    register_tag_layers(m);

    py::class_<Scope>(m, "Scope",
                      "A tag that records of the current thread carry while a `with` block "
                      "runs; made by scope().")
        .def(
            "__enter__",
            [](Scope& s) -> Scope& {
                s.enter();
                return s;
            },
            py::return_value_policy::reference_internal)
        .def("__exit__", [](Scope& s, const py::args& /*exc_info*/) {
            s.exit();
            return false;
        });

    m.def(
        "scope",
        [](std::string key, py::handle value) { return Scope(std::move(key), value_text(value)); },
        py::arg("key"), py::arg("value"),
        "A context manager: `with lumen.scope('run', 7):` adds run=7 to every record the "
        "calling thread produces inside the block (above thread and process tags; the "
        "innermost scope wins a key). Scopes belong to a thread, not to an asyncio task or a "
        "generator: leaving a scope also closes every scope entered after it on the same "
        "thread, and leaving one on another thread raises RuntimeError. Entering a Scope "
        "object that is already entered raises ValueError.");
}

}  // namespace lumen::python
