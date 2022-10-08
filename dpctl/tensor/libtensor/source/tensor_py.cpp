//===-- tensor_py.cpp - Implementation of _tensor_impl module  --*-C++-*-/===//
//
//                      Data Parallel Control (dpctl)
//
// Copyright 2020-2022 Intel Corporation
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
//===----------------------------------------------------------------------===//
///
/// \file
/// This file defines functions of dpctl.tensor._tensor_impl extensions
//===----------------------------------------------------------------------===//

#include <CL/sycl.hpp>
#include <algorithm>
#include <complex>
#include <cstdint>
#include <pybind11/complex.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <thread>
#include <type_traits>

#include "dpctl4pybind11.hpp"
#include "kernels/constructors.hpp"
#include "kernels/copy_and_cast.hpp"
#include "utils/strided_iters.hpp"
#include "utils/type_dispatch.hpp"
#include "utils/type_utils.hpp"

namespace py = pybind11;

namespace
{

using dpctl::tensor::c_contiguous_strides;
using dpctl::tensor::f_contiguous_strides;
using dpctl::tensor::kernels::copy_and_cast::copy_and_cast_1d_fn_ptr_t;
using dpctl::tensor::kernels::copy_and_cast::copy_and_cast_2d_fn_ptr_t;
using dpctl::tensor::kernels::copy_and_cast::copy_and_cast_generic_fn_ptr_t;

namespace _ns = dpctl::tensor::detail;

static copy_and_cast_generic_fn_ptr_t
    copy_and_cast_generic_dispatch_table[_ns::num_types][_ns::num_types];
static copy_and_cast_1d_fn_ptr_t
    copy_and_cast_1d_dispatch_table[_ns::num_types][_ns::num_types];
static copy_and_cast_2d_fn_ptr_t
    copy_and_cast_2d_dispatch_table[_ns::num_types][_ns::num_types];

using dpctl::utils::keep_args_alive;

void simplify_iteration_space(int &nd,
                              const py::ssize_t *&shape,
                              const py::ssize_t *&src_strides,
                              py::ssize_t src_itemsize,
                              bool is_src_c_contig,
                              bool is_src_f_contig,
                              const py::ssize_t *&dst_strides,
                              py::ssize_t dst_itemsize,
                              bool is_dst_c_contig,
                              bool is_dst_f_contig,
                              std::vector<py::ssize_t> &simplified_shape,
                              std::vector<py::ssize_t> &simplified_src_strides,
                              std::vector<py::ssize_t> &simplified_dst_strides,
                              py::ssize_t &src_offset,
                              py::ssize_t &dst_offset)
{
    if (nd > 1) {
        // Simplify iteration space to reduce dimensionality
        // and improve access pattern
        simplified_shape.reserve(nd);
        for (int i = 0; i < nd; ++i) {
            simplified_shape.push_back(shape[i]);
        }

        simplified_src_strides.reserve(nd);
        simplified_dst_strides.reserve(nd);
        if (src_strides == nullptr) {
            if (is_src_c_contig) {
                simplified_src_strides =
                    c_contiguous_strides(nd, shape, src_itemsize);
            }
            else if (is_src_f_contig) {
                simplified_src_strides =
                    f_contiguous_strides(nd, shape, src_itemsize);
            }
            else {
                throw std::runtime_error(
                    "Source array has null strides "
                    "but has neither C- nor F- contiguous flag set");
            }
        }
        else {
            for (int i = 0; i < nd; ++i) {
                simplified_src_strides.push_back(src_strides[i]);
            }
        }
        if (dst_strides == nullptr) {
            if (is_dst_c_contig) {
                simplified_dst_strides =
                    c_contiguous_strides(nd, shape, dst_itemsize);
            }
            else if (is_dst_f_contig) {
                simplified_dst_strides =
                    f_contiguous_strides(nd, shape, dst_itemsize);
            }
            else {
                throw std::runtime_error(
                    "Destination array has null strides "
                    "but has neither C- nor F- contiguous flag set");
            }
        }
        else {
            for (int i = 0; i < nd; ++i) {
                simplified_dst_strides.push_back(dst_strides[i]);
            }
        }

        assert(simplified_shape.size() == static_cast<size_t>(nd));
        assert(simplified_src_strides.size() == static_cast<size_t>(nd));
        assert(simplified_dst_strides.size() == static_cast<size_t>(nd));
        int contracted_nd = simplify_iteration_two_strides(
            nd, simplified_shape.data(), simplified_src_strides.data(),
            simplified_dst_strides.data(),
            src_offset, // modified by reference
            dst_offset  // modified by reference
        );
        simplified_shape.resize(contracted_nd);
        simplified_src_strides.resize(contracted_nd);
        simplified_dst_strides.resize(contracted_nd);

        nd = contracted_nd;
        shape = const_cast<const py::ssize_t *>(simplified_shape.data());
        src_strides =
            const_cast<const py::ssize_t *>(simplified_src_strides.data());
        dst_strides =
            const_cast<const py::ssize_t *>(simplified_dst_strides.data());
    }
    else if (nd == 1) {
        // Populate vectors
        simplified_shape.reserve(nd);
        simplified_shape.push_back(shape[0]);

        simplified_src_strides.reserve(nd);
        simplified_dst_strides.reserve(nd);

        if (src_strides == nullptr) {
            if (is_src_c_contig) {
                simplified_src_strides.push_back(src_itemsize);
            }
            else if (is_src_f_contig) {
                simplified_src_strides.push_back(src_itemsize);
            }
            else {
                throw std::runtime_error(
                    "Source array has null strides "
                    "but has neither C- nor F- contiguous flag set");
            }
        }
        else {
            simplified_src_strides.push_back(src_strides[0]);
        }
        if (dst_strides == nullptr) {
            if (is_dst_c_contig) {
                simplified_dst_strides.push_back(dst_itemsize);
            }
            else if (is_dst_f_contig) {
                simplified_dst_strides.push_back(dst_itemsize);
            }
            else {
                throw std::runtime_error(
                    "Destination array has null strides "
                    "but has neither C- nor F- contiguous flag set");
            }
        }
        else {
            simplified_dst_strides.push_back(dst_strides[0]);
        }

        assert(simplified_shape.size() == static_cast<size_t>(nd));
        assert(simplified_src_strides.size() == static_cast<size_t>(nd));
        assert(simplified_dst_strides.size() == static_cast<size_t>(nd));
    }
}

sycl::event _populate_packed_shape_strides_for_copycast_kernel(
    sycl::queue exec_q,
    py::ssize_t *device_shape_strides, // to be populated
    const std::vector<py::ssize_t> &common_shape,
    const std::vector<py::ssize_t> &src_strides,
    const std::vector<py::ssize_t> &dst_strides)
{
    // memory transfer optimization, use USM-host for temporary speeds up
    // tranfer to device, especially on dGPUs
    using usm_host_allocatorT =
        sycl::usm_allocator<py::ssize_t, sycl::usm::alloc::host>;
    using shT = std::vector<py::ssize_t, usm_host_allocatorT>;
    size_t nd = common_shape.size();

    usm_host_allocatorT allocator(exec_q);

    // create host temporary for packed shape and strides managed by shared
    // pointer. Packed vector is concatenation of common_shape, src_stride and
    // std_strides
    std::shared_ptr<shT> shp_host_shape_strides =
        std::make_shared<shT>(3 * nd, allocator);
    std::copy(common_shape.begin(), common_shape.end(),
              shp_host_shape_strides->begin());

    std::copy(src_strides.begin(), src_strides.end(),
              shp_host_shape_strides->begin() + nd);

    std::copy(dst_strides.begin(), dst_strides.end(),
              shp_host_shape_strides->begin() + 2 * nd);

    sycl::event copy_shape_ev = exec_q.copy<py::ssize_t>(
        shp_host_shape_strides->data(), device_shape_strides,
        shp_host_shape_strides->size());

    exec_q.submit([&](sycl::handler &cgh) {
        cgh.depends_on(copy_shape_ev);
        cgh.host_task([shp_host_shape_strides]() {
            // increment shared pointer ref-count to keep it alive
            // till copy operation completes;
        });
    });

    return copy_shape_ev;
}

std::pair<sycl::event, sycl::event>
copy_usm_ndarray_into_usm_ndarray(dpctl::tensor::usm_ndarray src,
                                  dpctl::tensor::usm_ndarray dst,
                                  sycl::queue exec_q,
                                  const std::vector<sycl::event> &depends = {})
{

    // array dimensions must be the same
    int src_nd = src.get_ndim();
    int dst_nd = dst.get_ndim();
    if (src_nd != dst_nd) {
        throw py::value_error("Array dimensions are not the same.");
    }

    // shapes must be the same
    const py::ssize_t *src_shape = src.get_shape_raw();
    const py::ssize_t *dst_shape = dst.get_shape_raw();

    bool shapes_equal(true);
    size_t src_nelems(1);

    for (int i = 0; i < src_nd; ++i) {
        src_nelems *= static_cast<size_t>(src_shape[i]);
        shapes_equal = shapes_equal && (src_shape[i] == dst_shape[i]);
    }
    if (!shapes_equal) {
        throw py::value_error("Array shapes are not the same.");
    }

    if (src_nelems == 0) {
        // nothing to do
        return std::make_pair(sycl::event(), sycl::event());
    }

    auto dst_offsets = dst.get_minmax_offsets();
    // destination must be ample enough to accomodate all elements
    {
        size_t range =
            static_cast<size_t>(dst_offsets.second - dst_offsets.first);
        if (range + 1 < src_nelems) {
            throw py::value_error(
                "Destination array can not accomodate all the "
                "elements of source array.");
        }
    }

    // check compatibility of execution queue and allocation queue
    sycl::queue src_q = src.get_queue();
    sycl::queue dst_q = dst.get_queue();

    if (!dpctl::utils::queues_are_compatible(exec_q, {src_q, dst_q})) {
        throw py::value_error(
            "Execution queue is not compatible with allocation queues");
    }

    int src_typenum = src.get_typenum();
    int dst_typenum = dst.get_typenum();

    auto array_types = dpctl::tensor::detail::usm_ndarray_types();
    int src_type_id = array_types.typenum_to_lookup_id(src_typenum);
    int dst_type_id = array_types.typenum_to_lookup_id(dst_typenum);

    char *src_data = src.get_data();
    char *dst_data = dst.get_data();

    // check that arrays do not overlap, and concurrent copying is safe.
    auto src_offsets = src.get_minmax_offsets();
    int src_elem_size = src.get_elemsize();
    int dst_elem_size = dst.get_elemsize();

    bool memory_overlap =
        ((dst_data - src_data > src_offsets.second * src_elem_size -
                                    dst_offsets.first * dst_elem_size) &&
         (src_data - dst_data > dst_offsets.second * dst_elem_size -
                                    src_offsets.first * src_elem_size));
    if (memory_overlap) {
        // TODO: could use a temporary, but this is done by the caller
        throw py::value_error("Arrays index overlapping segments of memory");
    }

    bool is_src_c_contig = src.is_c_contiguous();
    bool is_src_f_contig = src.is_f_contiguous();

    bool is_dst_c_contig = dst.is_c_contiguous();
    bool is_dst_f_contig = dst.is_f_contiguous();

    // check for applicability of special cases:
    //      (same type && (both C-contiguous || both F-contiguous)
    bool both_c_contig = (is_src_c_contig && is_dst_c_contig);
    bool both_f_contig = (is_src_f_contig && is_dst_f_contig);
    if (both_c_contig || both_f_contig) {
        if (src_type_id == dst_type_id) {

            sycl::event copy_ev =
                exec_q.memcpy(static_cast<void *>(dst_data),
                              static_cast<const void *>(src_data),
                              src_nelems * src_elem_size, depends);

            // make sure src and dst are not GC-ed before copy_ev is complete
            return std::make_pair(
                keep_args_alive(exec_q, {src, dst}, {copy_ev}), copy_ev);
        }
        // With contract_iter2 in place, there is no need to write
        // dedicated kernels for casting between contiguous arrays
    }

    const py::ssize_t *src_strides = src.get_strides_raw();
    const py::ssize_t *dst_strides = dst.get_strides_raw();

    using shT = std::vector<py::ssize_t>;
    shT simplified_shape;
    shT simplified_src_strides;
    shT simplified_dst_strides;
    py::ssize_t src_offset(0);
    py::ssize_t dst_offset(0);

    int nd = src_nd;
    const py::ssize_t *shape = src_shape;

    constexpr py::ssize_t src_itemsize = 1; // in elements
    constexpr py::ssize_t dst_itemsize = 1; // in elements

    // all args except itemsizes and is_?_contig bools can be modified by
    // reference
    simplify_iteration_space(nd, shape, src_strides, src_itemsize,
                             is_src_c_contig, is_src_f_contig, dst_strides,
                             dst_itemsize, is_dst_c_contig, is_dst_f_contig,
                             simplified_shape, simplified_src_strides,
                             simplified_dst_strides, src_offset, dst_offset);

    if (nd < 3) {
        if (nd == 1) {
            std::array<py::ssize_t, 1> shape_arr = {shape[0]};
            // strides may be null
            std::array<py::ssize_t, 1> src_strides_arr = {
                (src_strides ? src_strides[0] : 1)};
            std::array<py::ssize_t, 1> dst_strides_arr = {
                (dst_strides ? dst_strides[0] : 1)};

            auto fn = copy_and_cast_1d_dispatch_table[dst_type_id][src_type_id];
            sycl::event copy_and_cast_1d_event = fn(
                exec_q, src_nelems, shape_arr, src_strides_arr, dst_strides_arr,
                src_data, src_offset, dst_data, dst_offset, depends);

            return std::make_pair(
                keep_args_alive(exec_q, {src, dst}, {copy_and_cast_1d_event}),
                copy_and_cast_1d_event);
        }
        else if (nd == 2) {
            std::array<py::ssize_t, 2> shape_arr = {shape[0], shape[1]};
            std::array<py::ssize_t, 2> src_strides_arr = {src_strides[0],
                                                          src_strides[1]};
            std::array<py::ssize_t, 2> dst_strides_arr = {dst_strides[0],
                                                          dst_strides[1]};

            auto fn = copy_and_cast_2d_dispatch_table[dst_type_id][src_type_id];
            sycl::event copy_and_cast_2d_event = fn(
                exec_q, src_nelems, shape_arr, src_strides_arr, dst_strides_arr,
                src_data, src_offset, dst_data, dst_offset, depends);

            return std::make_pair(
                keep_args_alive(exec_q, {src, dst}, {copy_and_cast_2d_event}),
                copy_and_cast_2d_event);
        }
        else if (nd == 0) { // case of a scalar
            assert(src_nelems == 1);
            std::array<py::ssize_t, 1> shape_arr = {1};
            std::array<py::ssize_t, 1> src_strides_arr = {1};
            std::array<py::ssize_t, 1> dst_strides_arr = {1};

            auto fn = copy_and_cast_1d_dispatch_table[dst_type_id][src_type_id];
            sycl::event copy_and_cast_0d_event = fn(
                exec_q, src_nelems, shape_arr, src_strides_arr, dst_strides_arr,
                src_data, src_offset, dst_data, dst_offset, depends);

            return std::make_pair(
                keep_args_alive(exec_q, {src, dst}, {copy_and_cast_0d_event}),
                copy_and_cast_0d_event);
        }
    }

    // Generic implementation
    auto copy_and_cast_fn =
        copy_and_cast_generic_dispatch_table[dst_type_id][src_type_id];

    //   If shape/strides are accessed with accessors, buffer destructor
    //   will force syncronization.
    py::ssize_t *shape_strides =
        sycl::malloc_device<py::ssize_t>(3 * nd, exec_q);

    if (shape_strides == nullptr) {
        throw std::runtime_error("Unabled to allocate device memory");
    }

    sycl::event copy_shape_ev =
        _populate_packed_shape_strides_for_copycast_kernel(
            exec_q, shape_strides, simplified_shape, simplified_src_strides,
            simplified_dst_strides);

    sycl::event copy_and_cast_generic_ev = copy_and_cast_fn(
        exec_q, src_nelems, nd, shape_strides, src_data, src_offset, dst_data,
        dst_offset, depends, {copy_shape_ev});

    // async free of shape_strides temporary
    auto ctx = exec_q.get_context();
    exec_q.submit([&](sycl::handler &cgh) {
        cgh.depends_on(copy_and_cast_generic_ev);
        cgh.host_task(
            [ctx, shape_strides]() { sycl::free(shape_strides, ctx); });
    });

    return std::make_pair(
        keep_args_alive(exec_q, {src, dst}, {copy_and_cast_generic_ev}),
        copy_and_cast_generic_ev);
}

/* =========================== Copy for reshape ============================= */

using dpctl::tensor::kernels::copy_and_cast::copy_for_reshape_fn_ptr_t;

// define static vector
static copy_for_reshape_fn_ptr_t
    copy_for_reshape_generic_dispatch_vector[_ns::num_types];

/*
 * Copies src into dst (same data type) of different shapes by using flat
 * iterations.
 *
 * Equivalent to the following loop:
 *
 * for i for range(src.size):
 *     dst[np.multi_index(i, dst.shape)] = src[np.multi_index(i, src.shape)]
 */
std::pair<sycl::event, sycl::event>
copy_usm_ndarray_for_reshape(dpctl::tensor::usm_ndarray src,
                             dpctl::tensor::usm_ndarray dst,
                             py::ssize_t shift,
                             sycl::queue exec_q,
                             const std::vector<sycl::event> &depends = {})
{
    py::ssize_t src_nelems = src.get_size();
    py::ssize_t dst_nelems = dst.get_size();

    // Must have the same number of elements
    if (src_nelems != dst_nelems) {
        throw py::value_error(
            "copy_usm_ndarray_for_reshape requires src and dst to "
            "have the same number of elements.");
    }

    int src_typenum = src.get_typenum();
    int dst_typenum = dst.get_typenum();

    // typenames must be the same
    if (src_typenum != dst_typenum) {
        throw py::value_error(
            "copy_usm_ndarray_for_reshape requires src and dst to "
            "have the same type.");
    }

    if (src_nelems == 0) {
        return std::make_pair(sycl::event(), sycl::event());
    }

    // destination must be ample enough to accomodate all elements
    {
        auto dst_offsets = dst.get_minmax_offsets();
        py::ssize_t range =
            static_cast<py::ssize_t>(dst_offsets.second - dst_offsets.first);
        if (range + 1 < src_nelems) {
            throw py::value_error(
                "Destination array can not accomodate all the "
                "elements of source array.");
        }
    }

    // check same contexts
    sycl::queue src_q = src.get_queue();
    sycl::queue dst_q = dst.get_queue();

    if (!dpctl::utils::queues_are_compatible(exec_q, {src_q, dst_q})) {
        throw py::value_error(
            "Execution queue is not compatible with allocation queues");
    }

    if (src_nelems == 1) {
        // handle special case of 1-element array
        int src_elemsize = src.get_elemsize();
        char *src_data = src.get_data();
        char *dst_data = dst.get_data();
        sycl::event copy_ev =
            exec_q.copy<char>(src_data, dst_data, src_elemsize);
        return std::make_pair(keep_args_alive(exec_q, {src, dst}, {copy_ev}),
                              copy_ev);
    }

    // dimensions may be different
    int src_nd = src.get_ndim();
    int dst_nd = dst.get_ndim();

    const py::ssize_t *src_shape = src.get_shape_raw();
    const py::ssize_t *dst_shape = dst.get_shape_raw();

    auto array_types = dpctl::tensor::detail::usm_ndarray_types();
    int type_id = array_types.typenum_to_lookup_id(src_typenum);

    auto fn = copy_for_reshape_generic_dispatch_vector[type_id];

    // packed_shape_strides = [src_shape, src_strides, dst_shape, dst_strides]
    py::ssize_t *packed_shapes_strides =
        sycl::malloc_device<py::ssize_t>(2 * (src_nd + dst_nd), exec_q);

    if (packed_shapes_strides == nullptr) {
        throw std::runtime_error("Unabled to allocate device memory");
    }

    using usm_host_allocatorT =
        sycl::usm_allocator<py::ssize_t, sycl::usm::alloc::host>;
    using shT = std::vector<py::ssize_t, usm_host_allocatorT>;
    usm_host_allocatorT allocator(exec_q);
    std::shared_ptr<shT> packed_host_shapes_strides_shp =
        std::make_shared<shT>(2 * (src_nd + dst_nd), allocator);

    std::copy(src_shape, src_shape + src_nd,
              packed_host_shapes_strides_shp->begin());
    std::copy(dst_shape, dst_shape + dst_nd,
              packed_host_shapes_strides_shp->begin() + 2 * src_nd);

    const py::ssize_t *src_strides = src.get_strides_raw();
    if (src_strides == nullptr) {
        if (src.is_c_contiguous()) {
            const auto &src_contig_strides =
                c_contiguous_strides(src_nd, src_shape);
            std::copy(src_contig_strides.begin(), src_contig_strides.end(),
                      packed_host_shapes_strides_shp->begin() + src_nd);
        }
        else if (src.is_f_contiguous()) {
            const auto &src_contig_strides =
                f_contiguous_strides(src_nd, src_shape);
            std::copy(src_contig_strides.begin(), src_contig_strides.end(),
                      packed_host_shapes_strides_shp->begin() + src_nd);
        }
        else {
            sycl::free(packed_shapes_strides, exec_q);
            throw std::runtime_error(
                "Invalid src array encountered: in copy_for_reshape function");
        }
    }
    else {
        std::copy(src_strides, src_strides + src_nd,
                  packed_host_shapes_strides_shp->begin() + src_nd);
    }

    const py::ssize_t *dst_strides = dst.get_strides_raw();
    if (dst_strides == nullptr) {
        if (dst.is_c_contiguous()) {
            const auto &dst_contig_strides =
                c_contiguous_strides(dst_nd, dst_shape);
            std::copy(dst_contig_strides.begin(), dst_contig_strides.end(),
                      packed_host_shapes_strides_shp->begin() + 2 * src_nd +
                          dst_nd);
        }
        else if (dst.is_f_contiguous()) {
            const auto &dst_contig_strides =
                f_contiguous_strides(dst_nd, dst_shape);
            std::copy(dst_contig_strides.begin(), dst_contig_strides.end(),
                      packed_host_shapes_strides_shp->begin() + 2 * src_nd +
                          dst_nd);
        }
        else {
            sycl::free(packed_shapes_strides, exec_q);
            throw std::runtime_error(
                "Invalid dst array encountered: in copy_for_reshape function");
        }
    }
    else {
        std::copy(dst_strides, dst_strides + dst_nd,
                  packed_host_shapes_strides_shp->begin() + 2 * src_nd +
                      dst_nd);
    }

    // copy packed shapes and strides from host to devices
    sycl::event packed_shape_strides_copy_ev = exec_q.copy<py::ssize_t>(
        packed_host_shapes_strides_shp->data(), packed_shapes_strides,
        packed_host_shapes_strides_shp->size());
    exec_q.submit([&](sycl::handler &cgh) {
        cgh.depends_on(packed_shape_strides_copy_ev);
        cgh.host_task([packed_host_shapes_strides_shp] {
            // Capturing shared pointer ensures that the underlying vector is
            // not destroyed until after its data are copied into packed USM
            // vector
        });
    });

    char *src_data = src.get_data();
    char *dst_data = dst.get_data();

    std::vector<sycl::event> all_deps(depends.size() + 1);
    all_deps.push_back(packed_shape_strides_copy_ev);
    all_deps.insert(std::end(all_deps), std::begin(depends), std::end(depends));

    sycl::event copy_for_reshape_event =
        fn(exec_q, shift, src_nelems, src_nd, dst_nd, packed_shapes_strides,
           src_data, dst_data, all_deps);

    exec_q.submit([&](sycl::handler &cgh) {
        cgh.depends_on(copy_for_reshape_event);
        auto ctx = exec_q.get_context();
        cgh.host_task([packed_shapes_strides, ctx]() {
            sycl::free(packed_shapes_strides, ctx);
        });
    });

    return std::make_pair(
        keep_args_alive(exec_q, {src, dst}, {copy_for_reshape_event}),
        copy_for_reshape_event);
}

/* ============= Copy from numpy.ndarray to usm_ndarray ==================== */

using dpctl::tensor::kernels::copy_and_cast::
    copy_and_cast_from_host_blocking_fn_ptr_t;

static copy_and_cast_from_host_blocking_fn_ptr_t
    copy_and_cast_from_host_blocking_dispatch_table[_ns::num_types]
                                                   [_ns::num_types];

void copy_numpy_ndarray_into_usm_ndarray(
    py::array npy_src,
    dpctl::tensor::usm_ndarray dst,
    sycl::queue exec_q,
    const std::vector<sycl::event> &depends = {})
{
    int src_ndim = npy_src.ndim();
    int dst_ndim = dst.get_ndim();

    if (src_ndim != dst_ndim) {
        throw py::value_error("Source ndarray and destination usm_ndarray have "
                              "different array ranks, "
                              "i.e. different number of indices needed to "
                              "address array elements.");
    }

    const py::ssize_t *src_shape = npy_src.shape();
    const py::ssize_t *dst_shape = dst.get_shape_raw();
    bool shapes_equal(true);
    size_t src_nelems(1);
    for (int i = 0; i < src_ndim; ++i) {
        shapes_equal = shapes_equal && (src_shape[i] == dst_shape[i]);
        src_nelems *= static_cast<size_t>(src_shape[i]);
    }

    if (!shapes_equal) {
        throw py::value_error("Source ndarray and destination usm_ndarray have "
                              "difference shapes.");
    }

    if (src_nelems == 0) {
        // nothing to do
        return;
    }

    auto dst_offsets = dst.get_minmax_offsets();
    // destination must be ample enough to accomodate all elements of source
    // array
    {
        size_t range =
            static_cast<size_t>(dst_offsets.second - dst_offsets.first);
        if (range + 1 < src_nelems) {
            throw py::value_error(
                "Destination array can not accomodate all the "
                "elements of source array.");
        }
    }

    sycl::queue dst_q = dst.get_queue();

    if (!dpctl::utils::queues_are_compatible(exec_q, {dst_q})) {
        throw py::value_error("Execution queue is not compatible with the "
                              "allocation queue");
    }

    // here we assume that NumPy's type numbers agree with ours for types
    // supported in both
    int src_typenum =
        py::detail::array_descriptor_proxy(npy_src.dtype().ptr())->type_num;
    int dst_typenum = dst.get_typenum();

    auto array_types = dpctl::tensor::detail::usm_ndarray_types();
    int src_type_id = array_types.typenum_to_lookup_id(src_typenum);
    int dst_type_id = array_types.typenum_to_lookup_id(dst_typenum);

    py::buffer_info src_pybuf = npy_src.request();
    const char *const src_data = static_cast<const char *const>(src_pybuf.ptr);
    char *dst_data = dst.get_data();

    int src_flags = npy_src.flags();

    // check for applicability of special cases:
    //      (same type && (both C-contiguous || both F-contiguous)
    bool both_c_contig =
        ((src_flags & py::array::c_style) && dst.is_c_contiguous());
    bool both_f_contig =
        ((src_flags & py::array::f_style) && dst.is_f_contiguous());
    if (both_c_contig || both_f_contig) {
        if (src_type_id == dst_type_id) {
            int src_elem_size = npy_src.itemsize();

            sycl::event copy_ev =
                exec_q.memcpy(static_cast<void *>(dst_data),
                              static_cast<const void *>(src_data),
                              src_nelems * src_elem_size, depends);

            // wait for copy_ev to complete
            copy_ev.wait_and_throw();

            return;
        }
        // With contract_iter2 in place, there is no need to write
        // dedicated kernels for casting between contiguous arrays
    }

    const py::ssize_t *src_strides =
        npy_src.strides(); // N.B.: strides in bytes
    const py::ssize_t *dst_strides =
        dst.get_strides_raw(); // N.B.: strides in elements

    using shT = std::vector<py::ssize_t>;
    shT simplified_shape;
    shT simplified_src_strides;
    shT simplified_dst_strides;
    py::ssize_t src_offset(0);
    py::ssize_t dst_offset(0);

    py::ssize_t src_itemsize = npy_src.itemsize(); // item size in bytes
    constexpr py::ssize_t dst_itemsize = 1;        // item size in elements

    int nd = src_ndim;
    const py::ssize_t *shape = src_shape;

    bool is_src_c_contig = ((src_flags & py::array::c_style) != 0);
    bool is_src_f_contig = ((src_flags & py::array::f_style) != 0);

    bool is_dst_c_contig = dst.is_c_contiguous();
    bool is_dst_f_contig = dst.is_f_contiguous();

    // all args except itemsizes and is_?_contig bools can be modified by
    // reference
    simplify_iteration_space(nd, shape, src_strides, src_itemsize,
                             is_src_c_contig, is_src_f_contig, dst_strides,
                             dst_itemsize, is_dst_c_contig, is_dst_f_contig,
                             simplified_shape, simplified_src_strides,
                             simplified_dst_strides, src_offset, dst_offset);

    assert(simplified_shape.size() == static_cast<size_t>(nd));
    assert(simplified_src_strides.size() == static_cast<size_t>(nd));
    assert(simplified_dst_strides.size() == static_cast<size_t>(nd));

    // handle nd == 0
    if (nd == 0) {
        nd = 1;
        simplified_shape.reserve(nd);
        simplified_shape.push_back(1);

        simplified_src_strides.reserve(nd);
        simplified_src_strides.push_back(src_itemsize);

        simplified_dst_strides.reserve(nd);
        simplified_dst_strides.push_back(dst_itemsize);
    }

    // Minumum and maximum element offsets for source np.ndarray
    py::ssize_t npy_src_min_nelem_offset(0);
    py::ssize_t npy_src_max_nelem_offset(0);
    for (int i = 0; i < nd; ++i) {
        // convert source strides from bytes to elements
        simplified_src_strides[i] = simplified_src_strides[i] / src_itemsize;
        if (simplified_src_strides[i] < 0) {
            npy_src_min_nelem_offset +=
                simplified_src_strides[i] * (simplified_shape[i] - 1);
        }
        else {
            npy_src_max_nelem_offset +=
                simplified_src_strides[i] * (simplified_shape[i] - 1);
        }
    }

    // Create shared pointers with shape and src/dst strides, copy into device
    // memory
    using shT = std::vector<py::ssize_t>;

    // Get implementation function pointer
    auto copy_and_cast_from_host_blocking_fn =
        copy_and_cast_from_host_blocking_dispatch_table[dst_type_id]
                                                       [src_type_id];

    //   If shape/strides are accessed with accessors, buffer destructor
    //   will force syncronization.
    py::ssize_t *shape_strides =
        sycl::malloc_device<py::ssize_t>(3 * nd, exec_q);

    if (shape_strides == nullptr) {
        throw std::runtime_error("Unabled to allocate device memory");
    }

    using usm_host_allocatorT =
        sycl::usm_allocator<py::ssize_t, sycl::usm::alloc::host>;
    using usmshT = std::vector<py::ssize_t, usm_host_allocatorT>;
    usm_host_allocatorT alloc(exec_q);

    auto host_shape_strides_shp = std::make_shared<usmshT>(3 * nd, alloc);
    std::copy(simplified_shape.begin(), simplified_shape.end(),
              host_shape_strides_shp->begin());
    std::copy(simplified_src_strides.begin(), simplified_src_strides.end(),
              host_shape_strides_shp->begin() + nd);
    std::copy(simplified_dst_strides.begin(), simplified_dst_strides.end(),
              host_shape_strides_shp->begin() + 2 * nd);

    sycl::event copy_packed_ev =
        exec_q.copy<py::ssize_t>(host_shape_strides_shp->data(), shape_strides,
                                 host_shape_strides_shp->size());

    copy_and_cast_from_host_blocking_fn(
        exec_q, src_nelems, nd, shape_strides, src_data, src_offset,
        npy_src_min_nelem_offset, npy_src_max_nelem_offset, dst_data,
        dst_offset, depends, {copy_packed_ev});

    sycl::free(shape_strides, exec_q);

    return;
}

/* ============= linear-sequence ==================== */

using dpctl::tensor::kernels::constructors::lin_space_step_fn_ptr_t;

static lin_space_step_fn_ptr_t lin_space_step_dispatch_vector[_ns::num_types];

using dpctl::tensor::kernels::constructors::lin_space_affine_fn_ptr_t;

static lin_space_affine_fn_ptr_t
    lin_space_affine_dispatch_vector[_ns::num_types];

std::pair<sycl::event, sycl::event>
usm_ndarray_linear_sequence_step(py::object start,
                                 py::object dt,
                                 dpctl::tensor::usm_ndarray dst,
                                 sycl::queue exec_q,
                                 const std::vector<sycl::event> &depends = {})
{
    // dst must be 1D and C-contiguous
    // start, end should be coercible into data type of dst

    if (dst.get_ndim() != 1) {
        throw py::value_error(
            "usm_ndarray_linspace: Expecting 1D array to populate");
    }

    if (!dst.is_c_contiguous()) {
        throw py::value_error(
            "usm_ndarray_linspace: Non-contiguous arrays are not supported");
    }

    sycl::queue dst_q = dst.get_queue();
    if (!dpctl::utils::queues_are_compatible(exec_q, {dst_q})) {
        throw py::value_error(
            "Execution queue is not compatible with the allocation queue");
    }

    auto array_types = dpctl::tensor::detail::usm_ndarray_types();
    int dst_typenum = dst.get_typenum();
    int dst_typeid = array_types.typenum_to_lookup_id(dst_typenum);

    py::ssize_t len = dst.get_shape(0);
    if (len == 0) {
        // nothing to do
        return std::make_pair(sycl::event{}, sycl::event{});
    }

    char *dst_data = dst.get_data();
    sycl::event linspace_step_event;

    auto fn = lin_space_step_dispatch_vector[dst_typeid];

    linspace_step_event =
        fn(exec_q, static_cast<size_t>(len), start, dt, dst_data, depends);

    return std::make_pair(keep_args_alive(exec_q, {dst}, {linspace_step_event}),
                          linspace_step_event);
}

std::pair<sycl::event, sycl::event>
usm_ndarray_linear_sequence_affine(py::object start,
                                   py::object end,
                                   dpctl::tensor::usm_ndarray dst,
                                   bool include_endpoint,
                                   sycl::queue exec_q,
                                   const std::vector<sycl::event> &depends = {})
{
    // dst must be 1D and C-contiguous
    // start, end should be coercible into data type of dst

    if (dst.get_ndim() != 1) {
        throw py::value_error(
            "usm_ndarray_linspace: Expecting 1D array to populate");
    }

    if (!dst.is_c_contiguous()) {
        throw py::value_error(
            "usm_ndarray_linspace: Non-contiguous arrays are not supported");
    }

    sycl::queue dst_q = dst.get_queue();
    if (!dpctl::utils::queues_are_compatible(exec_q, {dst_q})) {
        throw py::value_error(
            "Execution queue context is not the same as allocation context");
    }

    auto array_types = dpctl::tensor::detail::usm_ndarray_types();
    int dst_typenum = dst.get_typenum();
    int dst_typeid = array_types.typenum_to_lookup_id(dst_typenum);

    py::ssize_t len = dst.get_shape(0);
    if (len == 0) {
        // nothing to do
        return std::make_pair(sycl::event{}, sycl::event{});
    }

    char *dst_data = dst.get_data();
    sycl::event linspace_affine_event;

    auto fn = lin_space_affine_dispatch_vector[dst_typeid];

    linspace_affine_event = fn(exec_q, static_cast<size_t>(len), start, end,
                               include_endpoint, dst_data, depends);

    return std::make_pair(
        keep_args_alive(exec_q, {dst}, {linspace_affine_event}),
        linspace_affine_event);
}

/* ================ Full ================== */

using dpctl::tensor::kernels::constructors::full_contig_fn_ptr_t;

static full_contig_fn_ptr_t full_contig_dispatch_vector[_ns::num_types];

std::pair<sycl::event, sycl::event>
usm_ndarray_full(py::object py_value,
                 dpctl::tensor::usm_ndarray dst,
                 sycl::queue exec_q,
                 const std::vector<sycl::event> &depends = {})
{
    // start, end should be coercible into data type of dst

    py::ssize_t dst_nelems = dst.get_size();

    if (dst_nelems == 0) {
        // nothing to do
        return std::make_pair(sycl::event(), sycl::event());
    }

    sycl::queue dst_q = dst.get_queue();
    if (!dpctl::utils::queues_are_compatible(exec_q, {dst_q})) {
        throw py::value_error(
            "Execution queue is not compatible with the allocation queue");
    }

    auto array_types = dpctl::tensor::detail::usm_ndarray_types();
    int dst_typenum = dst.get_typenum();
    int dst_typeid = array_types.typenum_to_lookup_id(dst_typenum);

    char *dst_data = dst.get_data();
    sycl::event full_event;

    if (dst_nelems == 1 || dst.is_c_contiguous() || dst.is_f_contiguous()) {
        auto fn = full_contig_dispatch_vector[dst_typeid];

        sycl::event full_contig_event =
            fn(exec_q, static_cast<size_t>(dst_nelems), py_value, dst_data,
               depends);

        return std::make_pair(
            keep_args_alive(exec_q, {dst}, {full_contig_event}),
            full_contig_event);
    }
    else {
        throw std::runtime_error(
            "Only population of contiguous usm_ndarray objects is supported.");
    }
}

/* ================ Eye ================== */

using dpctl::tensor::kernels::constructors::eye_fn_ptr_t;

static eye_fn_ptr_t eye_dispatch_vector[_ns::num_types];

std::pair<sycl::event, sycl::event>
eye(py::ssize_t k,
    dpctl::tensor::usm_ndarray dst,
    sycl::queue exec_q,
    const std::vector<sycl::event> &depends = {})
{
    // dst must be 2D

    if (dst.get_ndim() != 2) {
        throw py::value_error(
            "usm_ndarray_eye: Expecting 2D array to populate");
    }

    sycl::queue dst_q = dst.get_queue();
    if (!dpctl::utils::queues_are_compatible(exec_q, {dst_q})) {
        throw py::value_error("Execution queue is not compatible with the "
                              "allocation queue");
    }

    auto array_types = dpctl::tensor::detail::usm_ndarray_types();
    int dst_typenum = dst.get_typenum();
    int dst_typeid = array_types.typenum_to_lookup_id(dst_typenum);

    const py::ssize_t nelem = dst.get_size();
    const py::ssize_t rows = dst.get_shape(0);
    const py::ssize_t cols = dst.get_shape(1);
    if (rows == 0 || cols == 0) {
        // nothing to do
        return std::make_pair(sycl::event{}, sycl::event{});
    }

    bool is_dst_c_contig = dst.is_c_contiguous();
    bool is_dst_f_contig = dst.is_f_contiguous();
    if (!is_dst_c_contig && !is_dst_f_contig) {
        throw py::value_error("USM array is not contiguous");
    }

    py::ssize_t start;
    if (is_dst_c_contig) {
        start = (k < 0) ? -k * cols : k;
    }
    else {
        start = (k < 0) ? -k : k * rows;
    }

    const py::ssize_t *strides = dst.get_strides_raw();
    py::ssize_t step;
    if (strides == nullptr) {
        step = (is_dst_c_contig) ? cols + 1 : rows + 1;
    }
    else {
        step = strides[0] + strides[1];
    }

    const py::ssize_t length = std::min({rows, cols, rows + k, cols - k});
    const py::ssize_t end = start + step * (length - 1);

    char *dst_data = dst.get_data();
    sycl::event eye_event;

    auto fn = eye_dispatch_vector[dst_typeid];

    eye_event = fn(exec_q, static_cast<size_t>(nelem), start, end, step,
                   dst_data, depends);

    return std::make_pair(keep_args_alive(exec_q, {dst}, {eye_event}),
                          eye_event);
}

/* =========================== Tril and triu ============================== */

using dpctl::tensor::kernels::constructors::tri_fn_ptr_t;

static tri_fn_ptr_t tril_generic_dispatch_vector[_ns::num_types];
static tri_fn_ptr_t triu_generic_dispatch_vector[_ns::num_types];

std::pair<sycl::event, sycl::event>
tri(sycl::queue &exec_q,
    dpctl::tensor::usm_ndarray src,
    dpctl::tensor::usm_ndarray dst,
    char part,
    py::ssize_t k = 0,
    const std::vector<sycl::event> &depends = {})
{
    // array dimensions must be the same
    int src_nd = src.get_ndim();
    int dst_nd = dst.get_ndim();
    if (src_nd != dst_nd) {
        throw py::value_error("Array dimensions are not the same.");
    }

    if (src_nd < 2) {
        throw py::value_error("Array dimensions less than 2.");
    }

    // shapes must be the same
    const py::ssize_t *src_shape = src.get_shape_raw();
    const py::ssize_t *dst_shape = dst.get_shape_raw();

    bool shapes_equal(true);
    size_t src_nelems(1);

    for (int i = 0; shapes_equal && i < src_nd; ++i) {
        src_nelems *= static_cast<size_t>(src_shape[i]);
        shapes_equal = shapes_equal && (src_shape[i] == dst_shape[i]);
    }
    if (!shapes_equal) {
        throw py::value_error("Array shapes are not the same.");
    }

    if (src_nelems == 0) {
        // nothing to do
        return std::make_pair(sycl::event(), sycl::event());
    }

    char *src_data = src.get_data();
    char *dst_data = dst.get_data();

    // check that arrays do not overlap, and concurrent copying is safe.
    auto src_offsets = src.get_minmax_offsets();
    auto dst_offsets = dst.get_minmax_offsets();
    int src_elem_size = src.get_elemsize();
    int dst_elem_size = dst.get_elemsize();

    bool memory_overlap =
        ((dst_data - src_data > src_offsets.second * src_elem_size -
                                    dst_offsets.first * dst_elem_size) &&
         (src_data - dst_data > dst_offsets.second * dst_elem_size -
                                    src_offsets.first * src_elem_size));
    if (memory_overlap) {
        // TODO: could use a temporary, but this is done by the caller
        throw py::value_error("Arrays index overlapping segments of memory");
    }

    auto array_types = dpctl::tensor::detail::usm_ndarray_types();

    int src_typenum = src.get_typenum();
    int dst_typenum = dst.get_typenum();
    int src_typeid = array_types.typenum_to_lookup_id(src_typenum);
    int dst_typeid = array_types.typenum_to_lookup_id(dst_typenum);

    if (dst_typeid != src_typeid) {
        throw py::value_error("Array dtype are not the same.");
    }

    // check same contexts
    sycl::queue src_q = src.get_queue();
    sycl::queue dst_q = dst.get_queue();

    if (!dpctl::utils::queues_are_compatible(exec_q, {src_q, dst_q})) {
        throw py::value_error(
            "Execution queue context is not the same as allocation contexts");
    }

    using shT = std::vector<py::ssize_t>;
    shT src_strides(src_nd);

    bool is_src_c_contig = src.is_c_contiguous();
    bool is_src_f_contig = src.is_f_contiguous();

    const py::ssize_t *src_strides_raw = src.get_strides_raw();
    if (src_strides_raw == nullptr) {
        if (is_src_c_contig) {
            src_strides = c_contiguous_strides(src_nd, src_shape);
        }
        else if (is_src_f_contig) {
            src_strides = f_contiguous_strides(src_nd, src_shape);
        }
        else {
            throw std::runtime_error("Source array has null strides but has "
                                     "neither C- nor F- contiguous flag set");
        }
    }
    else {
        std::copy(src_strides_raw, src_strides_raw + src_nd,
                  src_strides.begin());
    }

    shT dst_strides(src_nd);

    bool is_dst_c_contig = dst.is_c_contiguous();
    bool is_dst_f_contig = dst.is_f_contiguous();

    const py::ssize_t *dst_strides_raw = dst.get_strides_raw();
    if (dst_strides_raw == nullptr) {
        if (is_dst_c_contig) {
            dst_strides =
                dpctl::tensor::c_contiguous_strides(src_nd, src_shape);
        }
        else if (is_dst_f_contig) {
            dst_strides =
                dpctl::tensor::f_contiguous_strides(src_nd, src_shape);
        }
        else {
            throw std::runtime_error("Source array has null strides but has "
                                     "neither C- nor F- contiguous flag set");
        }
    }
    else {
        std::copy(dst_strides_raw, dst_strides_raw + dst_nd,
                  dst_strides.begin());
    }

    shT simplified_shape;
    shT simplified_src_strides;
    shT simplified_dst_strides;
    py::ssize_t src_offset(0);
    py::ssize_t dst_offset(0);

    constexpr py::ssize_t src_itemsize = 1; // item size in elements
    constexpr py::ssize_t dst_itemsize = 1; // item size in elements

    int nd = src_nd - 2;
    const py::ssize_t *shape = src_shape;
    const py::ssize_t *p_src_strides = src_strides.data();
    const py::ssize_t *p_dst_strides = dst_strides.data();

    simplify_iteration_space(nd, shape, p_src_strides, src_itemsize,
                             is_src_c_contig, is_src_f_contig, p_dst_strides,
                             dst_itemsize, is_dst_c_contig, is_dst_f_contig,
                             simplified_shape, simplified_src_strides,
                             simplified_dst_strides, src_offset, dst_offset);

    if (src_offset != 0 || dst_offset != 0) {
        throw py::value_error("Reversed slice for dst is not supported");
    }

    nd += 2;

    using usm_host_allocatorT =
        sycl::usm_allocator<py::ssize_t, sycl::usm::alloc::host>;
    using usmshT = std::vector<py::ssize_t, usm_host_allocatorT>;

    usm_host_allocatorT allocator(exec_q);
    auto shp_host_shape_and_strides =
        std::make_shared<usmshT>(3 * nd, allocator);

    std::copy(simplified_shape.begin(), simplified_shape.end(),
              shp_host_shape_and_strides->begin());
    (*shp_host_shape_and_strides)[nd - 2] = src_shape[src_nd - 2];
    (*shp_host_shape_and_strides)[nd - 1] = src_shape[src_nd - 1];

    std::copy(simplified_src_strides.begin(), simplified_src_strides.end(),
              shp_host_shape_and_strides->begin() + nd);
    (*shp_host_shape_and_strides)[2 * nd - 2] = src_strides[src_nd - 2];
    (*shp_host_shape_and_strides)[2 * nd - 1] = src_strides[src_nd - 1];

    std::copy(simplified_dst_strides.begin(), simplified_dst_strides.end(),
              shp_host_shape_and_strides->begin() + 2 * nd);
    (*shp_host_shape_and_strides)[3 * nd - 2] = dst_strides[src_nd - 2];
    (*shp_host_shape_and_strides)[3 * nd - 1] = dst_strides[src_nd - 1];

    py::ssize_t *dev_shape_and_strides =
        sycl::malloc_device<ssize_t>(3 * nd, exec_q);
    if (dev_shape_and_strides == nullptr) {
        throw std::runtime_error("Unabled to allocate device memory");
    }
    sycl::event copy_shape_and_strides = exec_q.copy<ssize_t>(
        shp_host_shape_and_strides->data(), dev_shape_and_strides, 3 * nd);

    py::ssize_t inner_range = src_shape[src_nd - 1] * src_shape[src_nd - 2];
    py::ssize_t outer_range = src_nelems / inner_range;

    sycl::event tri_ev;
    if (part == 'l') {
        auto fn = tril_generic_dispatch_vector[src_typeid];
        tri_ev =
            fn(exec_q, inner_range, outer_range, src_data, dst_data, nd,
               dev_shape_and_strides, k, depends, {copy_shape_and_strides});
    }
    else {
        auto fn = triu_generic_dispatch_vector[src_typeid];
        tri_ev =
            fn(exec_q, inner_range, outer_range, src_data, dst_data, nd,
               dev_shape_and_strides, k, depends, {copy_shape_and_strides});
    }

    exec_q.submit([&](sycl::handler &cgh) {
        cgh.depends_on({tri_ev});
        auto ctx = exec_q.get_context();
        cgh.host_task(
            [shp_host_shape_and_strides, dev_shape_and_strides, ctx]() {
                // capture of shp_host_shape_and_strides ensure the underlying
                // vector exists for the entire execution of copying kernel
                sycl::free(dev_shape_and_strides, ctx);
            });
    });

    return std::make_pair(keep_args_alive(exec_q, {src, dst}, {tri_ev}),
                          tri_ev);
}

// populate dispatch tables
void init_copy_and_cast_dispatch_tables(void)
{
    using namespace dpctl::tensor::detail;

    using dpctl::tensor::kernels::copy_and_cast::CopyAndCast1DFactory;
    using dpctl::tensor::kernels::copy_and_cast::CopyAndCast2DFactory;
    using dpctl::tensor::kernels::copy_and_cast::CopyAndCastFromHostFactory;
    using dpctl::tensor::kernels::copy_and_cast::CopyAndCastGenericFactory;

    DispatchTableBuilder<copy_and_cast_generic_fn_ptr_t,
                         CopyAndCastGenericFactory, num_types>
        dtb_generic;
    dtb_generic.populate_dispatch_table(copy_and_cast_generic_dispatch_table);

    DispatchTableBuilder<copy_and_cast_1d_fn_ptr_t, CopyAndCast1DFactory,
                         num_types>
        dtb_1d;
    dtb_1d.populate_dispatch_table(copy_and_cast_1d_dispatch_table);

    DispatchTableBuilder<copy_and_cast_2d_fn_ptr_t, CopyAndCast2DFactory,
                         num_types>
        dtb_2d;
    dtb_2d.populate_dispatch_table(copy_and_cast_2d_dispatch_table);

    DispatchTableBuilder<copy_and_cast_from_host_blocking_fn_ptr_t,
                         CopyAndCastFromHostFactory, num_types>
        dtb_copy_from_numpy;

    dtb_copy_from_numpy.populate_dispatch_table(
        copy_and_cast_from_host_blocking_dispatch_table);

    return;
}

// populate dispatch vectors
void init_copy_for_reshape_dispatch_vector(void)
{
    using namespace dpctl::tensor::detail;
    using dpctl::tensor::kernels::constructors::EyeFactory;
    using dpctl::tensor::kernels::constructors::FullContigFactory;
    using dpctl::tensor::kernels::constructors::LinSpaceAffineFactory;
    using dpctl::tensor::kernels::constructors::LinSpaceStepFactory;
    using dpctl::tensor::kernels::constructors::TrilGenericFactory;
    using dpctl::tensor::kernels::constructors::TriuGenericFactory;
    using dpctl::tensor::kernels::copy_and_cast::CopyForReshapeGenericFactory;

    DispatchVectorBuilder<copy_for_reshape_fn_ptr_t,
                          CopyForReshapeGenericFactory, num_types>
        dvb;
    dvb.populate_dispatch_vector(copy_for_reshape_generic_dispatch_vector);

    DispatchVectorBuilder<lin_space_step_fn_ptr_t, LinSpaceStepFactory,
                          num_types>
        dvb1;
    dvb1.populate_dispatch_vector(lin_space_step_dispatch_vector);

    DispatchVectorBuilder<lin_space_affine_fn_ptr_t, LinSpaceAffineFactory,
                          num_types>
        dvb2;
    dvb2.populate_dispatch_vector(lin_space_affine_dispatch_vector);

    DispatchVectorBuilder<full_contig_fn_ptr_t, FullContigFactory, num_types>
        dvb3;
    dvb3.populate_dispatch_vector(full_contig_dispatch_vector);

    DispatchVectorBuilder<eye_fn_ptr_t, EyeFactory, num_types> dvb4;
    dvb4.populate_dispatch_vector(eye_dispatch_vector);

    DispatchVectorBuilder<tri_fn_ptr_t, TrilGenericFactory, num_types> dvb5;
    dvb5.populate_dispatch_vector(tril_generic_dispatch_vector);

    DispatchVectorBuilder<tri_fn_ptr_t, TriuGenericFactory, num_types> dvb6;
    dvb6.populate_dispatch_vector(triu_generic_dispatch_vector);

    return;
}

std::string get_default_device_fp_type(sycl::device d)
{
    if (d.has(sycl::aspect::fp64)) {
        return "f8";
    }
    else {
        return "f4";
    }
}

std::string get_default_device_int_type(sycl::device)
{
    return "i8";
}

std::string get_default_device_complex_type(sycl::device d)
{
    if (d.has(sycl::aspect::fp64)) {
        return "c16";
    }
    else {
        return "c8";
    }
}

std::string get_default_device_bool_type(sycl::device)
{
    return "b1";
}

} // namespace

