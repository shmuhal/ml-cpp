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

#include <core/CContainerPrinter.h>
#include <core/CJsonOutputStreamWrapper.h>
#include <core/CProgramCounters.h>
#include <core/CStringUtils.h>
#include <core/CVectorRange.h>

#include <maths/analytics/CDataFrameUtils.h>
#include <maths/analytics/COutliers.h>

#include <api/CDataFrameAnalysisSpecification.h>
#include <api/CDataFrameAnalyzer.h>

#include <test/BoostTestCloseAbsolute.h>
#include <test/CDataFrameAnalysisSpecificationFactory.h>
#include <test/CRandomNumbers.h>

#include <rapidjson/prettywriter.h>

#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>
#include <vector>

using TDoubleVec = std::vector<double>;
using TDoubleVecVec = std::vector<TDoubleVec>;
BOOST_TEST_DONT_PRINT_LOG_VALUE(TDoubleVec::iterator)
BOOST_TEST_DONT_PRINT_LOG_VALUE(TDoubleVecVec::iterator)

BOOST_AUTO_TEST_SUITE(CDataFrameAnalyzerOutlierTest)

using namespace ml;

namespace {
using TStrVec = std::vector<std::string>;
using TRowItr = core::CDataFrame::TRowItr;
using TDataFrameUPtrTemporaryDirectoryPtrPr =
    test::CDataFrameAnalysisSpecificationFactory::TDataFrameUPtrTemporaryDirectoryPtrPr;

void addOutlierTestData(TStrVec fieldNames,
                        TStrVec fieldValues,
                        api::CDataFrameAnalyzer& analyzer,
                        TDoubleVec& expectedScores,
                        TDoubleVecVec& expectedFeatureInfluences,
                        std::size_t numberInliers = 100,
                        std::size_t numberOutliers = 10,
                        maths::analytics::COutliers::EMethod method = maths::analytics::COutliers::E_Ensemble,
                        std::size_t numberNeighbours = 0,
                        bool computeFeatureInfluence = false) {

    test::CRandomNumbers rng;

    TDoubleVec mean{1.0, 10.0, 4.0, 8.0, 3.0};
    TDoubleVecVec covariance{{1.0, 0.1, -0.1, 0.3, 0.2},
                             {0.1, 1.3, -0.3, 0.1, 0.1},
                             {-0.1, -0.3, 2.1, 0.1, 0.2},
                             {0.3, 0.1, 0.1, 0.8, 0.2},
                             {0.2, 0.1, 0.2, 0.2, 2.2}};

    TDoubleVecVec inliers;
    rng.generateMultivariateNormalSamples(mean, covariance, numberInliers, inliers);

    TDoubleVec outliers;
    rng.generateUniformSamples(0.0, 10.0, numberOutliers * 5, outliers);

    auto frame = core::makeMainStorageDataFrame(5).first;

    for (std::size_t i = 0; i < inliers.size(); ++i) {
        for (std::size_t j = 0; j < 5; ++j) {
            fieldValues[j] = core::CStringUtils::typeToStringPrecise(
                inliers[i][j], core::CIEEE754::E_DoublePrecision);
        }
        analyzer.handleRecord(fieldNames, fieldValues);
        frame->parseAndWriteRow(core::make_const_range(fieldValues, 0, 5));
    }
    for (std::size_t i = 0; i < outliers.size(); i += 5) {
        for (std::size_t j = 0; j < 5; ++j) {
            fieldValues[j] = core::CStringUtils::typeToStringPrecise(
                outliers[i + j], core::CIEEE754::E_DoublePrecision);
        }
        analyzer.handleRecord(fieldNames, fieldValues);
        frame->parseAndWriteRow(core::make_const_range(fieldValues, 0, 5));
    }

    frame->finishWritingRows();
    maths::analytics::CDataFrameOutliersInstrumentationStub instrumentation;
    maths::analytics::COutliers::compute(
        {1, 1, true, method, numberNeighbours, computeFeatureInfluence, 0.05},
        *frame, instrumentation);

    expectedScores.resize(numberInliers + numberOutliers);
    expectedFeatureInfluences.resize(numberInliers + numberOutliers, TDoubleVec(5));

    frame->readRows(1, [&](const TRowItr& beginRows, const TRowItr& endRows) {
        for (auto row = beginRows; row != endRows; ++row) {
            expectedScores[row->index()] = (*row)[5];
            if (computeFeatureInfluence) {
                for (std::size_t i = 6; i < 11; ++i) {
                    expectedFeatureInfluences[row->index()][i - 6] = (*row)[i];
                }
            }
        }
    });
}
}

