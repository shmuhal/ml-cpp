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

#include <maths/common/CMultivariateConstantPrior.h>

#include <core/CContainerPrinter.h>
#include <core/CMemoryDef.h>
#include <core/CPersistUtils.h>
#include <core/CStatePersistInserter.h>
#include <core/CStateRestoreTraverser.h>
#include <core/Constants.h>
#include <core/RestoreMacros.h>

#include <maths/common/CChecksum.h>
#include <maths/common/CConstantPrior.h>
#include <maths/common/CMathsFuncs.h>

#include <ios>
#include <limits>
#include <optional>

namespace ml {
namespace maths {
namespace common {
namespace {

using TDouble10Vec = core::CSmallVector<double, 10>;
using TOptionalDouble10Vec = std::optional<TDouble10Vec>;

//! \brief Converts a constant value to a string.
class CConstantToString {
public:
    std::string operator()(double value) const {
        return core::CStringUtils::typeToStringPrecise(value, core::CIEEE754::E_DoublePrecision);
    }
};

//! Set the constant, validating the input.
void setConstant(std::size_t dimension, TDouble10Vec value, TOptionalDouble10Vec& result) {
    if (value.size() != dimension) {
        LOG_ERROR(<< "Unexpected dimension: " << value.size() << " != " << dimension);
    } else if (CMathsFuncs::isNan(value)) {
        LOG_ERROR(<< "NaN constant");
    } else {
        result.emplace(std::move(value));
    }
}

// We use short field names to reduce the state size
const std::string CONSTANT_TAG("a");

const std::string EMPTY_STRING;
}

CMultivariateConstantPrior::CMultivariateConstantPrior(std::size_t dimension,
                                                       const TOptionalDouble10Vec& constant)
    : CMultivariatePrior(maths_t::E_DiscreteData, 0.0), m_Dimension(dimension) {
    if (constant) {
        setConstant(m_Dimension, *constant, m_Constant);
    }
}

CMultivariateConstantPrior::CMultivariateConstantPrior(std::size_t dimension,
                                                       core::CStateRestoreTraverser& traverser)
    : CMultivariatePrior(maths_t::E_DiscreteData, 0.0), m_Dimension(dimension) {
    if (traverser.traverseSubLevel([this](auto& traverser_) {
            return this->acceptRestoreTraverser(traverser_);
        }) == false) {
        traverser.setBadState();
    }
}

bool CMultivariateConstantPrior::acceptRestoreTraverser(core::CStateRestoreTraverser& traverser) {
    do {
        const std::string& name = traverser.name();
        RESTORE_SETUP_TEARDOWN(CONSTANT_TAG, TDouble10Vec constant,
                               core::CPersistUtils::fromString(traverser.value(), constant),
                               m_Constant.emplace(constant))
    } while (traverser.next());

    return true;
}

CMultivariateConstantPrior* CMultivariateConstantPrior::clone() const {
    return new CMultivariateConstantPrior(*this);
}

std::size_t CMultivariateConstantPrior::dimension() const {
    return m_Dimension;
}

void CMultivariateConstantPrior::setToNonInformative(double /*offset*/, double /*decayRate*/) {
    m_Constant.reset();
}

void CMultivariateConstantPrior::adjustOffset(const TDouble10Vec1Vec& /*samples*/,
                                              const TDouble10VecWeightsAry1Vec& /*weights*/) {
}

void CMultivariateConstantPrior::addSamples(const TDouble10Vec1Vec& samples,
                                            const TDouble10VecWeightsAry1Vec& /*weights*/) {
    if (m_Constant || samples.empty()) {
        return;
    }
    setConstant(m_Dimension, samples[0], m_Constant);
}

void CMultivariateConstantPrior::propagateForwardsByTime(double /*time*/) {
}

CMultivariateConstantPrior::TUnivariatePriorPtrDoublePr
CMultivariateConstantPrior::univariate(const TSize10Vec& marginalize,
                                       const TSizeDoublePr10Vec& condition) const {
    if (!this->check(marginalize, condition)) {
        return {};
    }

    TSize10Vec i1;
    this->remainingVariables(marginalize, condition, i1);
    if (i1.size() != 1) {
        LOG_ERROR(<< "Invalid variables for computing univariate distribution: "
                  << "marginalize '" << marginalize << "'"
                  << ", condition '" << condition << "'");
        return {};
    }

    return this->isNonInformative()
               ? TUnivariatePriorPtrDoublePr(std::make_unique<CConstantPrior>(), 0.0)
               : TUnivariatePriorPtrDoublePr(
                     std::make_unique<CConstantPrior>((*m_Constant)[i1[0]]), 0.0);
}

CMultivariateConstantPrior::TPriorPtrDoublePr
CMultivariateConstantPrior::bivariate(const TSize10Vec& marginalize,
                                      const TSizeDoublePr10Vec& condition) const {
    if (m_Dimension == 2) {
        return {TPriorPtr(this->clone()), 0.0};
    }

    if (!this->check(marginalize, condition)) {
        return {};
    }

    TSize10Vec i1;
    this->remainingVariables(marginalize, condition, i1);
    if (i1.size() != 2) {
        LOG_ERROR(<< "Invalid variables for computing univariate distribution: "
                  << "marginalize '" << marginalize << "'"
                  << ", condition '" << condition << "'");
        return {};
    }

    if (!this->isNonInformative()) {
        TDouble10Vec constant;
        constant[0] = (*m_Constant)[i1[0]];
        constant[1] = (*m_Constant)[i1[1]];
        return {std::make_unique<CMultivariateConstantPrior>(2, constant), 0.0};
    }
    return {std::make_unique<CMultivariateConstantPrior>(2), 0.0};
}

CMultivariateConstantPrior::TDouble10VecDouble10VecPr
CMultivariateConstantPrior::marginalLikelihoodSupport() const {
    TDouble10Vec lowest(m_Dimension);
    TDouble10Vec highest(m_Dimension);
    for (std::size_t i = 0; i < m_Dimension; ++i) {
        lowest[i] = std::numeric_limits<double>::lowest();
        highest[i] = std::numeric_limits<double>::max();
    }
    return {lowest, highest};
}

CMultivariateConstantPrior::TDouble10Vec
CMultivariateConstantPrior::marginalLikelihoodMean() const {
    if (this->isNonInformative()) {
        return TDouble10Vec(m_Dimension, 0.0);
    }

    return *m_Constant;
}

CMultivariateConstantPrior::TDouble10Vec
CMultivariateConstantPrior::marginalLikelihoodMode(const TDouble10VecWeightsAry& /*weights*/) const {
    return this->marginalLikelihoodMean();
}

CMultivariateConstantPrior::TDouble10Vec10Vec
CMultivariateConstantPrior::marginalLikelihoodCovariance() const {
    TDouble10Vec10Vec result(m_Dimension, TDouble10Vec(m_Dimension, 0.0));
    if (this->isNonInformative()) {
        for (std::size_t i = 0; i < m_Dimension; ++i) {
            result[i][i] = std::numeric_limits<double>::max();
        }
    }
    return result;
}

CMultivariateConstantPrior::TDouble10Vec
CMultivariateConstantPrior::marginalLikelihoodVariances() const {
    return TDouble10Vec(m_Dimension, this->isNonInformative()
                                         ? std::numeric_limits<double>::max()
                                         : 0.0);
}

maths_t::EFloatingPointErrorStatus
CMultivariateConstantPrior::jointLogMarginalLikelihood(const TDouble10Vec1Vec& samples,
                                                       const TDouble10VecWeightsAry1Vec& weights,
                                                       double& result) const {
    result = 0.0;

    if (samples.empty()) {
        LOG_ERROR(<< "Can't compute likelihood for empty sample set");
        return maths_t::E_FpFailed;
    }

    if (samples.size() != weights.size()) {
        LOG_ERROR(<< "Mismatch in samples '" << samples << "' and weights '"
                  << weights << "'");
        return maths_t::E_FpFailed;
    }

    if (this->isNonInformative()) {
        // The non-informative likelihood is improper and effectively
        // zero everywhere. We use minus max double because
        // log(0) = HUGE_VALUE, which causes problems for Windows.
        // Calling code is notified when the calculation overflows
        // and should avoid taking the exponential since this will
        // underflow and pollute the floating point environment. This
        // may cause issues for some library function implementations
        // (see fe*exceptflag for more details).
        result = std::numeric_limits<double>::lowest();
        return maths_t::E_FpOverflowed;
    }

    double numberSamples = 0.0;

    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].size() != m_Dimension) {
            LOG_ERROR(<< "Unexpected dimension: " << samples[i].size() << " != " << m_Dimension);
            continue;
        }
        if (!std::equal(samples[i].begin(), samples[i].end(), m_Constant->begin())) {
            // Technically infinite, but just use minus max double.
            result = std::numeric_limits<double>::lowest();
            return maths_t::E_FpOverflowed;
        }