PYBIND11_MODULE(_tensor_impl, m)
{

    init_copy_and_cast_dispatch_tables();
    init_copy_for_reshape_dispatch_vector();
    import_dpctl();

    m.def(
        "_contract_iter", &contract_iter<py::ssize_t, py::value_error>,
        "Simplifies iteration of array of given shape & stride. Returns "
        "a triple: shape, stride and offset for the new iterator of possible "
        "smaller dimension, which traverses the same elements as the original "
        "iterator, possibly in a different order.");

    m.def("_copy_usm_ndarray_into_usm_ndarray",
          &copy_usm_ndarray_into_usm_ndarray,
          "Copies from usm_ndarray `src` into usm_ndarray `dst` of the same "
          "shape. "
          "Returns a tuple of events: (host_task_event, compute_task_event)",
          py::arg("src"), py::arg("dst"), py::arg("sycl_queue"),
          py::arg("depends") = py::list());

    m.def(
        "_contract_iter2", &contract_iter2<py::ssize_t, py::value_error>,
        "Simplifies iteration over elements of pair of arrays of given shape "
        "with strides stride1 and stride2. Returns "
        "a 5-tuple: shape, stride and offset for the new iterator of possible "
        "smaller dimension for each array, which traverses the same elements "
        "as the original "
        "iterator, possibly in a different order.");

    m.def("_copy_usm_ndarray_for_reshape", &copy_usm_ndarray_for_reshape,
          "Copies from usm_ndarray `src` into usm_ndarray `dst` with the same "
          "number of elements using underlying 'C'-contiguous order for flat "
          "traversal with shift. "
          "Returns a tuple of events: (ht_event, comp_event)",
          py::arg("src"), py::arg("dst"), py::arg("shift"),
          py::arg("sycl_queue"), py::arg("depends") = py::list());

    m.def("_linspace_step", &usm_ndarray_linear_sequence_step,
          "Fills input 1D contiguous usm_ndarray `dst` with linear sequence "
          "specified by "
          "starting point `start` and step `dt`. "
          "Returns a tuple of events: (ht_event, comp_event)",
          py::arg("start"), py::arg("dt"), py::arg("dst"),
          py::arg("sycl_queue"), py::arg("depends") = py::list());

    m.def("_linspace_affine", &usm_ndarray_linear_sequence_affine,
          "Fills input 1D contiguous usm_ndarray `dst` with linear sequence "
          "specified by "
          "starting point `start` and end point `end`. "
          "Returns a tuple of events: (ht_event, comp_event)",
          py::arg("start"), py::arg("end"), py::arg("dst"),
          py::arg("include_endpoint"), py::arg("sycl_queue"),
          py::arg("depends") = py::list());

    m.def("_copy_numpy_ndarray_into_usm_ndarray",
          &copy_numpy_ndarray_into_usm_ndarray,
          "Copy fom numpy array `src` into usm_ndarray `dst` synchronously.",
          py::arg("src"), py::arg("dst"), py::arg("sycl_queue"),
          py::arg("depends") = py::list());

    m.def("_full_usm_ndarray", &usm_ndarray_full,
          "Populate usm_ndarray `dst` with given fill_value.",
          py::arg("fill_value"), py::arg("dst"), py::arg("sycl_queue"),
          py::arg("depends") = py::list());

    m.def("_eye", &eye,
          "Fills input 2D contiguous usm_ndarray `dst` with "
          "zeros outside of the diagonal "
          "specified by "
          "the diagonal index `k` "
          "which is filled with ones."
          "Returns a tuple of events: (ht_event, comp_event)",
          py::arg("k"), py::arg("dst"), py::arg("sycl_queue"),
          py::arg("depends") = py::list());

    m.def("default_device_fp_type", [](sycl::queue q) -> std::string {
        return get_default_device_fp_type(q.get_device());
    });
    m.def("default_device_fp_type_device", [](sycl::device dev) -> std::string {
        return get_default_device_fp_type(dev);
    });

    m.def("default_device_int_type", [](sycl::queue q) -> std::string {
        return get_default_device_int_type(q.get_device());
    });
    m.def("default_device_int_type_device",
          [](sycl::device dev) -> std::string {
              return get_default_device_int_type(dev);
          });

    m.def("default_device_bool_type", [](sycl::queue q) -> std::string {
        return get_default_device_bool_type(q.get_device());
    });
    m.def("default_device_bool_type_device",
          [](sycl::device dev) -> std::string {
              return get_default_device_bool_type(dev);
          });

    m.def("default_device_complex_type", [](sycl::queue q) -> std::string {
        return get_default_device_complex_type(q.get_device());
    });
    m.def("default_device_complex_type_device",
          [](sycl::device dev) -> std::string {
              return get_default_device_complex_type(dev);
          });
    m.def(
        "_tril",
        [](dpctl::tensor::usm_ndarray src, dpctl::tensor::usm_ndarray dst,
           py::ssize_t k, sycl::queue exec_q,
           const std::vector<sycl::event> depends)
            -> std::pair<sycl::event, sycl::event> {
            return tri(exec_q, src, dst, 'l', k, depends);
        },
        "Tril helper function.", py::arg("src"), py::arg("dst"),
        py::arg("k") = 0, py::arg("sycl_queue"),
        py::arg("depends") = py::list());

    m.def(
        "_triu",
        [](dpctl::tensor::usm_ndarray src, dpctl::tensor::usm_ndarray dst,
           py::ssize_t k, sycl::queue exec_q,
           const std::vector<sycl::event> depends)
            -> std::pair<sycl::event, sycl::event> {
            return tri(exec_q, src, dst, 'u', k, depends);
        },
        "Triu helper function.", py::arg("src"), py::arg("dst"),
        py::arg("k") = 0, py::arg("sycl_queue"),
        py::arg("depends") = py::list());
}
