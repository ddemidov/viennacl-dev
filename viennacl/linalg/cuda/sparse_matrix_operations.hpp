#ifndef VIENNACL_LINALG_CUDA_SPARSE_MATRIX_OPERATIONS_HPP_
#define VIENNACL_LINALG_CUDA_SPARSE_MATRIX_OPERATIONS_HPP_

/* =========================================================================
   Copyright (c) 2010-2014, Institute for Microelectronics,
                            Institute for Analysis and Scientific Computing,
                            TU Wien.
   Portions of this software are copyright by UChicago Argonne, LLC.

                            -----------------
                  ViennaCL - The Vienna Computing Library
                            -----------------

   Project Head:    Karl Rupp                   rupp@iue.tuwien.ac.at

   (A list of authors and contributors can be found in the PDF manual)

   License:         MIT (X11), see file LICENSE in the base directory
============================================================================= */

/** @file viennacl/linalg/cuda/sparse_matrix_operations.hpp
    @brief Implementations of operations using sparse matrices using CUDA
*/

#include "viennacl/forwards.h"
#include "viennacl/scalar.hpp"
#include "viennacl/vector.hpp"
#include "viennacl/tools/tools.hpp"
#include "viennacl/linalg/cuda/common.hpp"

#include "viennacl/linalg/cuda/sparse_matrix_operations_solve.hpp"

namespace viennacl
{
namespace linalg
{
namespace cuda
{
//
// Compressed matrix
//

namespace detail
{

  template<typename NumericT>
  __global__ void csr_row_info_extractor_kernel(
            const unsigned int * row_indices,
            const unsigned int * column_indices,
            const NumericT * elements,
            NumericT * result,
            unsigned int size,
            unsigned int option)
  {
    for (unsigned int row  = blockDim.x * blockIdx.x + threadIdx.x;
                      row  < size;
                      row += gridDim.x * blockDim.x)
    {
      NumericT value = 0;
      unsigned int row_end = row_indices[row+1];

      switch (option)
      {
        case 0: //inf-norm
          for (unsigned int i = row_indices[row]; i < row_end; ++i)
            value = max(value, fabs(elements[i]));
          break;

        case 1: //1-norm
          for (unsigned int i = row_indices[row]; i < row_end; ++i)
            value += fabs(elements[i]);
          break;

        case 2: //2-norm
          for (unsigned int i = row_indices[row]; i < row_end; ++i)
            value += elements[i] * elements[i];
          value = sqrt(value);
          break;

        case 3: //diagonal entry
          for (unsigned int i = row_indices[row]; i < row_end; ++i)
          {
            if (column_indices[i] == row)
            {
              value = elements[i];
              break;
            }
          }
          break;

        default:
          break;
      }
      result[row] = value;
    }
  }


  template<typename NumericT, unsigned int AligmentV>
  void row_info(compressed_matrix<NumericT, AligmentV> const & mat,
                vector_base<NumericT> & vec,
                viennacl::linalg::detail::row_info_types info_selector)
  {
    csr_row_info_extractor_kernel<<<128, 128>>>(detail::cuda_arg<unsigned int>(mat.handle1().cuda_handle()),
                                                detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
                                                detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                                detail::cuda_arg<NumericT>(vec),
                                                static_cast<unsigned int>(mat.size1()),
                                                static_cast<unsigned int>(info_selector)
                                               );
    VIENNACL_CUDA_LAST_ERROR_CHECK("csr_row_info_extractor_kernel");
  }

} //namespace detail


template<typename NumericT>
__global__ void compressed_matrix_vec_mul_kernel(
          const unsigned int * row_indices,
          const unsigned int * column_indices,
          const NumericT * elements,
          const NumericT * x,
          unsigned int start_x,
          unsigned int inc_x,
          NumericT * result,
          unsigned int start_result,
          unsigned int inc_result,
          unsigned int size_result)
{
  for (unsigned int row  = blockDim.x * blockIdx.x + threadIdx.x;
                    row  < size_result;
                    row += gridDim.x * blockDim.x)
  {
    NumericT dot_prod = NumericT(0);
    unsigned int row_end = row_indices[row+1];
    for (unsigned int i = row_indices[row]; i < row_end; ++i)
      dot_prod += elements[i] * x[column_indices[i] * inc_x + start_x];
    result[row * inc_result + start_result] = dot_prod;
  }
}




template<typename NumericT>
__global__ void compressed_matrix_vec_mul_adaptive_kernel(
          const unsigned int * row_indices,
          const unsigned int * column_indices,
          const unsigned int * row_blocks,
          const NumericT * elements,
          unsigned int num_blocks,
          const NumericT * x,
          unsigned int start_x,
          unsigned int inc_x,
          NumericT * result,
          unsigned int start_result,
          unsigned int inc_result,
          unsigned int size_result)
{
  __shared__ NumericT     shared_elements[1024];

  for (unsigned int block_id = blockIdx.x; block_id < num_blocks; block_id += gridDim.x)
  {
    unsigned int row_start = row_blocks[block_id];
    unsigned int row_stop  = row_blocks[block_id + 1];
    unsigned int element_start = row_indices[row_start];
    unsigned int element_stop = row_indices[row_stop];
    unsigned int rows_to_process = row_stop - row_start;

    if (rows_to_process > 1)  // CSR stream with one thread per row
    {
      // load to shared buffer:
      for (unsigned int i = element_start + threadIdx.x; i < element_stop; i += blockDim.x)
        shared_elements[i - element_start] = elements[i] * x[column_indices[i] * inc_x + start_x];

      __syncthreads();

      // use one thread per row to sum:
      for (unsigned int row = row_start + threadIdx.x; row < row_stop; row += blockDim.x)
      {
        NumericT dot_prod = 0;
        unsigned int thread_row_start = row_indices[row]     - element_start;
        unsigned int thread_row_stop  = row_indices[row + 1] - element_start;
        for (unsigned int i = thread_row_start; i < thread_row_stop; ++i)
          dot_prod += shared_elements[i];
        result[row * inc_result + start_result] = dot_prod;
      }
    }
    // TODO here: Consider CSR vector for two to four rows (cf. OpenCL implementation. Experience on Fermi suggests that this may not be necessary)
    else // CSR vector for a single row
    {
      // load and sum to shared buffer:
      shared_elements[threadIdx.x] = 0;
      for (unsigned int i = element_start + threadIdx.x; i < element_stop; i += blockDim.x)
        shared_elements[threadIdx.x] += elements[i] * x[column_indices[i] * inc_x + start_x];

      // reduction to obtain final result
      for (unsigned int stride = blockDim.x/2; stride > 0; stride /= 2)
      {
        __syncthreads();
        if (threadIdx.x < stride)
          shared_elements[threadIdx.x] += shared_elements[threadIdx.x+stride];
      }

      if (threadIdx.x == 0)
        result[row_start * inc_result + start_result] = shared_elements[0];
    }

    __syncthreads();  // avoid race conditions
  }
}




/** @brief Carries out matrix-vector multiplication with a compressed_matrix
*
* Implementation of the convenience expression result = prod(mat, vec);
*
* @param mat    The matrix
* @param vec    The vector
* @param result The result vector
*/
template<class NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::compressed_matrix<NumericT, AlignmentV> & mat,
               const viennacl::vector_base<NumericT> & vec,
                     viennacl::vector_base<NumericT> & result)
{
  compressed_matrix_vec_mul_adaptive_kernel<<<256, 256>>>(detail::cuda_arg<unsigned int>(mat.handle1().cuda_handle()),
                                                 detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
                                                 detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
                                                 detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                                 static_cast<unsigned int>(mat.blocks1()),
                                                 detail::cuda_arg<NumericT>(vec),
                                                 static_cast<unsigned int>(vec.start()),
                                                 static_cast<unsigned int>(vec.stride()),
                                                 detail::cuda_arg<NumericT>(result),
                                                 static_cast<unsigned int>(result.start()),
                                                 static_cast<unsigned int>(result.stride()),
                                                 static_cast<unsigned int>(result.size())
                                                );
  VIENNACL_CUDA_LAST_ERROR_CHECK("compressed_matrix_vec_mul_adaptive_kernel");
}

/** @brief Helper struct for accessing an element of a row- or column-major matrix.
  *
  * @param LayoutT   The layout tag: Either row_major or column_major
  */
template<typename LayoutT>
struct mat_mult_matrix_index
{
  static __device__ unsigned int apply(unsigned int i, unsigned int j,
                                unsigned int row_start, unsigned int row_inc,
                                unsigned int col_start, unsigned int col_inc,
                                unsigned int internal_rows, unsigned int internal_cols)
  {
    return (row_start + i * row_inc) * internal_cols + col_start + j * col_inc;
  }
};

/** \cond */
template<>
struct mat_mult_matrix_index<viennacl::column_major>
{
  static __device__ unsigned int apply(unsigned int i, unsigned int j,
                                unsigned int row_start, unsigned int row_inc,
                                unsigned int col_start, unsigned int col_inc,
                                unsigned int internal_rows, unsigned int internal_cols)
  {
    return (row_start + i * row_inc) + (col_start + j * col_inc) * internal_rows;
  }
};
/** \endcond */


template<typename DMatIndexT, typename ResultIndexT, typename NumericT>
__global__ void compressed_matrix_d_mat_mul_kernel(
          const unsigned int * sp_mat_row_indices,
          const unsigned int * sp_mat_col_indices,
          const NumericT * sp_mat_elements,
          const NumericT * d_mat,
          unsigned int d_mat_row_start,
          unsigned int d_mat_col_start,
          unsigned int d_mat_row_inc,
          unsigned int d_mat_col_inc,
          unsigned int d_mat_row_size,
          unsigned int d_mat_col_size,
          unsigned int d_mat_internal_rows,
          unsigned int d_mat_internal_cols,
          NumericT * result,
          unsigned int result_row_start,
          unsigned int result_col_start,
          unsigned int result_row_inc,
          unsigned int result_col_inc,
          unsigned int result_row_size,
          unsigned int result_col_size,
          unsigned int result_internal_rows,
          unsigned int result_internal_cols)
{
  for (unsigned int row  = blockIdx.x; row  < result_row_size; row += gridDim.x)
  {
    unsigned int row_start = sp_mat_row_indices[row];
    unsigned int row_end = sp_mat_row_indices[row+1];

    for ( unsigned int col = threadIdx.x; col < result_col_size; col += blockDim.x)
    {
      NumericT r = 0;

      for (unsigned int k = row_start; k < row_end; k++)
      {
        unsigned int j = sp_mat_col_indices[k];
        NumericT x = sp_mat_elements[k];
        NumericT y = d_mat[ DMatIndexT::apply(j, col,
                                              d_mat_row_start, d_mat_row_inc,
                                              d_mat_col_start, d_mat_col_inc,
                                              d_mat_internal_rows, d_mat_internal_cols) ];

        r += x * y;
      }

      result[ResultIndexT::apply(row, col,
                                 result_row_start, result_row_inc,
                                 result_col_start, result_col_inc,
                                 result_internal_rows, result_internal_cols)] = r;
    }
  }
}


/** @brief Carries out sparse_matrix-dense_matrix multiplication first matrix being compressed
*
* Implementation of the convenience expression result = prod(mat, vec);
*
* @param sp_mat   The sparse matrix
* @param d_mat    The dense matrix
* @param result   The result matrix
*/
template<typename NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::compressed_matrix<NumericT, AlignmentV> & sp_mat,
               const viennacl::matrix_base<NumericT> & d_mat,
                     viennacl::matrix_base<NumericT> & result)
{
  if (d_mat.row_major() && result.row_major())
  {
    compressed_matrix_d_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<row_major> ><<<128, 128>>>
                                                  (detail::cuda_arg<unsigned int>(sp_mat.handle1().cuda_handle()),
                                                   detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                                   detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),

                                                   detail::cuda_arg<NumericT>(d_mat),
                                                   static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                                   detail::cuda_arg<NumericT>(result),
                                                   static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                  );
    VIENNACL_CUDA_LAST_ERROR_CHECK("compressed_matrix_d_mat_mul_kernel");
  }
  else if (d_mat.row_major() && !result.row_major())
  {
    compressed_matrix_d_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<column_major> ><<<128, 128>>>
                                                  (detail::cuda_arg<unsigned int>(sp_mat.handle1().cuda_handle()),
                                                   detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                                   detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),

                                                   detail::cuda_arg<NumericT>(d_mat),
                                                   static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                                   detail::cuda_arg<NumericT>(result),
                                                   static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                  );
    VIENNACL_CUDA_LAST_ERROR_CHECK("compressed_matrix_d_mat_mul_kernel");
  }
  else if (!d_mat.row_major() && result.row_major())
  {
    compressed_matrix_d_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<row_major> ><<<128, 128>>>
                                                  (detail::cuda_arg<unsigned int>(sp_mat.handle1().cuda_handle()),
                                                   detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                                   detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),

                                                   detail::cuda_arg<NumericT>(d_mat),
                                                   static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                                   detail::cuda_arg<NumericT>(result),
                                                   static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                  );
    VIENNACL_CUDA_LAST_ERROR_CHECK("compressed_matrix_d_mat_mul_kernel");
  }
  else
  {
    compressed_matrix_d_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<column_major> ><<<128, 128>>>
                                                  (detail::cuda_arg<unsigned int>(sp_mat.handle1().cuda_handle()),
                                                   detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                                   detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),

                                                   detail::cuda_arg<NumericT>(d_mat),
                                                   static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                                   detail::cuda_arg<NumericT>(result),
                                                   static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                  );
    VIENNACL_CUDA_LAST_ERROR_CHECK("compressed_matrix_d_mat_mul_kernel");
  }
}