BOOST_AUTO_TEST_CASE(testWithoutControlMessages) {

    std::stringstream output;
    auto outputWriterFactory = [&output]() {
        return std::make_unique<core::CJsonOutputStreamWrapper>(output);
    };

    TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
    auto spec = test::CDataFrameAnalysisSpecificationFactory{}.outlierSpec(&frameAndDirectory);
    api::CDataFrameAnalyzer analyzer{std::move(spec), std::move(frameAndDirectory),
                                     std::move(outputWriterFactory)};

    TDoubleVec expectedScores;
    TDoubleVecVec expectedFeatureInfluences;

    TStrVec fieldNames{"c1", "c2", "c3", "c4", "c5"};
    TStrVec fieldValues{"", "", "", "", ""};
    addOutlierTestData(fieldNames, fieldValues, analyzer, expectedScores,
                       expectedFeatureInfluences);

    analyzer.receivedAllRows();
    analyzer.run();

    rapidjson::Document results;
    rapidjson::ParseResult ok(results.Parse(output.str()));
    BOOST_TEST_REQUIRE(static_cast<bool>(ok) == true);

    auto expectedScore = expectedScores.begin();
    for (const auto& result : results.GetArray()) {
        if (result.HasMember("row_results")) {
            BOOST_TEST_REQUIRE(expectedScore != expectedScores.end());
            BOOST_REQUIRE_CLOSE_ABSOLUTE(
                *expectedScore,
                result["row_results"]["results"]["ml"]["outlier_score"].GetDouble(),
                1e-4 * *expectedScore);
            BOOST_TEST_REQUIRE(result.HasMember("phase_progress") == false);
            ++expectedScore;
        } else if (result.HasMember("phase_progress")) {
            BOOST_TEST_REQUIRE(result["phase_progress"]["progress_percent"].GetInt() >= 0);
            BOOST_TEST_REQUIRE(result["phase_progress"]["progress_percent"].GetInt() <= 100);
            BOOST_TEST_REQUIRE(result.HasMember("row_results") == false);
        }
    }
    BOOST_TEST_REQUIRE(expectedScore == expectedScores.end());
}

BOOST_AUTO_TEST_CASE(testRunOutlierDetection) {

    // Test the results the analyzer produces match running outlier detection
    // directly.

    std::stringstream output;
    auto outputWriterFactory = [&output]() {
        return std::make_unique<core::CJsonOutputStreamWrapper>(output);
    };

    TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
    auto spec = test::CDataFrameAnalysisSpecificationFactory{}.outlierSpec(&frameAndDirectory);
    api::CDataFrameAnalyzer analyzer{std::move(spec), std::move(frameAndDirectory),
                                     std::move(outputWriterFactory)};

    TDoubleVec expectedScores;
    TDoubleVecVec expectedFeatureInfluences;

    TStrVec fieldNames{"c1", "c2", "c3", "c4", "c5", ".", "."};
    TStrVec fieldValues{"", "", "", "", "", "0", ""};
    addOutlierTestData(fieldNames, fieldValues, analyzer, expectedScores,
                       expectedFeatureInfluences);
    analyzer.handleRecord(fieldNames, {"", "", "", "", "", "", "$"});

    rapidjson::Document results;
    rapidjson::ParseResult ok(results.Parse(output.str()));
    BOOST_TEST_REQUIRE(static_cast<bool>(ok) == true);

    auto expectedScore = expectedScores.begin();
    bool progressCompleted{false};
    for (const auto& result : results.GetArray()) {
        if (result.HasMember("row_results")) {
            BOOST_TEST_REQUIRE(expectedScore != expectedScores.end());
            BOOST_REQUIRE_CLOSE_ABSOLUTE(
                *expectedScore,
                result["row_results"]["results"]["ml"]["outlier_score"].GetDouble(),
                1e-4 * *expectedScore);
            ++expectedScore;
            BOOST_TEST_REQUIRE(result.HasMember("phase_progress") == false);
        } else if (result.HasMember("phase_progress")) {
            BOOST_TEST_REQUIRE(result["phase_progress"]["progress_percent"].GetInt() >= 0);
            BOOST_TEST_REQUIRE(result["phase_progress"]["progress_percent"].GetInt() <= 100);
            BOOST_TEST_REQUIRE(result.HasMember("row_results") == false);
            progressCompleted = result["phase_progress"]["progress_percent"].GetInt() == 100;
        }
    }
    BOOST_TEST_REQUIRE(expectedScore == expectedScores.end());
    BOOST_TEST_REQUIRE(progressCompleted);

    LOG_DEBUG(<< "number partitions = "
              << core::CProgramCounters::counter(counter_t::E_DFONumberPartitions));
    LOG_DEBUG(<< "estimated memory usage = "
              << core::CProgramCounters::counter(counter_t::E_DFOEstimatedPeakMemoryUsage));
    LOG_DEBUG(<< "peak memory = "
              << core::CProgramCounters::counter(counter_t::E_DFOPeakMemoryUsage));

    BOOST_TEST_REQUIRE(core::CProgramCounters::counter(counter_t::E_DFONumberPartitions) == 1);
    BOOST_TEST_REQUIRE(core::CProgramCounters::counter(counter_t::E_DFOPeakMemoryUsage) < 100000);
    // Allow a 20% margin
    BOOST_TEST_REQUIRE(
        core::CProgramCounters::counter(counter_t::E_DFOPeakMemoryUsage) <
        (120 * core::CProgramCounters::counter(counter_t::E_DFOEstimatedPeakMemoryUsage)) / 100);
}

