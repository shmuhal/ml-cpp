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

#include <maths/common/CNaiveBayes.h>

#include <core/CFunctional.h>
#include <core/CLogger.h>
#include <core/CMemoryDef.h>
#include <core/CStatePersistInserter.h>
#include <core/CStateRestoreTraverser.h>
#include <core/RestoreMacros.h>

#include <maths/common/CBasicStatistics.h>
#include <maths/common/CChecksum.h>
#include <maths/common/CPrior.h>
#include <maths/common/CPriorStateSerialiser.h>
#include <maths/common/CRestoreParams.h>
#include <maths/common/CTools.h>
#include <maths/common/MathsTypes.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <string>
#include <utility>

namespace ml {
namespace maths {
namespace common {
namespace {
const core::TPersistenceTag PRIOR_TAG{"a", "prior"};
const core::TPersistenceTag CLASS_LABEL_TAG{"b", "class_label"};
const core::TPersistenceTag CLASS_MODEL_TAG{"c", "class_model"};
const core::TPersistenceTag MIN_MAX_LOG_LIKELIHOOD_TO_USE_FEATURE_TAG{
    "d", "min_max_likelihood_to_use_feature"};
const core::TPersistenceTag COUNT_TAG{"e", "count"};
const core::TPersistenceTag CONDITIONAL_DENSITY_FROM_PRIOR_TAG{"f", "conditional_density_from_prior"};
}

CNaiveBayesFeatureDensityFromPrior::CNaiveBayesFeatureDensityFromPrior(const CPrior& prior)
    : m_Prior(prior.clone()) {
}

bool CNaiveBayesFeatureDensityFromPrior::improper() const {
    return m_Prior->isNonInformative();
}

void CNaiveBayesFeatureDensityFromPrior::add(const TDouble1Vec& x) {
    m_Prior->addSamples(x, maths_t::CUnitWeights::SINGLE_UNIT);
}

CNaiveBayesFeatureDensityFromPrior* CNaiveBayesFeatureDensityFromPrior::clone() const {
    return new CNaiveBayesFeatureDensityFromPrior(*m_Prior);
}

bool CNaiveBayesFeatureDensityFromPrior::acceptRestoreTraverser(
    const SDistributionRestoreParams& params,
    core::CStateRestoreTraverser& traverser) {
    do {
        const std::string& name{traverser.name()};
        RESTORE(PRIOR_TAG, traverser.traverseSubLevel(
                               [&, serialiser = CPriorStateSerialiser{} ](auto& traverser_) {
                                   return serialiser(params, m_Prior, traverser_);
                               }))
    } while (traverser.next());

    this->checkRestoredInvariants();

    return true;
}

void CNaiveBayesFeatureDensityFromPrior::checkRestoredInvariants() const {
    VIOLATES_INVARIANT_NO_EVALUATION(m_Prior, ==, nullptr);
}

void CNaiveBayesFeatureDensityFromPrior::acceptPersistInserter(core::CStatePersistInserter& inserter) const {
    inserter.insertLevel(
        PRIOR_TAG, [ this, serialiser = CPriorStateSerialiser{} ](auto& inserter_) {
            serialiser(*m_Prior, inserter_);
        });
}

double CNaiveBayesFeatureDensityFromPrior::logValue(const TDouble1Vec& x) const {
    double result;
    if (m_Prior->jointLogMarginalLikelihood(x, maths_t::CUnitWeights::SINGLE_UNIT,
                                            result) != maths_t::E_FpNoErrors) {
        LOG_ERROR(<< "Bad density value at " << x << " for " << m_Prior->print());
        return std::numeric_limits<double>::lowest();
    }
    return result;
}

double CNaiveBayesFeatureDensityFromPrior::logMaximumValue() const {
    double result;
    if (m_Prior->jointLogMarginalLikelihood({m_Prior->marginalLikelihoodMode()},
                                            maths_t::CUnitWeights::SINGLE_UNIT,
                                            result) != maths_t::E_FpNoErrors) {
        LOG_ERROR(<< "Bad density value for " << m_Prior->print());
        return std::numeric_limits<double>::lowest();
    }
    return result;
}

void CNaiveBayesFeatureDensityFromPrior::dataType(maths_t::EDataType dataType) {
    m_Prior->dataType(dataType);
}

void CNaiveBayesFeatureDensityFromPrior::propagateForwardsByTime(double time) {
    m_Prior->propagateForwardsByTime(time);
}

void CNaiveBayesFeatureDensityFromPrior::debugMemoryUsage(
    const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    return core::CMemoryDebug::dynamicSize("m_Prior", m_Prior, mem);
}

std::size_t CNaiveBayesFeatureDensityFromPrior::staticSize() const {
    return sizeof(*this);
}

std::size_t CNaiveBayesFeatureDensityFromPrior::memoryUsage() const {
    return core::CMemory::dynamicSize(m_Prior);
}

std::uint64_t CNaiveBayesFeatureDensityFromPrior::checksum(std::uint64_t seed) const {
    return CChecksum::calculate(seed, m_Prior);
}

std::string CNaiveBayesFeatureDensityFromPrior::print() const {
    std::string result;
    m_Prior->print("  ", result);
    return result;
}

CNaiveBayes::CNaiveBayes(const CNaiveBayesFeatureDensity& exemplar,
                         double decayRate,
                         TOptionalDouble minMaxLogLikelihoodToUseFeature)
    : m_MinMaxLogLikelihoodToUseFeature{minMaxLogLikelihoodToUseFeature},
      m_DecayRate{decayRate}, m_Exemplar{exemplar.clone()}, m_ClassConditionalDensities{2} {
}

CNaiveBayes::CNaiveBayes(const CNaiveBayesFeatureDensity& exemplar,
                         const SDistributionRestoreParams& params,
                         core::CStateRestoreTraverser& traverser)
    : m_DecayRate{params.s_DecayRate}, m_Exemplar{exemplar.clone()}, m_ClassConditionalDensities{2} {
    if (traverser.traverseSubLevel([&](auto& traverser_) {
            return this->acceptRestoreTraverser(params, traverser_);
        }) == false) {
        traverser.setBadState();
    }
}

CNaiveBayes::CNaiveBayes(const CNaiveBayes& other)
    : m_MinMaxLogLikelihoodToUseFeature{other.m_MinMaxLogLikelihoodToUseFeature},
      m_DecayRate{other.m_DecayRate}, m_Exemplar{other.m_Exemplar->clone()} {
    for (const auto& class_ : other.m_ClassConditionalDensities) {
        m_ClassConditionalDensities.emplace(class_.first, class_.second);
    }
}

bool CNaiveBayes::acceptRestoreTraverser(const SDistributionRestoreParams& params,
                                         core::CStateRestoreTraverser& traverser) {
    std::size_t label;
    do {
        const std::string& name{traverser.name()};
        RESTORE_BUILT_IN(CLASS_LABEL_TAG, label)
        RESTORE_SETUP_TEARDOWN(
            CLASS_MODEL_TAG, CClass class_, traverser.traverseSubLevel([&](auto& traverser_) {
                return class_.acceptRestoreTraverser(params, traverser_);
            }),
            m_ClassConditionalDensities.emplace(label, std::move(class_)))
        RESTORE_SETUP_TEARDOWN(MIN_MAX_LOG_LIKELIHOOD_TO_USE_FEATURE_TAG, double value,
                               core::CStringUtils::stringToType(traverser.value(), value),
                               m_MinMaxLogLikelihoodToUseFeature.emplace(value))
    } while (traverser.next());
    return true;
}

void CNaiveBayes::acceptPersistInserter(core::CStatePersistInserter& inserter) const {
    using TSizeClassUMapCItr = TSizeClassUMap::const_iterator;
    using TSizeClassUMapCItrVec = std::vector<TSizeClassUMapCItr>;

    TSizeClassUMapCItrVec classes;
    classes.reserve(m_ClassConditionalDensities.size());
    for (auto i = m_ClassConditionalDensities.begin();
         i != m_ClassConditionalDensities.end(); ++i) {
        classes.push_back(i);
    }
    std::sort(classes.begin(), classes.end(),
              core::CFunctional::SDereference<COrderings::SFirstLess>());
    for (const auto& class_ : classes) {
        inserter.insertValue(CLASS_LABEL_TAG, class_->first);
        inserter.insertLevel(CLASS_MODEL_TAG, [&class_](auto& inserter_) {
            class_->second.acceptPersistInserter(inserter_);
        });
    }

    if (m_MinMaxLogLikelihoodToUseFeature) {
        inserter.insertValue(MIN_MAX_LOG_LIKELIHOOD_TO_USE_FEATURE_TAG,
                             *m_MinMaxLogLikelihoodToUseFeature,
                             core::CIEEE754::E_SinglePrecision);
    }
}

CNaiveBayes& CNaiveBayes::operator=(const CNaiveBayes& other) {
    if (this != &other) {
        CNaiveBayes copy{other};
        this->swap(copy);
    }
    return *this;
}

void CNaiveBayes::swap(CNaiveBayes& other) {
    std::swap(m_DecayRate, other.m_DecayRate);
    m_Exemplar.swap(other.m_Exemplar);
    m_ClassConditionalDensities.swap(other.m_ClassConditionalDensities);
    std::swap(m_MinMaxLogLikelihoodToUseFeature, other.m_MinMaxLogLikelihoodToUseFeature);
}

bool CNaiveBayes::initialized() const {
    return m_ClassConditionalDensities.size() > 0 &&
           std::all_of(m_ClassConditionalDensities.begin(),
                       m_ClassConditionalDensities.end(),
                       [](const std::pair<std::size_t, CClass>& class_) {
                           return class_.second.initialized();
                       });
}

void CNaiveBayes::initialClassCounts(const TDoubleSizePrVec& counts) {
    for (const auto& count : counts) {
        m_ClassConditionalDensities.emplace(count.second, CClass{count.first});
    }
}

void CNaiveBayes::addTrainingDataPoint(std::size_t label, const TDouble1VecVec& x) {
    if (!this->validate(x)) {
        return;
    }

    auto& class_ = m_ClassConditionalDensities[label];

    if (class_.conditionalDensities().empty()) {
        class_.conditionalDensities().reserve(x.size());
        std::generate_n(
            std::back_inserter(class_.conditionalDensities()), x.size(),
            [this]() { return TFeatureDensityPtr{m_Exemplar->clone()}; });
    }

    bool updateCount{false};
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (x[i].size() > 0) {
            class_.conditionalDensities()[i]->add(x[i]);
            updateCount = true;
        }
    }