template<typename DMatIndexT, typename ResultIndexT, typename NumericT>
__global__ void compressed_matrix_d_tr_mat_mul_kernel(
          const unsigned int * sp_mat_row_indices,
          const unsigned int * sp_mat_col_indices,
          const NumericT * sp_mat_elements,
          const NumericT * d_mat,
          unsigned int d_mat_row_start,
          unsigned int d_mat_col_start,
          unsigned int d_mat_row_inc,
          unsigned int d_mat_col_inc,
          unsigned int d_mat_row_size,
          unsigned int d_mat_col_size,
          unsigned int d_mat_internal_rows,
          unsigned int d_mat_internal_cols,
          NumericT * result,
          unsigned int result_row_start,
          unsigned int result_col_start,
          unsigned int result_row_inc,
          unsigned int result_col_inc,
          unsigned int result_row_size,
          unsigned int result_col_size,
          unsigned int result_internal_rows,
          unsigned int result_internal_cols)
{
  for (unsigned int row  = blockIdx.x; row  < result_row_size; row += gridDim.x)
  {
    unsigned int row_start = sp_mat_row_indices[row];
    unsigned int row_end = sp_mat_row_indices[row+1];

    for ( unsigned int col = threadIdx.x; col < result_col_size; col += blockDim.x)
    {
      NumericT r = 0;

      for (unsigned int k = row_start; k < row_end; k++)
      {
        unsigned int j = sp_mat_col_indices[k];
        NumericT x = sp_mat_elements[k];
        NumericT y = d_mat[ DMatIndexT::apply(col, j,
                                              d_mat_row_start, d_mat_row_inc,
                                              d_mat_col_start, d_mat_col_inc,
                                              d_mat_internal_rows, d_mat_internal_cols) ];

        r += x * y;
      }

      result [ ResultIndexT::apply(row, col,
                                   result_row_start, result_row_inc,
                                   result_col_start, result_col_inc,
                                   result_internal_rows, result_internal_cols) ] = r;
    }
  }

}

/** @brief Carries out matrix-trans(matrix) multiplication first matrix being compressed
*          and the second transposed
*
* Implementation of the convenience expression result = prod(sp_mat, d_mat);
*
* @param sp_mat             The sparse matrix
* @param d_mat              The transposed dense matrix proxy
* @param result             The result matrix
*/
template<typename NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::compressed_matrix<NumericT, AlignmentV> & sp_mat,
               const viennacl::matrix_expression< const viennacl::matrix_base<NumericT>,
                                                  const viennacl::matrix_base<NumericT>,
                                                  viennacl::op_trans > & d_mat,
                viennacl::matrix_base<NumericT> & result)
{

  if (d_mat.lhs().row_major() && result.row_major())
  {
    compressed_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<row_major> ><<<128, 128>>>
                                                (detail::cuda_arg<unsigned int>(sp_mat.handle1().cuda_handle()),
                                                 detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                                 detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),

                                                 detail::cuda_arg<NumericT>(d_mat.lhs()),
                                                 static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                                 detail::cuda_arg<NumericT>(result),
                                                 static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                );
    VIENNACL_CUDA_LAST_ERROR_CHECK("compressed_matrix_d_tr_mat_mul_kernel");
  }
  else if (d_mat.lhs().row_major() && !result.row_major())
  {
    compressed_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<column_major> ><<<128, 128>>>
                                                (detail::cuda_arg<unsigned int>(sp_mat.handle1().cuda_handle()),
                                                 detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                                 detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),

                                                 detail::cuda_arg<NumericT>(d_mat.lhs()),
                                                 static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                                 detail::cuda_arg<NumericT>(result),
                                                 static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                );
    VIENNACL_CUDA_LAST_ERROR_CHECK("compressed_matrix_d_tr_mat_mul_kernel");
  }
  else if (!d_mat.lhs().row_major() && result.row_major())
  {
    compressed_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<row_major> ><<<128, 128>>>
                                                (detail::cuda_arg<unsigned int>(sp_mat.handle1().cuda_handle()),
                                                 detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                                 detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),

                                                 detail::cuda_arg<NumericT>(d_mat.lhs()),
                                                 static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                                 detail::cuda_arg<NumericT>(result),
                                                 static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                );
    VIENNACL_CUDA_LAST_ERROR_CHECK("compressed_matrix_d_tr_mat_mul_kernel");
  }
  else
  {
    compressed_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<column_major> ><<<128, 128>>>
                                                (detail::cuda_arg<unsigned int>(sp_mat.handle1().cuda_handle()),
                                                 detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                                 detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),

                                                 detail::cuda_arg<NumericT>(d_mat.lhs()),
                                                 static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                                 static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                                 detail::cuda_arg<NumericT>(result),
                                                 static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                 static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                );
    VIENNACL_CUDA_LAST_ERROR_CHECK("compressed_matrix_d_tr_mat_mul_kernel");
  }
}


//
// triangular solves for compressed_matrix
//

template<typename NumericT>
__global__ void compressed_matrix_diagonal_kernel(
          const unsigned int * row_indices,
          const unsigned int * column_indices,
          const NumericT * elements,
          NumericT * result,
          unsigned int size)
{
  for (unsigned int row  = blockDim.x * blockIdx.x + threadIdx.x;
                    row  < size;
                    row += gridDim.x * blockDim.x)
  {
    NumericT diag = NumericT(0);
    unsigned int row_end = row_indices[row+1];
    for (unsigned int i = row_indices[row]; i < row_end; ++i)
    {
      unsigned int col_index = column_indices[i];
      if (col_index == row)
      {
        diag = elements[i];
        break;
      }
    }
    result[row] = diag;
  }
}


/** @brief Carries out triangular inplace solves
*
* @param mat    The matrix
* @param vec    The vector holding the right hand side. Is overwritten by the solution.
*/
template<typename SparseMatrixT, typename NumericT>
typename viennacl::enable_if< viennacl::is_any_sparse_matrix<SparseMatrixT>::value>::type
inplace_solve(const SparseMatrixT & mat,
              viennacl::vector_base<NumericT> & vec,
              viennacl::linalg::unit_lower_tag)
{
  csr_unit_lu_forward_kernel<<<1, 128>>>(detail::cuda_arg<unsigned int>(mat.handle1().cuda_handle()),
                                         detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
                                         detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                         detail::cuda_arg<NumericT>(vec),
                                         static_cast<unsigned int>(mat.size1())
                                        );
  VIENNACL_CUDA_LAST_ERROR_CHECK("csr_unit_lu_forward_kernel");
}


/** @brief Carries out triangular inplace solves
*
* @param mat    The matrix
* @param vec    The vector holding the right hand side. Is overwritten by the solution.
*/
template<typename SparseMatrixT, typename NumericT>
typename viennacl::enable_if< viennacl::is_any_sparse_matrix<SparseMatrixT>::value>::type
inplace_solve(const SparseMatrixT & mat,
              viennacl::vector_base<NumericT> & vec,
              viennacl::linalg::lower_tag)
{
  csr_lu_forward_kernel<<<1, 128>>>(detail::cuda_arg<unsigned int>(mat.handle1().cuda_handle()),
                                    detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
                                    detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                    detail::cuda_arg<NumericT>(vec),
                                    static_cast<unsigned int>(mat.size1())
                                   );
  VIENNACL_CUDA_LAST_ERROR_CHECK("csr_lu_forward_kernel");
}



/** @brief Carries out triangular inplace solves
*
* @param mat    The matrix
* @param vec    The vector holding the right hand side. Is overwritten by the solution.
*/
template<typename SparseMatrixT, typename NumericT>
typename viennacl::enable_if< viennacl::is_any_sparse_matrix<SparseMatrixT>::value>::type
inplace_solve(const SparseMatrixT & mat,
              viennacl::vector_base<NumericT> & vec,
              viennacl::linalg::unit_upper_tag)
{
  csr_unit_lu_backward_kernel<<<1, 128>>>(detail::cuda_arg<unsigned int>(mat.handle1().cuda_handle()),
                                    detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
                                    detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                    detail::cuda_arg<NumericT>(vec),
                                    static_cast<unsigned int>(mat.size1())
                                   );
  VIENNACL_CUDA_LAST_ERROR_CHECK("csr_unit_lu_backward_kernel");
}


/** @brief Carries out triangular inplace solves
*
* @param mat    The matrix
* @param vec    The vector holding the right hand side. Is overwritten by the solution.
*/
template<typename SparseMatrixT, typename NumericT>
typename viennacl::enable_if< viennacl::is_any_sparse_matrix<SparseMatrixT>::value>::type
inplace_solve(const SparseMatrixT & mat,
              viennacl::vector_base<NumericT> & vec,
              viennacl::linalg::upper_tag)
{
  csr_lu_backward_kernel<<<1, 128>>>(detail::cuda_arg<unsigned int>(mat.handle1().cuda_handle()),
                                    detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
                                    detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                    detail::cuda_arg<NumericT>(vec),
                                    static_cast<unsigned int>(mat.size1())
                                   );
  VIENNACL_CUDA_LAST_ERROR_CHECK("csr_lu_backward_kernel");
}



