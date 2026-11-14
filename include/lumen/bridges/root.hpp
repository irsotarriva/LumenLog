#ifndef LUMEN_BRIDGES_ROOT_HPP
#define LUMEN_BRIDGES_ROOT_HPP

// ROOT helper. Nothing here hooks TTree::Fill or any other ROOT call: the
// function emits records only when the program calls it, typically after
// writing a tree, or every so many entries while filling one.
//
//   emit_tree_summary(tree)   entries and stored size of a TTree
//
// An event loop needs no bridge for progress: Long64_t is an integer type, so
//   LUMEN_LOOP("entry", i, tree.GetEntries()) { tree.GetEntry(i); ... }
// already reports progress and tags each record with entry=<i>.
//
// The header is empty unless <TTree.h> can be included.

#if __has_include(<TTree.h>)

#include <TTree.h>

#include <string_view>

#include "lumen/core.hpp"
#include "lumen/record_builder.hpp"

namespace lumen::bridges {

// Emits three metrics, each tagged tree=<tree.GetName()>:
//   root.tree.entries             tree.GetEntries()
//   root.tree.uncompressed_bytes  tree.GetTotBytes(), in bytes
//   root.tree.compressed_bytes    tree.GetZipBytes(), in bytes
// ROOT adds a basket to both byte counts when it writes the basket out, so
// while a tree is being filled they lag behind the entry count; after
// tree.Write() (or TTree::FlushBaskets()) on a tree attached to a file they
// describe every entry.
inline void emit_tree_summary(const TTree& tree, Core& core = lumen::core()) {
    const std::string_view name = tree.GetName();
    MetricBuilder(core, "root.tree.entries", static_cast<double>(tree.GetEntries()))
        .tag("tree", name);
    MetricBuilder(core, "root.tree.uncompressed_bytes", static_cast<double>(tree.GetTotBytes()))
        .tag("tree", name);
    MetricBuilder(core, "root.tree.compressed_bytes", static_cast<double>(tree.GetZipBytes()))
        .tag("tree", name);
}

}  // namespace lumen::bridges

#endif  // __has_include(<TTree.h>)

#endif