BOOST_AUTO_TEST_CASE(testRunOutlierDetectionPartitioned) {

    // Test the case we have to overflow to disk to compute outliers subject
    // to the memory constraints.

    std::stringstream output;
    auto outputWriterFactory = [&output]() {
        return std::make_unique<core::CJsonOutputStreamWrapper>(output);
    };

    TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
    auto spec = test::CDataFrameAnalysisSpecificationFactory{}
                    .memoryLimit(150000)
                    .rows(1000)
                    .outlierSpec(&frameAndDirectory);
    api::CDataFrameAnalyzer analyzer{std::move(spec), std::move(frameAndDirectory),
                                     std::move(outputWriterFactory)};

    TDoubleVec expectedScores;
    TDoubleVecVec expectedFeatureInfluences;

    TStrVec fieldNames{"c1", "c2", "c3", "c4", "c5", ".", "."};
    TStrVec fieldValues{"", "", "", "", "", "0", ""};
    addOutlierTestData(fieldNames, fieldValues, analyzer, expectedScores,
                       expectedFeatureInfluences, 990, 10);
    analyzer.handleRecord(fieldNames, {"", "", "", "", "", "", "$"});

    rapidjson::Document results;
    rapidjson::ParseResult ok(results.Parse(output.str()));
    BOOST_TEST_REQUIRE(static_cast<bool>(ok) == true);

    auto expectedScore = expectedScores.begin();
    for (const auto& result : results.GetArray()) {
        if (result.HasMember("row_results")) {
            BOOST_TEST_REQUIRE(expectedScore != expectedScores.end());
            BOOST_REQUIRE_CLOSE_ABSOLUTE(
                *expectedScore,
                result["row_results"]["results"]["ml"]["outlier_score"].GetDouble(),
                1e-4 * *expectedScore);
            ++expectedScore;
        }
    }
    BOOST_TEST_REQUIRE(expectedScore == expectedScores.end());

    LOG_DEBUG(<< "number partitions = "
              << core::CProgramCounters::counter(counter_t::E_DFONumberPartitions));
    LOG_DEBUG(<< "estimated memory usage = "
              << core::CProgramCounters::counter(counter_t::E_DFOEstimatedPeakMemoryUsage));
    LOG_DEBUG(<< "peak memory = "
              << core::CProgramCounters::counter(counter_t::E_DFOPeakMemoryUsage));

    BOOST_TEST_REQUIRE(core::CProgramCounters::counter(counter_t::E_DFONumberPartitions) > 1);
    // Allow a 20% margin
    BOOST_TEST_REQUIRE(core::CProgramCounters::counter(counter_t::E_DFOPeakMemoryUsage) < 150000);
    BOOST_TEST_REQUIRE(
        core::CProgramCounters::counter(counter_t::E_DFOPeakMemoryUsage) <
        (120 * core::CProgramCounters::counter(counter_t::E_DFOEstimatedPeakMemoryUsage)) / 100);
}

