#ifndef LUMEN_BRIDGES_ROOT_H
#define LUMEN_BRIDGES_ROOT_H

#if has_include_(<TTree.h>)

#include <TTree.h>
#include <cstdint>
#include <string>

#include "lumen/lumen.h"

namespace lumen::bridges {

class RootTreeHook {
public:
    explicit RootTreeHook(TTree& tree)
        : tree_(tree)
        , tree_name_(tree.GetName())
        , entries_at_start_(static_cast<uint64_t>(tree.GetEntries())) {}

    void before_fill() {
        fill_calls_++;
        if (fill_calls_ % 100 == 0) {
            lumen::metric("root.tree.entries",
                          static_cast<double>(tree_.GetEntries()))
                .tag("tree", tree_name_);
        }
    }

    void after_write() {
        lumen::metric("root.tree.bytes_written",
                      static_cast<double>(tree_.GetTotBytes()))
            .tag("tree", tree_name_);
    }

    void finalize() {
        uint64_t total_entries = static_cast<uint64_t>(tree_.GetEntries()) - entries_at_start_;
        lumen::metric("root.tree.total_entries",
                      static_cast<double>(total_entries))
            .tag("tree", tree_name_);
        lumen::metric("root.tree.bytes_total",
                      static_cast<double>(tree_.GetTotBytes()))
            .tag("tree", tree_name_);
    }

private:
    TTree&       tree_;
    std::string  tree_name_;
    uint64_t     entries_at_start_;
    uint64_t     fill_calls_ = 0;
};

}  // namespace lumen::bridges

#endif

#endif
