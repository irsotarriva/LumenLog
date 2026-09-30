#ifndef LUMEN_BRIDGES_EIGEN_H
#define LUMEN_BRIDGES_EIGEN_H

#if has_include_(<Eigen/Core>)

#include <Eigen/Core>
#include <chrono>
#include <string>

#include "lumen/lumen.h"

namespace lumen::bridges {

class EigenTimer {
public:
    explicit EigenTimer(std::string name)
        : name_(std::move(name))
        , start_(std::chrono::high_resolution_clock::now()) {}
    ~EigenTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        double us =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    end - start_).count());
        lumen::metric("eigen.op.duration_us", us)
            .tag("operation", name_);
    }

    EigenTimer(const EigenTimer&) = delete;
    EigenTimer& operator=(const EigenTimer&) = delete;

private:
    std::string name_;
    std::chrono::high_resolution_clock::time_point start_;
};

template <typename MatrixType>
void report_matrix_shape(const MatrixType& mat, std::string name) {
    lumen::metric("eigen.matrix.rows",
                  static_cast<double>(mat.rows()))
        .tag("matrix", name);
    lumen::metric("eigen.matrix.cols",
                  static_cast<double>(mat.cols()))
        .tag("matrix", name);
    lumen::metric("eigen.matrix.size",
                  static_cast<double>(mat.size()))
        .tag("matrix", name);
}

}  // namespace lumen::bridges

#endif

#endif