BOOST_AUTO_TEST_CASE(testRunOutlierFeatureInfluences) {

    // Test we compute and write out the feature influences when requested.

    std::stringstream output;
    auto outputWriterFactory = [&output]() {
        return std::make_unique<core::CJsonOutputStreamWrapper>(output);
    };

    TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
    auto spec = test::CDataFrameAnalysisSpecificationFactory{}
                    .outlierComputeInfluence(true)
                    .outlierSpec(&frameAndDirectory);
    api::CDataFrameAnalyzer analyzer{std::move(spec), std::move(frameAndDirectory),
                                     std::move(outputWriterFactory)};

    TDoubleVec expectedScores;
    TDoubleVecVec expectedFeatureInfluences;
    TStrVec expectedNames{"c1", "c2", "c3", "c4", "c5"};

    TStrVec fieldNames{"c1", "c2", "c3", "c4", "c5", ".", "."};
    TStrVec fieldValues{"", "", "", "", "", "0", ""};
    addOutlierTestData(fieldNames, fieldValues, analyzer, expectedScores,
                       expectedFeatureInfluences, 100, 10,
                       maths::analytics::COutliers::E_Ensemble, 0, true);
    analyzer.handleRecord(fieldNames, {"", "", "", "", "", "", "$"});

    rapidjson::Document results;
    rapidjson::ParseResult ok(results.Parse(output.str()));
    BOOST_TEST_REQUIRE(static_cast<bool>(ok) == true);

    auto expectedFeatureInfluence = expectedFeatureInfluences.begin();
    for (const auto& result : results.GetArray()) {
        if (result.HasMember("row_results")) {

            BOOST_TEST_REQUIRE(expectedFeatureInfluence !=
                               expectedFeatureInfluences.end());
            for (int i = 0; i < 5; ++i) {
                BOOST_REQUIRE_EQUAL(
                    expectedNames[i].c_str(),
                    result["row_results"]["results"]["ml"]["feature_influence"][i]["feature_name"]
                        .GetString());

                BOOST_REQUIRE_CLOSE_ABSOLUTE(
                    (*expectedFeatureInfluence)[i],
                    result["row_results"]["results"]["ml"]["feature_influence"][i]["influence"]
                        .GetDouble(),
                    1e-4 * (*expectedFeatureInfluence)[i]);
            }
            ++expectedFeatureInfluence;
        }
    }
    BOOST_TEST_REQUIRE(expectedFeatureInfluence == expectedFeatureInfluences.end());
}

BOOST_AUTO_TEST_CASE(testRunOutlierDetectionWithParams) {

    // Test the method and number of neighbours parameters are correctly
    // propagated to the analysis runner.

    TStrVec methods{"lof", "ldof", "distance_kth_nn", "distance_knn"};

    for (const auto& method :
         {maths::analytics::COutliers::E_Lof, maths::analytics::COutliers::E_Ldof,
          maths::analytics::COutliers::E_DistancekNN,
          maths::analytics::COutliers::E_TotalDistancekNN}) {
        for (const auto k : {5, 10}) {

            LOG_DEBUG(<< "Testing '" << methods[method] << "' and '" << k << "'");

            std::stringstream output;
            auto outputWriterFactory = [&output]() {
                return std::make_unique<core::CJsonOutputStreamWrapper>(output);
            };

            TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
            auto spec = test::CDataFrameAnalysisSpecificationFactory{}
                            .outlierMethod(methods[method])
                            .outlierNumberNeighbours(k)
                            .outlierComputeInfluence(false)
                            .memoryLimit(150000)
                            .outlierSpec(&frameAndDirectory);
            api::CDataFrameAnalyzer analyzer{std::move(spec), std::move(frameAndDirectory),
                                             std::move(outputWriterFactory)};

            TDoubleVec expectedScores;
            TDoubleVecVec expectedFeatureInfluences;

            TStrVec fieldNames{"c1", "c2", "c3", "c4", "c5", ".", "."};
            TStrVec fieldValues{"", "", "", "", "", "0", ""};
            addOutlierTestData(fieldNames, fieldValues, analyzer, expectedScores,
                               expectedFeatureInfluences, 100, 10, method, k);
            analyzer.handleRecord(fieldNames, {"", "", "", "", "", "", "$"});

            rapidjson::Document results;
            rapidjson::ParseResult ok(results.Parse(output.str()));
            BOOST_TEST_REQUIRE(static_cast<bool>(ok) == true);

            auto expectedScore = expectedScores.begin();
            for (const auto& result : results.GetArray()) {
                if (result.HasMember("row_results")) {
                    BOOST_TEST_REQUIRE(expectedScore != expectedScores.end());
                    BOOST_REQUIRE_CLOSE_ABSOLUTE(
                        *expectedScore,
                        result["row_results"]["results"]["ml"]["outlier_score"].GetDouble(),
                        1e-6 * *expectedScore);
                    ++expectedScore;
                }
            }
            BOOST_TEST_REQUIRE(expectedScore == expectedScores.end());
        }
    }
}