// transposed

/** @brief Carries out triangular inplace solves
*
* @param mat    The matrix
* @param vec    The vector holding the right hand side. Is overwritten by the solution.
*/
template<typename SparseMatrixT, typename NumericT>
typename viennacl::enable_if< viennacl::is_any_sparse_matrix<SparseMatrixT>::value>::type
inplace_solve(const matrix_expression<const SparseMatrixT, const SparseMatrixT, op_trans> & mat,
              viennacl::vector_base<NumericT> & vec,
              viennacl::linalg::unit_lower_tag)
{
  csr_trans_unit_lu_forward_kernel<<<1, 128>>>(detail::cuda_arg<unsigned int>(mat.lhs().handle1().cuda_handle()),
                                          detail::cuda_arg<unsigned int>(mat.lhs().handle2().cuda_handle()),
                                          detail::cuda_arg<NumericT>(mat.lhs().handle().cuda_handle()),
                                          detail::cuda_arg<NumericT>(vec),
                                          static_cast<unsigned int>(mat.lhs().size1())
                                         );
  VIENNACL_CUDA_LAST_ERROR_CHECK("csr_trans_unit_lu_forward_kernel");
}


/** @brief Carries out triangular inplace solves
*
* @param mat    The matrix
* @param vec    The vector holding the right hand side. Is overwritten by the solution.
*/
template<typename SparseMatrixT, typename NumericT>
typename viennacl::enable_if< viennacl::is_any_sparse_matrix<SparseMatrixT>::value>::type
inplace_solve(const matrix_expression<const SparseMatrixT, const SparseMatrixT, op_trans> & mat,
              viennacl::vector_base<NumericT> & vec,
              viennacl::linalg::lower_tag)
{
  viennacl::vector<NumericT> diagonal(vec.size());

  compressed_matrix_diagonal_kernel<<<1, 128>>>(detail::cuda_arg<unsigned int>(mat.lhs().handle1().cuda_handle()),
                                                detail::cuda_arg<unsigned int>(mat.lhs().handle2().cuda_handle()),
                                                detail::cuda_arg<NumericT>(mat.lhs().handle().cuda_handle()),
                                                detail::cuda_arg<NumericT>(diagonal),
                                                static_cast<unsigned int>(mat.size1())
                                               );

  csr_trans_lu_forward_kernel<<<1, 128>>>(detail::cuda_arg<unsigned int>(mat.lhs().handle1().cuda_handle()),
                                          detail::cuda_arg<unsigned int>(mat.lhs().handle2().cuda_handle()),
                                          detail::cuda_arg<NumericT>(mat.lhs().handle().cuda_handle()),
                                          detail::cuda_arg<NumericT>(diagonal),
                                          detail::cuda_arg<NumericT>(vec),
                                          static_cast<unsigned int>(mat.lhs().size1())
                                         );
  VIENNACL_CUDA_LAST_ERROR_CHECK("csr_trans_lu_forward_kernel");
}


/** @brief Carries out triangular inplace solves
*
* @param mat    The matrix
* @param vec    The vector holding the right hand side. Is overwritten by the solution.
*/
template<typename SparseMatrixT, typename NumericT>
typename viennacl::enable_if< viennacl::is_any_sparse_matrix<SparseMatrixT>::value>::type
inplace_solve(const matrix_expression<const SparseMatrixT, const SparseMatrixT, op_trans> & mat,
              viennacl::vector_base<NumericT> & vec,
              viennacl::linalg::unit_upper_tag)
{
  csr_trans_unit_lu_backward_kernel<<<1, 128>>>(detail::cuda_arg<unsigned int>(mat.lhs().handle1().cuda_handle()),
                                                detail::cuda_arg<unsigned int>(mat.lhs().handle2().cuda_handle()),
                                                detail::cuda_arg<NumericT>(mat.lhs().handle().cuda_handle()),
                                                detail::cuda_arg<NumericT>(vec),
                                                static_cast<unsigned int>(mat.lhs().size1())
                                              );
  VIENNACL_CUDA_LAST_ERROR_CHECK("csr_trans_unit_lu_backward_kernel");
}


/** @brief Carries out triangular inplace solves
*
* @param mat    The matrix
* @param vec    The vector holding the right hand side. Is overwritten by the solution.
*/
template<typename SparseMatrixT, typename NumericT>
typename viennacl::enable_if< viennacl::is_any_sparse_matrix<SparseMatrixT>::value>::type
inplace_solve(const matrix_expression<const SparseMatrixT, const SparseMatrixT, op_trans> & mat,
              viennacl::vector_base<NumericT> & vec,
              viennacl::linalg::upper_tag)
{
  viennacl::vector<NumericT> diagonal(vec.size());

  compressed_matrix_diagonal_kernel<<<1, 128>>>(detail::cuda_arg<unsigned int>(mat.lhs().handle1().cuda_handle()),
                                                detail::cuda_arg<unsigned int>(mat.lhs().handle2().cuda_handle()),
                                                detail::cuda_arg<NumericT>(mat.lhs().handle().cuda_handle()),
                                                detail::cuda_arg<NumericT>(diagonal),
                                                static_cast<unsigned int>(mat.size1())
                                               );

  csr_trans_lu_backward_kernel<<<1, 128>>>(detail::cuda_arg<unsigned int>(mat.lhs().handle1().cuda_handle()),
                                           detail::cuda_arg<unsigned int>(mat.lhs().handle2().cuda_handle()),
                                           detail::cuda_arg<NumericT>(mat.lhs().handle().cuda_handle()),
                                           detail::cuda_arg<NumericT>(diagonal),
                                           detail::cuda_arg<NumericT>(vec),
                                           static_cast<unsigned int>(mat.lhs().size1())
                                          );
  VIENNACL_CUDA_LAST_ERROR_CHECK("csr_trans_lu_backward_kernel");
}

namespace detail
{
  //
  // block solves
  //
  template<typename NumericT, unsigned int AlignmentV>
  void block_inplace_solve(const matrix_expression<const compressed_matrix<NumericT, AlignmentV>,
                                                   const compressed_matrix<NumericT, AlignmentV>,
                                                   op_trans> & L,
                           viennacl::backend::mem_handle const & block_indices, vcl_size_t num_blocks,
                           vector_base<NumericT> const & /* L_diagonal */,  //ignored
                           vector_base<NumericT> & vec,
                           viennacl::linalg::unit_lower_tag)
  {
    csr_block_trans_unit_lu_forward<<<num_blocks, 128>>>(detail::cuda_arg<unsigned int>(L.lhs().handle1().cuda_handle()),
                                                         detail::cuda_arg<unsigned int>(L.lhs().handle2().cuda_handle()),
                                                         detail::cuda_arg<NumericT>(L.lhs().handle().cuda_handle()),
                                                         detail::cuda_arg<unsigned int>(block_indices.cuda_handle()),
                                                         detail::cuda_arg<NumericT>(vec),
                                                         static_cast<unsigned int>(L.lhs().size1())
                                                        );
  }


  template<typename NumericT, unsigned int AlignmentV>
  void block_inplace_solve(const matrix_expression<const compressed_matrix<NumericT, AlignmentV>,
                                                   const compressed_matrix<NumericT, AlignmentV>,
                                                   op_trans> & U,
                           viennacl::backend::mem_handle const & block_indices, vcl_size_t num_blocks,
                           vector_base<NumericT> const & U_diagonal,
                           vector_base<NumericT> & vec,
                           viennacl::linalg::upper_tag)
  {
    csr_block_trans_lu_backward<<<num_blocks, 128>>>(detail::cuda_arg<unsigned int>(U.lhs().handle1().cuda_handle()),
                                                     detail::cuda_arg<unsigned int>(U.lhs().handle2().cuda_handle()),
                                                     detail::cuda_arg<NumericT>(U.lhs().handle().cuda_handle()),
                                                     detail::cuda_arg<NumericT>(U_diagonal.handle().cuda_handle()),
                                                     detail::cuda_arg<unsigned int>(block_indices.cuda_handle()),
                                                     detail::cuda_arg<NumericT>(vec),
                                                     static_cast<unsigned int>(U.lhs().size1())
                                                    );
  }


}


//
// Compressed Compressed Matrix
//

template<typename NumericT>
__global__ void compressed_compressed_matrix_vec_mul_kernel(
          const unsigned int * row_jumper,
          const unsigned int * row_indices,
          const unsigned int * column_indices,
          const NumericT * elements,
          unsigned int nonzero_rows,
          const NumericT * x,
          unsigned int start_x,
          unsigned int inc_x,
          NumericT * result,
          unsigned int start_result,
          unsigned int inc_result,
          unsigned int size_result)
{
  for (unsigned int i  = blockDim.x * blockIdx.x + threadIdx.x;
                    i  < size_result;
                    i += gridDim.x * blockDim.x)
  {
    result[i * inc_result + start_result] = 0;
  }

  for (unsigned int i  = blockDim.x * blockIdx.x + threadIdx.x;
                    i  < nonzero_rows;
                    i += gridDim.x * blockDim.x)
  {
    NumericT dot_prod = NumericT(0);
    unsigned int row_end = row_jumper[i+1];
    for (unsigned int j = row_jumper[i]; j < row_end; ++j)
      dot_prod += elements[j] * x[column_indices[j] * inc_x + start_x];
    result[row_indices[i] * inc_result + start_result] = dot_prod;
  }
}


/** @brief Carries out matrix-vector multiplication with a compressed_compressed_matrix
*
* Implementation of the convenience expression result = prod(mat, vec);
*
* @param mat    The matrix
* @param vec    The vector
* @param result The result vector
*/
template<typename NumericT>
void prod_impl(const viennacl::compressed_compressed_matrix<NumericT> & mat,
               const viennacl::vector_base<NumericT> & vec,
                     viennacl::vector_base<NumericT> & result)
{
  compressed_compressed_matrix_vec_mul_kernel<<<128, 128>>>(detail::cuda_arg<unsigned int>(mat.handle1().cuda_handle()),
                                                            detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
                                                            detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
                                                            detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                                            static_cast<unsigned int>(mat.nnz1()),
                                                            detail::cuda_arg<NumericT>(vec),
                                                            static_cast<unsigned int>(vec.start()),
                                                            static_cast<unsigned int>(vec.stride()),
                                                            detail::cuda_arg<NumericT>(result),
                                                            static_cast<unsigned int>(result.start()),
                                                            static_cast<unsigned int>(result.stride()),
                                                            static_cast<unsigned int>(result.size())
                                                           );
  VIENNACL_CUDA_LAST_ERROR_CHECK("compressed_compressed_matrix_vec_mul_kernel");
}

//
// Coordinate Matrix
//


namespace detail
{

