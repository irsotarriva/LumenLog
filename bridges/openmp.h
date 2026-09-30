#ifndef LUMEN_BRIDGES_OPENMP_H
#define LUMEN_BRIDGES_OPENMP_H

#ifdef _OPENMP

#include <omp.h>
#include <chrono>
#include <string>

#include "lumen/lumen.h"

namespace lumen::bridges {

inline void openmp_emit_thread_metrics() {
    #pragma omp parallel
    {
        #pragma omp single
        {
            int num_threads = omp_get_num_threads();
            int max_threads = omp_get_max_threads();
            lumen::metric("openmp.num_threads",
                          static_cast<double>(num_threads));
            lumen::metric("openmp.max_threads",
                          static_cast<double>(max_threads));
        }
    }
}

class OmpParallelTimer {
public:
    explicit OmpParallelTimer(std::string label)
        : label_(std::move(label))
        , start_(std::chrono::high_resolution_clock::now()) {}
    ~OmpParallelTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        double us =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    end - start_).count());
        lumen::metric("openmp.parallel_duration_us", us)
            .tag("region", label_);
    }

    OmpParallelTimer(const OmpParallelTimer&) = delete;
    OmpParallelTimer& operator=(const OmpParallelTimer&) = delete;

private:
    std::string label_;
    std::chrono::high_resolution_clock::time_point start_;
};

inline void openmp_emit_loop_metrics(std::string label,
                                     int num_iterations,
                                     int chunk_size = 0) {
    lumen::metric("openmp.loop.iterations",
                  static_cast<double>(num_iterations))
        .tag("label", label);
    int actual_chunk = chunk_size > 0 ? chunk_size
                                      : num_iterations / omp_get_max_threads();
    lumen::metric("openmp.loop.chunk_size",
                  static_cast<double>(actual_chunk))
        .tag("label", label);
}

}  // namespace lumen::bridges

#endif

#endif