BOOST_AUTO_TEST_CASE(testFlushMessage) {

    // Test that white space is just ignored.

    std::stringstream output;
    auto outputWriterFactory = [&output]() {
        return std::make_unique<core::CJsonOutputStreamWrapper>(output);
    };

    TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
    auto spec = test::CDataFrameAnalysisSpecificationFactory{}.outlierSpec(&frameAndDirectory);
    api::CDataFrameAnalyzer analyzer{std::move(spec), std::move(frameAndDirectory),
                                     std::move(outputWriterFactory)};
    BOOST_REQUIRE_EQUAL(
        true, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                                    {"", "", "", "", "", "", "           "}));
}

BOOST_AUTO_TEST_CASE(testErrors) {

    std::vector<std::string> errors;
    std::mutex errorsMutex;
    auto errorHandler = [&errors, &errorsMutex](std::string error) {
        std::lock_guard<std::mutex> lock{errorsMutex};
        errors.push_back(error);
    };

    std::stringstream output;
    auto outputWriterFactory = [&output]() {
        return std::make_unique<core::CJsonOutputStreamWrapper>(output);
    };

    core::CLogger::CScopeSetFatalErrorHandler scope{errorHandler};

    // Test with bad analysis specification.
    {
        errors.clear();
        TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
        auto spec = std::make_unique<api::CDataFrameAnalysisSpecification>(
            "junk", &frameAndDirectory);
        api::CDataFrameAnalyzer analyzer{
            std::move(spec), std::move(frameAndDirectory), outputWriterFactory};
        LOG_DEBUG(<< errors);
        BOOST_TEST_REQUIRE(errors.size() > 0);
        BOOST_REQUIRE_EQUAL(false, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5"},
                                                         {"10", "10", "10", "10", "10"}));
    }

    // Test special field in the wrong position.
    {
        errors.clear();
        TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
        auto spec = test::CDataFrameAnalysisSpecificationFactory{}.outlierSpec(&frameAndDirectory);
        api::CDataFrameAnalyzer analyzer{
            std::move(spec), std::move(frameAndDirectory), outputWriterFactory};
        BOOST_REQUIRE_EQUAL(
            false, analyzer.handleRecord({"c1", "c2", "c3", ".", "c4", "c5", "."},
                                         {"10", "10", "10", "", "10", "10", ""}));
        LOG_DEBUG(<< errors);
        BOOST_TEST_REQUIRE(errors.size() > 0);
    }

    // Test missing special field
    {
        TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
        auto spec = test::CDataFrameAnalysisSpecificationFactory{}.outlierSpec(&frameAndDirectory);
        api::CDataFrameAnalyzer analyzer{
            std::move(spec), std::move(frameAndDirectory), outputWriterFactory};
        errors.clear();
        BOOST_REQUIRE_EQUAL(
            false, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", "."},
                                         {"10", "10", "10", "10", "10", ""}));
        LOG_DEBUG(<< errors);
        BOOST_TEST_REQUIRE(errors.size() > 0);
    }

    // Test bad control message.
    {
        TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
        auto spec = test::CDataFrameAnalysisSpecificationFactory{}.outlierSpec(&frameAndDirectory);
        api::CDataFrameAnalyzer analyzer{
            std::move(spec), std::move(frameAndDirectory), outputWriterFactory};
        errors.clear();
        BOOST_REQUIRE_EQUAL(
            false, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                                         {"10", "10", "10", "10", "10", "", "foo"}));
        LOG_DEBUG(<< errors);
        BOOST_TEST_REQUIRE(errors.size() > 0);
    }

    // Test bad input.
    {
        TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
        auto spec = test::CDataFrameAnalysisSpecificationFactory{}.outlierSpec(&frameAndDirectory);
        api::CDataFrameAnalyzer analyzer{
            std::move(spec), std::move(frameAndDirectory), outputWriterFactory};
        errors.clear();
        BOOST_REQUIRE_EQUAL(
            false, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                                         {"10", "10", "10", "10", "10"}));
        LOG_DEBUG(<< errors);
        BOOST_TEST_REQUIRE(errors.size() > 0);
    }

    // Test inconsistent number of rows
    {
        // Fewer rows than expected is ignored.
        TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
        auto spec = test::CDataFrameAnalysisSpecificationFactory{}.rows(2).outlierSpec(
            &frameAndDirectory);
        api::CDataFrameAnalyzer analyzer{
            std::move(spec), std::move(frameAndDirectory), outputWriterFactory};
        errors.clear();
        BOOST_REQUIRE_EQUAL(
            true, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                                        {"10", "10", "10", "10", "10", "0", ""}));
        BOOST_REQUIRE_EQUAL(
            true, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                                        {"", "", "", "", "", "", "$"}));
        LOG_DEBUG(<< errors);
        BOOST_TEST_REQUIRE(errors.empty());
    }
    {
        TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
        auto spec = test::CDataFrameAnalysisSpecificationFactory{}.rows(2).outlierSpec(
            &frameAndDirectory);
        api::CDataFrameAnalyzer analyzer{
            std::move(spec), std::move(frameAndDirectory), outputWriterFactory};
        errors.clear();
        BOOST_REQUIRE_EQUAL(
            true, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                                        {"10", "10", "10", "10", "10", "0", ""}));
        BOOST_REQUIRE_EQUAL(
            true, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                                        {"10", "10", "10", "10", "10", "0", ""}));
        BOOST_REQUIRE_EQUAL(
            true, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                                        {"10", "10", "10", "10", "10", "0", ""}));
        BOOST_REQUIRE_EQUAL(
            true, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                                        {"", "", "", "", "", "", "$"}));
        LOG_DEBUG(<< errors);
        BOOST_TEST_REQUIRE(errors.size() > 0);
    }

    // No data.
    {
        TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
        auto spec = test::CDataFrameAnalysisSpecificationFactory{}.rows(2).outlierSpec(
            &frameAndDirectory);
        api::CDataFrameAnalyzer analyzer{
            std::move(spec), std::move(frameAndDirectory), outputWriterFactory};
        errors.clear();
        BOOST_REQUIRE_EQUAL(
            true, analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                                        {"", "", "", "", "", "", "$"}));
        LOG_DEBUG(<< errors);
        BOOST_TEST_REQUIRE(errors.size() > 0);
        BOOST_REQUIRE_EQUAL(std::string{"Input error: no data sent."}, errors[0]);
    }

    // Memory limit exceeded.
    {
        output.str("");
        errors.clear();
        TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
        auto spec =
            test::CDataFrameAnalysisSpecificationFactory{}.memoryLimit(10000).outlierSpec(
                &frameAndDirectory);
        api::CDataFrameAnalyzer analyzer{
            std::move(spec), std::move(frameAndDirectory), outputWriterFactory};
        TStrVec fieldNames{"c1", "c2", "c3", "c4", "c5", ".", "."};
        TStrVec fieldValues{"", "", "", "", "", "0", ""};
        TDoubleVec expectedScores;
        TDoubleVecVec expectedFeatureInfluences;
        addOutlierTestData(fieldNames, fieldValues, analyzer, expectedScores,
                           expectedFeatureInfluences, 100, 10);
        analyzer.handleRecord(fieldNames, {"", "", "", "", "", "", "$"});
        LOG_DEBUG(<< errors);
        BOOST_TEST_REQUIRE(errors.size() > 0);
        bool memoryLimitExceed{false};
        for (const auto& error : errors) {
            if (error.find("Input error: memory limit") != std::string::npos) {
                memoryLimitExceed = true;
                break;
            }
        }
        BOOST_TEST_REQUIRE(memoryLimitExceed);

        // verify memory status change
        rapidjson::Document results;
        rapidjson::ParseResult ok(results.Parse(output.str()));
        BOOST_TEST_REQUIRE(static_cast<bool>(ok) == true);
        bool memoryStatusOk{false};
        bool memoryStatusHardLimit{false};
        bool memoryReestimateAvailable{false};
        for (const auto& result : results.GetArray()) {
            if (result.HasMember("analytics_memory_usage")) {
                std::string status{result["analytics_memory_usage"]["status"].GetString()};
                if (status == "ok") {
                    memoryStatusOk = true;
                } else if (status == "hard_limit") {
                    memoryStatusHardLimit = true;
                    if (result["analytics_memory_usage"].HasMember("memory_reestimate_bytes") &&
                        result["analytics_memory_usage"]["memory_reestimate_bytes"]
                                .GetInt() > 0) {
                        memoryReestimateAvailable = true;
                    }
                }
            }
        }
        BOOST_TEST_REQUIRE(memoryStatusOk);
        BOOST_TEST_REQUIRE(memoryStatusHardLimit);
        BOOST_TEST_REQUIRE(memoryReestimateAvailable);
    }
}

