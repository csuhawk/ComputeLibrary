/*
 * Copyright (c) 2018 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "arm_compute/core/NEON/kernels/assembly/NEGEMMInterleavedPrepareBWrapperKernel.h"

#include "NEGEMMInterleavedStrategies.h"
#include "arm_compute/core/Helpers.h"
#include "arm_compute/core/ITensor.h"
#include "arm_compute/core/Utils.h"
#include "arm_compute/core/Validate.h"

namespace arm_compute
{
namespace
{
// Call the lambda function for each workload generated by the passed window.
template <typename To, bool use_dot, typename Lambda>
void for_each_element_in_window(const Window &window, const ITensor *b, ITensor *transformed_b, unsigned int N, unsigned int K, Lambda &&lambda)
{
    using strategy = typename Kernel<To, use_dot>::strategy;

    unsigned int offset_transformed_b = transformed_b->info()->offset_first_element_in_bytes();
    execute_window_loop(window, [&](const Coordinates & coordinates)
    {
        const unsigned int x0    = coordinates.x();
        const unsigned int k0    = coordinates.y();
        const unsigned int multi = coordinates.z();

        const unsigned int offset_b = b->info()->offset_element_in_bytes(Coordinates(0, 0, multi));
        const unsigned int xmax     = std::min(x0 + window.x().step(), N);
        const unsigned int kmax     = std::min(k0 + window.y().step(), K);

        /* Figure out the size of each block. */
        unsigned int x_size = (xmax - x0);
        unsigned int k_size = (kmax - k0);

        /* Round sizes up as needed. */
        x_size = ceil_to_multiple(x_size, strategy::out_width());
        k_size = ceil_to_multiple(k_size, strategy::k_unroll());

        lambda(PrepareBWorkload(offset_b, offset_transformed_b, x0, xmax, k0, kmax));

        //Each workload represents one block:
        offset_transformed_b += (x_size * k_size * sizeof(To));
    });
}

// Calculate the size of transformed_b:
template <typename To, bool use_dot>
unsigned int get_B_pretransposed_array_size(unsigned int N, unsigned int K, const BlockSizes &bs)
{
    using strategy = typename Kernel<To, use_dot>::strategy;

    // How many full blocks do N / K contain ?
    size_t num_full_k = K / bs.k_block;
    size_t num_full_x = N / bs.x_block;

    ARM_COMPUTE_ERROR_ON(bs.x_block % strategy::out_width() != 0);
    ARM_COMPUTE_ERROR_ON(bs.k_block % strategy::k_unroll() != 0);

    size_t normal_x_size = bs.x_block;
    size_t normal_k_size = bs.k_block;

    // Round up the leftovers to be a multiple of the strategy processing size:
    size_t left_over_x_size = ceil_to_multiple(N % bs.x_block, strategy::out_width());
    size_t left_over_k_size = ceil_to_multiple(K % bs.k_block, strategy::k_unroll());

    // Calculate the total size of the buffer:
    size_t total = num_full_k * normal_k_size * (num_full_x * normal_x_size + left_over_x_size);
    total += left_over_k_size * (left_over_x_size + num_full_x * normal_x_size);
    total *= sizeof(To);
    return total;
}

} // namespace

template <typename To, bool use_dot>
BlockSizes NEGEMMInterleavedPrepareBWrapperKernelTemplate<To, use_dot>::block_sizes() const
{
    return _block_sizes;
}

template <typename To, bool use_dot>
void NEGEMMInterleavedPrepareBWrapperKernelTemplate<To, use_dot>::configure(const ITensor *b, ITensor *transformed_b, bool transpose_b, const CPUInfo &ci, const INEGEMMWrapperKernel::Params &params)
{
    using strategy = typename Kernel<To, use_dot>::strategy;

    const unsigned int multis = b->info()->tensor_shape().z();
    _Nsize                    = b->info()->tensor_shape().x();
    _Ksize                    = b->info()->tensor_shape().y();
    _b                        = b;
    _transformed_b            = transformed_b;
    _transpose_b              = transpose_b;

    _block_sizes = calculate_block_sizes<strategy>(ci, params.M, params.N, params.K);

    auto_init_if_empty(*transformed_b->info(), b->info()->clone()->set_tensor_shape(TensorShape{ get_B_pretransposed_array_size<To, use_dot>(_Nsize, _Ksize, _block_sizes) }));

    Window window;
    window.set(Window::DimX, Window::Dimension(0, ceil_to_multiple(_Nsize, _block_sizes.x_block), _block_sizes.x_block));
    window.set(Window::DimY, Window::Dimension(0, ceil_to_multiple(_Ksize, _block_sizes.k_block), _block_sizes.k_block));
    window.set(Window::DimZ, Window::Dimension(0, multis));

    INEKernel::configure(window);
}

template <typename To, bool use_dot>
void NEGEMMInterleavedPrepareBWrapperKernelTemplate<To, use_dot>::transform(const PrepareBWorkload &wl, const ThreadInfo &info)
{
    using strategy = typename Kernel<To, use_dot>::strategy;

    strategy strat(info.cpu_info);
    strat.transforms.PrepareB(reinterpret_cast<To *>(_transformed_b->buffer() + wl._offset_transformed_b),
                              reinterpret_cast<To *>(_b->buffer() + wl._offset_b),
                              _b->info()->strides_in_bytes().y() / sizeof(To),
                              wl._x0, wl._xmax, wl._k0, wl._kmax, _transpose_b);
}

template <typename To, bool use_dot>
void NEGEMMInterleavedPrepareBWrapperKernelTemplate<To, use_dot>::create_workloads(std::vector<PrepareBWorkload> &workloads)
{
    for_each_element_in_window<To, use_dot>(window(), _b, _transformed_b, _Nsize, _Ksize, [&workloads](PrepareBWorkload && wl)
    {
        workloads.push_back(std::move(wl));
    });
}

template <typename To, bool use_dot>
void NEGEMMInterleavedPrepareBWrapperKernelTemplate<To, use_dot>::run(const Window &window, const ThreadInfo &info)
{
    ARM_COMPUTE_ERROR_ON_MISMATCHING_WINDOWS(window, INEKernel::window());
    for_each_element_in_window<To, use_dot>(window, _b, _transformed_b, _Nsize, _Ksize, [&](PrepareBWorkload && wl)
    {
        this->transform(wl, info);
    });
}

template class NEGEMMInterleavedPrepareBWrapperKernelTemplate<float>;
#ifdef __aarch64__
template class NEGEMMInterleavedPrepareBWrapperKernelTemplate<uint8_t>;
template class NEGEMMInterleavedPrepareBWrapperKernelTemplate<int8_t>;
template class NEGEMMInterleavedPrepareBWrapperKernelTemplate<uint8_t, true>;
template class NEGEMMInterleavedPrepareBWrapperKernelTemplate<int8_t, true>;
#endif /* __aarch64__ */

#ifdef __ARM_FEATURE_FP16_VECTOR_ARITHMETIC
template class NEGEMMInterleavedPrepareBWrapperKernelTemplate<float16_t>;
#endif /* __ARM_FEATURE_FP16_VECTOR_ARITHMETIC */
} // namespace arm_compute