    if (updateCount) {
        class_.count() += 1.0;
    } else {
        LOG_TRACE(<< "Ignoring empty feature vector");
    }
}

void CNaiveBayes::dataType(maths_t::EDataType dataType) {
    for (auto& class_ : m_ClassConditionalDensities) {
        for (auto& density : class_.second.conditionalDensities()) {
            density->dataType(dataType);
        }
    }
}

void CNaiveBayes::propagateForwardsByTime(double time) {
    double factor{std::exp(-m_DecayRate * time)};
    for (auto& class_ : m_ClassConditionalDensities) {
        class_.second.count() *= factor;
        for (auto& density : class_.second.conditionalDensities()) {
            density->propagateForwardsByTime(time);
        }
    }
}

CNaiveBayes::TDoubleSizePrVec
CNaiveBayes::highestClassProbabilities(std::size_t n, const TDouble1VecVec& x) const {
    TDoubleSizePrVec p(this->classProbabilities(x));
    n = std::min(n, p.size());
    std::sort(p.begin(), p.begin() + n, std::greater<>());
    return TDoubleSizePrVec{p.begin(), p.begin() + n};
}

double CNaiveBayes::classProbability(std::size_t label, const TDouble1VecVec& x) const {
    TDoubleSizePrVec p(this->classProbabilities(x));
    auto i = std::find_if(p.begin(), p.end(), [label](const TDoubleSizePr& p_) {
        return p_.second == label;
    });
    return i == p.end() ? 0.0 : i->first;
}

CNaiveBayes::TDoubleSizePrVec CNaiveBayes::classProbabilities(const TDouble1VecVec& x) const {
    if (!this->validate(x)) {
        return {};
    }
    if (m_ClassConditionalDensities.empty()) {
        LOG_ERROR(<< "Trying to compute class probabilities without supplying training data");
        return {};
    }

    using TDoubleVec = std::vector<double>;
    using TMaxAccumulator = CBasicStatistics::SMax<double>::TAccumulator;

    TDoubleSizePrVec p;
    p.reserve(m_ClassConditionalDensities.size());
    for (const auto& class_ : m_ClassConditionalDensities) {
        p.emplace_back(CTools::fastLog(class_.second.count()), class_.first);
    }

    TDoubleVec logLikelihoods;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (x[i].size() > 0) {
            TMaxAccumulator maxLogLikelihood;
            logLikelihoods.clear();
            for (const auto& class_ : m_ClassConditionalDensities) {
                const auto& density = class_.second.conditionalDensities()[i];
                double logLikelihood{density->logValue(x[i])};
                double logMaximumLikelihood{density->logMaximumValue()};
                maxLogLikelihood.add(logLikelihood - logMaximumLikelihood);
                logLikelihoods.push_back(logLikelihood);
            }
            double weight{1.0};
            if (m_MinMaxLogLikelihoodToUseFeature) {
                weight = CTools::logisticFunction(
                    (maxLogLikelihood[0] - *m_MinMaxLogLikelihoodToUseFeature) /
                        std::fabs(*m_MinMaxLogLikelihoodToUseFeature),
                    0.1);
            }
            for (std::size_t j = 0; j < logLikelihoods.size(); ++j) {
                p[j].first += weight * logLikelihoods[j];
            }
        }
    }