BOOST_AUTO_TEST_CASE(testRoundTripDocHashes) {

    std::stringstream output;
    auto outputWriterFactory = [&output]() {
        return std::make_unique<core::CJsonOutputStreamWrapper>(output);
    };

    TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
    auto spec = test::CDataFrameAnalysisSpecificationFactory{}.rows(9).outlierSpec(&frameAndDirectory);
    api::CDataFrameAnalyzer analyzer{std::move(spec), std::move(frameAndDirectory),
                                     std::move(outputWriterFactory)};
    for (const auto& i : {"1", "2", "3", "4", "5", "6", "7", "8", "9"}) {
        analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                              {i, i, i, i, i, i, ""});
    }

    analyzer.handleRecord({"c1", "c2", "c3", "c4", "c5", ".", "."},
                          {"", "", "", "", "", "", "$"});

    rapidjson::Document results;
    rapidjson::ParseResult ok(results.Parse(output.str()));
    BOOST_TEST_REQUIRE(static_cast<bool>(ok) == true);

    int expectedHash{0};
    for (const auto& result : results.GetArray()) {
        if (result.HasMember("row_results")) {
            LOG_DEBUG(<< "checksum = " << result["row_results"]["checksum"].GetInt());
            BOOST_REQUIRE_EQUAL(++expectedHash,
                                result["row_results"]["checksum"].GetInt());
        }
    }
}