        numberSamples += this->smallest(maths_t::countForUpdate(weights[i]));
    }

    result = numberSamples * core::constants::LOG_MAX_DOUBLE;
    return maths_t::E_FpNoErrors;
}

void CMultivariateConstantPrior::sampleMarginalLikelihood(std::size_t numberSamples,
                                                          TDouble10Vec1Vec& samples) const {
    samples.clear();

    if (this->isNonInformative()) {
        return;
    }

    samples.resize(numberSamples, *m_Constant);
}

bool CMultivariateConstantPrior::isNonInformative() const {
    return !m_Constant;
}

void CMultivariateConstantPrior::print(const std::string& separator, std::string& result) const {
    result += core_t::LINE_ENDING + separator + "constant " +
              (this->isNonInformative() ? std::string("non-informative")
                                        : core::CContainerPrinter::print(*m_Constant));
}

std::uint64_t CMultivariateConstantPrior::checksum(std::uint64_t seed) const {
    seed = this->CMultivariatePrior::checksum(seed);
    return CChecksum::calculate(seed, m_Constant);
}

void CMultivariateConstantPrior::debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CMultivariateConstantPrior");
    core::CMemoryDebug::dynamicSize("m_Constant", m_Constant, mem);
}

std::size_t CMultivariateConstantPrior::memoryUsage() const {
    return core::CMemory::dynamicSize(m_Constant);
}

std::size_t CMultivariateConstantPrior::staticSize() const {
    return sizeof(*this);
}

void CMultivariateConstantPrior::acceptPersistInserter(core::CStatePersistInserter& inserter) const {
    if (m_Constant) {
        inserter.insertValue(CONSTANT_TAG, core::CPersistUtils::toString(
                                               *m_Constant, CConstantToString()));
    }
}

std::string CMultivariateConstantPrior::persistenceTag() const {
    return CONSTANT_TAG + core::CStringUtils::typeToString(m_Dimension);
}

const CMultivariateConstantPrior::TOptionalDouble10Vec&
CMultivariateConstantPrior::constant() const {
    return m_Constant;
}
}
}
}