    double scale{std::max_element(p.begin(), p.end())->first};
    double Z{0.0};
    for (auto& pc : p) {
        pc.first = std::exp(pc.first - scale);
        Z += pc.first;
    }
    for (auto& pc : p) {
        pc.first /= Z;
    }

    return p;
}

void CNaiveBayes::debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    core::CMemoryDebug::dynamicSize("m_Exemplar", m_Exemplar, mem);
    core::CMemoryDebug::dynamicSize("m_ClassConditionalDensities",
                                    m_ClassConditionalDensities, mem);
}

std::size_t CNaiveBayes::memoryUsage() const {
    return core::CMemory::dynamicSize(m_Exemplar) +
           core::CMemory::dynamicSize(m_ClassConditionalDensities);
}

std::uint64_t CNaiveBayes::checksum(std::uint64_t seed) const {
    CChecksum::calculate(seed, m_MinMaxLogLikelihoodToUseFeature);
    CChecksum::calculate(seed, m_DecayRate);
    CChecksum::calculate(seed, m_Exemplar);
    return CChecksum::calculate(seed, m_ClassConditionalDensities);
}

std::string CNaiveBayes::print() const {
    std::ostringstream result;
    result << "\n";
    for (const auto& class_ : m_ClassConditionalDensities) {
        result << "CLASS(" << class_.first << ")\n";
        for (const auto& density : class_.second.conditionalDensities()) {
            result << "---";
            result << density->print() << "\n";
        }
    }
    return result.str();
}

