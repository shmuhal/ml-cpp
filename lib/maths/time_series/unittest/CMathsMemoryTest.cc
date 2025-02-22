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

#include <core/CMemoryDef.h>

#include <maths/common/CBjkstUniqueValues.h>
#include <maths/common/CConstantPrior.h>
#include <maths/common/CGammaRateConjugate.h>
#include <maths/common/CLogNormalMeanPrecConjugate.h>
#include <maths/common/CMultimodalPrior.h>
#include <maths/common/CMultinomialConjugate.h>
#include <maths/common/CNormalMeanPrecConjugate.h>
#include <maths/common/CPoissonMeanConjugate.h>
#include <maths/common/CPrior.h>
#include <maths/common/CTools.h>
#include <maths/common/CXMeansOnline1d.h>

#include <maths/time_series/CTimeSeriesDecomposition.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(CMathsMemoryTest)

using namespace ml;
using namespace maths;
using namespace common;

BOOST_AUTO_TEST_CASE(testTimeSeriesDecompositions) {
    maths::time_series::CTimeSeriesDecomposition decomp(0.95, 3600, 55);

    core_t::TTime time;
    time = 140390672;

    for (unsigned i = 0; i < 600000; i += 600) {
        decomp.addPoint(time + i, (0.55 * (0.2 + (i % 86400))));
    }

    core::CMemoryUsage mem;
    decomp.debugMemoryUsage(mem.addChild());
    BOOST_REQUIRE_EQUAL(decomp.memoryUsage(), mem.usage());
}

BOOST_AUTO_TEST_CASE(testPriors) {
    CConstantPrior::TOptionalDouble d;
    CConstantPrior constantPrior(d);
    BOOST_REQUIRE_EQUAL(0, constantPrior.memoryUsage());

    CGammaRateConjugate::TDoubleVec samples;
    samples.push_back(0.996);
    maths_t::TDoubleWeightsAry weight(maths_t::countWeight(0.2));
    maths_t::TDoubleWeightsAry1Vec weights{weight};

    CGammaRateConjugate gammaRateConjugate(maths_t::E_ContinuousData, 0.0, 0.9, 0.8, 0.7);
    BOOST_REQUIRE_EQUAL(0, gammaRateConjugate.memoryUsage());
    gammaRateConjugate.addSamples(samples, weights);
    BOOST_REQUIRE_EQUAL(0, gammaRateConjugate.memoryUsage());

    CLogNormalMeanPrecConjugate logNormalConjugate(maths_t::E_ContinuousData,
                                                   0.0, 0.9, 0.8, 0.7, 0.2);
    BOOST_REQUIRE_EQUAL(0, logNormalConjugate.memoryUsage());
    logNormalConjugate.addSamples(samples, weights);
    BOOST_REQUIRE_EQUAL(0, logNormalConjugate.memoryUsage());

    CPoissonMeanConjugate poissonConjugate(0.0, 0.8, 0.7, 0.3);
    BOOST_REQUIRE_EQUAL(0, poissonConjugate.memoryUsage());
    poissonConjugate.addSamples(samples, weights);
    BOOST_REQUIRE_EQUAL(0, poissonConjugate.memoryUsage());

    CNormalMeanPrecConjugate normalConjugate(maths_t::E_ContinuousData, 0.0,
                                             0.9, 0.8, 0.7, 0.2);
    BOOST_REQUIRE_EQUAL(0, normalConjugate.memoryUsage());
    normalConjugate.addSamples(samples, weights);
    BOOST_REQUIRE_EQUAL(0, normalConjugate.memoryUsage());

    CMultinomialConjugate multinomialConjugate;
    BOOST_REQUIRE_EQUAL(0, multinomialConjugate.memoryUsage());
    multinomialConjugate.addSamples(samples, weights);
    BOOST_REQUIRE_EQUAL(0, multinomialConjugate.memoryUsage());

    CXMeansOnline1d clusterer(maths_t::E_ContinuousData, CAvailableModeDistributions::ALL,
                              maths_t::E_ClustersEqualWeight);

    // Check that the clusterer has size at least as great as the sum of it's fixed members
    std::size_t clustererSize = sizeof(maths_t::EDataType) + 4 * sizeof(double) +
                                sizeof(maths_t::EClusterWeightCalc) +
                                sizeof(CClusterer1d::CIndexGenerator) +
                                sizeof(CXMeansOnline1d::TClusterVec);

    BOOST_TEST_REQUIRE(clusterer.memoryUsage() >= clustererSize);

    CClusterer1d::TPointPreciseDoublePrVec clusters{
        {0.1, 0.7}, {0.01, 0.6}, {0.9, 0.5}, {0.6, 0.2}, {0.3, 0.3}, {0.4, 0.9},
        {0.7, 0.8}, {0.8, 0.9},  {0.2, 0.4}, {0.3, 0.5}, {0.3, 0.5}};
    clusterer.add(clusters);

    // Check that the CMultimodalPrior increases in size
    CMultimodalPrior multimodalPrior(maths_t::E_ContinuousData, clusterer, constantPrior);

    std::size_t initialMultimodalPriorSize = multimodalPrior.memoryUsage();

    multimodalPrior.addSamples(samples, weights);
    BOOST_TEST_REQUIRE(initialMultimodalPriorSize < multimodalPrior.memoryUsage());

    core::CMemoryUsage mem;
    multimodalPrior.debugMemoryUsage(mem.addChild());
    BOOST_REQUIRE_EQUAL(multimodalPrior.memoryUsage(), mem.usage());
}

BOOST_AUTO_TEST_CASE(testBjkstVec) {
    using TBjkstValuesVec = std::vector<maths::common::CBjkstUniqueValues>;
    {
        // Test empty
        TBjkstValuesVec values;
        auto mem = std::make_shared<core::CMemoryUsage>();
        mem->setName("root", 0);
        core::CMemoryDebug::dynamicSize("values", values, mem);
        BOOST_REQUIRE_EQUAL(core::CMemory::dynamicSize(values), mem->usage());
    }
    {
        // Test adding values to the vector part
        TBjkstValuesVec values;
        maths::common::CBjkstUniqueValues seed(3, 100);
        values.resize(5, seed);
        for (std::size_t i = 0; i < 5; i++) {
            for (int j = 0; j < 100; j++) {
                values[i].add(j);
            }
        }
        auto mem = std::make_shared<core::CMemoryUsage>();
        mem->setName("root", 0);
        core::CMemoryDebug::dynamicSize("values", values, mem);
        BOOST_REQUIRE_EQUAL(core::CMemory::dynamicSize(values), mem->usage());
    }
    {
        // Test adding values to the sketch part
        TBjkstValuesVec values;
        maths::common::CBjkstUniqueValues seed(3, 100);
        values.resize(5, seed);
        for (std::size_t i = 0; i < 5; i++) {
            for (int j = 0; j < 1000; j++) {
                values[i].add(j);
            }
        }
        auto mem = std::make_shared<core::CMemoryUsage>();
        mem->setName("root", 0);
        core::CMemoryDebug::dynamicSize("values", values, mem);
        BOOST_REQUIRE_EQUAL(core::CMemory::dynamicSize(values), mem->usage());
    }
}

BOOST_AUTO_TEST_SUITE_END()
