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
#include <model/CCategoryExamplesCollector.h>

#include <core/CLogger.h>
#include <core/CMemoryDef.h>
#include <core/CStatePersistInserter.h>
#include <core/CStateRestoreTraverser.h>
#include <core/CStringUtils.h>

#include <algorithm>
#include <vector>

namespace ml {
namespace model {

namespace {

const std::string EXAMPLES_BY_CATEGORY_TAG("a");
const std::string CATEGORY_TAG("b");
const std::string EXAMPLE_TAG("c");

const CCategoryExamplesCollector::TStrFSet EMPTY_EXAMPLES;

const std::string ELLIPSIS(3, '.');

} // unnamed

const std::size_t CCategoryExamplesCollector::MAX_EXAMPLE_LENGTH(1000);

CCategoryExamplesCollector::CCategoryExamplesCollector(std::size_t maxExamples)
    : m_MaxExamples(maxExamples) {
}

CCategoryExamplesCollector::CCategoryExamplesCollector(std::size_t maxExamples,
                                                       core::CStateRestoreTraverser& traverser)
    : m_MaxExamples(maxExamples) {
    if (traverser.traverseSubLevel(std::bind(&CCategoryExamplesCollector::acceptRestoreTraverser,
                                             this, std::placeholders::_1)) == false) {
        traverser.setBadState();
    }
}

bool CCategoryExamplesCollector::add(CLocalCategoryId categoryId, const std::string& example) {
    if (m_MaxExamples == 0) {
        return false;
    }
    TStrFSet& examplesForCategory = m_ExamplesByCategory[categoryId];
    if (examplesForCategory.size() >= m_MaxExamples) {
        return false;
    }
    return examplesForCategory.insert(truncateExample(example)).second;
}

std::size_t CCategoryExamplesCollector::numberOfExamplesForCategory(CLocalCategoryId categoryId) const {
    auto iterator = m_ExamplesByCategory.find(categoryId);
    return (iterator == m_ExamplesByCategory.end()) ? 0 : iterator->second.size();
}

const CCategoryExamplesCollector::TStrFSet&
CCategoryExamplesCollector::examples(CLocalCategoryId categoryId) const {
    auto iterator = m_ExamplesByCategory.find(categoryId);
    if (iterator == m_ExamplesByCategory.end()) {
        return EMPTY_EXAMPLES;
    }
    return iterator->second;
}

void CCategoryExamplesCollector::acceptPersistInserter(core::CStatePersistInserter& inserter) const {
    // Persist the examples sorted by category ID to make it easier to compare
    // persisted state

    using TLocalCategoryIdStrFSetCPtrPr = std::pair<CLocalCategoryId, const TStrFSet*>;
    using TLocalCategoryIdStrFSetCPtrPrVec = std::vector<TLocalCategoryIdStrFSetCPtrPr>;

    TLocalCategoryIdStrFSetCPtrPrVec orderedData;
    orderedData.reserve(m_ExamplesByCategory.size());

    for (const auto& exampleByCategory : m_ExamplesByCategory) {
        orderedData.emplace_back(exampleByCategory.first, &exampleByCategory.second);
    }

    std::sort(orderedData.begin(), orderedData.end());

    for (const auto& exampleByCategory : orderedData) {
        inserter.insertLevel(EXAMPLES_BY_CATEGORY_TAG,
                             std::bind(&CCategoryExamplesCollector::persistExamples,
                                       this, exampleByCategory.first,
                                       std::cref(*exampleByCategory.second),
                                       std::placeholders::_1));
    }
}

void CCategoryExamplesCollector::persistExamples(CLocalCategoryId categoryId,
                                                 const TStrFSet& examples,
                                                 core::CStatePersistInserter& inserter) const {
    inserter.insertValue(CATEGORY_TAG, categoryId.id());
    for (TStrFSetCItr itr = examples.begin(); itr != examples.end(); ++itr) {
        inserter.insertValue(EXAMPLE_TAG, *itr);
    }
}

bool CCategoryExamplesCollector::acceptRestoreTraverser(core::CStateRestoreTraverser& traverser) {
    m_ExamplesByCategory.clear();
    do {
        const std::string& name = traverser.name();
        if (name == EXAMPLES_BY_CATEGORY_TAG) {
            if (traverser.traverseSubLevel(std::bind(&CCategoryExamplesCollector::restoreExamples,
                                                     this, std::placeholders::_1)) == false) {
                LOG_ERROR(<< "Error restoring examples by category");
                return false;
            }
        }
    } while (traverser.next());

    return true;
}

bool CCategoryExamplesCollector::restoreExamples(core::CStateRestoreTraverser& traverser) {
    CLocalCategoryId categoryId;
    TStrFSet examples;
    do {
        const std::string& name = traverser.name();
        if (name == CATEGORY_TAG) {
            if (categoryId.fromString(traverser.value()) == false) {
                LOG_ERROR(<< "Error restoring category: " << traverser.value());
                return false;
            }
        } else if (name == EXAMPLE_TAG) {
            examples.insert(traverser.value());
        }
    } while (traverser.next());

    if (categoryId.isValid()) {
        LOG_TRACE(<< "Restoring examples for category " << categoryId << ": " << examples);
        m_ExamplesByCategory[categoryId].swap(examples);
    }

    return true;
}

void CCategoryExamplesCollector::clear() {
    m_ExamplesByCategory.clear();
}

void CCategoryExamplesCollector::debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CCategoryExamplesCollector");
    core::CMemoryDebug::dynamicSize("m_ExamplesByCategory", m_ExamplesByCategory, mem);
}

std::size_t CCategoryExamplesCollector::memoryUsage() const {
    std::size_t mem = 0;
    mem += core::CMemory::dynamicSize(m_ExamplesByCategory);
    return mem;
}

std::string CCategoryExamplesCollector::truncateExample(std::string example) {
    if (example.length() > MAX_EXAMPLE_LENGTH) {
        std::size_t replacePos(MAX_EXAMPLE_LENGTH - ELLIPSIS.length());

        // Ensure truncation doesn't result in a partial UTF-8 character
        while (replacePos > 0 &&
               core::CStringUtils::utf8ByteType(example[replacePos]) == -1) {
            --replacePos;
        }
        example.replace(replacePos, example.length() - replacePos, ELLIPSIS);
    }

    // Shouldn't be as inefficient as it looks in C++11 due to move
    // semantics on return
    return example;
}
}
}
