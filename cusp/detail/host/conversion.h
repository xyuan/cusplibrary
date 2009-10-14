/*
 *  Copyright 2008-2009 NVIDIA Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */



#pragma once

#include <algorithm>

#include <cusp/coo_matrix.h>
#include <cusp/csr_matrix.h>
#include <cusp/dia_matrix.h>
#include <cusp/ell_matrix.h>
#include <cusp/hyb_matrix.h>
#include <cusp/dense_matrix.h>

#include <cusp/detail/host/conversion_utils.h>


namespace cusp
{

namespace detail
{

namespace host
{

/////////////////////
// COO Conversions //
/////////////////////
    
template <typename IndexType, typename ValueType>
void coo_to_csr(      cusp::csr_matrix<IndexType,ValueType,cusp::host>& dst,
                const cusp::coo_matrix<IndexType,ValueType,cusp::host>& src)
{
    dst.resize(src.num_rows, src.num_cols, src.num_entries);
    
    //compute number of non-zero entries per row of A 
    std::fill(dst.row_offsets.begin(), dst.row_offsets.end(), 0);

    for (IndexType n = 0; n < src.num_entries; n++){            
        dst.row_offsets[src.row_indices[n]]++;
    }

    //cumsum the num_entries per row to get dst.row_offsets[]
    for(IndexType i = 0, cumsum = 0; i < src.num_rows; i++){     
        IndexType temp = dst.row_offsets[i];
        dst.row_offsets[i] = cumsum;
        cumsum += temp;
    }
    dst.row_offsets[src.num_rows] = src.num_entries; 

    //write Aj,Ax into dst.column_indices,dst.values
    for(IndexType n = 0; n < src.num_entries; n++){
        IndexType row  = src.row_indices[n];
        IndexType dest = dst.row_offsets[row];

        dst.column_indices[dest] = src.column_indices[n];
        dst.values[dest]         = src.values[n];

        dst.row_offsets[row]++;
    }

    for(IndexType i = 0, last = 0; i <= src.num_rows; i++){
        IndexType temp = dst.row_offsets[i];
        dst.row_offsets[i]  = last;
        last   = temp;
    }

    //csr may contain duplicates
}

template <typename IndexType, typename ValueType, class Orientation>
void coo_to_dense(      cusp::dense_matrix<ValueType,cusp::host,Orientation>& dst,
                  const cusp::coo_matrix<IndexType,ValueType,cusp::host>& src)
{
    dst.resize(src.num_rows, src.num_cols);

    std::fill(dst.values.begin(), dst.values.end(), 0);

    for(IndexType n = 0; n < src.num_entries; n++)
        dst(src.row_indices[n], src.column_indices[n]) += src.values[n]; //sum duplicates
}

/////////////////////
// CSR Conversions //
/////////////////////

template <typename IndexType, typename ValueType>
void csr_to_coo(      cusp::coo_matrix<IndexType,ValueType,cusp::host>& dst,
                const cusp::csr_matrix<IndexType,ValueType,cusp::host>& src)
{
    dst.resize(src.num_rows, src.num_cols, src.num_entries);
    
    for(IndexType i = 0; i < src.num_rows; i++)
        for(IndexType jj = src.row_offsets[i]; jj < src.row_offsets[i + 1]; jj++)
            dst.row_indices[jj] = i;

    dst.column_indices = src.column_indices;
    dst.values         = src.values;
}

template <typename IndexType, typename ValueType>
void csr_to_dia(       cusp::dia_matrix<IndexType,ValueType,cusp::host>& dia,
                 const cusp::csr_matrix<IndexType,ValueType,cusp::host>& csr,
                 const IndexType alignment = 16)
{
    // compute number of occupied diagonals and enumerate them
    IndexType num_diagonals = 0;

    cusp::host_vector<IndexType> diag_map(csr.num_rows + csr.num_cols, 0);

    for(IndexType i = 0; i < csr.num_rows; i++)
    {
        for(IndexType jj = csr.row_offsets[i]; jj < csr.row_offsets[i+1]; jj++)
        {
            IndexType j = csr.column_indices[jj];
            IndexType map_index = (csr.num_rows - i) + j; //offset shifted by + num_rows
            if(diag_map[map_index] == 0)
            {
                diag_map[map_index] = 1;
                num_diagonals++;
            }
        }
    }
   

    // length of each diagonal in memory
    IndexType stride = alignment * ((csr.num_rows + alignment - 1) / alignment);
   
    // allocate DIA structure
    dia.resize(csr.num_rows, csr.num_cols, csr.num_entries, num_diagonals, stride);

    // fill in diagonal_offsets array
    for(IndexType n = 0, diag = 0; n < csr.num_rows + csr.num_cols; n++)
    {
        if(diag_map[n] == 1)
        {
            diag_map[n] = diag;
            dia.diagonal_offsets[diag] = (IndexType) n - (IndexType) csr.num_rows;
            diag++;
        }
    }

    // fill in values array
    std::fill(dia.values.begin(), dia.values.end(), 0);

    for(IndexType i = 0; i < csr.num_rows; i++)
    {
        for(IndexType jj = csr.row_offsets[i]; jj < csr.row_offsets[i+1]; jj++)
        {
            IndexType j = csr.column_indices[jj];
            IndexType map_index = (csr.num_rows - i) + j; //offset shifted by + num_rows
            IndexType diag = diag_map[map_index];
        
            dia.values[diag * dia.stride + i] = csr.values[jj];
        }
    }
}
    

template <typename IndexType, typename ValueType>
void csr_to_hyb(      cusp::hyb_matrix<IndexType, ValueType,cusp::host>& hyb, 
                const cusp::csr_matrix<IndexType,ValueType,cusp::host>&  csr,
                const IndexType num_entries_per_row,
                const IndexType alignment = 16)
{
    // The ELL portion of the HYB matrix will have 'num_entries_per_row' columns.
    // Nonzero values that do not fit within the ELL structure are placed in the 
    // COO format portion of the HYB matrix.
    
    cusp::ell_matrix<IndexType, ValueType, cusp::host> & ell = hyb.ell;
    cusp::coo_matrix<IndexType, ValueType, cusp::host> & coo = hyb.coo;

    const IndexType stride = alignment * ((csr.num_rows + alignment - 1) / alignment);

    // compute number of nonzeros in the ELL and COO portions
    IndexType num_ell_entries = 0;
    for(IndexType i = 0; i < csr.num_rows; i++)
        num_ell_entries += std::min(num_entries_per_row, csr.row_offsets[i+1] - csr.row_offsets[i]); 

    IndexType num_coo_entries = csr.num_entries - num_ell_entries;

    hyb.resize(csr.num_rows, csr.num_cols, 
               num_ell_entries, num_coo_entries, 
               num_entries_per_row, stride);

    const IndexType invalid_index = cusp::ell_matrix<IndexType, ValueType, cusp::host>::invalid_index;

    // pad out ELL format with zeros
    std::fill(ell.column_indices.begin(), ell.column_indices.end(), invalid_index);
    std::fill(ell.values.begin(),         ell.values.end(),         0);

    for(IndexType i = 0, coo_nnz = 0; i < csr.num_rows; i++)
    {
        IndexType n = 0;
        IndexType jj = csr.row_offsets[i];

        // copy up to num_cols_per_row values of row i into the ELL
        while(jj < csr.row_offsets[i+1] && n < ell.num_entries_per_row)
        {
            ell.column_indices[ell.stride * n + i] = csr.column_indices[jj];
            ell.values[ell.stride * n + i]         = csr.values[jj];
            jj++, n++;
        }

        // copy any remaining values in row i into the COO
        while(jj < csr.row_offsets[i+1])
        {
            coo.row_indices[coo_nnz]    = i;
            coo.column_indices[coo_nnz] = csr.column_indices[jj];
            coo.values[coo_nnz]         = csr.values[jj];
            jj++; coo_nnz++;
        }
    }
}


template <typename IndexType, typename ValueType>
void csr_to_ell(      cusp::ell_matrix<IndexType,ValueType,cusp::host>&  ell,
                 const cusp::csr_matrix<IndexType,ValueType,cusp::host>&  csr,
                 const IndexType num_entries_per_row, const IndexType alignment = 16)
{
    // Constructs an ELL matrix with 'num_entries_per_row' consisting of the first
    // 'num_entries_per_row' entries in each row of the CSR matrix.
    cusp::hyb_matrix<IndexType, ValueType, cusp::host> hyb;

    csr_to_hyb(hyb, csr, num_entries_per_row, alignment);

    ell.swap(hyb.ell);
}

    
template <typename IndexType, typename ValueType, class Orientation>
void csr_to_dense(      cusp::dense_matrix<ValueType,cusp::host,Orientation>& dst,
                  const cusp::csr_matrix<IndexType,ValueType,cusp::host>& src)
{
    dst.resize(src.num_rows, src.num_cols);

    std::fill(dst.values.begin(), dst.values.end(), 0);

    for(IndexType i = 0; i < src.num_rows; i++)
        for(IndexType jj = src.row_offsets[i]; jj < src.row_offsets[i+1]; jj++)
            dst(i, src.column_indices[jj]) += src.values[jj]; //sum duplicates
}


/////////////////////
// DIA Conversions //
/////////////////////

template <typename IndexType, typename ValueType>
void dia_to_csr(      cusp::csr_matrix<IndexType,ValueType,cusp::host>& dst,
                const cusp::dia_matrix<IndexType,ValueType,cusp::host>& src)
{
    IndexType num_entries = 0;

    for(IndexType n = 0; n < src.num_diagonals; n++)
    {
        const IndexType k = src.diagonal_offsets[n];  //diagonal offset

        const IndexType i_start = std::max((IndexType) 0, -k);
        const IndexType j_start = std::max((IndexType) 0,  k);
        
        const IndexType base = n * src.stride + i_start;

        //number of elements to process
        const IndexType M = std::min(src.num_rows - i_start, src.num_cols - j_start);

        for(IndexType m = 0; m < M; m++)
        {
            if(src.values[base + m] != 0)
                num_entries++;
        }
    }

    dst.resize(src.num_rows, src.num_cols, num_entries);

    num_entries = 0;
    dst.row_offsets[0] = 0;

    for(IndexType i = 0; i < src.num_rows; i++)
    {
        for(IndexType n = 0; n < src.num_diagonals; n++)
        {
            const IndexType j = i + src.diagonal_offsets[n];

            if(j >= 0 && j < src.num_cols)
            {
                const ValueType value = src.values[n * src.stride + i];

                if (value != 0)
                {
                    dst.column_indices[num_entries] = j;
                    dst.values[num_entries] = value;
                    num_entries++;
                }
            }
        }

        dst.row_offsets[i + 1] = num_entries;
    }
}

/////////////////////
// ELL Conversions //
/////////////////////

template <typename IndexType, typename ValueType>
void ell_to_csr(      cusp::csr_matrix<IndexType,ValueType,cusp::host>& dst,
                const cusp::ell_matrix<IndexType,ValueType,cusp::host>& src)
{
    const IndexType invalid_index = cusp::ell_matrix<IndexType, ValueType, cusp::host>::invalid_index;

    dst.resize(src.num_rows, src.num_cols, src.num_entries);

    IndexType num_entries = 0;
    dst.row_offsets[0] = 0;

    for(IndexType i = 0; i < src.num_rows; i++)
    {
        for(IndexType n = 0; n < src.num_entries_per_row; n++)
        {
            const IndexType j = src.column_indices[src.stride * n + i];
            const IndexType v = src.values[src.stride * n + i];
            if(j != invalid_index)
            {
                dst.column_indices[num_entries] = j;
                dst.values[num_entries] = v;
                num_entries++;
            }
        }

        dst.row_offsets[i + 1] = num_entries;
    }
}

/////////////////////
// HYB Conversions //
/////////////////////

template <typename IndexType, typename ValueType>
void hyb_to_csr(      cusp::csr_matrix<IndexType,ValueType,cusp::host>& dst,
                const cusp::hyb_matrix<IndexType,ValueType,cusp::host>& src)
{
    cusp::csr_matrix<IndexType,ValueType,cusp::host> ell_part;
    cusp::csr_matrix<IndexType,ValueType,cusp::host> coo_part;

    ell_to_csr(ell_part, src.ell);
    coo_to_csr(coo_part, src.coo);

    dst.resize(src.num_rows, src.num_cols, src.num_entries);

    // merge the two CSR parts
    IndexType num_entries = 0;
    dst.row_offsets[0] = 0;
    for(IndexType i = 0; i < src.num_rows; i++)
    {
        for(IndexType jj = ell_part.row_offsets[i]; jj < ell_part.row_offsets[i + 1]; jj++)
        {
            dst.column_indices[num_entries] = ell_part.column_indices[jj];
            dst.values[num_entries]         = ell_part.values[jj];
            num_entries++;
        }

        for(IndexType jj = coo_part.row_offsets[i]; jj < coo_part.row_offsets[i + 1]; jj++)
        {
            dst.column_indices[num_entries] = coo_part.column_indices[jj];
            dst.values[num_entries]         = coo_part.values[jj];
            num_entries++;
        }

        dst.row_offsets[i + 1] = num_entries;
    }
}


///////////////////////
// Dense Conversions //
///////////////////////
template <typename IndexType, typename ValueType, class Orientation>
void dense_to_coo(      cusp::coo_matrix<IndexType,ValueType,cusp::host>& dst,
                  const cusp::dense_matrix<ValueType,cusp::host,Orientation>& src)
{
    IndexType nnz = src.num_entries - std::count(src.values.begin(), src.values.end(), ValueType(0));

    dst.resize(src.num_rows, src.num_cols, nnz);

    nnz = 0;

    for(size_t i = 0; i < src.num_rows; i++)
    {
        for(size_t j = 0; j < src.num_cols; j++)
        {
            if (src(i,j) != 0)
            {
                dst.row_indices[nnz]    = i;
                dst.column_indices[nnz] = j;
                dst.values[nnz]         = src(i,j);
                nnz++;
            }
        }
    }
}

template <typename IndexType, typename ValueType, class Orientation>
void dense_to_csr(      cusp::csr_matrix<IndexType,ValueType,cusp::host>& dst,
                  const cusp::dense_matrix<ValueType,cusp::host,Orientation>& src)
{
    IndexType nnz = src.num_entries - std::count(src.values.begin(), src.values.end(), ValueType(0));

    dst.resize(src.num_rows, src.num_cols, nnz);

    IndexType num_entries = 0;

    for(size_t i = 0; i < src.num_rows; i++)
    {
        dst.row_offsets[i] = num_entries;

        for(size_t j = 0; j < src.num_cols; j++)
        {
            if (src(i,j) != 0){
                dst.column_indices[num_entries] = j;
                dst.values[num_entries]         = src(i,j);
                num_entries++;
            }
        }
    }

    dst.row_offsets[src.num_rows] = num_entries;
}


} // end namespace host

} // end namespace detail

} // end namespace cusp

