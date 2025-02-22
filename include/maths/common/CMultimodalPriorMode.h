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

#ifndef INCLUDED_ml_maths_common_SMultimodalPriorMode_h
#define INCLUDED_ml_maths_common_SMultimodalPriorMode_h

#include <core/CMemoryDec.h>
#include <core/CStatePersistInserter.h>
#include <core/CStateRestoreTraverser.h>
#include <core/RestoreMacros.h>

#include <maths/common/CChecksum.h>
#include <maths/common/CPriorStateSerialiser.h>

#include <cstddef>
#include <functional>
#include <iomanip>
#include <sstream>
#include <vector>

namespace ml {
namespace maths {
namespace common {
//! \brief The prior of a mode of the likelihood function and
//! a unique identifier for the clusterer.
//!
//! DESCRIPTION:\n
//! See, for example, CMultimodalPrior for usage.
template<typename PRIOR_PTR>
struct SMultimodalPriorMode {
    static const core::TPersistenceTag INDEX_TAG;
    static const core::TPersistenceTag PRIOR_TAG;

    SMultimodalPriorMode() : s_Index(0), s_Prior() {}
    SMultimodalPriorMode(std::size_t index, const PRIOR_PTR& prior)
        : s_Index(index), s_Prior(prior->clone()) {}
    SMultimodalPriorMode(std::size_t index, PRIOR_PTR&& prior)
        : s_Index(index), s_Prior(std::move(prior)) {}

    //! Get the weight of this sample.
    double weight() const { return s_Prior->numberSamples(); }

    //! Get a checksum for this object.
    std::uint64_t checksum(std::uint64_t seed) const {
        seed = CChecksum::calculate(seed, s_Index);
        return CChecksum::calculate(seed, s_Prior);
    }

    //! Get the memory used by this component
    void debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
        mem->setName("CMultimodalPrior::SMode");
        core::CMemoryDebug::dynamicSize("s_Prior", s_Prior, mem);
    }

    //! Get the memory used by this component
    std::size_t memoryUsage() const {
        return core::CMemory::dynamicSize(s_Prior);
    }

    //! Create from part of a state document.
    bool acceptRestoreTraverser(const SDistributionRestoreParams& params,
                                core::CStateRestoreTraverser& traverser) {
        do {
            const std::string& name = traverser.name();
            RESTORE_BUILT_IN(INDEX_TAG, s_Index)
            RESTORE(PRIOR_TAG, traverser.traverseSubLevel(
                                   [&, serializer = CPriorStateSerialiser{} ](auto& traverser_) {
                                       return serializer(params, s_Prior, traverser_);
                                   }))
        } while (traverser.next());

        this->checkRestoredInvariants();

        return true;
    }

    //! Check the state invariants after restoration
    //! Abort on failure.
    void checkRestoredInvariants() const {
        VIOLATES_INVARIANT_NO_EVALUATION(s_Prior, ==, nullptr);
    }

    //! Persist state by passing information to the supplied inserter.
    void acceptPersistInserter(core::CStatePersistInserter& inserter) const {
        inserter.insertValue(INDEX_TAG, s_Index);
        inserter.insertLevel(
            PRIOR_TAG, [&, serializer = CPriorStateSerialiser{} ](auto& inserter_) {
                serializer(*s_Prior, inserter_);
            });
    }

    //! Full debug dump of the mode weights.
    template<typename T>
    static std::string debugWeights(const std::vector<SMultimodalPriorMode<T>>& modes) {
        if (modes.empty()) {
            return std::string();
        }
        std::ostringstream result;
        result << std::scientific << std::setprecision(15) << modes[0].weight();
        for (std::size_t i = 1; i < modes.size(); ++i) {
            result << " " << modes[i].weight();
        }
        return result.str();
    }

    std::size_t s_Index;
    PRIOR_PTR s_Prior;
};

template<typename PRIOR>
const core::TPersistenceTag SMultimodalPriorMode<PRIOR>::INDEX_TAG("a", "index");
template<typename PRIOR>
const core::TPersistenceTag SMultimodalPriorMode<PRIOR>::PRIOR_TAG("b", "prior");
}
}
}

#endif // INCLUDED_ml_maths_common_SMultimodalPriorMode_h