  template<typename NumericT>
  __global__ void coo_row_info_extractor( const unsigned int * coords, //(row_index, column_index)
                                          const NumericT * elements,
                                          const unsigned int * group_boundaries,
                                          NumericT * result,
                                          unsigned int option)
  {
    __shared__ unsigned int shared_rows[128];
    __shared__ NumericT inter_results[128];

    uint2 tmp;
    NumericT val;
    unsigned int last_index  = blockDim.x - 1;
    unsigned int group_start = group_boundaries[blockIdx.x];
    unsigned int group_end   = group_boundaries[blockIdx.x + 1];
    unsigned int k_end = (group_end > group_start) ? 1 + (group_end - group_start - 1) / blockDim.x : 0;   // -1 in order to have correct behavior if group_end - group_start == j * blockDim.x

    unsigned int local_index = 0;

    for (unsigned int k = 0; k < k_end; ++k)
    {
      local_index = group_start + k * blockDim.x + threadIdx.x;

      tmp = (local_index < group_end) ? ((const uint2 *)coords)[local_index] : ::make_uint2(0, 0);
      val = (local_index < group_end && (option != 3 || tmp.x == tmp.y) ) ? elements[local_index] : 0;

      //check for carry from previous loop run:
      if (threadIdx.x == 0 && k > 0)
      {
        if (tmp.x == shared_rows[last_index])
        {
          switch (option)
          {
            case 0: //inf-norm
            case 3: //diagonal entry
              val = max(val, fabs(inter_results[last_index]));
              break;

            case 1: //1-norm
              val = fabs(val) + inter_results[last_index];
              break;

            case 2: //2-norm
              val = sqrt(val * val + inter_results[last_index]);
              break;

            default:
              break;
          }
        }
        else
        {
          switch (option)
          {
            case 0: //inf-norm
            case 1: //1-norm
            case 3: //diagonal entry
              result[shared_rows[last_index]] = inter_results[last_index];
              break;

            case 2: //2-norm
              result[shared_rows[last_index]] = sqrt(inter_results[last_index]);
            default:
              break;
          }
        }
      }

      //segmented parallel reduction begin
      __syncthreads();
      shared_rows[threadIdx.x] = tmp.x;
      switch (option)
      {
        case 0:
        case 3:
          inter_results[threadIdx.x] = val;
          break;
        case 1:
          inter_results[threadIdx.x] = fabs(val);
          break;
        case 2:
          inter_results[threadIdx.x] = val * val;
        default:
          break;
      }
      __syncthreads();

      for (unsigned int stride = 1; stride < blockDim.x; stride *= 2)
      {
        NumericT left = (threadIdx.x >= stride && tmp.x == shared_rows[threadIdx.x - stride]) ? inter_results[threadIdx.x - stride] : 0;
        __syncthreads();
        switch (option)
        {
          case 0: //inf-norm
          case 3: //diagonal entry
            inter_results[threadIdx.x] = max(inter_results[threadIdx.x], left);
            break;

          case 1: //1-norm
            inter_results[threadIdx.x] += left;
            break;

          case 2: //2-norm
            inter_results[threadIdx.x] += left;
            break;

          default:
            break;
        }
        __syncthreads();
      }
      //segmented parallel reduction end

      if (threadIdx.x != last_index &&
          shared_rows[threadIdx.x] != shared_rows[threadIdx.x + 1] &&
          inter_results[threadIdx.x] != 0)
      {
        result[tmp.x] = (option == 2) ? sqrt(inter_results[threadIdx.x]) : inter_results[threadIdx.x];
      }

      __syncthreads();
    } //for k

    if (local_index + 1 == group_end && inter_results[threadIdx.x] != 0)
      result[tmp.x] = (option == 2) ? sqrt(inter_results[threadIdx.x]) : inter_results[threadIdx.x];
  }

  template<typename NumericT, unsigned int AlignmentV>
  void row_info(coordinate_matrix<NumericT, AlignmentV> const & mat,
                vector_base<NumericT> & vec,
                viennacl::linalg::detail::row_info_types info_selector)
  {
    coo_row_info_extractor<<<64, 128>>>(detail::cuda_arg<unsigned int>(mat.handle12().cuda_handle()),
                                         detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                         detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
                                         detail::cuda_arg<NumericT>(vec),
                                         static_cast<unsigned int>(info_selector)
                                        );
    VIENNACL_CUDA_LAST_ERROR_CHECK("coo_row_info_extractor");
  }

} //namespace detail


template<typename NumericT>
__global__ void coordinate_matrix_vec_mul_kernel(const unsigned int * coords, //(row_index, column_index)
                                                 const NumericT * elements,
                                                 const unsigned int * group_boundaries,
                                                 const NumericT * x,
                                                 unsigned int start_x,
                                                 unsigned int inc_x,
                                                       NumericT * result,
                                                 unsigned int start_result,
                                                 unsigned int inc_result
                                                 )
{
  __shared__ unsigned int shared_rows[128];
  __shared__ NumericT inter_results[128];

  uint2 tmp;
  NumericT val;
  unsigned int group_start = group_boundaries[blockIdx.x];
  unsigned int group_end   = group_boundaries[blockIdx.x + 1];
  unsigned int k_end = (group_end > group_start) ? 1 + (group_end - group_start - 1) / blockDim.x : 0;   // -1 in order to have correct behavior if group_end - group_start == j * blockDim.x

  unsigned int local_index = 0;

  for (unsigned int k = 0; k < k_end; ++k)
  {
    local_index = group_start + k * blockDim.x + threadIdx.x;

    tmp = (local_index < group_end) ? ((const uint2 *)coords)[local_index] : ::make_uint2(0, 0);
    val = (local_index < group_end) ? elements[local_index] * x[tmp.y * inc_x + start_x] : 0;

    //check for carry from previous loop run:
    if (threadIdx.x == 0 && k > 0)
    {
      if (tmp.x == shared_rows[blockDim.x-1])
        val += inter_results[blockDim.x-1];
      else
        result[shared_rows[blockDim.x-1] * inc_result + start_result] = inter_results[blockDim.x-1];
    }

    //segmented parallel reduction begin
    __syncthreads();
    shared_rows[threadIdx.x] = tmp.x;
    inter_results[threadIdx.x] = val;
    NumericT left = 0;
    __syncthreads();

    for (unsigned int stride = 1; stride < blockDim.x; stride *= 2)
    {
      left = (threadIdx.x >= stride && tmp.x == shared_rows[threadIdx.x - stride]) ? inter_results[threadIdx.x - stride] : 0;
      __syncthreads();
      inter_results[threadIdx.x] += left;
      __syncthreads();
    }
    //segmented parallel reduction end

    if (local_index < group_end && threadIdx.x < blockDim.x-1 &&
        shared_rows[threadIdx.x] != shared_rows[threadIdx.x + 1])
    {
      result[tmp.x * inc_result + start_result] = inter_results[threadIdx.x];
    }

    __syncthreads();
  } //for k

  if (local_index + 1 == group_end)
    result[tmp.x * inc_result + start_result] = inter_results[threadIdx.x];
}


/** @brief Carries out matrix-vector multiplication with a coordinate_matrix
*
* Implementation of the convenience expression result = prod(mat, vec);
*
* @param mat    The matrix
* @param vec    The vector
* @param result The result vector
*/
template<typename NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::coordinate_matrix<NumericT, AlignmentV> & mat,
               const viennacl::vector_base<NumericT> & vec,
                     viennacl::vector_base<NumericT> & result)
{
  result.clear();

  coordinate_matrix_vec_mul_kernel<<<64, 128>>>(detail::cuda_arg<unsigned int>(mat.handle12().cuda_handle()),
                                                detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                                detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
                                                detail::cuda_arg<NumericT>(vec),
                                                static_cast<unsigned int>(vec.start()),
                                                static_cast<unsigned int>(vec.stride()),
                                                detail::cuda_arg<NumericT>(result),
                                                static_cast<unsigned int>(result.start()),
                                                static_cast<unsigned int>(result.stride())
                                               );
  VIENNACL_CUDA_LAST_ERROR_CHECK("coordinate_matrix_vec_mul_kernel");
}




template<typename DMatIndexT, typename ResultIndexT, typename NumericT>
__global__ void coordinate_matrix_d_mat_mul_kernel(const unsigned int * coords, //(row_index, column_index)
                                                   const NumericT * elements,
                                                   const unsigned int * group_boundaries,
                                                   const NumericT * d_mat,
                                                   unsigned int d_mat_row_start,
                                                   unsigned int d_mat_col_start,
                                                   unsigned int d_mat_row_inc,
                                                   unsigned int d_mat_col_inc,
                                                   unsigned int d_mat_row_size,
                                                   unsigned int d_mat_col_size,
                                                   unsigned int d_mat_internal_rows,
                                                   unsigned int d_mat_internal_cols,
                                                   NumericT * result,
                                                   unsigned int result_row_start,
                                                   unsigned int result_col_start,
                                                   unsigned int result_row_inc,
                                                   unsigned int result_col_inc,
                                                   unsigned int result_row_size,
                                                   unsigned int result_col_size,
                                                   unsigned int result_internal_rows,
                                                   unsigned int result_internal_cols)
{
  __shared__ unsigned int shared_rows[128];
  __shared__ NumericT inter_results[128];

  uint2 tmp;
  NumericT val;
  unsigned int group_start = group_boundaries[blockIdx.x];
  unsigned int group_end   = group_boundaries[blockIdx.x + 1];
  unsigned int k_end = (group_end > group_start) ? 1 + (group_end - group_start - 1) / blockDim.x : 0;   // -1 in order to have correct behavior if group_end - group_start == j * blockDim.x

  unsigned int local_index = 0;

  for (unsigned int result_col = 0; result_col < result_col_size; ++result_col)
  {
    for (unsigned int k = 0; k < k_end; ++k)
    {
      local_index = group_start + k * blockDim.x + threadIdx.x;

      tmp = (local_index < group_end) ? ((const uint2 *)coords)[local_index] : ::make_uint2(0, 0);
      val = (local_index < group_end) ? elements[local_index] * d_mat[DMatIndexT::apply(tmp.y, result_col,
                                                                                        d_mat_row_start, d_mat_row_inc,
                                                                                        d_mat_col_start, d_mat_col_inc,
                                                                                        d_mat_internal_rows, d_mat_internal_cols) ] : 0;

      //check for carry from previous loop run:
      if (threadIdx.x == 0 && k > 0)
      {
        if (tmp.x == shared_rows[blockDim.x-1])
          val += inter_results[blockDim.x-1];
        else
          result[ResultIndexT::apply(shared_rows[blockDim.x-1], result_col,
                                     result_row_start, result_row_inc,
                                     result_col_start, result_col_inc,
                                     result_internal_rows, result_internal_cols)] = inter_results[blockDim.x-1];
      }

      //segmented parallel reduction begin
      __syncthreads();
      shared_rows[threadIdx.x] = tmp.x;
      inter_results[threadIdx.x] = val;
      NumericT left = 0;
      __syncthreads();

      for (unsigned int stride = 1; stride < blockDim.x; stride *= 2)
      {
        left = (threadIdx.x >= stride && tmp.x == shared_rows[threadIdx.x - stride]) ? inter_results[threadIdx.x - stride] : 0;
        __syncthreads();
        inter_results[threadIdx.x] += left;
        __syncthreads();
      }
      //segmented parallel reduction end

      if (local_index < group_end && threadIdx.x < blockDim.x-1 &&
          shared_rows[threadIdx.x] != shared_rows[threadIdx.x + 1])
      {
        result[ResultIndexT::apply(tmp.x, result_col,
                                   result_row_start, result_row_inc,
                                   result_col_start, result_col_inc,
                                   result_internal_rows, result_internal_cols)] = inter_results[threadIdx.x];
      }

      __syncthreads();
    } //for k

    if (local_index + 1 == group_end)
      result[ResultIndexT::apply(tmp.x, result_col,
                                 result_row_start, result_row_inc,
                                 result_col_start, result_col_inc,
                                 result_internal_rows, result_internal_cols)] = inter_results[threadIdx.x];
  }
}


