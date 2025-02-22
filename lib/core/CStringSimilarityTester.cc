/*
 * Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
 * or more contributor license agreements. Licensed under the Elastic License
 * 2.0 and the following additional limitation. Functionality enabled by the
 * files subject to the Elastic License 2.0 may only be used in production when
 * invoked by an Elasticsearch process with a license key installed that permits
 * use of machine learning features. You may not use this file except in
 * compliance with the Elastic License 2.0 and the foregoing additional
 * limitation.
 */
#include <core/CStringSimilarityTester.h>

#include <core/CMemoryDef.h>

#include <limits>

namespace ml {
namespace core {

const int CStringSimilarityTester::MINUS_INFINITE_INT(std::numeric_limits<int>::min());

CStringSimilarityTester::CStringSimilarityTester() : m_Compressor(true) {
}

bool CStringSimilarityTester::similarity(const std::string& first,
                                         const std::string& second,
                                         double& result) const {
    size_t firstCompLength(0);
    size_t secondCompLength(0);

    if (m_Compressor.addString(first) == false ||
        m_Compressor.length(true, firstCompLength) == false ||
        m_Compressor.addString(second) == false ||
        m_Compressor.length(true, secondCompLength) == false) {
        // The compressor will have logged the detailed reason
        LOG_ERROR(<< "Compression problem");
        return false;
    }

    return this->similarity(first, firstCompLength, second, secondCompLength, result);
}

bool CStringSimilarityTester::similarity(const std::string& first,
                                         size_t firstCompLength,
                                         const std::string& second,
                                         size_t secondCompLength,
                                         double& result) const {
    if (first.empty() && second.empty()) {
        // Special case that will cause a divide by zero error if
        // we're not careful
        result = 1.0;
        return true;
    }

    size_t firstPlusSecondCompLength(0);
    size_t secondPlusFirstCompLength(0);

    if (m_Compressor.addString(first) == false || m_Compressor.addString(second) == false ||
        m_Compressor.length(true, firstPlusSecondCompLength) == false ||
        m_Compressor.addString(second) == false || m_Compressor.addString(first) == false ||
        m_Compressor.length(true, secondPlusFirstCompLength) == false) {
        // The compressor will have logged the detailed reason
        LOG_ERROR(<< "Compression problem");
        return false;
    }

    double individual(static_cast<double>(firstCompLength + secondCompLength));
    double combined(static_cast<double>(firstPlusSecondCompLength + secondPlusFirstCompLength));

    // You'd expect (fs + sf) / (2 * (s + f)) to be between 0.5 and 1, where
    // 1 means completely different and 0.5 means identical - in practice
    // the compression won't give this exactly, but is close enough to be useful
    // Formula below is a simplification of 2 * (1 - (fs + sf) / (2 * (s + f)))
    // Higher => more similar
    result = 2.0 - (combined / individual);

    return true;
}

bool CStringSimilarityTester::compressedLengthOf(const std::string& str, size_t& length) const {
    return m_Compressor.addString(str) && m_Compressor.length(true, length);
}

int** CStringSimilarityTester::setupBerghelRoachMatrix(int maxDist,
                                                       TScopedIntArray& dataArray,
                                                       TScopedIntPArray& matrixArray) {
    // Ensure that we don't suffer memory corruption due to an incorrect input
    if (maxDist <= 0) {
        LOG_ERROR(<< "Programmatic error - maxDist too small " << maxDist);
        return nullptr;
    } else if (maxDist >= std::numeric_limits<int>::max() / 2) {
        LOG_ERROR(<< "Programmatic error - maxDist too big " << maxDist);
        return nullptr;
    }

    int rows(maxDist * 2 + 1);
    int columns(maxDist + 2);

    // Allocate a block of memory for the matrix cells
    dataArray.reset(new int[rows * columns]);

    // Allocate a block of pointers that we can use to make the matrix appear
    // two dimensional
    matrixArray.reset(new int*[rows]);

    // The column indexes go from -1 to maxDist inclusive, so add 1 to the
    // pointer such that row[-1] points to the beginning of the row memory
    int* rowZero(dataArray.get() + 1);
    for (int row = 0; row < rows; ++row) {
        matrixArray[row] = rowZero;
        rowZero += columns;
    }

    // The row indexes go from -maxDist to maxDist inclusive, so add maxDist to
    // the pointer such that matrix[-maxDist] points to the first row memory.
    // (Then matrix[-maxDist][-1] will point to the very beginning of the
    // memory.)
    int** matrix;
    matrix = matrixArray.get() + maxDist;

    // Initialise the matrix.  This is an optimised version of the pseudo-code
    // near the end of the sub-section titled "The Algorithm Kernel" in
    // http://berghel.net/publications/asm/asm.pdf
    for (int k = -maxDist; k < 0; ++k) {
        // Here note that (::abs(k) - 1) == -1 - k;
        int absKMinusOne(-1 - k);
        matrix[k][absKMinusOne - 1] = MINUS_INFINITE_INT;
        matrix[k][absKMinusOne] = absKMinusOne;
    }
    // k = 0, so (::abs(k) - 1) == -1
    matrix[0][-1] = -1;
    for (int k = 1; k <= maxDist; ++k) {
        // Here note that (::abs(k) - 1) == k - 1;
        int absKMinusOne(k - 1);
        matrix[k][absKMinusOne - 1] = MINUS_INFINITE_INT;
        matrix[k][absKMinusOne] = -1;
    }

    return matrix;
}

void CStringSimilarityTester::debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CStringSimilarityTester");
    core::CMemoryDebug::dynamicSize("m_Compressor", m_Compressor, mem);
}

std::size_t CStringSimilarityTester::memoryUsage() const {
    std::size_t mem = 0;
    mem += core::CMemory::dynamicSize(m_Compressor);
    return mem;
}
}
}
