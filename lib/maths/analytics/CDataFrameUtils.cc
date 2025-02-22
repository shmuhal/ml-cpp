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

#include <maths/analytics/CDataFrameUtils.h>

#include <core/CLogger.h>
#include <core/CPackedBitVector.h>
#include <core/Concurrency.h>

#include <maths/analytics/CDataFrameCategoryEncoder.h>
#include <maths/analytics/CMic.h>

#include <maths/common/CBasicStatistics.h>
#include <maths/common/CLbfgs.h>
#include <maths/common/CLinearAlgebraEigen.h>
#include <maths/common/CMathsFuncs.h>
#include <maths/common/COrderings.h>
#include <maths/common/CPRNG.h>
#include <maths/common/CQuantileSketch.h>
#include <maths/common/CSampling.h>
#include <maths/common/CSolvers.h>
#include <maths/common/CTools.h>
#include <maths/common/CToolsDetail.h>

#include <boost/unordered_map.hpp>

#include <cmath>
#include <limits>
#include <memory>
#include <numeric>
#include <vector>

namespace ml {
namespace maths {
namespace analytics {
namespace {
using TDoubleVec = std::vector<double>;
using TSizeVec = std::vector<std::size_t>;
using TSizeVecVec = std::vector<TSizeVec>;
using TFloatVec = std::vector<common::CFloatStorage>;
using TFloatVecVec = std::vector<TFloatVec>;
using TRowItr = core::CDataFrame::TRowItr;
using TRowRef = core::CDataFrame::TRowRef;
using TRowSampler = common::CSampling::CReservoirSampler<TRowRef>;
using TRowSamplerVec = std::vector<TRowSampler>;
using TSizeEncoderPtrUMap =
    boost::unordered_map<std::size_t, std::unique_ptr<CDataFrameUtils::CColumnValue>>;
using TPackedBitVectorVec = CDataFrameUtils::TPackedBitVectorVec;
using TDoubleVector = CDataFrameUtils::TDoubleVector;

//! Reduce the results of a call to core::CDataFrame::readRows using \p reduceFirst
//! for the first and \p reduce for the rest and writing the result \p reduction.
//!
//! \tparam REDUCER Must be a binary operator whose logical type is the function
//! void (typeof(READER.s_FunctionState[0]), REDUCTION&).
template<typename READER, typename REDUCER, typename FIRST_REDUCER, typename REDUCTION>
bool doReduce(std::pair<std::vector<READER>, bool> readResults,
              FIRST_REDUCER reduceFirst,
              REDUCER reduce,
              REDUCTION& reduction) {
    if (readResults.second == false) {
        return false;
    }
    reduceFirst(std::move(readResults.first[0].s_FunctionState), reduction);
    for (std::size_t i = 1; i < readResults.first.size(); ++i) {
        reduce(std::move(readResults.first[i].s_FunctionState), reduction);
    }
    return true;
}

//! \brief Manages stratified sampling.
class CStratifiedSampler {
public:
    using TSamplerSelector = std::function<std::size_t(const TRowRef&)>;

public:
    explicit CStratifiedSampler(std::size_t size) : m_SampledRowIndices(size) {
        m_DesiredCounts.reserve(size);
        m_Samplers.reserve(size);
    }

    void sample(const TRowRef& row) { m_Samplers[m_Selector(row)].sample(row); }

    //! Add one of the strata samplers.
    void addSampler(std::size_t count, common::CPRNG::CXorOShiro128Plus rng) {
        TSizeVec& samples{m_SampledRowIndices[m_Samplers.size()]};
        samples.reserve(count);
        auto sampler = [&](std::size_t slot, const TRowRef& row) {
            if (slot >= samples.size()) {
                samples.resize(slot + 1);
            }
            samples[slot] = row.index();
        };
        m_DesiredCounts.push_back(count);
        m_Samplers.emplace_back(count, sampler, rng);
    }

    //! Define the callback to select the sampler.
    void samplerSelector(TSamplerSelector selector) {
        m_Selector = std::move(selector);
    }