/** @brief Carries out Compressed Matrix(COO)-Dense Matrix multiplication
*
* Implementation of the convenience expression result = prod(sp_mat, d_mat);
*
* @param sp_mat     The Sparse Matrix (Coordinate format)
* @param d_mat      The Dense Matrix
* @param result     The Result Matrix
*/
template<typename NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::coordinate_matrix<NumericT, AlignmentV> & sp_mat,
               const viennacl::matrix_base<NumericT> & d_mat,
                     viennacl::matrix_base<NumericT> & result)
{
  if (d_mat.row_major() && result.row_major())
  {
    coordinate_matrix_d_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<row_major> ><<<64, 128>>>
                                                  (detail::cuda_arg<unsigned int>(sp_mat.handle12().cuda_handle()),
                                                   detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                                   detail::cuda_arg<unsigned int>(sp_mat.handle3().cuda_handle()),

                                                   detail::cuda_arg<NumericT>(d_mat),
                                                   static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                                   detail::cuda_arg<NumericT>(result),
                                                   static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                   );
    VIENNACL_CUDA_LAST_ERROR_CHECK("coordinate_matrix_d_mat_mul_kernel");
  }
  else if (d_mat.row_major() && !result.row_major())
  {
    coordinate_matrix_d_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<column_major> ><<<64, 128>>>
                                                  (detail::cuda_arg<unsigned int>(sp_mat.handle12().cuda_handle()),
                                                   detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                                   detail::cuda_arg<unsigned int>(sp_mat.handle3().cuda_handle()),

                                                   detail::cuda_arg<NumericT>(d_mat),
                                                   static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                                   detail::cuda_arg<NumericT>(result),
                                                   static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                   );
    VIENNACL_CUDA_LAST_ERROR_CHECK("coordinate_matrix_d_mat_mul_kernel");
  }
  else if (!d_mat.row_major() && result.row_major())
  {
    coordinate_matrix_d_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<row_major> ><<<64, 128>>>
                                                  (detail::cuda_arg<unsigned int>(sp_mat.handle12().cuda_handle()),
                                                   detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                                   detail::cuda_arg<unsigned int>(sp_mat.handle3().cuda_handle()),

                                                   detail::cuda_arg<NumericT>(d_mat),
                                                   static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                                   detail::cuda_arg<NumericT>(result),
                                                   static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                   );
    VIENNACL_CUDA_LAST_ERROR_CHECK("coordinate_matrix_d_mat_mul_kernel");
  }
  else
  {
    coordinate_matrix_d_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<column_major> ><<<64, 128>>>
                                                  (detail::cuda_arg<unsigned int>(sp_mat.handle12().cuda_handle()),
                                                   detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                                   detail::cuda_arg<unsigned int>(sp_mat.handle3().cuda_handle()),

                                                   detail::cuda_arg<NumericT>(d_mat),
                                                   static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                                   detail::cuda_arg<NumericT>(result),
                                                   static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                   static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                   );
    VIENNACL_CUDA_LAST_ERROR_CHECK("coordinate_matrix_d_mat_mul_kernel");
  }

}

template<typename DMatIndexT, typename ResultIndexT, typename NumericT>
__global__ void coordinate_matrix_d_tr_mat_mul_kernel(const unsigned int * coords, //(row_index, column_index)
                                                     const NumericT * elements,
                                                     const unsigned int * group_boundaries,
                                                     const NumericT * d_mat,
                                                     unsigned int d_mat_row_start,
                                                     unsigned int d_mat_col_start,
                                                     unsigned int d_mat_row_inc,
                                                     unsigned int d_mat_col_inc,
                                                     unsigned int d_mat_row_size,
                                                     unsigned int d_mat_col_size,
                                                     unsigned int d_mat_internal_rows,
                                                     unsigned int d_mat_internal_cols,
                                                     NumericT * result,
                                                     unsigned int result_row_start,
                                                     unsigned int result_col_start,
                                                     unsigned int result_row_inc,
                                                     unsigned int result_col_inc,
                                                     unsigned int result_row_size,
                                                     unsigned int result_col_size,
                                                     unsigned int result_internal_rows,
                                                     unsigned int result_internal_cols)
{
  __shared__ unsigned int shared_rows[128];
  __shared__ NumericT inter_results[128];

  uint2 tmp;
  NumericT val;
  unsigned int group_start = group_boundaries[blockIdx.x];
  unsigned int group_end   = group_boundaries[blockIdx.x + 1];
  unsigned int k_end = (group_end > group_start) ? 1 + (group_end - group_start - 1) / blockDim.x : 0;   // -1 in order to have correct behavior if group_end - group_start == j * blockDim.x

  unsigned int local_index = 0;

  for (unsigned int result_col = 0; result_col < result_col_size; ++result_col)
  {
    for (unsigned int k = 0; k < k_end; ++k)
    {
      local_index = group_start + k * blockDim.x + threadIdx.x;

      tmp = (local_index < group_end) ? ((const uint2 *)coords)[local_index] : ::make_uint2(0, 0);
      val = (local_index < group_end) ? elements[local_index] * d_mat[DMatIndexT::apply(result_col, tmp.y,
                                                                                        d_mat_row_start, d_mat_row_inc,
                                                                                        d_mat_col_start, d_mat_col_inc,
                                                                                        d_mat_internal_rows, d_mat_internal_cols)] : 0;

      //check for carry from previous loop run:
      if (threadIdx.x == 0 && k > 0)
      {
        if (tmp.x == shared_rows[blockDim.x-1])
          val += inter_results[blockDim.x-1];
        else
          result[ResultIndexT::apply(shared_rows[blockDim.x-1], result_col,
                                     result_row_start, result_row_inc,
                                     result_col_start, result_col_inc,
                                     result_internal_rows, result_internal_cols) ] = inter_results[blockDim.x-1];
      }

      //segmented parallel reduction begin
      __syncthreads();
      shared_rows[threadIdx.x] = tmp.x;
      inter_results[threadIdx.x] = val;
      NumericT left = 0;
      __syncthreads();

      for (unsigned int stride = 1; stride < blockDim.x; stride *= 2)
      {
        left = (threadIdx.x >= stride && tmp.x == shared_rows[threadIdx.x - stride]) ? inter_results[threadIdx.x - stride] : 0;
        __syncthreads();
        inter_results[threadIdx.x] += left;
        __syncthreads();
      }
      //segmented parallel reduction end

      if (local_index < group_end && threadIdx.x < blockDim.x-1 &&
          shared_rows[threadIdx.x] != shared_rows[threadIdx.x + 1])
      {
        result[ ResultIndexT::apply(tmp.x, result_col,
                                    result_row_start, result_row_inc,
                                    result_col_start, result_col_inc,
                                    result_internal_rows, result_internal_cols) ] = inter_results[threadIdx.x];
      }

      __syncthreads();
    } //for k

    if (local_index + 1 == group_end)
      result[ ResultIndexT::apply(tmp.x, result_col,
                                  result_row_start, result_row_inc,
                                  result_col_start, result_col_inc,
                                  result_internal_rows, result_internal_cols) ] = inter_results[threadIdx.x];
  }
}

/** @brief Carries out Compressed Matrix(COO)-Dense Transposed Matrix multiplication
*
* Implementation of the convenience expression result = prod(sp_mat, trans(d_mat));
*
* @param sp_mat     The Sparse Matrix (Coordinate format)
* @param d_mat      The Dense Transposed Matrix
* @param result     The Result Matrix
*/
template<typename NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::coordinate_matrix<NumericT, AlignmentV> & sp_mat,
               const viennacl::matrix_expression< const viennacl::matrix_base<NumericT>,
                                                  const viennacl::matrix_base<NumericT>,
                                                  viennacl::op_trans > & d_mat,
                     viennacl::matrix_base<NumericT> & result)
{
  if (d_mat.lhs().row_major() && result.row_major())
  {
    coordinate_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<row_major> ><<<64, 128>>>
                                                    (detail::cuda_arg<unsigned int>(sp_mat.handle12().cuda_handle()),
                                                     detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                                     detail::cuda_arg<unsigned int>(sp_mat.handle3().cuda_handle()),

                                                     detail::cuda_arg<NumericT>(d_mat.lhs()),
                                                     static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                                     detail::cuda_arg<NumericT>(result),
                                                     static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                    );
    VIENNACL_CUDA_LAST_ERROR_CHECK("coordinate_matrix_d_tr_mat_mul_kernel");
  }
  else if (d_mat.lhs().row_major() && !result.row_major())
  {
    coordinate_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<column_major> ><<<64, 128>>>
                                                    (detail::cuda_arg<unsigned int>(sp_mat.handle12().cuda_handle()),
                                                     detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                                     detail::cuda_arg<unsigned int>(sp_mat.handle3().cuda_handle()),

                                                     detail::cuda_arg<NumericT>(d_mat.lhs()),
                                                     static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                                     detail::cuda_arg<NumericT>(result),
                                                     static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                    );
    VIENNACL_CUDA_LAST_ERROR_CHECK("coordinate_matrix_d_tr_mat_mul_kernel");
  }
  else if (!d_mat.lhs().row_major() && result.row_major())
  {
    coordinate_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<row_major> ><<<64, 128>>>
                                                    (detail::cuda_arg<unsigned int>(sp_mat.handle12().cuda_handle()),
                                                     detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                                     detail::cuda_arg<unsigned int>(sp_mat.handle3().cuda_handle()),

                                                     detail::cuda_arg<NumericT>(d_mat.lhs()),
                                                     static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                                     detail::cuda_arg<NumericT>(result),
                                                     static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                    );
    VIENNACL_CUDA_LAST_ERROR_CHECK("coordinate_matrix_d_tr_mat_mul_kernel");
  }
  else
  {
    coordinate_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<column_major> ><<<64, 128>>>
                                                    (detail::cuda_arg<unsigned int>(sp_mat.handle12().cuda_handle()),
                                                     detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                                     detail::cuda_arg<unsigned int>(sp_mat.handle3().cuda_handle()),

                                                     detail::cuda_arg<NumericT>(d_mat.lhs()),
                                                     static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                                     static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                                     detail::cuda_arg<NumericT>(result),
                                                     static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                                     static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                                    );
    VIENNACL_CUDA_LAST_ERROR_CHECK("coordinate_matrix_d_tr_mat_mul_kernel");
  }
}


//
// ELL Matrix
//

template<typename NumericT>
__global__ void ell_matrix_vec_mul_kernel(const unsigned int * coords,
                                          const NumericT * elements,
                                          const NumericT * x,
                                          unsigned int start_x,
                                          unsigned int inc_x,
                                                NumericT * result,
                                          unsigned int start_result,
                                          unsigned int inc_result,
                                          unsigned int row_num,
                                          unsigned int col_num,
                                          unsigned int internal_row_num,
                                          unsigned int items_per_row,
                                          unsigned int aligned_items_per_row
                                         )
{
  unsigned int glb_id = blockDim.x * blockIdx.x + threadIdx.x;
  unsigned int glb_sz = gridDim.x * blockDim.x;

  for (unsigned int row_id = glb_id; row_id < row_num; row_id += glb_sz)
  {
    NumericT sum = 0;

    unsigned int offset = row_id;
    for (unsigned int item_id = 0; item_id < items_per_row; item_id++, offset += internal_row_num)
    {
      NumericT val = elements[offset];

      if (val != NumericT(0))
      {
        int col = coords[offset];
        sum += x[col * inc_x + start_x] * val;
      }
    }

    result[row_id * inc_result + start_result] = sum;
  }
}


