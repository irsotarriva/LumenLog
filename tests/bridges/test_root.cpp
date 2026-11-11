// lumen/bridges/root.hpp against the real ROOT: the exact metrics and tags
// that reach a sink.
#include <gtest/gtest.h>

#include <TFile.h>
#include <TTree.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "lumen/bridges/root.hpp"
#include "lumen/macros.hpp"
#include "test_support.hpp"

namespace lumen {
namespace {

using bridges::emit_tree_summary;
using test::tags_of;
using Tags = std::vector<std::pair<std::string, std::string>>;

class RootBridgeTest : public test::LocalCoreTest {
protected:
    [[nodiscard]] std::vector<MetricRecord> _metrics() {
        _core.flush();
        return _captured->metrics();
    }
};

// A file in the temporary directory, removed when the test ends.
class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view name)
        : path_(std::filesystem::temp_directory_path() / name) {}
    ~TemporaryFile() {
        std::error_code ignored;  // a file that was never created is fine
        std::filesystem::remove(path_, ignored);
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;

    [[nodiscard]] std::string string() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

TEST_F(RootBridgeTest, TreeWrittenToAFileReportsItsEntriesAndBothByteCounts) {
    const TemporaryFile path("lumen_root_bridge_written.root");
    TFile file(path.string().c_str(), "RECREATE");
    ASSERT_FALSE(file.IsZombie());
    {
        // Destroyed before `file`: a TTree on the stack must leave its
        // directory before the TFile, which would otherwise delete it.
        TTree tree("events", "test events");
        double energy = 0.0;
        tree.Branch("energy", &energy, "energy/D");
        for (const double e : {1.0, 2.0, 3.0}) {
            energy = e;
            ASSERT_GT(tree.Fill(), 0);
        }
        ASSERT_GT(tree.Write(), 0);

        emit_tree_summary(tree, _core);

        const auto metrics = _metrics();
        ASSERT_EQ(metrics.size(), 3U);
        const Tags expected_tags = {{"tree", "events"}};
        EXPECT_EQ(metrics[0].name, "root.tree.entries");
        EXPECT_EQ(metrics[0].value, 3.0);
        EXPECT_EQ(tags_of(metrics[0].tags), expected_tags);
        EXPECT_EQ(metrics[1].name, "root.tree.uncompressed_bytes");
        EXPECT_EQ(metrics[1].value, static_cast<double>(tree.GetTotBytes()));
        EXPECT_GT(metrics[1].value, 0.0);
        EXPECT_EQ(tags_of(metrics[1].tags), expected_tags);
        EXPECT_EQ(metrics[2].name, "root.tree.compressed_bytes");
        EXPECT_EQ(metrics[2].value, static_cast<double>(tree.GetZipBytes()));
        EXPECT_GT(metrics[2].value, 0.0);
        EXPECT_EQ(tags_of(metrics[2].tags), expected_tags);
    }
}

TEST_F(RootBridgeTest, EmptyTreeReportsZeroEntriesAndZeroBytes) {
    const TTree tree("empty", "no entries");
    emit_tree_summary(tree, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 3U);
    EXPECT_EQ(metrics[0].value, 0.0);
    EXPECT_EQ(metrics[1].value, 0.0);
    EXPECT_EQ(metrics[2].value, 0.0);
    EXPECT_EQ(tags_of(metrics[0].tags), (Tags{{"tree", "empty"}}));
}

TEST_F(RootBridgeTest, EntriesAreCountedBeforeAnyBasketIsWritten) {
    TTree tree("memory", "memory-resident tree");
    int32_t hits = 0;
    tree.Branch("hits", &hits, "hits/I");
    for (int32_t i = 0; i < 5; ++i) {
        hits = i;
        ASSERT_GT(tree.Fill(), 0);
    }
    emit_tree_summary(tree, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 3U);
    EXPECT_EQ(metrics[0].value, 5.0);
    EXPECT_EQ(metrics[1].value, static_cast<double>(tree.GetTotBytes()));
    EXPECT_EQ(metrics[2].value, static_cast<double>(tree.GetZipBytes()));
}

TEST_F(RootBridgeTest, RecordsCarryTheAmbientScopeAfterTheTreeTag) {
    const TTree tree("scoped", "");
    LUMEN_SCOPE("run", "12");
    emit_tree_summary(tree, _core);

    const auto metrics = _metrics();
    ASSERT_EQ(metrics.size(), 3U);
    EXPECT_EQ(tags_of(metrics[0].tags), (Tags{{"tree", "scoped"}, {"run", "12"}}));
}

class RootBridgeGlobalCoreTest : public test::GlobalCoreTest {};

// The event loop in docs/bridges.md: LUMEN_LOOP over GetEntries() (a
// Long64_t) needs no bridge, and reports progress on the global core.
TEST_F(RootBridgeGlobalCoreTest, LumenLoopOverGetEntriesVisitsEveryEntryAndReportsProgress) {
    TTree tree("events", "");
    double energy = 0.0;
    tree.Branch("energy", &energy, "energy/D");
    for (const double e : {1.0, 2.0, 3.0}) {
        energy = e;
        ASSERT_GT(tree.Fill(), 0);
    }

    double sum = 0.0;
    LUMEN_LOOP("entry", i, tree.GetEntries()) {
        ASSERT_GT(tree.GetEntry(i), 0);
        sum += energy;
    }
    core().flush();

    EXPECT_EQ(sum, 6.0);
    const auto progress = _captured->progress();
    ASSERT_FALSE(progress.empty());
    EXPECT_EQ(progress.back().label, "entry");
    EXPECT_EQ(progress.back().current, 3U);
    EXPECT_EQ(progress.back().total, 3U);
}

TEST_F(RootBridgeGlobalCoreTest, WithoutACoreArgumentTheRecordsGoToTheGlobalCore) {
    const TTree tree("global", "");
    emit_tree_summary(tree);
    core().flush();

    const auto metrics = _captured->metrics();
    ASSERT_EQ(metrics.size(), 3U);
    EXPECT_EQ(metrics[0].name, "root.tree.entries");
    EXPECT_EQ(metrics[0].value, 0.0);
}

}  // namespace
}  // namespace lumen
