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

#include <maths/time_series/CCalendarComponentAdaptiveBucketing.h>

#include <core/CLogger.h>
#include <core/CMemoryDef.h>
#include <core/CPersistUtils.h>
#include <core/CStatePersistInserter.h>
#include <core/CStateRestoreTraverser.h>
#include <core/CStringUtils.h>
#include <core/RestoreMacros.h>

#include <maths/common/CBasicStatisticsPersist.h>
#include <maths/common/CChecksum.h>
#include <maths/common/CMathsFuncs.h>
#include <maths/common/CTools.h>

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace ml {
namespace maths {
namespace time_series {
namespace {
const core::TPersistenceTag ADAPTIVE_BUCKETING_TAG{"a", "adaptive_bucketing"};
const core::TPersistenceTag FEATURE_TAG{"b", "feature"};
const core::TPersistenceTag VALUES_TAG{"c", "values"};
const core::TPersistenceTag TIME_ZONE_OFFSET_TAG{"d", "time_zone"};
const std::string EMPTY_STRING;
}

CCalendarComponentAdaptiveBucketing::CCalendarComponentAdaptiveBucketing()
    : CAdaptiveBucketing{0.0, 0.0} {
}

CCalendarComponentAdaptiveBucketing::CCalendarComponentAdaptiveBucketing(CCalendarFeature feature,
                                                                         core_t::TTime timeZoneOffset,
                                                                         double decayRate,
                                                                         double minimumBucketLength)
    : CAdaptiveBucketing{decayRate, minimumBucketLength}, m_Feature{feature}, m_TimeZoneOffset{timeZoneOffset} {
}

CCalendarComponentAdaptiveBucketing::CCalendarComponentAdaptiveBucketing(
    double decayRate,
    double minimumBucketLength,
    core::CStateRestoreTraverser& traverser)
    : CAdaptiveBucketing{decayRate, minimumBucketLength} {
    if (traverser.traverseSubLevel([this](auto& traverser_) {
            return this->acceptRestoreTraverser(traverser_);
        }) == false) {
        traverser.setBadState();
    }
}

void CCalendarComponentAdaptiveBucketing::acceptPersistInserter(core::CStatePersistInserter& inserter) const {
    inserter.insertLevel(ADAPTIVE_BUCKETING_TAG, this->getAcceptPersistInserter());
    inserter.insertValue(FEATURE_TAG, m_Feature.toDelimited());
    inserter.insertValue(TIME_ZONE_OFFSET_TAG, m_TimeZoneOffset);
    core::CPersistUtils::persist(VALUES_TAG, m_Values, inserter);
}

void CCalendarComponentAdaptiveBucketing::swap(CCalendarComponentAdaptiveBucketing& other) {
    this->CAdaptiveBucketing::swap(other);
    std::swap(m_Feature, other.m_Feature);
    std::swap(m_TimeZoneOffset, other.m_TimeZoneOffset);
    m_Values.swap(other.m_Values);
}

bool CCalendarComponentAdaptiveBucketing::initialize(std::size_t n) {
    double a{0.0};
    double b{static_cast<double>(m_Feature.window())};
    if (this->CAdaptiveBucketing::initialize(a, b, n)) {
        m_Values.clear();
        m_Values.resize(this->size());
        return true;
    }
    return false;
}

void CCalendarComponentAdaptiveBucketing::clear() {
    this->CAdaptiveBucketing::clear();
    m_Values.clear();
    m_Values.shrink_to_fit();
}

void CCalendarComponentAdaptiveBucketing::linearScale(double scale) {
    for (auto& value : m_Values) {
        common::CBasicStatistics::moment<0>(value) *= scale;
    }
}

void CCalendarComponentAdaptiveBucketing::add(core_t::TTime time, double value, double weight) {
    std::size_t bucket{0};
    if (this->initialized() && this->bucket(time, bucket)) {
        this->CAdaptiveBucketing::add(bucket, time, weight);

        TFloatMeanVarAccumulator moments{m_Values[bucket]};
        double prediction{common::CBasicStatistics::mean(moments)};
        moments.add(value, weight * weight);

        m_Values[bucket].add(value, weight);
        common::CBasicStatistics::moment<1>(m_Values[bucket]) =
            common::CBasicStatistics::maximumLikelihoodVariance(moments);
        if (std::fabs(value - prediction) >
            LARGE_ERROR_STANDARD_DEVIATIONS *
                std::sqrt(common::CBasicStatistics::maximumLikelihoodVariance(moments))) {
            this->addLargeError(bucket, time);
        }
    }
}

CCalendarFeatureAndTZ CCalendarComponentAdaptiveBucketing::feature() const {
    return {m_Feature, m_TimeZoneOffset};
}

void CCalendarComponentAdaptiveBucketing::propagateForwardsByTime(double time) {
    if (time < 0.0) {
        LOG_ERROR(<< "Can't propagate bucketing backwards in time");
    } else if (this->initialized()) {
        double factor{std::exp(-this->decayRate() * time)};
        this->age(factor);
        for (auto& value : m_Values) {
            value.age(factor);
        }
    }
}

double CCalendarComponentAdaptiveBucketing::count(core_t::TTime time) const {
    const TFloatMeanVarAccumulator* value{this->value(time)};
    return value != nullptr
               ? static_cast<double>(common::CBasicStatistics::count(*value))
               : 0.0;
}

const CCalendarComponentAdaptiveBucketing::TFloatMeanVarAccumulator*
CCalendarComponentAdaptiveBucketing::value(core_t::TTime time) const {
    const TFloatMeanVarAccumulator* result{nullptr};
    if (this->initialized()) {
        std::size_t bucket{0};
        this->bucket(time, bucket);
        bucket = common::CTools::truncate(bucket, std::size_t(0), m_Values.size() - 1);
        result = &m_Values[bucket];
    }
    return result;
}

std::uint64_t CCalendarComponentAdaptiveBucketing::checksum(std::uint64_t seed) const {
    seed = this->CAdaptiveBucketing::checksum(seed);
    seed = common::CChecksum::calculate(seed, m_Feature);
    seed = common::CChecksum::calculate(seed, m_TimeZoneOffset);
    return common::CChecksum::calculate(seed, m_Values);
}

void CCalendarComponentAdaptiveBucketing::debugMemoryUsage(
    const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CCalendarComponentAdaptiveBucketing");
    core::CMemoryDebug::dynamicSize("m_Endpoints", this->endpoints(), mem);
    core::CMemoryDebug::dynamicSize("m_Centres", this->centres(), mem);
    core::CMemoryDebug::dynamicSize("m_LargeErrorCounts", this->largeErrorCounts(), mem);
    core::CMemoryDebug::dynamicSize("m_Values", m_Values, mem);
}

std::size_t CCalendarComponentAdaptiveBucketing::memoryUsage() const {
    return this->CAdaptiveBucketing::memoryUsage() + core::CMemory::dynamicSize(m_Values);
}

bool CCalendarComponentAdaptiveBucketing::acceptRestoreTraverser(core::CStateRestoreTraverser& traverser) {
    do {
        const std::string& name{traverser.name()};
        RESTORE(ADAPTIVE_BUCKETING_TAG,
                traverser.traverseSubLevel(this->getAcceptRestoreTraverser()))
        RESTORE(FEATURE_TAG, m_Feature.fromDelimited(traverser.value()))
        RESTORE(TIME_ZONE_OFFSET_TAG,
                core::CStringUtils::stringToType(traverser.value(), m_TimeZoneOffset))
        RESTORE_WITH_UTILS(VALUES_TAG, m_Values)
    } while (traverser.next());

    this->checkRestoredInvariants();

    return true;
}

void CCalendarComponentAdaptiveBucketing::checkRestoredInvariants() const {
    VIOLATES_INVARIANT(m_Values.size(), !=, this->centres().size());
}

void CCalendarComponentAdaptiveBucketing::refresh(const TFloatVec& oldEndpoints) {
    // Values are assigned based on their intersection with each
    // bucket in the previous configuration. The moments and
    // variance are computed using the appropriate combination
    // rules. Note that the count is weighted by the square of
    // the fractional intersection between the old and new buckets.
    // This means that the effective weight of buckets whose end
    // points change significantly is reduced. This is reasonable
    // because the periodic trend is assumed to be unchanging
    // throughout the interval, when of course it is varying, so
    // adjusting the end points introduces error in the bucket
    // value, which we handle by reducing its significance in the
    // new bucket values.
    //
    // A better approximation is to assume that it the trend is
    // continuous. In fact, this can be done by using a spline
    // with the constraint that the mean of the spline in each
    // interval is equal to the mean value. We can additionally
    // decompose the variance into a contribution from noise and
    // a contribution from the trend. Under these assumptions it
    // is then possible (but not trivial) to update the bucket
    // means and variances based on the new end point positions.
    // This might be worth considering at some point.

    using TDoubleMeanAccumulator = common::CBasicStatistics::SSampleMean<double>::TAccumulator;
    using TDoubleMeanVarAccumulator =
        common::CBasicStatistics::SSampleMeanVar<double>::TAccumulator;

    std::size_t m{m_Values.size()};
    std::size_t n{oldEndpoints.size()};
    if (m + 1 != n) {
        LOG_ERROR(<< "Inconsistent end points and regressions");
        return;
    }

    const TFloatVec& newEndpoints{this->endpoints()};
    const TFloatVec& oldCentres{this->centres()};
    const TFloatVec& oldLargeErrorCounts{this->largeErrorCounts()};

    TFloatMeanVarVec newValues;
    TFloatVec newCentres;
    TFloatVec newLargeErrorCounts;
    newValues.reserve(m);
    newCentres.reserve(m);
    newLargeErrorCounts.reserve(m);

    for (std::size_t i = 1; i < n; ++i) {
        double yl{newEndpoints[i - 1]};
        double yr{newEndpoints[i]};
        std::size_t r(std::lower_bound(oldEndpoints.begin(), oldEndpoints.end(), yr) -
                      oldEndpoints.begin());
        r = common::CTools::truncate(r, std::size_t(1), n - 1);

        std::size_t l(std::upper_bound(oldEndpoints.begin(), oldEndpoints.end(), yl) -
                      oldEndpoints.begin());
        l = common::CTools::truncate(l, std::size_t(1), r);

        LOG_TRACE(<< "interval = [" << yl << "," << yr << "]");
        LOG_TRACE(<< "l = " << l << ", r = " << r);
        LOG_TRACE(<< "[x(l), x(r)] = [" << oldEndpoints[l - 1] << ","
                  << oldEndpoints[r] << "]");

        double xl{oldEndpoints[l - 1]};
        double xr{oldEndpoints[l]};
        if (l == r) {
            double interval{newEndpoints[i] - newEndpoints[i - 1]};
            double w{common::CTools::truncate(interval / (xr - xl), 0.0, 1.0)};
            newValues.push_back(common::CBasicStatistics::scaled(m_Values[l - 1], w * w));
            newCentres.push_back(common::CTools::truncate(
                static_cast<double>(oldCentres[l - 1]), yl, yr));
            newLargeErrorCounts.push_back(w * oldLargeErrorCounts[l - 1]);
        } else {
            double interval{xr - newEndpoints[i - 1]};
            double w{common::CTools::truncate(interval / (xr - xl), 0.0, 1.0)};
            TDoubleMeanVarAccumulator value{
                common::CBasicStatistics::scaled(m_Values[l - 1], w)};
            TDoubleMeanAccumulator centre{common::CBasicStatistics::momentsAccumulator(
                w * common::CBasicStatistics::count(m_Values[l - 1]),
                static_cast<double>(oldCentres[l - 1]))};
            double largeErrorCount{w * oldLargeErrorCounts[l - 1]};
            double count{w * w * common::CBasicStatistics::count(m_Values[l - 1])};
            while (++l < r) {
                value += m_Values[l - 1];
                centre += common::CBasicStatistics::momentsAccumulator(
                    common::CBasicStatistics::count(m_Values[l - 1]),
                    static_cast<double>(oldCentres[l - 1]));
                largeErrorCount += oldLargeErrorCounts[l - 1];
                count += common::CBasicStatistics::count(m_Values[l - 1]);
            }
            xl = oldEndpoints[l - 1];
            xr = oldEndpoints[l];
            interval = newEndpoints[i] - xl;
            w = common::CTools::truncate(interval / (xr - xl), 0.0, 1.0);
            value += common::CBasicStatistics::scaled(m_Values[l - 1], w);
            centre += common::CBasicStatistics::momentsAccumulator(
                w * common::CBasicStatistics::count(m_Values[l - 1]),
                static_cast<double>(oldCentres[l - 1]));
            largeErrorCount += w * oldLargeErrorCounts[l - 1];
            count += w * w * common::CBasicStatistics::count(m_Values[l - 1]);
            // Defend against 0 / 0: if common::CBasicStatistics::count(value)
            // is zero then count must be too.
            double scale{count == common::CBasicStatistics::count(value)
                             ? 1.0
                             : count / common::CBasicStatistics::count(value)};
            newValues.push_back(common::CBasicStatistics::scaled(value, scale));
            newCentres.push_back(common::CTools::truncate(
                common::CBasicStatistics::mean(centre), yl, yr));
            newLargeErrorCounts.push_back(largeErrorCount);
        }
    }

    // We want all values to respond at the same rate to changes
    // in the trend. To achieve this we should assign them a weight
    // that is equal to the number of points they will receive in one
    // period.
    double count{0.0};
    for (const auto& value : newValues) {
        count += common::CBasicStatistics::count(value);
    }
    count /= (oldEndpoints[m] - oldEndpoints[0]);
    for (std::size_t i = 0; i < m; ++i) {
        double ci{common::CBasicStatistics::count(newValues[i])};
        if (ci > 0.0) {
            common::CBasicStatistics::scale(
                count * (oldEndpoints[i + 1] - oldEndpoints[i]) / ci, newValues[i]);
        }
    }

    LOG_TRACE(<< "old endpoints = " << oldEndpoints);
    LOG_TRACE(<< "old centres   = " << oldCentres);
    LOG_TRACE(<< "new endpoints = " << newEndpoints);
    LOG_TRACE(<< "new centres   = " << newCentres);
    m_Values.swap(newValues);
    this->centres().swap(newCentres);
    this->largeErrorCounts().swap(newLargeErrorCounts);
}

bool CCalendarComponentAdaptiveBucketing::inWindow(core_t::TTime time) const {
    return m_Feature.inWindow(time + m_TimeZoneOffset);
}

void CCalendarComponentAdaptiveBucketing::addInitialValue(std::size_t bucket,
                                                          core_t::TTime time,
                                                          double value,
                                                          double weight) {
    this->CAdaptiveBucketing::add(bucket, time, weight);
    m_Values[bucket].add(value, weight);
}

double CCalendarComponentAdaptiveBucketing::offset(core_t::TTime time) const {
    return static_cast<double>(m_Feature.offset(time + m_TimeZoneOffset));
}

double CCalendarComponentAdaptiveBucketing::bucketCount(std::size_t bucket) const {
    return common::CBasicStatistics::count(m_Values[bucket]);
}

double CCalendarComponentAdaptiveBucketing::predict(std::size_t bucket,
                                                    core_t::TTime /*time*/,
                                                    double /*offset*/) const {
    return common::CBasicStatistics::mean(m_Values[bucket]);
}

double CCalendarComponentAdaptiveBucketing::variance(std::size_t bucket) const {
    return common::CBasicStatistics::maximumLikelihoodVariance(m_Values[bucket]);
}

void CCalendarComponentAdaptiveBucketing::split(std::size_t bucket) {
    // We don't know the fraction of values' (weights) which would
    // have fallen in each half of the split bucket. However, some
    // fraction of them would ideally not be included in these
    // statistics, i.e. the values in the other half of the split.
    // If we assume an equal split but assign a weight of 0.0 to the
    // samples included in error we arrive at a multiplier of 0.25.
    // In practice this simply means we increase the significance
    // of new samples for some time which is reasonable.
    common::CBasicStatistics::scale(0.25, m_Values[bucket]);
    m_Values.insert(m_Values.begin() + bucket, m_Values[bucket]);
}

std::string CCalendarComponentAdaptiveBucketing::name() const {
    return "Calendar[" + std::to_string(this->decayRate()) + "," +
           std::to_string(this->minimumBucketLength()) + "]";
}

bool CCalendarComponentAdaptiveBucketing::isBad() const {
    // Check for bad values in both the means and the variances.
    return std::any_of(m_Values.begin(), m_Values.end(), [](const auto& value) {
        return ((common::CMathsFuncs::isFinite(common::CBasicStatistics::mean(value)) == false) ||
                (common::CMathsFuncs::isFinite(common::CBasicStatistics::variance(value))) == false);
    });
}
}
}
}