/** @brief Carries out matrix-vector multiplication with a ell_matrix
*
* Implementation of the convenience expression result = prod(mat, vec);
*
* @param mat    The matrix
* @param vec    The vector
* @param result The result vector
*/
template<typename NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::ell_matrix<NumericT, AlignmentV> & mat,
               const viennacl::vector_base<NumericT> & vec,
                     viennacl::vector_base<NumericT> & result)
{
  ell_matrix_vec_mul_kernel<<<256, 128>>>(detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
                                          detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                          detail::cuda_arg<NumericT>(vec),
                                          static_cast<unsigned int>(vec.start()),
                                          static_cast<unsigned int>(vec.stride()),
                                          detail::cuda_arg<NumericT>(result),
                                          static_cast<unsigned int>(result.start()),
                                          static_cast<unsigned int>(result.stride()),
                                          static_cast<unsigned int>(mat.size1()),
                                          static_cast<unsigned int>(mat.size2()),
                                          static_cast<unsigned int>(mat.internal_size1()),
                                          static_cast<unsigned int>(mat.maxnnz()),
                                          static_cast<unsigned int>(mat.internal_maxnnz())
                                         );
  VIENNACL_CUDA_LAST_ERROR_CHECK("ell_matrix_vec_mul_kernel");
}

template<typename DMatIndexT, typename ResultIndexT, typename NumericT>
__global__ void ell_matrix_d_mat_mul_kernel(const unsigned int * sp_mat_coords,
                                            const NumericT * sp_mat_elements,
                                            unsigned int sp_mat_row_num,
                                            unsigned int sp_mat_col_num,
                                            unsigned int sp_mat_internal_row_num,
                                            unsigned int sp_mat_items_per_row,
                                            unsigned int sp_mat_aligned_items_per_row,
                                            const NumericT * d_mat,
                                            unsigned int d_mat_row_start,
                                            unsigned int d_mat_col_start,
                                            unsigned int d_mat_row_inc,
                                            unsigned int d_mat_col_inc,
                                            unsigned int d_mat_row_size,
                                            unsigned int d_mat_col_size,
                                            unsigned int d_mat_internal_rows,
                                            unsigned int d_mat_internal_cols,
                                            NumericT * result,
                                            unsigned int result_row_start,
                                            unsigned int result_col_start,
                                            unsigned int result_row_inc,
                                            unsigned int result_col_inc,
                                            unsigned int result_row_size,
                                            unsigned int result_col_size,
                                            unsigned int result_internal_rows,
                                            unsigned int result_internal_cols)
{
  unsigned int glb_id = blockDim.x * blockIdx.x + threadIdx.x;
  unsigned int glb_sz = gridDim.x * blockDim.x;

  for ( unsigned int rc = glb_id; rc < (sp_mat_row_num * d_mat_col_size); rc += glb_sz)
  {
    unsigned int row = rc % sp_mat_row_num;
    unsigned int col = rc / sp_mat_row_num;

    unsigned int offset = row;
    NumericT r = (NumericT)0;

    for (unsigned int k = 0; k < sp_mat_items_per_row; k++, offset += sp_mat_internal_row_num)
    {
      unsigned int j = sp_mat_coords[offset];
      NumericT x = static_cast<NumericT>(sp_mat_elements[offset]);

      if (x != (NumericT)0)
      {
        NumericT y = d_mat[ DMatIndexT::apply(j, col,
                                              d_mat_row_start, d_mat_row_inc,
                                              d_mat_col_start, d_mat_col_inc,
                                              d_mat_internal_rows, d_mat_internal_cols) ];

        r += x*y;
      }
    }
    result [ ResultIndexT::apply(row, col,
                                 result_row_start, result_row_inc,
                                 result_col_start, result_col_inc,
                                 result_internal_rows, result_internal_cols) ] = r;
  }

}

/** @brief Carries out Sparse Matrix(ELL)-Dense Matrix multiplication
*
* Implementation of the convenience expression result = prod(sp_mat, d_mat);
* sp_mat being in ELL format
*
* @param sp_mat     The sparse matrix (ELL)
* @param d_mat      The dense matrix
* @param result     The result matrix
*/
template<typename NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::ell_matrix<NumericT, AlignmentV> & sp_mat,
               const viennacl::matrix_base<NumericT> & d_mat,
                     viennacl::matrix_base<NumericT> & result)
{
  if (d_mat.row_major() && result.row_major())
  {
    ell_matrix_d_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<row_major> ><<<128, 128>>>
                                           (detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                            detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                            static_cast<unsigned int>(sp_mat.size1()),
                                            static_cast<unsigned int>(sp_mat.size2()),
                                            static_cast<unsigned int>(sp_mat.internal_size1()),
                                            static_cast<unsigned int>(sp_mat.maxnnz()),
                                            static_cast<unsigned int>(sp_mat.internal_maxnnz()),
                                            detail::cuda_arg<NumericT>(d_mat),
                                            static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                            detail::cuda_arg<NumericT>(result),
                                            static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                            static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                            static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                            static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                         );
    VIENNACL_CUDA_LAST_ERROR_CHECK("ell_matrix_d_mat_mul_kernel");
  }
  else if (d_mat.row_major() && !result.row_major())
  {
    ell_matrix_d_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<column_major> ><<<128, 128>>>
                                           (detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                            detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                            static_cast<unsigned int>(sp_mat.size1()),
                                            static_cast<unsigned int>(sp_mat.size2()),
                                            static_cast<unsigned int>(sp_mat.internal_size1()),
                                            static_cast<unsigned int>(sp_mat.maxnnz()),
                                            static_cast<unsigned int>(sp_mat.internal_maxnnz()),
                                            detail::cuda_arg<NumericT>(d_mat),
                                            static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                            detail::cuda_arg<NumericT>(result),
                                            static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                            static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                            static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                            static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                         );
    VIENNACL_CUDA_LAST_ERROR_CHECK("ell_matrix_d_mat_mul_kernel");
  }
  else if (!d_mat.row_major() && result.row_major())
  {
    ell_matrix_d_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<row_major> ><<<128, 128>>>
                                           (detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                            detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                            static_cast<unsigned int>(sp_mat.size1()),
                                            static_cast<unsigned int>(sp_mat.size2()),
                                            static_cast<unsigned int>(sp_mat.internal_size1()),
                                            static_cast<unsigned int>(sp_mat.maxnnz()),
                                            static_cast<unsigned int>(sp_mat.internal_maxnnz()),
                                            detail::cuda_arg<NumericT>(d_mat),
                                            static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                            detail::cuda_arg<NumericT>(result),
                                            static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                            static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                            static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                            static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                         );
    VIENNACL_CUDA_LAST_ERROR_CHECK("ell_matrix_d_mat_mul_kernel");
  }
  else
  {
    ell_matrix_d_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<column_major> ><<<128, 128>>>
                                           (detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                            detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                            static_cast<unsigned int>(sp_mat.size1()),
                                            static_cast<unsigned int>(sp_mat.size2()),
                                            static_cast<unsigned int>(sp_mat.internal_size1()),
                                            static_cast<unsigned int>(sp_mat.maxnnz()),
                                            static_cast<unsigned int>(sp_mat.internal_maxnnz()),
                                            detail::cuda_arg<NumericT>(d_mat),
                                            static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
                                            static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

                                            detail::cuda_arg<NumericT>(result),
                                            static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                            static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                            static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                            static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                         );
    VIENNACL_CUDA_LAST_ERROR_CHECK("ell_matrix_d_mat_mul_kernel");
  }
}

template<typename DMatIndexT, typename ResultIndexT, typename NumericT >
__global__ void ell_matrix_d_tr_mat_mul_kernel(const unsigned int * sp_mat_coords,
                                            const NumericT * sp_mat_elements,
                                            unsigned int sp_mat_row_num,
                                            unsigned int sp_mat_col_num,
                                            unsigned int sp_mat_internal_row_num,
                                            unsigned int sp_mat_items_per_row,
                                            unsigned int sp_mat_aligned_items_per_row,
                                            const NumericT * d_mat,
                                            unsigned int d_mat_row_start,
                                            unsigned int d_mat_col_start,
                                            unsigned int d_mat_row_inc,
                                            unsigned int d_mat_col_inc,
                                            unsigned int d_mat_row_size,
                                            unsigned int d_mat_col_size,
                                            unsigned int d_mat_internal_rows,
                                            unsigned int d_mat_internal_cols,
                                            NumericT * result,
                                            unsigned int result_row_start,
                                            unsigned int result_col_start,
                                            unsigned int result_row_inc,
                                            unsigned int result_col_inc,
                                            unsigned int result_row_size,
                                            unsigned int result_col_size,
                                            unsigned int result_internal_rows,
                                            unsigned int result_internal_cols)
{
  unsigned int glb_id = blockDim.x * blockIdx.x + threadIdx.x;
  unsigned int glb_sz = gridDim.x * blockDim.x;

  for ( unsigned int rc = glb_id; rc < (sp_mat_row_num * d_mat_row_size); rc += glb_sz)
  {
    unsigned int row = rc % sp_mat_row_num;
    unsigned int col = rc / sp_mat_row_num;

    unsigned int offset = row;
    NumericT r = (NumericT)0;

    for (unsigned int k = 0; k < sp_mat_items_per_row; k++, offset += sp_mat_internal_row_num)
    {
      unsigned int j = sp_mat_coords[offset];
      NumericT x = static_cast<NumericT>(sp_mat_elements[offset]);

      if (x != (NumericT)0)
      {
        NumericT y = d_mat[ DMatIndexT::apply(col, j,
                                              d_mat_row_start, d_mat_row_inc,
                                              d_mat_col_start, d_mat_col_inc,
                                              d_mat_internal_rows, d_mat_internal_cols) ];

        r += x*y;
      }
    }
    result [ ResultIndexT::apply(row, col,
                                 result_row_start, result_row_inc,
                                 result_col_start, result_col_inc,
                                 result_internal_rows, result_internal_cols) ] = r;
  }

}

