//===-- ------------ Implementation of _tensor_impl module  ----*-C++-*-/===//
//
//                      Data Parallel Control (dpctl)
//
// Copyright 2020-2025 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===--------------------------------------------------------------------===//
///
/// \file
/// This file defines functions of dpctl.tensor._tensor_impl extensions
//===--------------------------------------------------------------------===//

#pragma once
#include <cstddef>
#include <sycl/sycl.hpp>
#include <utility>
#include <vector>

#include "dpctl4pybind11.hpp"
#include <pybind11/pybind11.h>

namespace dpctl
{
namespace tensor
{
namespace py_internal
{

extern void populate_mask_positions_dispatch_vectors(void);

extern std::size_t
py_mask_positions(const dpctl::tensor::usm_ndarray &mask,
                  const dpctl::tensor::usm_ndarray &cumsum,
                  sycl::queue &exec_q,
                  const std::vector<sycl::event> &depends = {});

extern void populate_cumsum_1d_dispatch_vectors(void);

extern std::size_t py_cumsum_1d(const dpctl::tensor::usm_ndarray &src,
                                const dpctl::tensor::usm_ndarray &cumsum,
                                sycl::queue &exec_q,
                                std::vector<sycl::event> const &depends = {});

} // namespace py_internal
} // namespace tensor
} // namespace dpctl
