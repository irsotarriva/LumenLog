#ifndef LUMEN_BRIDGES_EIGEN_H
#define LUMEN_BRIDGES_EIGEN_H

#if __has_include(<Eigen/Core>)

#include <Eigen/Core>
#include <chrono>
#include <string>

#include "lumen/lumen.h"

namespace lumen::bridges {

class EigenTimer {
public:
    explicit EigenTimer(std::string name)
        : __name(std::move(name))
        , __start(std::chrono::high_resolution_clock::now()) {}
    ~EigenTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        double us =
            static_cast<double>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    end - __start).count());
        lumen::metric("eigen.op.duration_us", us)
            .tag("operation", __name);
    }

    EigenTimer(const EigenTimer&) = delete;
    EigenTimer& operator=(const EigenTimer&) = delete;

private:
    std::string __name;
    std::chrono::high_resolution_clock::time_point __start;
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