/** @brief Carries out Sparse Matrix(ELL)-Dense Transposed Matrix multiplication
*
* Implementation of the convenience expression result = prod(sp_mat, trans(d_mat));
* sp_mat being in ELL format
*
* @param sp_mat     The sparse matrix (ELL)
* @param d_mat      The dense matrix
* @param result     The result matrix
*/
template<typename NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::ell_matrix<NumericT, AlignmentV> & sp_mat,
               const viennacl::matrix_expression< const viennacl::matrix_base<NumericT>,
                                                  const viennacl::matrix_base<NumericT>,
                                                  viennacl::op_trans > & d_mat,
                     viennacl::matrix_base<NumericT> & result)
{
  if (d_mat.lhs().row_major() && result.row_major())
  {
    ell_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<row_major> ><<<128, 128>>>
                                              (detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                               detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                               static_cast<unsigned int>(sp_mat.size1()),
                                               static_cast<unsigned int>(sp_mat.size2()),
                                               static_cast<unsigned int>(sp_mat.internal_size1()),
                                               static_cast<unsigned int>(sp_mat.maxnnz()),
                                               static_cast<unsigned int>(sp_mat.internal_maxnnz()),

                                               detail::cuda_arg<NumericT>(d_mat.lhs()),
                                               static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                               detail::cuda_arg<NumericT>(result),
                                               static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                               static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                               static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                               static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                         );
    VIENNACL_CUDA_LAST_ERROR_CHECK("ell_matrix_d_tr_mat_mul_kernel");
  }
  else if (d_mat.lhs().row_major() && !result.row_major())
  {
    ell_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<column_major> ><<<128, 128>>>
                                              (detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                               detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                               static_cast<unsigned int>(sp_mat.size1()),
                                               static_cast<unsigned int>(sp_mat.size2()),
                                               static_cast<unsigned int>(sp_mat.internal_size1()),
                                               static_cast<unsigned int>(sp_mat.maxnnz()),
                                               static_cast<unsigned int>(sp_mat.internal_maxnnz()),

                                               detail::cuda_arg<NumericT>(d_mat.lhs()),
                                               static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                               detail::cuda_arg<NumericT>(result),
                                               static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                               static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                               static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                               static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                         );
    VIENNACL_CUDA_LAST_ERROR_CHECK("ell_matrix_d_tr_mat_mul_kernel");
  }
  else if (!d_mat.lhs().row_major() && result.row_major())
  {
    ell_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<row_major> ><<<128, 128>>>
                                              (detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                               detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                               static_cast<unsigned int>(sp_mat.size1()),
                                               static_cast<unsigned int>(sp_mat.size2()),
                                               static_cast<unsigned int>(sp_mat.internal_size1()),
                                               static_cast<unsigned int>(sp_mat.maxnnz()),
                                               static_cast<unsigned int>(sp_mat.internal_maxnnz()),

                                               detail::cuda_arg<NumericT>(d_mat.lhs()),
                                               static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                               detail::cuda_arg<NumericT>(result),
                                               static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                               static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                               static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                               static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                         );
    VIENNACL_CUDA_LAST_ERROR_CHECK("ell_matrix_d_tr_mat_mul_kernel");
  }
  else
  {
    ell_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<column_major> ><<<128, 128>>>
                                              (detail::cuda_arg<unsigned int>(sp_mat.handle2().cuda_handle()),
                                               detail::cuda_arg<NumericT>(sp_mat.handle().cuda_handle()),
                                               static_cast<unsigned int>(sp_mat.size1()),
                                               static_cast<unsigned int>(sp_mat.size2()),
                                               static_cast<unsigned int>(sp_mat.internal_size1()),
                                               static_cast<unsigned int>(sp_mat.maxnnz()),
                                               static_cast<unsigned int>(sp_mat.internal_maxnnz()),

                                               detail::cuda_arg<NumericT>(d_mat.lhs()),
                                               static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
                                               static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

                                               detail::cuda_arg<NumericT>(result),
                                               static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
                                               static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
                                               static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
                                               static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
                                         );
    VIENNACL_CUDA_LAST_ERROR_CHECK("ell_matrix_d_tr_mat_mul_kernel");
  }
}

//
// SELL-C-\sigma Matrix
//

template<typename NumericT>
__global__ void sliced_ell_matrix_vec_mul_kernel(const unsigned int * columns_per_block,
                                                 const unsigned int * column_indices,
                                                 const unsigned int * block_start,
                                                 const NumericT * elements,
                                                 const NumericT * x,
                                                 unsigned int start_x,
                                                 unsigned int inc_x,
                                                 unsigned int size_x,
                                                 NumericT * result,
                                                 unsigned int start_result,
                                                 unsigned int inc_result,
                                                 unsigned int size_result)
{
  unsigned int local_id = threadIdx.x;
  unsigned int local_size = blockDim.x;
  unsigned int num_rows = size_result;

  for (unsigned int block_idx = blockIdx.x; block_idx <= num_rows / local_size; block_idx += gridDim.x)
  {
    unsigned int row         = block_idx * local_size + local_id;
    unsigned int offset      = block_start[block_idx];
    unsigned int num_columns = columns_per_block[block_idx];

    NumericT sum = 0;
    for (unsigned int item_id = 0; item_id < num_columns; item_id++)
    {
      unsigned int index = offset + item_id * local_size + local_id;
      NumericT val = elements[index];

      sum += val ? (x[column_indices[index] * inc_x + start_x] * val) : 0;
    }

    if (row < num_rows)
      result[row * inc_result + start_result] = sum;
  }
}

/** @brief Carries out matrix-vector multiplication with a sliced_ell_matrix
*
* Implementation of the convenience expression result = prod(mat, vec);
*
* @param mat    The matrix
* @param vec    The vector
* @param result The result vector
*/
template<typename NumericT, typename IndexT>
void prod_impl(const viennacl::sliced_ell_matrix<NumericT, IndexT> & mat,
               const viennacl::vector_base<NumericT> & vec,
                     viennacl::vector_base<NumericT> & result)
{
  sliced_ell_matrix_vec_mul_kernel<<<128, mat.rows_per_block()>>>(detail::cuda_arg<unsigned int>(mat.handle1().cuda_handle()),
                                                                  detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
                                                                  detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
                                                                  detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                                                  detail::cuda_arg<NumericT>(vec),
                                                                  static_cast<unsigned int>(vec.start()),
                                                                  static_cast<unsigned int>(vec.stride()),
                                                                  static_cast<unsigned int>(vec.size()),
                                                                  detail::cuda_arg<NumericT>(result),
                                                                  static_cast<unsigned int>(result.start()),
                                                                  static_cast<unsigned int>(result.stride()),
                                                                  static_cast<unsigned int>(result.size())
                                                                 );
  VIENNACL_CUDA_LAST_ERROR_CHECK("sliced_ell_matrix_vec_mul_kernel");
}


//
// Hybrid Matrix
//


template<typename NumericT>
__global__ void hyb_matrix_vec_mul_kernel(const unsigned int * ell_coords,
                                          const NumericT * ell_elements,
                                          const unsigned int * csr_rows,
                                          const unsigned int * csr_cols,
                                          const NumericT * csr_elements,
                                          const NumericT * x,
                                          unsigned int start_x,
                                          unsigned int inc_x,
                                                NumericT * result,
                                          unsigned int start_result,
                                          unsigned int inc_result,
                                          unsigned int row_num,
                                          unsigned int internal_row_num,
                                          unsigned int items_per_row,
                                          unsigned int aligned_items_per_row
                                         )
{
  unsigned int glb_id = blockDim.x * blockIdx.x + threadIdx.x;
  unsigned int glb_sz = gridDim.x * blockDim.x;

  for (unsigned int row_id = glb_id; row_id < row_num; row_id += glb_sz)
  {
    NumericT sum = 0;

    unsigned int offset = row_id;
    for (unsigned int item_id = 0; item_id < items_per_row; item_id++, offset += internal_row_num)
    {
      NumericT val = ell_elements[offset];


      if (val != NumericT(0))
      {
        int col = ell_coords[offset];
        sum += (x[col * inc_x + start_x] * val);
      }
    }

    unsigned int col_begin = csr_rows[row_id];
    unsigned int col_end   = csr_rows[row_id + 1];

    for (unsigned int item_id = col_begin; item_id < col_end; item_id++)
      sum += x[csr_cols[item_id] * inc_x + start_x] * csr_elements[item_id];

    result[row_id * inc_result + start_result] = sum;
  }
}



/** @brief Carries out matrix-vector multiplication with a hyb_matrix
*
* Implementation of the convenience expression result = prod(mat, vec);
*
* @param mat    The matrix
* @param vec    The vector
* @param result The result vector
*/
template<typename NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::hyb_matrix<NumericT, AlignmentV> & mat,
               const viennacl::vector_base<NumericT> & vec,
                     viennacl::vector_base<NumericT> & result)
{
  hyb_matrix_vec_mul_kernel<<<256, 128>>>(detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
                                          detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
                                          detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
                                          detail::cuda_arg<unsigned int>(mat.handle4().cuda_handle()),
                                          detail::cuda_arg<NumericT>(mat.handle5().cuda_handle()),
                                          detail::cuda_arg<NumericT>(vec),
                                          static_cast<unsigned int>(vec.start()),
                                          static_cast<unsigned int>(vec.stride()),
                                          detail::cuda_arg<NumericT>(result),
                                          static_cast<unsigned int>(result.start()),
                                          static_cast<unsigned int>(result.stride()),
                                          static_cast<unsigned int>(mat.size1()),
                                          static_cast<unsigned int>(mat.internal_size1()),
                                          static_cast<unsigned int>(mat.ell_nnz()),
                                          static_cast<unsigned int>(mat.internal_ellnnz())
                                         );
  VIENNACL_CUDA_LAST_ERROR_CHECK("hyb_matrix_vec_mul_kernel");
}



template<typename DMatIndexT, typename ResultIndexT, typename NumericT>
__global__ void hyb_matrix_d_mat_mul_kernel(const unsigned int * ell_coords,
                                          const NumericT * ell_elements,
                                          const unsigned int * csr_rows,
                                          const unsigned int * csr_cols,
                                          const NumericT * csr_elements,
                                          unsigned int row_num,
                                          unsigned int internal_row_num,
                                          unsigned int items_per_row,
                                          unsigned int aligned_items_per_row,
                                          const NumericT * d_mat,
                                          unsigned int d_mat_row_start,
                                          unsigned int d_mat_col_start,
                                          unsigned int d_mat_row_inc,
                                          unsigned int d_mat_col_inc,
                                          unsigned int d_mat_row_size,
                                          unsigned int d_mat_col_size,
                                          unsigned int d_mat_internal_rows,
                                          unsigned int d_mat_internal_cols,
                                          NumericT * result,
                                          unsigned int result_row_start,
                                          unsigned int result_col_start,
                                          unsigned int result_row_inc,
                                          unsigned int result_col_inc,
                                          unsigned int result_row_size,
                                          unsigned int result_col_size,
                                          unsigned int result_internal_rows,
                                          unsigned int result_internal_cols)
{
  unsigned int glb_id = blockDim.x * blockIdx.x + threadIdx.x;
  unsigned int glb_sz = gridDim.x * blockDim.x;

  for (unsigned int result_col = 0; result_col < result_col_size; ++result_col)
  {
    for (unsigned int row_id = glb_id; row_id < row_num; row_id += glb_sz)
    {
      NumericT sum = 0;

      unsigned int offset = row_id;
      for (unsigned int item_id = 0; item_id < items_per_row; item_id++, offset += internal_row_num)
      {
        NumericT val = ell_elements[offset];

        if (val != 0.0f)
        {
          sum += d_mat[DMatIndexT::apply(ell_coords[offset], result_col,
                                         d_mat_row_start, d_mat_row_inc,
                                         d_mat_col_start, d_mat_col_inc,
                                         d_mat_internal_rows, d_mat_internal_cols)] * val;
        }
      }

      unsigned int col_begin = csr_rows[row_id];
      unsigned int col_end   = csr_rows[row_id + 1];

      for (unsigned int item_id = col_begin; item_id < col_end; item_id++)
      {
        sum += d_mat[DMatIndexT::apply(csr_cols[item_id], result_col,
                                       d_mat_row_start, d_mat_row_inc,
                                       d_mat_col_start, d_mat_col_inc,
                                       d_mat_internal_rows, d_mat_internal_cols)] * csr_elements[item_id];
      }

      result[ResultIndexT::apply(row_id, result_col,
                                 result_row_start, result_row_inc,
                                 result_col_start, result_col_inc,
                                 result_internal_rows, result_internal_cols)] = sum;
    }
  }
}