BOOST_AUTO_TEST_CASE(testProgress) {

    // Test we get 100% progress reported for all stages of the analysis.

    std::stringstream output;
    auto outputWriterFactory = [&output]() {
        return std::make_unique<core::CJsonOutputStreamWrapper>(output);
    };

    TDataFrameUPtrTemporaryDirectoryPtrPr frameAndDirectory;
    auto spec = test::CDataFrameAnalysisSpecificationFactory{}.outlierSpec(&frameAndDirectory);
    api::CDataFrameAnalyzer analyzer{std::move(spec), std::move(frameAndDirectory),
                                     std::move(outputWriterFactory)};

    TDoubleVec expectedScores;
    TDoubleVecVec expectedFeatureInfluences;

    TStrVec fieldNames{"c1", "c2", "c3", "c4", "c5", ".", "."};
    TStrVec fieldValues{"", "", "", "", "", "0", ""};
    addOutlierTestData(fieldNames, fieldValues, analyzer, expectedScores,
                       expectedFeatureInfluences);
    analyzer.handleRecord(fieldNames, {"", "", "", "", "", "", "$"});

    rapidjson::Document results;
    rapidjson::ParseResult ok(results.Parse(output.str()));
    BOOST_TEST_REQUIRE(static_cast<bool>(ok) == true);

    int computingOutliersProgress{0};
    for (const auto& result : results.GetArray()) {
        if (result.HasMember("phase_progress")) {
            rapidjson::StringBuffer sb;
            rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
            result["phase_progress"].Accept(writer);
            LOG_DEBUG(<< sb.GetString());
            if (result["phase_progress"]["phase"] ==
                maths::analytics::COutliers::COMPUTING_OUTLIERS) {
                computingOutliersProgress =
                    std::max(computingOutliersProgress,
                             result["phase_progress"]["progress_percent"].GetInt());
            }
        }
    }

    BOOST_REQUIRE_EQUAL(100, computingOutliersProgress);
}

BOOST_AUTO_TEST_SUITE_END()