    //! This selects the final samples, writing to \p result, and resets the sampling
    //! state so this is ready to sample again.
    void finishSampling(common::CPRNG::CXorOShiro128Plus& rng, TSizeVec& result) {
        result.clear();
        for (std::size_t i = 0; i < m_SampledRowIndices.size(); ++i) {
            std::size_t sampleSize{m_Samplers[i].sampleSize()};
            std::size_t desiredCount{std::min(m_DesiredCounts[i], sampleSize)};
            common::CSampling::random_shuffle(rng, m_SampledRowIndices[i].begin(),
                                              m_SampledRowIndices[i].begin() + sampleSize);
            result.insert(result.end(), m_SampledRowIndices[i].begin(),
                          m_SampledRowIndices[i].begin() + desiredCount);
            m_SampledRowIndices[i].clear();
            m_Samplers[i].reset();
        }
    }

private:
    TSizeVec m_DesiredCounts;
    TSizeVecVec m_SampledRowIndices;
    TRowSamplerVec m_Samplers;
    TSamplerSelector m_Selector;
};

using TStratifiedSamplerUPtr = std::unique_ptr<CStratifiedSampler>;

//! Get a classifier stratified row sampler for cross fold validation.
std::pair<TStratifiedSamplerUPtr, TDoubleVec>
classifierStratifiedCrossValidationRowSampler(std::size_t numberThreads,
                                              const core::CDataFrame& frame,
                                              std::size_t targetColumn,
                                              common::CPRNG::CXorOShiro128Plus rng,
                                              std::size_t desiredCount,
                                              const core::CPackedBitVector& rowMask) {

    TDoubleVec categoryFrequencies{CDataFrameUtils::categoryFrequencies(
        numberThreads, frame, rowMask, {targetColumn})[targetColumn]};
    LOG_TRACE(<< "category frequencies = " << categoryFrequencies);

    TSizeVec categoryCounts;
    common::CSampling::weightedSample(desiredCount, categoryFrequencies, categoryCounts);
    LOG_TRACE(<< "desired category counts per test fold = " << categoryCounts);

    auto sampler = std::make_unique<CStratifiedSampler>(categoryCounts.size());
    for (auto categoryCount : categoryCounts) {
        sampler->addSampler(categoryCount, rng);
    }
    sampler->samplerSelector([targetColumn](const TRowRef& row) mutable {
        return static_cast<std::size_t>(row[targetColumn]);
    });

    return {std::move(sampler), std::move(categoryFrequencies)};
}

//! Get a regression stratified row sampler for cross fold validation.
TStratifiedSamplerUPtr
regressionStratifiedCrossValidationRowSampler(std::size_t numberThreads,
                                              const core::CDataFrame& frame,
                                              std::size_t targetColumn,
                                              common::CPRNG::CXorOShiro128Plus rng,
                                              std::size_t desiredCount,
                                              std::size_t numberBuckets,
                                              const core::CPackedBitVector& rowMask) {

    auto quantiles =
        CDataFrameUtils::columnQuantiles(
            numberThreads, frame, rowMask, {targetColumn},
            common::CFastQuantileSketch{common::CFastQuantileSketch::E_Linear, 75,
                                        common::CPRNG::CXorOShiro128Plus{}, 0.9})
            .first;

    TDoubleVec buckets;
    for (double step = 100.0 / static_cast<double>(numberBuckets), percentile = step;
         percentile < 100.0; percentile += step) {
        double xQuantile;
        quantiles[0].quantile(percentile, xQuantile);
        buckets.push_back(xQuantile);
    }
    buckets.erase(std::unique(buckets.begin(), buckets.end()), buckets.end());
    buckets.push_back(std::numeric_limits<double>::max());
    LOG_TRACE(<< "buckets = " << buckets);

    auto bucketSelector = [buckets, targetColumn](const TRowRef& row) mutable {
        return static_cast<std::size_t>(
            std::upper_bound(buckets.begin(), buckets.end(), row[targetColumn]) -
            buckets.begin());
    };

    auto countBucketRows = core::bindRetrievableState(
        [&](TDoubleVec& bucketCounts, const TRowItr& beginRows, const TRowItr& endRows) {
            for (auto row = beginRows; row != endRows; ++row) {
                bucketCounts[bucketSelector(*row)] += 1.0;
            }
        },
        TDoubleVec(buckets.size(), 0.0));
    auto copyBucketRowCounts = [](TDoubleVec counts_, TDoubleVec& counts) {
        counts = std::move(counts_);
    };
    auto reduceBucketRowCounts = [](TDoubleVec counts_, TDoubleVec& counts) {
        for (std::size_t i = 0; i < counts.size(); ++i) {
            counts[i] += counts_[i];
        }
    };

    TDoubleVec bucketFrequencies;
    doReduce(frame.readRows(numberThreads, 0, frame.numberRows(), countBucketRows, &rowMask),
             copyBucketRowCounts, reduceBucketRowCounts, bucketFrequencies);
    double totalCount{std::accumulate(bucketFrequencies.begin(),
                                      bucketFrequencies.end(), 0.0)};
    for (auto& frequency : bucketFrequencies) {
        frequency /= totalCount;
    }

    TSizeVec bucketCounts;
    common::CSampling::weightedSample(desiredCount, bucketFrequencies, bucketCounts);
    LOG_TRACE(<< "desired bucket counts per fold = " << bucketCounts);

    auto sampler = std::make_unique<CStratifiedSampler>(buckets.size());
    for (std::size_t i = 0; i < buckets.size(); ++i) {
        sampler->addSampler(bucketCounts[i], rng);
    }
    sampler->samplerSelector(bucketSelector);

    return sampler;
}

TStratifiedSamplerUPtr
classifierDistributionPreservingRowSampler(std::size_t numberThreads,
                                           const core::CDataFrame& frame,
                                           std::size_t targetColumn,
                                           common::CPRNG::CXorOShiro128Plus rng,
                                           const core::CPackedBitVector& rowMask) {
    TDoubleVec categoryCounts{CDataFrameUtils::categoryCounts(
        numberThreads, frame, rowMask, {targetColumn})[targetColumn]};

    auto sampler = std::make_unique<CStratifiedSampler>(categoryCounts.size());
    for (auto categoryCount : categoryCounts) {
        sampler->addSampler(static_cast<std::size_t>(categoryCount), rng);
    }
    sampler->samplerSelector([targetColumn](const TRowRef& row) {
        return static_cast<std::size_t>(row[targetColumn]);
    });

    return sampler;
}

//! Get the test row masks corresponding to \p foldRowMasks.
TPackedBitVectorVec complementRowMasks(const TPackedBitVectorVec& foldRowMasks,
                                       core::CPackedBitVector allRowsMask) {
    TPackedBitVectorVec complementFoldRowMasks(foldRowMasks.size(), std::move(allRowsMask));
    for (std::size_t fold = 0; fold < foldRowMasks.size(); ++fold) {
        complementFoldRowMasks[fold] ^= foldRowMasks[fold];
    }
    return complementFoldRowMasks;
}

//! Get a row feature sampler.
template<typename TARGET>
auto rowFeatureSampler(std::size_t i, const TARGET& target, TFloatVecVec& samples) {
    return [i, &target, &samples](std::size_t slot, const TRowRef& row) {
        if (slot >= samples.size()) {
            samples.resize(slot + 1, {0.0, 0.0});
        }
        samples[slot][0] = row[i];
        samples[slot][1] = target(row);
    };
}

//! Get a row sampler.
auto rowSampler(TFloatVecVec& samples) {
    return [&samples](std::size_t slot, const TRowRef& row) {
        if (slot >= samples.size()) {
            samples.resize(slot + 1, TFloatVec(row.numberColumns()));
        }
        row.copyTo(samples[slot].begin());
    };
}

template<typename TARGET>
auto computeEncodedCategory(CMic& mic,
                            const TARGET& target,
                            TSizeEncoderPtrUMap& encoders,
                            TFloatVecVec& samples) {

    CDataFrameUtils::TSizeDoublePrVec encodedMics;
    encodedMics.reserve(encoders.size());
    for (const auto& encoder : encoders) {
        std::size_t category{encoder.first};
        const auto& encode = *encoder.second;
        mic.clear();
        for (const auto& sample : samples) {
            mic.add(encode(sample), target(sample));
        }
        encodedMics.emplace_back(category, mic.compute());
    }
    return encodedMics;
}

//! Check that all components are neither infinite nor NaN.
bool allFinite(TDoubleVector x) {
    return std::all_of(x.begin(), x.end(),
                       [](auto xi) { return common::CMathsFuncs::isFinite(xi); });
}

const std::size_t NUMBER_SAMPLES_TO_COMPUTE_MIC{10000};
}

std::string CDataFrameUtils::SDataType::toDelimited() const {
    // clang-format off
    return core::CStringUtils::typeToString(static_cast<int>(s_IsInteger)) +
           INTERNAL_DELIMITER +
           core::CStringUtils::typeToStringPrecise(s_Min, core::CIEEE754::E_DoublePrecision) +
           INTERNAL_DELIMITER +
           core::CStringUtils::typeToStringPrecise(s_Max, core::CIEEE754::E_DoublePrecision) +
           INTERNAL_DELIMITER;
    // clang-format on
}

bool CDataFrameUtils::SDataType::fromDelimited(const std::string& delimited) {
    TDoubleVec state(3);
    std::size_t pos{0}, i{0};
    for (auto delimiter = delimited.find(INTERNAL_DELIMITER);
         delimiter != std::string::npos && i < state.size();
         delimiter = delimited.find(INTERNAL_DELIMITER, pos)) {
        if (core::CStringUtils::stringToType(delimited.substr(pos, delimiter - pos),
                                             state[i++]) == false) {
            return false;
        }
        pos = delimiter + 1;
    }
    std::tie(s_IsInteger, s_Min, s_Max) =
        std::make_tuple(state[0] == 1.0, state[1], state[2]);
    return true;
}

const char CDataFrameUtils::SDataType::INTERNAL_DELIMITER{':'};
const char CDataFrameUtils::SDataType::EXTERNAL_DELIMITER{';'};

bool CDataFrameUtils::standardizeColumns(std::size_t numberThreads, core::CDataFrame& frame) {

    using TMeanVarAccumulatorVec =
        std::vector<common::CBasicStatistics::SSampleMeanVar<double>::TAccumulator>;

    if (frame.numberRows() == 0 || frame.numberColumns() == 0) {
        return true;
    }

    auto readColumnMoments = core::bindRetrievableState(
        [](TMeanVarAccumulatorVec& moments_, const TRowItr& beginRows, const TRowItr& endRows) {
            for (auto row = beginRows; row != endRows; ++row) {
                for (std::size_t i = 0; i < row->numberColumns(); ++i) {
                    if (isMissing((*row)[i]) == false) {
                        moments_[i].add((*row)[i]);
                    }
                }
            }
        },
        TMeanVarAccumulatorVec(frame.numberColumns()));
    auto copyColumnMoments = [](TMeanVarAccumulatorVec moments_,
                                TMeanVarAccumulatorVec& moments) {
        moments = std::move(moments_);
    };
    auto reduceColumnMoments = [](TMeanVarAccumulatorVec moments_,
                                  TMeanVarAccumulatorVec& moments) {
        for (std::size_t i = 0; i < moments.size(); ++i) {
            moments[i] += moments_[i];
        }
    };

    TMeanVarAccumulatorVec moments;
    if (doReduce(frame.readRows(numberThreads, readColumnMoments),
                 copyColumnMoments, reduceColumnMoments, moments) == false) {
        LOG_ERROR(<< "Failed to standardise columns");
        return false;
    }

    TDoubleVec mean(moments.size());
    TDoubleVec scale(moments.size());
    for (std::size_t i = 0; i < moments.size(); ++i) {
        double variance{common::CBasicStatistics::variance(moments[i])};
        mean[i] = common::CBasicStatistics::mean(moments[i]);
        scale[i] = variance == 0.0 ? 1.0 : 1.0 / std::sqrt(variance);
    }

    LOG_TRACE(<< "means = " << mean);
    LOG_TRACE(<< "scales = " << scale);

    auto standardiseColumns = [&mean, &scale](const TRowItr& beginRows,
                                              const TRowItr& endRows) {
        for (auto row = beginRows; row != endRows; ++row) {
            for (std::size_t i = 0; i < row->numberColumns(); ++i) {
                row->writeColumn(i, scale[i] * ((*row)[i] - mean[i]));
            }
        }
    };

    return frame.writeColumns(numberThreads, standardiseColumns).second;
}

CDataFrameUtils::TDataTypeVec
CDataFrameUtils::columnDataTypes(std::size_t numberThreads,
                                 const core::CDataFrame& frame,
                                 const core::CPackedBitVector& rowMask,
                                 const TSizeVec& columnMask,
                                 const CDataFrameCategoryEncoder* encoder) {

    if (frame.numberRows() == 0) {
        return {};
    }

    using TMinMax = common::CBasicStatistics::CMinMax<double>;
    using TMinMaxBoolPrVec = std::vector<std::pair<TMinMax, bool>>;

    auto readDataTypes = core::bindRetrievableState(
        [&](TMinMaxBoolPrVec& types, const TRowItr& beginRows, const TRowItr& endRows) {
            double integerPart;
            if (encoder != nullptr) {
                for (auto row = beginRows; row != endRows; ++row) {
                    CEncodedDataFrameRowRef encodedRow{encoder->encode(*row)};
                    for (auto i : columnMask) {
                        double value{encodedRow[i]};
                        if (isMissing(value) == false) {
                            types[i].first.add(value);
                            types[i].second = types[i].second &&
                                              (std::modf(value, &integerPart) == 0.0);
                        }
                    }
                }
            } else {
                for (auto row = beginRows; row != endRows; ++row) {
                    for (auto i : columnMask) {
                        double value{(*row)[i]};
                        if (isMissing(value) == false) {
                            types[i].first.add(value);
                            types[i].second = types[i].second &&
                                              (std::modf(value, &integerPart) == 0.0);
                        }
                    }
                }
            }
        },
        TMinMaxBoolPrVec(encoder != nullptr ? encoder->numberEncodedColumns()
                                            : frame.numberColumns(),
                         {TMinMax{}, true}));

    auto copyDataTypes = [](TMinMaxBoolPrVec types, TMinMaxBoolPrVec& result) {
        result = std::move(types);
    };
    auto reduceDataTypes = [&](TMinMaxBoolPrVec types, TMinMaxBoolPrVec& result) {
        for (auto i : columnMask) {
            result[i].first += types[i].first;
            result[i].second = result[i].second && types[i].second;
        }
    };

    TMinMaxBoolPrVec types;
    doReduce(frame.readRows(numberThreads, 0, frame.numberRows(), readDataTypes, &rowMask),
             copyDataTypes, reduceDataTypes, types);

    TDataTypeVec result(types.size());
    for (auto i : columnMask) {
        result[i] = SDataType{types[i].second, types[i].first.min(),
                              types[i].first.max()};
    }

    return result;
}

std::pair<CDataFrameUtils::TFastQuantileSketchVec, bool>
CDataFrameUtils::columnQuantiles(std::size_t numberThreads,
                                 const core::CDataFrame& frame,
                                 const core::CPackedBitVector& rowMask,
                                 const TSizeVec& columnMask,
                                 common::CFastQuantileSketch quantileEstimator,
                                 const CDataFrameCategoryEncoder* encoder,
                                 const TWeightFunc& weight) {

    auto readQuantiles = core::bindRetrievableState(
        [&](TFastQuantileSketchVec& quantiles, const TRowItr& beginRows, const TRowItr& endRows) {
            if (encoder != nullptr) {
                for (auto row = beginRows; row != endRows; ++row) {
                    CEncodedDataFrameRowRef encodedRow{encoder->encode(*row)};
                    for (std::size_t i = 0; i < columnMask.size(); ++i) {
                        if (isMissing(encodedRow[columnMask[i]]) == false) {
                            quantiles[i].add(encodedRow[columnMask[i]], weight(*row));
                        }
                    }
                }
            } else {
                for (auto row = beginRows; row != endRows; ++row) {
                    for (std::size_t i = 0; i < columnMask.size(); ++i) {
                        if (isMissing((*row)[columnMask[i]]) == false) {
                            quantiles[i].add((*row)[columnMask[i]], weight(*row));
                        }
                    }
                }
            }
        },
        TFastQuantileSketchVec(columnMask.size(), quantileEstimator));
    auto copyQuantiles = [](TFastQuantileSketchVec quantiles, TFastQuantileSketchVec& result) {
        result = std::move(quantiles);
    };
    auto reduceQuantiles = [&](TFastQuantileSketchVec quantiles,
                               TFastQuantileSketchVec& result) {
        for (std::size_t i = 0; i < columnMask.size(); ++i) {
            result[i] += quantiles[i];
        }
    };

    TFastQuantileSketchVec result;
    if (doReduce(frame.readRows(numberThreads, 0, frame.numberRows(), readQuantiles, &rowMask),
                 copyQuantiles, reduceQuantiles, result) == false) {
        LOG_ERROR(<< "Failed to compute column quantiles");
        return {std::move(result), false};
    }

    return {std::move(result), true};
}

std::tuple<TPackedBitVectorVec, TPackedBitVectorVec, TDoubleVec>
CDataFrameUtils::stratifiedCrossValidationRowMasks(std::size_t numberThreads,
                                                   const core::CDataFrame& frame,
                                                   std::size_t targetColumn,
                                                   common::CPRNG::CXorOShiro128Plus rng,
                                                   std::size_t numberFolds,
                                                   double trainFractionPerFold,
                                                   std::size_t numberBuckets,
                                                   const core::CPackedBitVector& allTrainingRowMask) {

    double numberTrainingRows{allTrainingRowMask.manhattan()};
    if (static_cast<std::size_t>(numberTrainingRows) < numberFolds) {
        HANDLE_FATAL(<< "Input error: insufficient training data provided.");
        return {{}, {}, {}};
    }

    double sampleFraction{std::min(trainFractionPerFold, 1.0 - trainFractionPerFold)};
    double excessSampleFraction{
        std::max(sampleFraction - 1.0 / static_cast<double>(numberFolds), 0.0)};

    // We sample the smaller of the test or train set in the loop.
    std::size_t excessSampleSize{static_cast<std::size_t>(
        std::ceil(excessSampleFraction * numberTrainingRows))};
    std::size_t sampleSize{static_cast<std::size_t>(std::max(
        (1.0 + 1e-8) * (sampleFraction - excessSampleFraction) * numberTrainingRows, 1.0))};
    LOG_TRACE(<< "excess sample size = " << excessSampleSize
              << ", sample size = " << sampleSize);

    TDoubleVec frequencies;
    auto makeSampler = [&](std::size_t size, const core::CPackedBitVector& rowMask) {
        TStratifiedSamplerUPtr result;
        if (size > 0) {
            if (frame.columnIsCategorical()[targetColumn]) {
                std::tie(result, frequencies) = classifierStratifiedCrossValidationRowSampler(
                    numberThreads, frame, targetColumn, rng, size, rowMask);
            } else {
                result = regressionStratifiedCrossValidationRowSampler(
                    numberThreads, frame, targetColumn, rng, size, numberBuckets, rowMask);
            }
        }
        return result;
    };

    auto excessSampler = makeSampler(excessSampleSize, allTrainingRowMask);

    LOG_TRACE(<< "number training rows = " << allTrainingRowMask.manhattan());

    TPackedBitVectorVec testingRowMasks(numberFolds);

    TSizeVec rowIndices;
    auto sample = [&](const TStratifiedSamplerUPtr& sampler_,
                      const core::CPackedBitVector& rowMask) {
        frame.readRows(1, 0, frame.numberRows(),
                       [&](const TRowItr& beginRows, const TRowItr& endRows) {
                           for (auto row = beginRows; row != endRows; ++row) {
                               sampler_->sample(*row);
                           }
                       },
                       &rowMask);
        sampler_->finishSampling(rng, rowIndices);
        std::sort(rowIndices.begin(), rowIndices.end());
        LOG_TRACE(<< "# row indices = " << rowIndices.size());

        core::CPackedBitVector result;
        for (auto row : rowIndices) {
            result.extend(false, row - result.size());
            result.extend(true);
        }
        result.extend(false, rowMask.size() - result.size());
        return result;
    };

    core::CPackedBitVector candidateTestingRowMask{allTrainingRowMask};
    for (auto& testingRowMask : testingRowMasks) {
        if (static_cast<std::size_t>(candidateTestingRowMask.manhattan()) <= sampleSize) {
            testingRowMask = std::move(candidateTestingRowMask);
            candidateTestingRowMask = core::CPackedBitVector{testingRowMask.size(), false};
        } else {
            auto sampler = makeSampler(sampleSize, candidateTestingRowMask);
            if (sampler == nullptr) {
                HANDLE_FATAL(<< "Internal error: failed to create train/test splits.");
                return {{}, {}, {}};
            }
            testingRowMask = sample(sampler, candidateTestingRowMask);
            candidateTestingRowMask ^= testingRowMask;
        }
        if (excessSampler != nullptr) {
            testingRowMask |= sample(excessSampler, allTrainingRowMask ^ testingRowMask);
        }
    }

    TPackedBitVectorVec trainingRowMasks{complementRowMasks(testingRowMasks, allTrainingRowMask)};

    if (trainFractionPerFold < 0.5) {
        std::swap(trainingRowMasks, testingRowMasks);
    }

    return {std::move(trainingRowMasks), std::move(testingRowMasks), std::move(frequencies)};
}

core::CPackedBitVector
CDataFrameUtils::stratifiedSamplingRowMask(std::size_t numberThreads,
                                           const core::CDataFrame& frame,
                                           std::size_t targetColumn,
                                           common::CPRNG::CXorOShiro128Plus rng,
                                           std::size_t desiredNumberSamples,
                                           std::size_t numberBuckets,
                                           const core::CPackedBitVector& allTrainingRowMask) {
    TDoubleVec frequencies;
    TStratifiedSamplerUPtr sampler;
    core::CPackedBitVector samplesRowMask;

    double numberTrainingRows{allTrainingRowMask.manhattan()};
    if (numberTrainingRows < 2.0) {
        HANDLE_FATAL(<< "Input error: insufficient training data provided.");
        return {};
    }

    if (frame.columnIsCategorical()[targetColumn]) {
        std::tie(sampler, frequencies) = classifierStratifiedCrossValidationRowSampler(
            numberThreads, frame, targetColumn, rng, desiredNumberSamples, allTrainingRowMask);
    } else {
        sampler = regressionStratifiedCrossValidationRowSampler(
            numberThreads, frame, targetColumn, rng, desiredNumberSamples,
            numberBuckets, allTrainingRowMask);
    }

    LOG_TRACE(<< "number training rows = " << allTrainingRowMask.manhattan());

    TSizeVec rowIndices;
    core::CPackedBitVector candidateSamplesRowMask{allTrainingRowMask};
    frame.readRows(1, 0, frame.numberRows(),
                   [&](const TRowItr& beginRows, const TRowItr& endRows) {
                       for (auto row = beginRows; row != endRows; ++row) {
                           sampler->sample(*row);
                       }
                   },
                   &candidateSamplesRowMask);
    sampler->finishSampling(rng, rowIndices);
    std::sort(rowIndices.begin(), rowIndices.end());
    LOG_TRACE(<< "# row indices = " << rowIndices.size());

    for (auto row : rowIndices) {
        samplesRowMask.extend(false, row - samplesRowMask.size());
        samplesRowMask.extend(true);
    }
    samplesRowMask.extend(false, allTrainingRowMask.size() - samplesRowMask.size());

    // We exclusive or here to remove the rows we've selected for the current
    //test fold. This is equivalent to samplng without replacement
    candidateSamplesRowMask ^= samplesRowMask;

    LOG_TRACE(<< "# selected rows = " << samplesRowMask.manhattan());
    return samplesRowMask;
}

core::CPackedBitVector CDataFrameUtils::distributionPreservingSamplingRowMask(
    std::size_t numberThreads,
    const core::CDataFrame& frame,
    std::size_t targetColumn,
    common::CPRNG::CXorOShiro128Plus rng,
    std::size_t desiredNumberSamples,
    std::size_t numberBuckets,
    const core::CPackedBitVector& distributionSourceRowMask,
    const core::CPackedBitVector& allTrainingRowMask) {
    TStratifiedSamplerUPtr sampler;
    core::CPackedBitVector samplesRowMask;

    double numberTrainingRows{allTrainingRowMask.manhattan()};
    if (numberTrainingRows < 2.0) {
        HANDLE_FATAL(<< "Input error: insufficient training data provided.");
        return {};
    }

    if (frame.columnIsCategorical()[targetColumn]) {
        sampler = classifierDistributionPreservingRowSampler(
            numberThreads, frame, targetColumn, rng, distributionSourceRowMask);
    } else {
        sampler = regressionStratifiedCrossValidationRowSampler(
            numberThreads, frame, targetColumn, rng, desiredNumberSamples,
            numberBuckets, distributionSourceRowMask);
    }

    LOG_TRACE(<< "number training rows = " << allTrainingRowMask.manhattan());

    TSizeVec rowIndices;
    frame.readRows(1, 0, frame.numberRows(),
                   [&](const TRowItr& beginRows, const TRowItr& endRows) {
                       for (auto row = beginRows; row != endRows; ++row) {
                           sampler->sample(*row);
                       }
                   },
                   &allTrainingRowMask);
    sampler->finishSampling(rng, rowIndices);
    std::sort(rowIndices.begin(), rowIndices.end());
    LOG_TRACE(<< "# row indices = " << rowIndices.size());

    for (auto row : rowIndices) {
        samplesRowMask.extend(false, row - samplesRowMask.size());
        samplesRowMask.extend(true);
    }
    samplesRowMask.extend(false, allTrainingRowMask.size() - samplesRowMask.size());

    LOG_TRACE(<< "# selected rows = " << samplesRowMask.manhattan());
    return samplesRowMask;
}

CDataFrameUtils::TDoubleVecVec
CDataFrameUtils::categoryFrequencies(std::size_t numberThreads,
                                     const core::CDataFrame& frame,
                                     const core::CPackedBitVector& rowMask,
                                     TSizeVec columnMask) {
    TDoubleVecVec result{CDataFrameUtils::categoryCounts(numberThreads, frame, rowMask,
                                                         std::move(columnMask))};

    double Z{rowMask.manhattan()};
    for (auto& i : result) {
        for (double& j : i) {
            j /= Z;
        }
    }

    return result;
}

CDataFrameUtils::TDoubleVecVec
CDataFrameUtils::categoryCounts(std::size_t numberThreads,
                                const core::CDataFrame& frame,
                                const core::CPackedBitVector& rowMask,
                                TSizeVec columnMask) {
    removeMetricColumns(frame, columnMask);
    if (frame.numberRows() == 0 || columnMask.empty()) {
        return TDoubleVecVec(frame.numberColumns());
    }

    // Note this can throw a length_error in resize hence the try block around read.
    auto readCategoryCounts = core::bindRetrievableState(
        [&](TDoubleVecVec& counts, const TRowItr& beginRows, const TRowItr& endRows) {
            for (auto row = beginRows; row != endRows; ++row) {
                for (std::size_t i : columnMask) {
                    if (isMissing((*row)[i]) == false) {
                        std::size_t category{static_cast<std::size_t>((*row)[i])};
                        counts[i].resize(std::max(counts[i].size(), category + 1), 0.0);
                        counts[i][category] += 1.0;
                    }
                }
            }
        },
        TDoubleVecVec(frame.numberColumns()));
    auto copyCategoryCounts = [](TDoubleVecVec counts, TDoubleVecVec& result) {
        result = std::move(counts);
    };
    auto reduceCategoryCounts = [](TDoubleVecVec counts, TDoubleVecVec& result) {
        for (std::size_t i = 0; i < counts.size(); ++i) {
            result[i].resize(std::max(result[i].size(), counts[i].size()), 0.0);
            for (std::size_t j = 0; j < counts[i].size(); ++j) {
                result[i][j] += counts[i][j];
            }
        }
    };

    TDoubleVecVec result;
    try {
        doReduce(frame.readRows(numberThreads, 0, frame.numberRows(),
                                readCategoryCounts, &rowMask),
                 copyCategoryCounts, reduceCategoryCounts, result);
    } catch (const std::exception& e) {
        HANDLE_FATAL(<< "Internal error: '" << e.what() << "' exception calculating"
                     << " category frequencies. Please report this problem.");
    }
    return result;
}

CDataFrameUtils::TDoubleVecVec
CDataFrameUtils::meanValueOfTargetForCategories(const CColumnValue& target,
                                                std::size_t numberThreads,
                                                const core::CDataFrame& frame,
                                                const core::CPackedBitVector& rowMask,
                                                TSizeVec columnMask) {

    TDoubleVecVec result(frame.numberColumns());

    removeMetricColumns(frame, columnMask);
    if (frame.numberRows() == 0 || columnMask.empty()) {
        return result;
    }

    using TMeanAccumulatorVec =
        std::vector<common::CBasicStatistics::SSampleMean<double>::TAccumulator>;
    using TMeanAccumulatorVecVec = std::vector<TMeanAccumulatorVec>;

    // Note this can throw a length_error in resize hence the try block around read.
    auto readColumnMeans = core::bindRetrievableState(
        [&](TMeanAccumulatorVecVec& means_, const TRowItr& beginRows, const TRowItr& endRows) {
            for (auto row = beginRows; row != endRows; ++row) {
                for (std::size_t i : columnMask) {
                    if (isMissing((*row)[i]) == false && isMissing(target(*row)) == false) {
                        std::size_t category{static_cast<std::size_t>((*row)[i])};
                        means_[i].resize(std::max(means_[i].size(), category + 1));
                        means_[i][category].add(target(*row));
                    }
                }
            }
        },
        TMeanAccumulatorVecVec(frame.numberColumns()));
    auto copyColumnMeans = [](TMeanAccumulatorVecVec means_, TMeanAccumulatorVecVec& means) {
        means = std::move(means_);
    };
    auto reduceColumnMeans = [](TMeanAccumulatorVecVec means_, TMeanAccumulatorVecVec& means) {
        for (std::size_t i = 0; i < means_.size(); ++i) {
            means[i].resize(std::max(means[i].size(), means_[i].size()));
            for (std::size_t j = 0; j < means_[i].size(); ++j) {
                means[i][j] += means_[i][j];
            }
        }
    };

    TMeanAccumulatorVecVec means;
    try {
        doReduce(frame.readRows(numberThreads, 0, frame.numberRows(), readColumnMeans, &rowMask),
                 copyColumnMeans, reduceColumnMeans, means);
    } catch (const std::exception& e) {
        HANDLE_FATAL(<< "Internal error: '" << e.what() << "' exception calculating"
                     << " mean target values for categories. Please report this problem.");
        return result;
    }
    for (std::size_t i = 0; i < result.size(); ++i) {
        result[i].resize(means[i].size());
        for (std::size_t j = 0; j < means[i].size(); ++j) {
            result[i][j] = common::CBasicStatistics::mean(means[i][j]);
        }
    }

    return result;
}

CDataFrameUtils::TSizeDoublePrVecVecVec
CDataFrameUtils::categoricalMicWithColumn(const CColumnValue& target,
                                          std::size_t numberThreads,
                                          const core::CDataFrame& frame,
                                          const core::CPackedBitVector& rowMask,
                                          TSizeVec columnMask,
                                          const TEncoderFactoryVec& encoderFactories) {

    TSizeDoublePrVecVecVec none(encoderFactories.size(),
                                TSizeDoublePrVecVec(frame.numberColumns()));

    removeMetricColumns(frame, columnMask);
    if (frame.numberRows() == 0 || columnMask.empty()) {
        return none;
    }

    auto method = frame.inMainMemory() ? categoricalMicWithColumnDataFrameInMemory
                                       : categoricalMicWithColumnDataFrameOnDisk;

    TDoubleVecVec frequencies(categoryFrequencies(numberThreads, frame, rowMask, columnMask));
    LOG_TRACE(<< "frequencies = " << frequencies);

    TSizeDoublePrVecVecVec mics(
        method(target, frame, rowMask, columnMask, encoderFactories, frequencies,
               std::min(NUMBER_SAMPLES_TO_COMPUTE_MIC, frame.numberRows())));

    for (auto& encoderMics : mics) {
        for (auto& categoryMics : encoderMics) {
            std::sort(categoryMics.begin(), categoryMics.end(),
                      [](const TSizeDoublePr& lhs, const TSizeDoublePr& rhs) {
                          return common::COrderings::lexicographical_compare(
                              -lhs.second, lhs.first, -rhs.second, rhs.first);
                      });
        }
    }

    return mics;
}

CDataFrameUtils::TDoubleVec
CDataFrameUtils::metricMicWithColumn(const CColumnValue& target,
                                     const core::CDataFrame& frame,
                                     const core::CPackedBitVector& rowMask,
                                     TSizeVec columnMask) {

    TDoubleVec zeros(frame.numberColumns(), 0.0);

    removeCategoricalColumns(frame, columnMask);
    if (frame.numberRows() == 0 || columnMask.empty()) {
        return zeros;
    }

    auto method = frame.inMainMemory() ? metricMicWithColumnDataFrameInMemory
                                       : metricMicWithColumnDataFrameOnDisk;

    return method(target, frame, rowMask, columnMask,
                  std::min(NUMBER_SAMPLES_TO_COMPUTE_MIC, frame.numberRows()));
}

CDataFrameUtils::TDoubleVector
CDataFrameUtils::maximumMinimumRecallClassWeights(std::size_t numberThreads,
                                                  const core::CDataFrame& frame,
                                                  const core::CPackedBitVector& rowMask,
                                                  std::size_t numberClasses,
                                                  std::size_t targetColumn,
                                                  const TReadPredictionFunc& readPrediction) {

    return numberClasses == 2
               ? maximizeMinimumRecallForBinary(numberThreads, frame, rowMask,
                                                targetColumn, readPrediction)
               : maximizeMinimumRecallForMulticlass(numberThreads, frame, rowMask, numberClasses,
                                                    targetColumn, readPrediction);
}

bool CDataFrameUtils::isMissing(double value) {
    return core::CDataFrame::isMissing(value);
}

CDataFrameUtils::TSizeDoublePrVecVecVec CDataFrameUtils::categoricalMicWithColumnDataFrameInMemory(
    const CColumnValue& target,
    const core::CDataFrame& frame,
    const core::CPackedBitVector& rowMask,
    const TSizeVec& columnMask,
    const TEncoderFactoryVec& encoderFactories,
    const TDoubleVecVec& frequencies,
    std::size_t numberSamples) {

    TSizeDoublePrVecVecVec encoderMics;
    encoderMics.reserve(encoderFactories.size());

    TFloatVecVec samples;
    TSizeEncoderPtrUMap encoders;
    CMic mic;
    samples.reserve(numberSamples);
    mic.reserve(numberSamples);

    for (const auto& encoderFactory : encoderFactories) {

        TEncoderFactory makeEncoder;
        double minimumFrequency;
        std::tie(makeEncoder, minimumFrequency) = encoderFactory;

        TSizeDoublePrVecVec mics(frame.numberColumns());

        for (auto i : columnMask) {

            // Sample

            samples.clear();
            TRowSampler sampler{numberSamples, rowFeatureSampler(i, target, samples)};
            frame.readRows(
                1, 0, frame.numberRows(),
                [&](const TRowItr& beginRows, const TRowItr& endRows) {
                    for (auto row = beginRows; row != endRows; ++row) {
                        if (isMissing((*row)[i]) || isMissing(target(*row))) {
                            continue;
                        }
                        std::size_t category{static_cast<std::size_t>((*row)[i])};
                        if (frequencies[i][category] >= minimumFrequency) {
                            sampler.sample(*row);
                        }
                    }
                },
                &rowMask);
            LOG_TRACE(<< "# samples = " << samples.size());

            // Setup encoders

            encoders.clear();
            for (const auto& sample : samples) {
                std::size_t category{static_cast<std::size_t>(sample[0])};
                auto encoder = makeEncoder(i, 0, category);
                std::size_t hash{encoder->hash()};
                encoders.emplace(hash, std::move(encoder));
            }

            auto target_ = [](const TFloatVec& sample) { return sample[1]; };
            mics[i] = computeEncodedCategory(mic, target_, encoders, samples);
        }

        encoderMics.push_back(std::move(mics));
    }

    return encoderMics;
}

CDataFrameUtils::TSizeDoublePrVecVecVec CDataFrameUtils::categoricalMicWithColumnDataFrameOnDisk(
    const CColumnValue& target,
    const core::CDataFrame& frame,
    const core::CPackedBitVector& rowMask,
    const TSizeVec& columnMask,
    const TEncoderFactoryVec& encoderFactories,
    const TDoubleVecVec& frequencies,
    std::size_t numberSamples) {

    TSizeDoublePrVecVecVec encoderMics;
    encoderMics.reserve(encoderFactories.size());

    TFloatVecVec samples;
    TSizeEncoderPtrUMap encoders;
    CMic mic;
    samples.reserve(numberSamples);
    mic.reserve(numberSamples);

    for (const auto& encoderFactory : encoderFactories) {

        TEncoderFactory makeEncoder;
        double minimumFrequency;
        std::tie(makeEncoder, minimumFrequency) = encoderFactory;

        TSizeDoublePrVecVec mics(frame.numberColumns());

        // Sample
        //
        // The law of large numbers means we have a high probability of sampling
        // each category provided minimumFrequency * NUMBER_SAMPLES_TO_COMPUTE_MIC
        // is large (which we ensure it is).

        samples.clear();
        TRowSampler sampler{numberSamples, rowSampler(samples)};
        frame.readRows(1, 0, frame.numberRows(),
                       [&](const TRowItr& beginRows, const TRowItr& endRows) {
                           for (auto row = beginRows; row != endRows; ++row) {
                               if (isMissing(target(*row)) == false) {
                                   sampler.sample(*row);
                               }
                           }
                       },
                       &rowMask);
        LOG_TRACE(<< "# samples = " << samples.size());

        for (auto i : columnMask) {

            // Setup encoders

            encoders.clear();
            for (const auto& sample : samples) {
                if (isMissing(sample[i])) {
                    continue;
                }
                std::size_t category{static_cast<std::size_t>(sample[i])};
                if (frequencies[i][category] >= minimumFrequency) {
                    auto encoder = makeEncoder(i, i, category);
                    std::size_t hash{encoder->hash()};
                    encoders.emplace(hash, std::move(encoder));
                }
            }

            mics[i] = computeEncodedCategory(mic, target, encoders, samples);
        }

        encoderMics.push_back(std::move(mics));
    }

    return encoderMics;
}

CDataFrameUtils::TDoubleVec
CDataFrameUtils::metricMicWithColumnDataFrameInMemory(const CColumnValue& target,
                                                      const core::CDataFrame& frame,
                                                      const core::CPackedBitVector& rowMask,
                                                      const TSizeVec& columnMask,
                                                      std::size_t numberSamples) {

    TDoubleVec mics(frame.numberColumns(), 0.0);

    TFloatVecVec samples;
    samples.reserve(numberSamples);
    double numberMaskedRows{rowMask.manhattan()};

    for (auto i : columnMask) {

        // Do sampling

        TRowSampler sampler{numberSamples, rowFeatureSampler(i, target, samples)};
        auto missingCount = frame.readRows(
            1, 0, frame.numberRows(),
            core::bindRetrievableState(
                [&](std::size_t& missing, const TRowItr& beginRows, const TRowItr& endRows) {
                    for (auto row = beginRows; row != endRows; ++row) {
                        if (isMissing((*row)[i])) {
                            ++missing;
                        } else if (isMissing(target(*row)) == false) {
                            sampler.sample(*row);
                        }
                    }
                },
                std::size_t{0}),
            &rowMask);
        LOG_TRACE(<< "# samples = " << samples.size());

        double fractionMissing{static_cast<double>(missingCount.first[0].s_FunctionState) /
                               numberMaskedRows};
        LOG_TRACE(<< "feature = " << i << " fraction missing = " << fractionMissing);

        // Compute MICe

        CMic mic;
        mic.reserve(samples.size());
        for (const auto& sample : samples) {
            mic.add(sample[0], sample[1]);
        }

        mics[i] = (1.0 - fractionMissing) * mic.compute();
        samples.clear();
    }

    return mics;
}

CDataFrameUtils::TDoubleVec
CDataFrameUtils::metricMicWithColumnDataFrameOnDisk(const CColumnValue& target,
                                                    const core::CDataFrame& frame,
                                                    const core::CPackedBitVector& rowMask,
                                                    const TSizeVec& columnMask,
                                                    std::size_t numberSamples) {

    TDoubleVec mics(frame.numberColumns(), 0.0);

    TFloatVecVec samples;
    samples.reserve(numberSamples);
    double numberMaskedRows{rowMask.manhattan()};

    // Do sampling

    TRowSampler sampler{numberSamples, rowSampler(samples)};
    auto missingCounts = frame.readRows(
        1, 0, frame.numberRows(),
        core::bindRetrievableState(
            [&](TSizeVec& missing, const TRowItr& beginRows, const TRowItr& endRows) {
                for (auto row = beginRows; row != endRows; ++row) {
                    for (std::size_t i = 0; i < row->numberColumns(); ++i) {
                        missing[i] += isMissing((*row)[i]) ? 1 : 0;
                    }
                    if (isMissing(target(*row)) == false) {
                        sampler.sample(*row);
                    }
                }
            },
            TSizeVec(frame.numberColumns(), 0)),
        &rowMask);
    LOG_TRACE(<< "# samples = " << samples.size());

    TDoubleVec fractionMissing(frame.numberColumns());
    for (std::size_t i = 0; i < fractionMissing.size(); ++i) {
        for (const auto& missingCount : missingCounts.first) {
            fractionMissing[i] +=
                static_cast<double>(missingCount.s_FunctionState[i]) / numberMaskedRows;
        }
    }
    LOG_TRACE(<< "Fraction missing = " << fractionMissing);

    // Compute MICe

    for (auto i : columnMask) {
        CMic mic;
        mic.reserve(samples.size());
        for (const auto& sample : samples) {
            if (isMissing(sample[i]) == false) {
                mic.add(sample[i], target(sample));
            }
        }
        mics[i] = (1.0 - fractionMissing[i]) * mic.compute();
    }

    return mics;
}

CDataFrameUtils::TDoubleVector
CDataFrameUtils::maximizeMinimumRecallForBinary(std::size_t numberThreads,
                                                const core::CDataFrame& frame,
                                                const core::CPackedBitVector& rowMask,
                                                std::size_t targetColumn,
                                                const TReadPredictionFunc& readPrediction) {
    auto readQuantiles = core::bindRetrievableState(
        [&](TQuantileSketchVec& quantiles, const TRowItr& beginRows, const TRowItr& endRows) {
            TDoubleVector probabilities;
            for (auto row = beginRows; row != endRows; ++row) {
                if (isMissing((*row)[targetColumn]) == false) {
                    std::size_t actualClass{static_cast<std::size_t>((*row)[targetColumn])};
                    if (actualClass < quantiles.size()) {
                        probabilities = readPrediction(*row);
                        if (allFinite(probabilities)) {
                            common::CTools::inplaceSoftmax(probabilities);
                            quantiles[actualClass].add(probabilities(1));
                        } else {
                            LOG_WARN(<< "Ignoring unexpected probabilities " << probabilities);
                        }
                    } else {
                        LOG_WARN(<< "Ignoring class " << actualClass << " which is out-of-range. "
                                 << "Should be less than " << quantiles.size() << ". Classes "
                                 << frame.categoricalColumnValues()[targetColumn] << ".");
                    }
                }
            }
        },
        TQuantileSketchVec(2, common::CQuantileSketch{common::CQuantileSketch::E_Linear, 100}));
    auto copyQuantiles = [](TQuantileSketchVec quantiles, TQuantileSketchVec& result) {
        result = std::move(quantiles);
    };
    auto reduceQuantiles = [&](TQuantileSketchVec quantiles, TQuantileSketchVec& result) {
        for (std::size_t i = 0; i < 2; ++i) {
            result[i] += quantiles[i];
        }
    };

    TQuantileSketchVec classProbabilityClassOneQuantiles;
    if (doReduce(frame.readRows(numberThreads, 0, frame.numberRows(), readQuantiles, &rowMask),
                 copyQuantiles, reduceQuantiles, classProbabilityClassOneQuantiles) == false) {
        HANDLE_FATAL(<< "Failed to compute category quantiles");
        return TDoubleVector::Ones(2);
    }

    auto minRecall = [&](double threshold) -> double {
        double cdf[2];
        classProbabilityClassOneQuantiles[0].cdf(threshold, cdf[0]);
        classProbabilityClassOneQuantiles[1].cdf(threshold, cdf[1]);
        return std::min(cdf[0], 1.0 - cdf[1]);
    };

    double threshold;
    double minRecallAtThreshold;
    std::size_t maxIterations{20};
    common::CSolvers::maximize(0.01, 0.99, minRecall(0.01), minRecall(0.99), minRecall,
                               1e-3, maxIterations, threshold, minRecallAtThreshold);
    LOG_TRACE(<< "threshold = " << threshold
              << ", min recall at threshold = " << minRecallAtThreshold);

    TDoubleVector result{2};
    result(0) = threshold < 0.5 ? threshold / (1.0 - threshold) : 1.0;
    result(1) = threshold < 0.5 ? 1.0 : (1.0 - threshold) / threshold;
    return result;
}

CDataFrameUtils::TDoubleVector
CDataFrameUtils::maximizeMinimumRecallForMulticlass(std::size_t numberThreads,
                                                    const core::CDataFrame& frame,
                                                    const core::CPackedBitVector& rowMask,
                                                    std::size_t numberClasses,
                                                    std::size_t targetColumn,
                                                    const TReadPredictionFunc& readPrediction) {

    // Use a large random sample of the data frame and compute the expected
    // optimisation objective for the whole data set from this.

    using TDoubleMatrix = common::CDenseMatrix<double>;
    using TMinAccumulator = common::CBasicStatistics::SMin<double>::TAccumulator;

    common::CPRNG::CXorOShiro128Plus rng;
    std::size_t numberSamples{
        static_cast<std::size_t>(std::min(1000.0, rowMask.manhattan()))};

    core::CPackedBitVector sampleMask;

    // No need to sample if were going to use every row we've been given.
    if (numberSamples < static_cast<std::size_t>(rowMask.manhattan())) {
        TStratifiedSamplerUPtr sampler;
        std::tie(sampler, std::ignore) = classifierStratifiedCrossValidationRowSampler(
            numberThreads, frame, targetColumn, rng, numberSamples, rowMask);

        TSizeVec rowIndices;
        frame.readRows(1, 0, frame.numberRows(),
                       [&](const TRowItr& beginRows, const TRowItr& endRows) {
                           for (auto row = beginRows; row != endRows; ++row) {
                               sampler->sample(*row);
                           }
                       },
                       &rowMask);
        sampler->finishSampling(rng, rowIndices);
        std::sort(rowIndices.begin(), rowIndices.end());
        LOG_TRACE(<< "# row indices = " << rowIndices.size());

        for (auto row : rowIndices) {
            sampleMask.extend(false, row - sampleMask.size());
            sampleMask.extend(true);
        }
        sampleMask.extend(false, rowMask.size() - sampleMask.size());
    } else {
        sampleMask = rowMask;
    }

    // Compute the count of each class in the sample set.
    auto readClassCountsAndRecalls = core::bindRetrievableState(
        [&](TDoubleVector& state, const TRowItr& beginRows, const TRowItr& endRows) {
            for (auto row = beginRows; row != endRows; ++row) {
                if (isMissing((*row)[targetColumn]) == false) {
                    int j{static_cast<int>((*row)[targetColumn])};
                    int k;
                    readPrediction(*row).maxCoeff(&k);
                    state(j) += 1.0;
                    state(numberClasses + j) += j == k ? 1.0 : 0.0;
                }
            }
        },
        TDoubleVector{TDoubleVector::Zero(2 * numberClasses)});
    auto copyClassCountsAndRecalls = [](TDoubleVector state, TDoubleVector& result) {
        result = std::move(state);
    };
    auto reduceClassCountsAndRecalls =
        [&](TDoubleVector state, TDoubleVector& result) { result += state; };
    TDoubleVector classCountsAndRecalls;
    doReduce(frame.readRows(numberThreads, 0, frame.numberRows(),
                            readClassCountsAndRecalls, &sampleMask),
             copyClassCountsAndRecalls, reduceClassCountsAndRecalls, classCountsAndRecalls);
    TDoubleVector classCounts{classCountsAndRecalls.topRows(numberClasses)};
    TDoubleVector classRecalls{classCountsAndRecalls.bottomRows(numberClasses)};
    // If a class count is zero the derivative of the loss functin w.r.t. that
    // class is not well defined. The objective is independent of such a class
    // so the choice for its count and recall is not important. However, its
    // count must be non-zero so that we don't run into a NaN cascade.
    classCounts = classCounts.cwiseMax(1.0);
    classRecalls.array() /= classCounts.array();
    LOG_TRACE(<< "class counts = " << classCounts.transpose());
    LOG_TRACE(<< "class recalls = " << classRecalls.transpose());

    TSizeVec recallOrder(numberClasses);
    std::iota(recallOrder.begin(), recallOrder.end(), 0);
    std::sort(recallOrder.begin(), recallOrder.end(), [&](std::size_t lhs, std::size_t rhs) {
        return classRecalls(lhs) > classRecalls(rhs);
    });
    LOG_TRACE(<< "decreasing recall order = " << recallOrder);

    // We want to solve max_w{min_j{recall(class_j)}} = max_w{min_j{c_j(w) / n_j}}
    // where c_j(w) and n_j are correct predictions for weight w and count of class_j
    // in the sample set, respectively. We use an equivalent formulation
    //
    //   min_w{max_j{f_j(w)}} = min_w{max_j{1 - c_j(w) / n_j}}
    //
    // We can write f_j(w) as
    //
    //    max_j{sum_i{1 - 1{argmax_i(w_i p_i) == j}} / n_j}                     (1)
    //
    // where 1{.} denotes the indicator function. (1) has a smooth relaxation given
    // by f_j(w) = max_j{sum_i{1 - softmax_j(w_i p_i)} / n_j}. Note that this isn't
    // convex so we use multiple restarts.

    auto objective = [&](const TDoubleVector& weights) -> double {
        TDoubleVector probabilities;
        TDoubleVector scores;
        auto computeObjective = core::bindRetrievableState(
            [=](TDoubleVector& state, const TRowItr& beginRows, const TRowItr& endRows) mutable {
                for (auto row = beginRows; row != endRows; ++row) {
                    if (isMissing((*row)[targetColumn]) == false) {
                        std::size_t j{static_cast<std::size_t>((*row)[targetColumn])};
                        probabilities = readPrediction(*row);
                        if (allFinite(probabilities)) {
                            common::CTools::inplaceSoftmax(probabilities);
                            scores = probabilities.cwiseProduct(weights);
                            common::CTools::inplaceSoftmax(scores);
                            state(j) += (1.0 - scores(j)) / classCounts(j);
                        } else {
                            LOG_WARN(<< "Ignoring unexpected probabilities " << probabilities);
                        }
                    }
                }
            },
            TDoubleVector{TDoubleVector::Zero(numberClasses)});
        auto copyObjective = [](TDoubleVector state, TDoubleVector& result) {
            result = std::move(state);
        };
        auto reduceObjective = [&](TDoubleVector state, TDoubleVector& result) {
            result += state;
        };

        TDoubleVector objective_;
        doReduce(frame.readRows(numberThreads, 0, frame.numberRows(),
                                computeObjective, &sampleMask),
                 copyObjective, reduceObjective, objective_);

        return objective_.maxCoeff();
    };

    auto objectiveGradient = [&](const TDoubleVector& weights) -> TDoubleVector {
        TDoubleVector probabilities;
        TDoubleVector scores;
        auto computeObjectiveAndGradient = core::bindRetrievableState(
            [=](TDoubleMatrix& state, const TRowItr& beginRows, const TRowItr& endRows) mutable {
                for (auto row = beginRows; row != endRows; ++row) {
                    if (isMissing((*row)[targetColumn]) == false) {
                        std::size_t j{static_cast<std::size_t>((*row)[targetColumn])};
                        probabilities = readPrediction(*row);
                        if (allFinite(probabilities)) {
                            common::CTools::inplaceSoftmax(probabilities);
                            scores = probabilities.cwiseProduct(weights);
                            common::CTools::inplaceSoftmax(scores);
                            state(j, 0) += (1.0 - scores(j)) / classCounts(j);
                            state.col(j + 1) +=
                                scores(j) *
                                probabilities
                                    .cwiseProduct(scores - TDoubleVector::Unit(numberClasses, j))
                                    .cwiseQuotient(classCounts);
                        } else {
                            LOG_WARN(<< "Ignoring unexpected probabilities " << probabilities);
                        }
                    }
                }
            },
            TDoubleMatrix{TDoubleMatrix::Zero(numberClasses, numberClasses + 1)});
        auto copyObjectiveAndGradient = [](TDoubleMatrix state, TDoubleMatrix& result) {
            result = std::move(state);
        };
        auto reduceObjectiveAndGradient =
            [&](TDoubleMatrix state, TDoubleMatrix& result) { result += state; };

        TDoubleMatrix objectiveAndGradient;
        doReduce(frame.readRows(numberThreads, 0, frame.numberRows(),
                                computeObjectiveAndGradient, &sampleMask),
                 copyObjectiveAndGradient, reduceObjectiveAndGradient, objectiveAndGradient);
        std::size_t max;
        objectiveAndGradient.col(0).maxCoeff(&max);

        return objectiveAndGradient.col(max + 1);
    };

    // We always try initialising with all weights equal because our minimization
    // algorithm ensures we'll only decrease the initial value of the optimisation
    // objective. This means we'll never do worse than not reweighting. Also, we
    // expect weights to be (roughly) monotonic increasing for decreasing recall
    // and use this to bias initial values.

    TMinAccumulator minLoss;
    TDoubleVector minLossWeights;
    TDoubleVector w0{TDoubleVector::Ones(numberClasses)};
    for (std::size_t i = 0; i < 5; ++i) {
        common::CLbfgs<TDoubleVector> lbfgs{5};
        double loss;
        std::tie(w0, loss) = lbfgs.minimize(objective, objectiveGradient, std::move(w0));
        LOG_TRACE(<< "weights* = " << w0.transpose() << ", loss* = " << loss);
        if (minLoss.add(loss)) {
            minLossWeights = std::move(w0);
        }
        w0 = TDoubleVector::Ones(numberClasses);
        for (std::size_t j = 1; j < numberClasses; ++j) {
            w0(j) = w0(j - 1) * common::CSampling::uniformSample(rng, 0.9, 1.3);
        }
    }

    // Since we take argmax_i w_i p_i we can multiply by a constant. We arrange for
    // the largest weight to always be one.
    minLossWeights.array() /= minLossWeights.maxCoeff();
    LOG_TRACE(<< "weights = " << minLossWeights.transpose());

    return minLossWeights;
}

void CDataFrameUtils::removeMetricColumns(const core::CDataFrame& frame, TSizeVec& columnMask) {
    const auto& columnIsCategorical = frame.columnIsCategorical();
    columnMask.erase(std::remove_if(columnMask.begin(), columnMask.end(),
                                    [&columnIsCategorical](std::size_t i) {
                                        return columnIsCategorical[i] == false;
                                    }),
                     columnMask.end());
}

void CDataFrameUtils::removeCategoricalColumns(const core::CDataFrame& frame,
                                               TSizeVec& columnMask) {
    const auto& columnIsCategorical = frame.columnIsCategorical();
    columnMask.erase(std::remove_if(columnMask.begin(), columnMask.end(),
                                    [&columnIsCategorical](std::size_t i) {
                                        return columnIsCategorical[i];
                                    }),
                     columnMask.end());
}

double CDataFrameUtils::unitWeight(const TRowRef&) {
    return 1.0;
}
}
}
}