/** @brief Carries out matrix-vector multiplication with a hyb_matrix
*
* Implementation of the convenience expression result = prod(mat, d_mat);
*
* @param mat      The sparse matrix
* @param d_mat    The dense matrix (row- or column-major)
* @param result   The dense result matrix (row- or column-major)
*/
template<typename NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::hyb_matrix<NumericT, AlignmentV> & mat,
               const viennacl::matrix_base<NumericT> & d_mat,
                     viennacl::matrix_base<NumericT> & result)
{
  if (d_mat.row_major() && result.row_major())
  {
    hyb_matrix_d_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<row_major> ><<<256, 128>>>(
      detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle4().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle5().cuda_handle()),
      static_cast<unsigned int>(mat.size1()),
      static_cast<unsigned int>(mat.internal_size1()),
      static_cast<unsigned int>(mat.ell_nnz()),
      static_cast<unsigned int>(mat.internal_ellnnz()),

      detail::cuda_arg<NumericT>(d_mat),
      static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

      detail::cuda_arg<NumericT>(result),
      static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
      static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
      static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
     );
    VIENNACL_CUDA_LAST_ERROR_CHECK("hyb_matrix_vec_mul_kernel");
  }
  else if (d_mat.row_major() && !result.row_major())
  {
    hyb_matrix_d_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<column_major> ><<<256, 128>>>(
      detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle4().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle5().cuda_handle()),
      static_cast<unsigned int>(mat.size1()),
      static_cast<unsigned int>(mat.internal_size1()),
      static_cast<unsigned int>(mat.ell_nnz()),
      static_cast<unsigned int>(mat.internal_ellnnz()),

      detail::cuda_arg<NumericT>(d_mat),
      static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

      detail::cuda_arg<NumericT>(result),
      static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
      static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
      static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
     );
    VIENNACL_CUDA_LAST_ERROR_CHECK("hyb_matrix_vec_mul_kernel");
  }
  else if (!d_mat.row_major() && result.row_major())
  {
    hyb_matrix_d_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<row_major> ><<<256, 128>>>(
      detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle4().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle5().cuda_handle()),
      static_cast<unsigned int>(mat.size1()),
      static_cast<unsigned int>(mat.internal_size1()),
      static_cast<unsigned int>(mat.ell_nnz()),
      static_cast<unsigned int>(mat.internal_ellnnz()),

      detail::cuda_arg<NumericT>(d_mat),
      static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

      detail::cuda_arg<NumericT>(result),
      static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
      static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
      static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
     );
    VIENNACL_CUDA_LAST_ERROR_CHECK("hyb_matrix_vec_mul_kernel");
  }
  else
  {
    hyb_matrix_d_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<column_major> ><<<256, 128>>>(
      detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle4().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle5().cuda_handle()),
      static_cast<unsigned int>(mat.size1()),
      static_cast<unsigned int>(mat.internal_size1()),
      static_cast<unsigned int>(mat.ell_nnz()),
      static_cast<unsigned int>(mat.internal_ellnnz()),

      detail::cuda_arg<NumericT>(d_mat),
      static_cast<unsigned int>(viennacl::traits::start1(d_mat)),         static_cast<unsigned int>(viennacl::traits::start2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::stride1(d_mat)),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::size1(d_mat)),          static_cast<unsigned int>(viennacl::traits::size2(d_mat)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat)), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat)),

      detail::cuda_arg<NumericT>(result),
      static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
      static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
      static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
     );
    VIENNACL_CUDA_LAST_ERROR_CHECK("hyb_matrix_vec_mul_kernel");
  }
}



template<typename DMatIndexT, typename ResultIndexT, typename NumericT>
__global__ void hyb_matrix_d_tr_mat_mul_kernel(const unsigned int * ell_coords,
                                          const NumericT * ell_elements,
                                          const unsigned int * csr_rows,
                                          const unsigned int * csr_cols,
                                          const NumericT * csr_elements,
                                          unsigned int row_num,
                                          unsigned int internal_row_num,
                                          unsigned int items_per_row,
                                          unsigned int aligned_items_per_row,
                                          const NumericT * d_mat,
                                          unsigned int d_mat_row_start,
                                          unsigned int d_mat_col_start,
                                          unsigned int d_mat_row_inc,
                                          unsigned int d_mat_col_inc,
                                          unsigned int d_mat_row_size,
                                          unsigned int d_mat_col_size,
                                          unsigned int d_mat_internal_rows,
                                          unsigned int d_mat_internal_cols,
                                          NumericT * result,
                                          unsigned int result_row_start,
                                          unsigned int result_col_start,
                                          unsigned int result_row_inc,
                                          unsigned int result_col_inc,
                                          unsigned int result_row_size,
                                          unsigned int result_col_size,
                                          unsigned int result_internal_rows,
                                          unsigned int result_internal_cols)
{
  unsigned int glb_id = blockDim.x * blockIdx.x + threadIdx.x;
  unsigned int glb_sz = gridDim.x * blockDim.x;

  for (unsigned int result_col = 0; result_col < result_col_size; ++result_col)
  {
    for (unsigned int row_id = glb_id; row_id < row_num; row_id += glb_sz)
    {
      NumericT sum = 0;

      unsigned int offset = row_id;
      for (unsigned int item_id = 0; item_id < items_per_row; item_id++, offset += internal_row_num)
      {
        NumericT val = ell_elements[offset];

        if (val != 0.0f)
        {
          sum += d_mat[DMatIndexT::apply(result_col, ell_coords[offset],
                                         d_mat_row_start, d_mat_row_inc,
                                         d_mat_col_start, d_mat_col_inc,
                                         d_mat_internal_rows, d_mat_internal_cols)] * val;
        }
      }

      unsigned int col_begin = csr_rows[row_id];
      unsigned int col_end   = csr_rows[row_id + 1];

      for (unsigned int item_id = col_begin; item_id < col_end; item_id++)
      {
        sum += d_mat[DMatIndexT::apply(result_col, csr_cols[item_id],
                                       d_mat_row_start, d_mat_row_inc,
                                       d_mat_col_start, d_mat_col_inc,
                                       d_mat_internal_rows, d_mat_internal_cols)] * csr_elements[item_id];
      }

      result[ResultIndexT::apply(row_id, result_col,
                                 result_row_start, result_row_inc,
                                 result_col_start, result_col_inc,
                                 result_internal_rows, result_internal_cols)] = sum;
    }
  }
}



/** @brief Carries out matrix-vector multiplication with a hyb_matrix
*
* Implementation of the convenience expression result = prod(mat, trans(d_mat));
*
* @param mat      The sparse matrix
* @param d_mat    Transposed matrix proxy object for the rhs dense matrix (row- or column-major)
* @param result   The dense result matrix (row- or column-major)
*/
template<typename NumericT, unsigned int AlignmentV>
void prod_impl(const viennacl::hyb_matrix<NumericT, AlignmentV> & mat,
               const viennacl::matrix_expression< const viennacl::matrix_base<NumericT>,
                                                  const viennacl::matrix_base<NumericT>,
                                                  viennacl::op_trans > & d_mat,
                     viennacl::matrix_base<NumericT> & result)
{
  if (d_mat.lhs().row_major() && result.row_major())
  {
    hyb_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<row_major> ><<<256, 128>>>(
      detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle4().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle5().cuda_handle()),
      static_cast<unsigned int>(mat.size1()),
      static_cast<unsigned int>(mat.internal_size1()),
      static_cast<unsigned int>(mat.ell_nnz()),
      static_cast<unsigned int>(mat.internal_ellnnz()),

      detail::cuda_arg<NumericT>(d_mat.lhs()),
      static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

      detail::cuda_arg<NumericT>(result),
      static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
      static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
      static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
     );
    VIENNACL_CUDA_LAST_ERROR_CHECK("hyb_matrix_vec_mul_kernel");
  }
  else if (d_mat.lhs().row_major() && !result.row_major())
  {
    hyb_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<row_major>, mat_mult_matrix_index<column_major> ><<<256, 128>>>(
      detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle4().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle5().cuda_handle()),
      static_cast<unsigned int>(mat.size1()),
      static_cast<unsigned int>(mat.internal_size1()),
      static_cast<unsigned int>(mat.ell_nnz()),
      static_cast<unsigned int>(mat.internal_ellnnz()),

      detail::cuda_arg<NumericT>(d_mat.lhs()),
      static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

      detail::cuda_arg<NumericT>(result),
      static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
      static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
      static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
     );
    VIENNACL_CUDA_LAST_ERROR_CHECK("hyb_matrix_vec_mul_kernel");
  }
  else if (!d_mat.lhs().row_major() && result.row_major())
  {
    hyb_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<row_major> ><<<256, 128>>>(
      detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle4().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle5().cuda_handle()),
      static_cast<unsigned int>(mat.size1()),
      static_cast<unsigned int>(mat.internal_size1()),
      static_cast<unsigned int>(mat.ell_nnz()),
      static_cast<unsigned int>(mat.internal_ellnnz()),

      detail::cuda_arg<NumericT>(d_mat.lhs()),
      static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

      detail::cuda_arg<NumericT>(result),
      static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
      static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
      static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
     );
    VIENNACL_CUDA_LAST_ERROR_CHECK("hyb_matrix_vec_mul_kernel");
  }
  else
  {
    hyb_matrix_d_tr_mat_mul_kernel<mat_mult_matrix_index<column_major>, mat_mult_matrix_index<column_major> ><<<256, 128>>>(
      detail::cuda_arg<unsigned int>(mat.handle2().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle3().cuda_handle()),
      detail::cuda_arg<unsigned int>(mat.handle4().cuda_handle()),
      detail::cuda_arg<NumericT>(mat.handle5().cuda_handle()),
      static_cast<unsigned int>(mat.size1()),
      static_cast<unsigned int>(mat.internal_size1()),
      static_cast<unsigned int>(mat.ell_nnz()),
      static_cast<unsigned int>(mat.internal_ellnnz()),

      detail::cuda_arg<NumericT>(d_mat.lhs()),
      static_cast<unsigned int>(viennacl::traits::start1(d_mat.lhs())),         static_cast<unsigned int>(viennacl::traits::start2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::stride1(d_mat.lhs())),        static_cast<unsigned int>(viennacl::traits::stride2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::size1(d_mat.lhs())),          static_cast<unsigned int>(viennacl::traits::size2(d_mat.lhs())),
      static_cast<unsigned int>(viennacl::traits::internal_size1(d_mat.lhs())), static_cast<unsigned int>(viennacl::traits::internal_size2(d_mat.lhs())),

      detail::cuda_arg<NumericT>(result),
      static_cast<unsigned int>(viennacl::traits::start1(result)),         static_cast<unsigned int>(viennacl::traits::start2(result)),
      static_cast<unsigned int>(viennacl::traits::stride1(result)),        static_cast<unsigned int>(viennacl::traits::stride2(result)),
      static_cast<unsigned int>(viennacl::traits::size1(result)),          static_cast<unsigned int>(viennacl::traits::size2(result)),
      static_cast<unsigned int>(viennacl::traits::internal_size1(result)), static_cast<unsigned int>(viennacl::traits::internal_size2(result))
     );
    VIENNACL_CUDA_LAST_ERROR_CHECK("hyb_matrix_vec_mul_kernel");
  }
}


} // namespace cuda
} //namespace linalg
} //namespace viennacl


#endif
