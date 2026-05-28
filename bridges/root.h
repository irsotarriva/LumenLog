#ifndef LUMEN_BRIDGES_ROOT_H
#define LUMEN_BRIDGES_ROOT_H

#if __has_include(<TTree.h>)

#include <TTree.h>
#include <cstdint>
#include <string>

#include "lumen/lumen.h"

namespace lumen::bridges {

class RootTreeHook {
public:
    explicit RootTreeHook(TTree& tree)
        : __tree(tree)
        , __tree_name(tree.GetName())
        , __entries_at_start(static_cast<uint64_t>(tree.GetEntries())) {}

    void before_fill() {
        __fill_calls++;
        if (__fill_calls % 100 == 0) {
            lumen::metric("root.tree.entries",
                          static_cast<double>(__tree.GetEntries()))
                .tag("tree", __tree_name);
        }
    }

    void after_write() {
        lumen::metric("root.tree.bytes_written",
                      static_cast<double>(__tree.GetTotBytes()))
            .tag("tree", __tree_name);
    }

    void finalize() {
        uint64_t total_entries = static_cast<uint64_t>(__tree.GetEntries()) - __entries_at_start;
        lumen::metric("root.tree.total_entries",
                      static_cast<double>(total_entries))
            .tag("tree", __tree_name);
        lumen::metric("root.tree.bytes_total",
                      static_cast<double>(__tree.GetTotBytes()))
            .tag("tree", __tree_name);
    }

private:
    TTree&       __tree;
    std::string  __tree_name;
    uint64_t     __entries_at_start;
    uint64_t     __fill_calls = 0;
};

}  // namespace lumen::bridges

#endif

#endif
