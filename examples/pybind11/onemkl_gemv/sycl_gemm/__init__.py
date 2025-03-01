#                      Data Parallel Control (dpctl)
#
# Copyright 2020-2025 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from ._onemkl import (
    axpby_inplace,
    cpp_cg_solve,
    dot_blocking,
    gemv,
    norm_squared_blocking,
    sub,
)

__all__ = [
    "gemv",
    "sub",
    "axpby_inplace",
    "norm_squared_blocking",
    "dot_blocking",
    "cpp_cg_solve",
]
