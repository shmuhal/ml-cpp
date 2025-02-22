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

#ifndef INCLUDED_ml_maths_common_CAnnotatedVector_h
#define INCLUDED_ml_maths_common_CAnnotatedVector_h

#include <core/CMemoryDec.h>

#include <maths/common/CTypeTraits.h>

#include <cstddef>

namespace ml {
namespace maths {
namespace common {
//! \brief A vector on to which data have been annotated.
//!
//! \tparam VECTOR The vector type.
//! \tparam ANNOTATION The annotated data type.
template<typename VECTOR, typename ANNOTATION>
class CAnnotatedVector : public VECTOR {
public:
    using TAnnotation = ANNOTATION;
    using TCoordinate = typename SCoordinate<VECTOR>::Type;

    //! See core::CMemory.
    static constexpr bool dynamicSizeAlwaysZero() {
        return core::memory_detail::SDynamicSizeAlwaysZero<VECTOR>::value() &&
               core::memory_detail::SDynamicSizeAlwaysZero<ANNOTATION>::value();
    }

public:
    //! Construct with a vector and annotation data.
    CAnnotatedVector(const VECTOR& vector = VECTOR(),
                     const ANNOTATION& annotation = ANNOTATION())
        : VECTOR(vector), m_Annotation(annotation) {}

    //! Assign from \p rhs.
    const CAnnotatedVector& operator=(const VECTOR& rhs) {
        static_cast<VECTOR&>(*this) = rhs;
        return *this;
    }

    //! Get the annotation data by constant reference.
    const ANNOTATION& annotation() const { return m_Annotation; }

    //! Get the annotation data by reference.
    ANNOTATION& annotation() { return m_Annotation; }

    //! Debug the memory usage of this object.
    void debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
        mem->setName("CAnnotatedVector");
        mem->addItem("vector", core::CMemory::dynamicSize(static_cast<VECTOR>(*this)));
        mem->addItem("annotation", core::CMemory::dynamicSize(m_Annotation));
    }
    //! Get the memory used by this object.
    std::size_t memoryUsage() const {
        return core::CMemory::dynamicSize(static_cast<VECTOR>(*this)) +
               core::CMemory::dynamicSize(m_Annotation);
    }

private:
    //! The data which has been annotated onto the vector.
    ANNOTATION m_Annotation;
};

//! Get the concomitant constant annotated vector.
template<typename VECTOR, typename ANNOTATION>
struct SConstant<CAnnotatedVector<VECTOR, ANNOTATION>> {
    static auto get(std::size_t dimension,
                    typename CAnnotatedVector<VECTOR, ANNOTATION>::TCoordinate constant)
        -> CAnnotatedVector<decltype(SConstant<VECTOR>::get(dimension, constant)), ANNOTATION> {
        return {SConstant<VECTOR>::get(dimension, constant)};
    }
};
}
}
}

#endif // INCLUDED_ml_maths_common_CAnnotatedVector_h