bool CNaiveBayes::validate(const TDouble1VecVec& x) const {
    auto class_ = m_ClassConditionalDensities.begin();
    if (class_ != m_ClassConditionalDensities.end() &&
        class_->second.conditionalDensities().size() > 0 &&
        class_->second.conditionalDensities().size() != x.size()) {
        LOG_ERROR(<< "Unexpected feature vector: " << x);
        return false;
    }
    return true;
}

CNaiveBayes::CClass::CClass(double count) : m_Count{count} {
}

CNaiveBayes::CClass::CClass(const CClass& other) : m_Count{other.m_Count} {
    m_ConditionalDensities.reserve(other.m_ConditionalDensities.size());
    for (const auto& density : other.m_ConditionalDensities) {
        m_ConditionalDensities.emplace_back(density->clone());
    }
}

bool CNaiveBayes::CClass::acceptRestoreTraverser(const SDistributionRestoreParams& params,
                                                 core::CStateRestoreTraverser& traverser) {
    do {
        const std::string& name{traverser.name()};
        RESTORE_BUILT_IN(COUNT_TAG, m_Count)
        RESTORE_SETUP_TEARDOWN(CONDITIONAL_DENSITY_FROM_PRIOR_TAG,
                               CNaiveBayesFeatureDensityFromPrior density,
                               traverser.traverseSubLevel([&](auto& traverser_) {
                                   return density.acceptRestoreTraverser(params, traverser_);
                               }),
                               m_ConditionalDensities.emplace_back(density.clone()))
        // Add other implementations' restore code here.
    } while (traverser.next());
    return true;
}

void CNaiveBayes::CClass::acceptPersistInserter(core::CStatePersistInserter& inserter) const {
    inserter.insertValue(COUNT_TAG, m_Count, core::CIEEE754::E_SinglePrecision);
    for (const auto& density : m_ConditionalDensities) {
        if (dynamic_cast<const CNaiveBayesFeatureDensityFromPrior*>(density.get())) {
            inserter.insertLevel(CONDITIONAL_DENSITY_FROM_PRIOR_TAG,
                                 [&density](auto& inserter_) {
                                     density->acceptPersistInserter(inserter_);
                                 });
            continue;
        }
        // Add other implementations' persist code here.
    }
}

bool CNaiveBayes::CClass::initialized() const {
    return std::none_of(
        m_ConditionalDensities.begin(), m_ConditionalDensities.end(),
        [](const TFeatureDensityPtr& density) { return density->improper(); });
}

double CNaiveBayes::CClass::count() const {
    return m_Count;
}

double& CNaiveBayes::CClass::count() {
    return m_Count;
}

const CNaiveBayes::TFeatureDensityPtrVec& CNaiveBayes::CClass::conditionalDensities() const {
    return m_ConditionalDensities;
}

CNaiveBayes::TFeatureDensityPtrVec& CNaiveBayes::CClass::conditionalDensities() {
    return m_ConditionalDensities;
}

void CNaiveBayes::CClass::debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    core::CMemoryDebug::dynamicSize("s_ConditionalDensities", m_ConditionalDensities, mem);
}

std::size_t CNaiveBayes::CClass::memoryUsage() const {
    return core::CMemory::dynamicSize(m_ConditionalDensities);
}

std::uint64_t CNaiveBayes::CClass::checksum(std::uint64_t seed) const {
    seed = CChecksum::calculate(seed, m_Count);
    return CChecksum::calculate(seed, m_ConditionalDensities);
}
}
}
}
