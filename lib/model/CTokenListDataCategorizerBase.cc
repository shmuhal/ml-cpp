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
#include <model/CTokenListDataCategorizerBase.h>

#include <core/CLogger.h>
#include <core/CMemoryDefMultiIndex.h>
#include <core/CStatePersistInserter.h>
#include <core/CStateRestoreTraverser.h>
#include <core/CStringUtils.h>

#include <maths/common/COrderings.h>

#include <model/CTokenListReverseSearchCreator.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ostream>
#include <set>

namespace ml {
namespace model {

// Initialise statics
const std::string CTokenListDataCategorizerBase::PRETOKENISED_TOKEN_FIELD{"..."};

// We use short field names to reduce the state size
namespace {
const std::string TOKEN_TAG{"a"};
const std::string TOKEN_CATEGORY_COUNT_TAG{"b"};
const std::string CATEGORY_TAG{"c"};
const std::string MEMORY_CATEGORIZATION_FAILURES_TAG{"d"};
}

CTokenListDataCategorizerBase::CTokenListDataCategorizerBase(CLimits& limits,
                                                             const TTokenListReverseSearchCreatorCPtr& reverseSearchCreator,
                                                             double threshold,
                                                             const std::string& fieldName)
    : CDataCategorizer{limits, fieldName}, m_ReverseSearchCreator{reverseSearchCreator},
      m_LowerThreshold{std::min(0.99, std::max(0.01, threshold))},
      // Upper threshold is half way between the lower threshold and 1
      m_UpperThreshold{(1.0 + m_LowerThreshold) / 2.0} {
}

void CTokenListDataCategorizerBase::dumpStats(const TLocalCategoryIdFormatterFunc& idFormatter) const {
    // ML local category ID is vector index plus one.  If global category IDs
    // are different then the supplied formatter should print that too.
    for (std::size_t index = 0; index < m_Categories.size(); ++index) {
        const CTokenListCategory& category{m_Categories[index]};
        LOG_DEBUG(<< "ML category=" << idFormatter(CLocalCategoryId{index}) << '-'
                  << category.numMatches() << ' ' << category.baseString());
    }
}

CLocalCategoryId
CTokenListDataCategorizerBase::computeCategory(bool isDryRun,
                                               const TStrStrUMap& fields,
                                               const std::string& str,
                                               std::size_t rawStringLen) {
    // First tokenise string
    std::size_t workWeight{0};
    std::size_t minReweightedWorkWeight{0};
    std::size_t maxReweightedWorkWeight{0};
    auto preTokenisedIter = fields.find(PRETOKENISED_TOKEN_FIELD);
    if (preTokenisedIter != fields.end()) {
        if (this->addPretokenisedTokens(preTokenisedIter->second, m_WorkTokenIds,
                                        m_WorkTokenUniqueIds, workWeight, minReweightedWorkWeight,
                                        maxReweightedWorkWeight) == false) {
            return CLocalCategoryId::softFailure();
        }
    } else {
        this->tokeniseString(fields, str, m_WorkTokenIds, m_WorkTokenUniqueIds, workWeight,
                             minReweightedWorkWeight, maxReweightedWorkWeight);
    }

    // Determine the minimum and maximum token weight that could possibly
    // match the weight we've got
    std::size_t minWeight{CTokenListDataCategorizerBase::minMatchingWeight(
        minReweightedWorkWeight, m_LowerThreshold)};
    std::size_t maxWeight{CTokenListDataCategorizerBase::maxMatchingWeight(
        maxReweightedWorkWeight, m_LowerThreshold)};

    // We search previous categories in descending order of the number of matches
    // we've seen for them
    auto bestSoFarIter = m_CategoriesByCount.end();
    double bestSoFarSimilarity{m_LowerThreshold};
    for (auto iter = m_CategoriesByCount.begin(); iter != m_CategoriesByCount.end(); ++iter) {
        const CTokenListCategory& compCategory{m_Categories[iter->second]};
        const TSizeSizePrVec& baseTokenIds{compCategory.baseTokenIds()};
        std::size_t baseWeight{compCategory.baseWeight()};

        // Check whether the current record matches the search for the existing
        // category - if it does then we'll put it in the existing category without any
        // further checks.  The first condition here ensures that we never say
        // a string with tokens matches the reverse search of a string with no
        // tokens (which the other criteria alone might say matched).
        bool matchesSearch{compCategory.matchesSearchForCategory(
            workWeight, rawStringLen, m_WorkTokenUniqueIds, m_WorkTokenIds)};
        if (matchesSearch == false) {
            // Quickly rule out wildly different token weights prior to doing
            // the expensive similarity calculations
            if (baseWeight < minWeight || baseWeight > maxWeight) {
                continue;
            }

            // Rule out categories where adding the current string would unacceptably
            // reduce the number of unique common tokens
            std::size_t missingCommonTokenWeight{
                compCategory.missingCommonTokenWeight(m_WorkTokenUniqueIds)};
            if (missingCommonTokenWeight > 0) {
                std::size_t origUniqueTokenWeight{compCategory.origUniqueTokenWeight()};
                std::size_t commonUniqueTokenWeight{compCategory.commonUniqueTokenWeight()};
                double proportionOfOrig{
                    static_cast<double>(commonUniqueTokenWeight - missingCommonTokenWeight) /
                    static_cast<double>(origUniqueTokenWeight)};
                if (proportionOfOrig < m_LowerThreshold) {
                    continue;
                }
            }
        }

        double similarity{this->similarity(m_WorkTokenIds, workWeight, baseTokenIds, baseWeight)};

        LOG_TRACE(<< similarity << '-' << compCategory.baseString() << '|' << str);

        if (matchesSearch || similarity > m_UpperThreshold) {
            if (similarity <= m_LowerThreshold) {
                // Not an ideal situation, but log at trace level to avoid
                // excessive log file spam
                LOG_TRACE(<< "Reverse search match below threshold : " << similarity
                          << '-' << compCategory.baseString() << '|' << str);
            }

            // This is a strong match, so accept it immediately and stop
            // looking for better matches
            CLocalCategoryId categoryId{iter->second};
            this->addCategoryMatch(isDryRun, str, rawStringLen, m_WorkTokenIds,
                                   m_WorkTokenUniqueIds, iter);
            return categoryId;
        }

        if (similarity > bestSoFarSimilarity) {
            // This is a weak match, but remember it because it's the best we've
            // seen
            bestSoFarIter = iter;
            bestSoFarSimilarity = similarity;

            // Recalculate the minimum and maximum token counts that might
            // produce a better match
            minWeight = CTokenListDataCategorizerBase::minMatchingWeight(
                minReweightedWorkWeight, similarity);
            maxWeight = CTokenListDataCategorizerBase::maxMatchingWeight(
                maxReweightedWorkWeight, similarity);
        }
    }

    if (bestSoFarIter != m_CategoriesByCount.end()) {
        // Return the best match - use vector index plus one as ML category
        CLocalCategoryId categoryId{bestSoFarIter->second};
        this->addCategoryMatch(isDryRun, str, rawStringLen, m_WorkTokenIds,
                               m_WorkTokenUniqueIds, bestSoFarIter);
        return categoryId;
    }

    if (this->areNewCategoriesAllowed() == false) {
        // Only log once per job, as logging every time this happens could
        // generate enormous log spam
        if (++m_MemoryCategorizationFailures == 1) {
            LOG_WARN(<< "Categories are not being created due to lack of memory");
        }
        return CLocalCategoryId::hardFailure();
    }

    // If we get here we haven't matched, so create a new category
    m_CategoriesByCount.emplace_back(1, m_Categories.size());
    ++m_TotalCount;
    if (this->isCategoryCountRare(1)) {
        ++m_NumRareCategories;
    }
    m_Categories.emplace_back(isDryRun, str, rawStringLen, m_WorkTokenIds,
                              workWeight, m_WorkTokenUniqueIds);

    // Increment the counts of categories that use a given token
    for (const auto& workTokenId : m_WorkTokenIds) {
        // We get away with casting away constness ONLY because the category count
        // is not used in any of the multi-index keys
        const_cast<CTokenInfoItem&>(m_TokenIdLookup[workTokenId.first]).incCategoryCount();
    }

    return CLocalCategoryId{m_Categories.size() - 1};
}

bool CTokenListDataCategorizerBase::cacheReverseSearch(CLocalCategoryId categoryId) {

    if (m_ReverseSearchCreator == nullptr) {
        LOG_ERROR(<< "Cannot create reverse search - no reverse search creator");
        return false;
    }

    // Find the correct category object
    if (categoryId.isValid() == false || categoryId.index() >= m_Categories.size()) {
        // Soft failure is supposed to be the only special value used for the
        // category ID that permits subsequent processing like asking for a
        // reverse search.
        if (categoryId.isSoftFailure() == false) {
            LOG_ERROR(<< "Programmatic error - unexpected ML local category: " << categoryId);
        }

        return false;
    }

    CTokenListCategory& category{m_Categories[categoryId.index()]};

    // If we can retrieve cached reverse search terms we'll save a lot of time
    if (category.hasCachedReverseSearch()) {
        return false;
    }

    std::string part1;
    std::string part2;

    const TSizeSizePrVec& baseTokenIds{category.baseTokenIds()};
    const TSizeSizePrVec& commonUniqueTokenIds{category.commonUniqueTokenIds()};
    if (commonUniqueTokenIds.empty()) {
        // There's quite a high chance this call will return false
        if (m_ReverseSearchCreator->createNoUniqueTokenSearch(
                categoryId, category.baseString(),
                category.maxMatchingStringLen(), part1, part2) == false) {
            // More detail should have been logged by the failed call
            LOG_ERROR(<< "Could not create reverse search");
            return false;
        }

        category.cacheReverseSearch(std::move(part1), std::move(part2));

        return true;
    }

    std::size_t availableCost{m_ReverseSearchCreator->availableCost()};

    // Determine the rarest tokens that we can afford within the available
    // length
    using TSizeSizeSizePrMMap = std::multimap<std::size_t, TSizeSizePr>;
    TSizeSizeSizePrMMap rareIdsWithCost;
    std::size_t lowestCost{std::numeric_limits<std::size_t>::max()};
    std::size_t lowestCostTokenId{std::numeric_limits<std::size_t>::max()};
    for (const auto& commonUniqueTokenId : commonUniqueTokenIds) {
        std::size_t tokenId{commonUniqueTokenId.first};
        std::size_t occurrences{static_cast<std::size_t>(std::count_if(
            baseTokenIds.begin(), baseTokenIds.end(), CSizePairFirstElementEquals(tokenId)))};
        const CTokenInfoItem& info{m_TokenIdLookup[tokenId]};
        std::size_t cost{m_ReverseSearchCreator->costOfToken(info.str(), occurrences)};
        rareIdsWithCost.emplace(info.categoryCount(), TSizeSizePr(tokenId, cost));
        if (lowestCost > cost) {
            lowestCost = cost;
            lowestCostTokenId = tokenId;
        }
    }

    if (availableCost < lowestCost) {
        LOG_WARN(<< "No token was short enough to include in reverse search for "
                 << categoryId << " - cheapest token was " << lowestCostTokenId << " with cost "
                 << lowestCost << " and available cost is " << availableCost);
        return false;
    }

    using TSizeSet = std::set<std::size_t>;
    TSizeSet costedCommonUniqueTokenIds;
    for (auto rareIdWithCost : rareIdsWithCost) {

        std::size_t cost{rareIdWithCost.second.second};
        // Can we afford this token?
        if (availableCost < cost) {
            // Can we afford any possible token?
            if (availableCost < lowestCost) {
                break;
            }
            continue;
        }

        availableCost -= cost;
        // By this point we don't care about the weights or costs
        std::size_t tokenId{rareIdWithCost.second.first};
        costedCommonUniqueTokenIds.insert(tokenId);
    }

    // If we get here we're going to create a search in the standard way - there
    // shouldn't be any more errors after this point

    m_ReverseSearchCreator->initStandardSearch(categoryId, category.baseString(),
                                               category.maxMatchingStringLen(),
                                               part1, part2);

    TSizeSizePr orderedCommonTokenBounds{category.orderedCommonTokenBounds()};
    for (std::size_t index = 0; index < baseTokenIds.size(); ++index) {
        std::size_t tokenId(baseTokenIds[index].first);
        if (costedCommonUniqueTokenIds.find(tokenId) !=
            costedCommonUniqueTokenIds.end()) {
            if (index >= orderedCommonTokenBounds.first &&
                index < orderedCommonTokenBounds.second) {
                m_ReverseSearchCreator->addInOrderCommonToken(
                    m_TokenIdLookup[tokenId].str(), part1, part2);
            } else {
                m_ReverseSearchCreator->addOutOfOrderCommonToken(
                    m_TokenIdLookup[tokenId].str(), part1, part2);
            }
        }
    }

    m_ReverseSearchCreator->closeStandardSearch(part1, part2);

    category.cacheReverseSearch(std::move(part1), std::move(part2));

    return true;
}

bool CTokenListDataCategorizerBase::acceptRestoreTraverser(core::CStateRestoreTraverser& traverser) {
    m_Categories.clear();
    m_CategoriesByCount.clear();
    m_TotalCount = 0;
    m_NumRareCategories = 0;
    m_TokenIdLookup.clear();
    m_WorkTokenIds.clear();
    m_WorkTokenUniqueIds.clear();
    m_MemoryCategorizationFailures = 0;
    m_LastCategorizerStats = SCategorizerStats{};

    do {
        const std::string& name{traverser.name()};
        if (name == TOKEN_TAG) {
            std::size_t nextIndex{m_TokenIdLookup.size()};
            m_TokenIdLookup.push_back(CTokenInfoItem(traverser.value(), nextIndex));
        } else if (name == TOKEN_CATEGORY_COUNT_TAG) {
            if (m_TokenIdLookup.empty()) {
                LOG_ERROR(<< "Token category count precedes token string in "
                          << traverser.value());
                return false;
            }

            std::size_t categoryCount{0};
            if (core::CStringUtils::stringToType(traverser.value(), categoryCount) == false) {
                LOG_ERROR(<< "Invalid token category count in " << traverser.value());
                return false;
            }

            // We get away with casting away constness ONLY because the category
            // count is not used in any of the multi-index keys
            const_cast<CTokenInfoItem&>(m_TokenIdLookup.back()).categoryCount(categoryCount);
        } else if (name == CATEGORY_TAG) {
            CTokenListCategory category{traverser};
            std::size_t count{category.numMatches()};
            m_CategoriesByCount.emplace_back(count, m_Categories.size());
            m_TotalCount += count;
            if (this->isCategoryCountRare(count)) {
                ++m_NumRareCategories;
            }
            m_Categories.emplace_back(std::move(category));
        } else if (name == MEMORY_CATEGORIZATION_FAILURES_TAG) {
            if (core::CStringUtils::stringToType(
                    traverser.value(), m_MemoryCategorizationFailures) == false) {
                LOG_ERROR(<< "Invalid memory categorization failures count in "
                          << traverser.value());
                return false;
            }
        }
    } while (traverser.next());

    // Categories are persisted in order of creation, but this list needs to be
    // sorted by descending count instead
    std::stable_sort(m_CategoriesByCount.begin(), m_CategoriesByCount.end(),
                     maths::common::COrderings::SFirstGreater{});

    this->updateCategorizerStats(m_LastCategorizerStats);

    return true;
}

void CTokenListDataCategorizerBase::acceptPersistInserter(core::CStatePersistInserter& inserter) const {
    CTokenListDataCategorizerBase::acceptPersistInserter(
        m_TokenIdLookup, m_Categories, m_MemoryCategorizationFailures, inserter);
}

void CTokenListDataCategorizerBase::acceptPersistInserter(
    const TTokenMIndex& tokenIdLookup,
    const TTokenListCategoryVec& categories,
    std::size_t memoryCategorizationFailures,
    core::CStatePersistInserter& inserter) {
    for (const CTokenInfoItem& item : tokenIdLookup) {
        inserter.insertValue(TOKEN_TAG, item.str());
        inserter.insertValue(TOKEN_CATEGORY_COUNT_TAG, item.categoryCount());
    }

    for (const CTokenListCategory& category : categories) {
        inserter.insertLevel(CATEGORY_TAG,
                             std::bind(&CTokenListCategory::acceptPersistInserter,
                                       &category, std::placeholders::_1));
    }

    inserter.insertValue(MEMORY_CATEGORIZATION_FAILURES_TAG, memoryCategorizationFailures);
}

CDataCategorizer::TPersistFunc CTokenListDataCategorizerBase::makeForegroundPersistFunc() const {
    return [this](core::CStatePersistInserter& inserter) {
        return CTokenListDataCategorizerBase::acceptPersistInserter(
            m_TokenIdLookup, m_Categories, m_MemoryCategorizationFailures, inserter);
    };
}

CDataCategorizer::TPersistFunc CTokenListDataCategorizerBase::makeBackgroundPersistFunc() const {
    // Do NOT change this to capture the member variables by
    // reference - they MUST be copied for thread safety
    return [
        tokenIdLookup = m_TokenIdLookup, categories = m_Categories,
        memoryCategorizationFailures = m_MemoryCategorizationFailures
    ](core::CStatePersistInserter & inserter) {
        return CTokenListDataCategorizerBase::acceptPersistInserter(
            tokenIdLookup, categories, memoryCategorizationFailures, inserter);
    };
}

void CTokenListDataCategorizerBase::addCategoryMatch(bool isDryRun,
                                                     const std::string& str,
                                                     std::size_t rawStringLen,
                                                     const TSizeSizePrVec& tokenIds,
                                                     const TSizeSizeMap& tokenUniqueIds,
                                                     TSizeSizePrVecItr iter) {
    m_Categories[iter->second].addString(isDryRun, str, rawStringLen, tokenIds, tokenUniqueIds);

    std::size_t& count{iter->first};
    bool wasCountRare{this->isCategoryCountRare(count)};
    ++count;
    ++m_TotalCount;
    bool isCountRare{this->isCategoryCountRare(count)};
    if (isCountRare != wasCountRare) {
        if (isCountRare) {
            ++m_NumRareCategories;
        } else {
            --m_NumRareCategories;
        }
    }

    // Search backwards for the point where the incremented count belongs
    auto swapIter = iter;
    while (swapIter != m_CategoriesByCount.begin()) {
        --swapIter;
        if (count <= swapIter->first) {
            // Move the changed category as little as possible - if its
            // incremented count is equal to another category's count then
            // leave that other category nearer the beginning of the vector
            ++swapIter;
            break;
        }
    }

    // Move the iterator we've matched nearer the front of the list if it
    // deserves this
    if (swapIter != iter) {
        std::iter_swap(swapIter, iter);
    }
}

std::size_t CTokenListDataCategorizerBase::minMatchingWeight(std::size_t weight,
                                                             double threshold) {
    if (weight == 0) {
        return 0;
    }

    // When we build with aggressive optimisation, the result of the floating
    // point multiplication can be slightly out, so add a small amount of
    // tolerance
    static const double EPSILON{0.00000000001};

    // This assumes threshold is not negative - other code in this file must
    // enforce this.  Using floor + 1 due to threshold check being exclusive.
    // If threshold check is changed to inclusive, change formula to ceil
    // (without the + 1).
    return static_cast<std::size_t>(
               std::floor(static_cast<double>(weight) * threshold + EPSILON)) +
           1;
}

std::size_t CTokenListDataCategorizerBase::maxMatchingWeight(std::size_t weight,
                                                             double threshold) {
    if (weight == 0) {
        return 0;
    }

    // When we build with aggressive optimisation, the result of the floating
    // point division can be slightly out, so subtract a small amount of
    // tolerance
    static const double EPSILON{0.00000000001};

    // This assumes threshold is not negative - other code in this file must
    // enforce this.  Using ceil - 1 due to threshold check being exclusive.
    // If threshold check is changed to inclusive, change formula to floor
    // (without the - 1).
    return static_cast<std::size_t>(
               std::ceil(static_cast<double>(weight) / threshold - EPSILON)) -
           1;
}

std::size_t CTokenListDataCategorizerBase::idForToken(const std::string& token) {
    auto iter = boost::multi_index::get<SToken>(m_TokenIdLookup).find(token);
    if (iter != boost::multi_index::get<SToken>(m_TokenIdLookup).end()) {
        return iter->index();
    }

    std::size_t nextIndex{m_TokenIdLookup.size()};
    m_TokenIdLookup.push_back(CTokenInfoItem(token, nextIndex));
    return nextIndex;
}

bool CTokenListDataCategorizerBase::addPretokenisedTokens(const std::string& tokensCsv,
                                                          TSizeSizePrVec& tokenIds,
                                                          TSizeSizeMap& tokenUniqueIds,
                                                          std::size_t& totalWeight,
                                                          std::size_t& minReweightedTotalWeight,
                                                          std::size_t& maxReweightedTotalWeight) {
    tokenIds.clear();
    tokenUniqueIds.clear();
    totalWeight = 0;

    m_CsvLineParser.reset(tokensCsv);
    std::string token;
    while (!m_CsvLineParser.atEnd()) {
        if (m_CsvLineParser.parseNext(token) == false) {
            return false;
        }

        this->tokenToIdAndWeight(token, tokenIds, tokenUniqueIds, totalWeight,
                                 minReweightedTotalWeight, maxReweightedTotalWeight);
    }

    this->reset();

    return true;
}

model_t::ECategorizationStatus CTokenListDataCategorizerBase::categorizationStatus() const {
    return m_LastCategorizerStats.s_CategorizationStatus;
}

void CTokenListDataCategorizerBase::debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CTokenListDataCategorizerBase");
    this->CDataCategorizer::debugMemoryUsage(mem->addChild());
    core::CMemoryDebug::dynamicSize("m_ReverseSearchCreator", m_ReverseSearchCreator, mem);
    core::CMemoryDebug::dynamicSize("m_Categories", m_Categories, mem);
    core::CMemoryDebug::dynamicSize("m_CategoriesByCount", m_CategoriesByCount, mem);
    core::CMemoryDebug::dynamicSize("m_TokenIdLookup", m_TokenIdLookup, mem);
    core::CMemoryDebug::dynamicSize("m_WorkTokenIds", m_WorkTokenIds, mem);
    core::CMemoryDebug::dynamicSize("m_WorkTokenUniqueIds", m_WorkTokenUniqueIds, mem);
    core::CMemoryDebug::dynamicSize("m_CsvLineParser", m_CsvLineParser, mem);
}

std::size_t CTokenListDataCategorizerBase::memoryUsage() const {
    std::size_t mem = this->CDataCategorizer::memoryUsage();
    mem += core::CMemory::dynamicSize(m_ReverseSearchCreator);
    mem += core::CMemory::dynamicSize(m_Categories);
    mem += core::CMemory::dynamicSize(m_CategoriesByCount);
    mem += core::CMemory::dynamicSize(m_TokenIdLookup);
    mem += core::CMemory::dynamicSize(m_WorkTokenIds);
    mem += core::CMemory::dynamicSize(m_WorkTokenUniqueIds);
    mem += core::CMemory::dynamicSize(m_CsvLineParser);
    return mem;
}

void CTokenListDataCategorizerBase::updateCategorizerStats(SCategorizerStats& categorizerStats) const {

    categorizerStats.s_TotalCategories += m_Categories.size();

    categorizerStats.s_CategorizedMessages += m_TotalCount;
    categorizerStats.s_MemoryCategorizationFailures += m_MemoryCategorizationFailures;

    std::size_t frequentCategoriesThisCategorizer{0};
    std::size_t deadCategoriesThisCategorizer{0};
    for (std::size_t i = 0; i < m_CategoriesByCount.size(); ++i) {
        const CTokenListCategory& category{m_Categories[m_CategoriesByCount[i].second]};
        if (this->isCategoryCountFrequent(category.numMatches())) {
            ++frequentCategoriesThisCategorizer;
        }
        for (std::size_t j = 0; j < i; ++j) {
            const CTokenListCategory& moreFrequentCategory{
                m_Categories[m_CategoriesByCount[j].second]};
            if (moreFrequentCategory.matchesSearchForCategory(category)) {
                ++deadCategoriesThisCategorizer;
                break;
            }
        }
    }
    categorizerStats.s_FrequentCategories += frequentCategoriesThisCategorizer;
    categorizerStats.s_RareCategories += m_NumRareCategories;
    categorizerStats.s_DeadCategories += deadCategoriesThisCategorizer;

    categorizerStats.s_CategorizationStatus = std::max(
        categorizerStats.s_CategorizationStatus,
        CTokenListDataCategorizerBase::calculateCategorizationStatus(
            m_TotalCount, m_Categories.size(), frequentCategoriesThisCategorizer,
            m_NumRareCategories, deadCategoriesThisCategorizer));
}

bool CTokenListDataCategorizerBase::isCategoryCountRare(std::size_t count) const {
    // Definition of rare is a single match
    return count == 1;
}

bool CTokenListDataCategorizerBase::isCategoryCountFrequent(std::size_t count) const {
    // Definition of frequent is matching more than 1% of messages, and not one
    return count * 100 > m_TotalCount && count != 1;
}

void CTokenListDataCategorizerBase::updateModelSizeStats(CResourceMonitor::SModelSizeStats& modelSizeStats) const {
    this->updateCategorizerStats(modelSizeStats.s_OverallCategorizerStats);
}

model_t::ECategorizationStatus
CTokenListDataCategorizerBase::calculateCategorizationStatus(std::size_t categorizedMessages,
                                                             std::size_t totalCategories,
                                                             std::size_t frequentCategories,
                                                             std::size_t rareCategories,
                                                             std::size_t deadCategories) {
    // Categorization status is "warn" if:

    // - At least 100 messages have been categorized
    if (categorizedMessages < 100) {
        return model_t::E_CategorizationStatusOk;
    }

    // and one of the following holds:

    // - There is only 1 category
    if (totalCategories == 1) {
        return model_t::E_CategorizationStatusWarn;
    }

    // - More than 90% of categories are rare
    if (10 * rareCategories > 9 * totalCategories) {
        return model_t::E_CategorizationStatusWarn;
    }

    // - The number of categories is greater than 50% of the number of categorized messages
    if (2 * totalCategories > categorizedMessages) {
        return model_t::E_CategorizationStatusWarn;
    }

    // - There are no frequent match categories
    if (frequentCategories == 0) {
        return model_t::E_CategorizationStatusWarn;
    }

    // - More than 50% of categories are dead
    if (2 * deadCategories > totalCategories) {
        return model_t::E_CategorizationStatusWarn;
    }

    return model_t::E_CategorizationStatusOk;
}

std::size_t CTokenListDataCategorizerBase::numMatches(CLocalCategoryId categoryId) {
    if (categoryId.isValid() == false || categoryId.index() >= m_Categories.size()) {
        LOG_ERROR(<< "Programmatic error - unexpected ML local category: " << categoryId);
        return 0;
    }
    return m_Categories[categoryId.index()].numMatches();
}

CDataCategorizer::TLocalCategoryIdVec
CTokenListDataCategorizerBase::usurpedCategories(CLocalCategoryId categoryId) const {
    if (categoryId.isValid() == false || categoryId.index() >= m_Categories.size()) {
        LOG_ERROR(<< "Programmatic error - unexpected ML local category: " << categoryId);
        return {};
    }
    // If the category is not found it indicates a bug in other code in this
    // class, as it means m_Categories and m_CategoriesByCount are inconsistent,
    // so a class invariant is violated.  The next method will log this case.
    return this->usurpedCategories(
        std::find_if(m_CategoriesByCount.begin(), m_CategoriesByCount.end(),
                     [categoryId](const TSizeSizePr& pr) {
                         return pr.second == categoryId.index();
                     }));
}

CDataCategorizer::TLocalCategoryIdVec
CTokenListDataCategorizerBase::usurpedCategories(TSizeSizePrVecCItr iter) const {
    CDataCategorizer::TLocalCategoryIdVec usurped;
    if (iter == m_CategoriesByCount.end()) {
        LOG_ERROR(<< "Programmatic error - categories and "
                     "categories by count are inconsistent");
        return usurped;
    }
    const CTokenListCategory& category{m_Categories[iter->second]};
    for (++iter; iter != m_CategoriesByCount.end(); ++iter) {
        const CTokenListCategory& lessFrequentCategory{m_Categories[iter->second]};
        if (category.matchesSearchForCategory(lessFrequentCategory)) {
            usurped.emplace_back(iter->second);
        }
    }
    std::sort(usurped.begin(), usurped.end());
    return usurped;
}

bool CTokenListDataCategorizerBase::writeCategoryIfChanged(CLocalCategoryId categoryId,
                                                           const TCategoryOutputFunc& outputFunc) {

    if (categoryId.isValid() == false || categoryId.index() >= m_Categories.size()) {
        LOG_ERROR(<< "Programmatic error - unexpected ML local category: " << categoryId);
        return false;
    }

    CTokenListCategory& category{m_Categories[categoryId.index()]};
    if (category.isChangedAndReset() == false) {
        return false;
    }

    this->cacheReverseSearch(categoryId);
    outputFunc(categoryId, category.reverseSearchPart1(),
               category.reverseSearchPart2(), category.maxMatchingStringLen(),
               this->examplesCollector().examples(categoryId),
               category.numMatches(), this->usurpedCategories(categoryId));

    return true;
}

std::size_t CTokenListDataCategorizerBase::writeChangedCategories(const TCategoryOutputFunc& outputFunc) {

    std::size_t numWritten{0};

    // Iterating m_CategoriesByCount rather than m_Categories means we can call
    // the O(N) version of usurpedCategories() rather than the O(N^2) version
    for (auto iter = m_CategoriesByCount.begin(); iter != m_CategoriesByCount.end(); ++iter) {
        CTokenListCategory& category{m_Categories[iter->second]};
        if (category.isChangedAndReset()) {
            CLocalCategoryId categoryId{iter->second};
            this->cacheReverseSearch(categoryId);
            outputFunc(categoryId, category.reverseSearchPart1(),
                       category.reverseSearchPart2(), category.maxMatchingStringLen(),
                       this->examplesCollector().examples(categoryId),
                       category.numMatches(), this->usurpedCategories(iter));
        }
    }

    return numWritten;
}

bool CTokenListDataCategorizerBase::writeCategorizerStatsIfChanged(const TCategorizerStatsOutputFunc& outputFunc) {

    SCategorizerStats newCategorizerStats;
    this->updateCategorizerStats(newCategorizerStats);
    if (newCategorizerStats == m_LastCategorizerStats) {
        return false;
    }
    outputFunc(newCategorizerStats, newCategorizerStats.s_CategorizationStatus !=
                                        m_LastCategorizerStats.s_CategorizationStatus);
    m_LastCategorizerStats = std::move(newCategorizerStats);
    return true;
}

bool CTokenListDataCategorizerBase::isStatsWriteUrgent() const {

    // Ensure we write the stats after seeing many messages regardless of
    // status or numbers of rare/frequent categories
    if (m_TotalCount >= m_LastCategorizerStats.s_CategorizedMessages + 100000) {
        return true;
    }

    // Otherwise, the main reason for this check is to detect that we've entered
    // the "warn" status - if we're already in it then skip the work
    if (m_LastCategorizerStats.s_CategorizationStatus == model_t::E_CategorizationStatusWarn) {
        return false;
    }

    // m_CategoriesByCount is sorted by descending count, so all the
    // frequent categories must be at the beginning
    auto firstNonFrequentIter =
        std::find_if_not(m_CategoriesByCount.begin(), m_CategoriesByCount.end(),
                         [this](const TSizeSizePr& entry) {
                             return this->isCategoryCountFrequent(entry.first);
                         });

    // Dead categories is passed as 0.  This may cause a warning status to be
    // missed.  However, dead categories are quite unusual and quite expensive
    // to calculate, so the cost/benefit does not make it worthwhile for a check
    // that needs to be performed for every successfully categorized message.
    // The warning status will eventually be detected when the dead category
    // count is calculated in a memory usage check (which is done far less
    // frequently).
    return this->calculateCategorizationStatus(
               m_TotalCount, m_Categories.size(),
               firstNonFrequentIter - m_CategoriesByCount.begin(),
               m_NumRareCategories, 0) == model_t::E_CategorizationStatusWarn;
}

std::size_t CTokenListDataCategorizerBase::numCategories() const {
    return m_Categories.size();
}

CTokenListDataCategorizerBase::CTokenInfoItem::CTokenInfoItem(const std::string& str,
                                                              std::size_t index)
    : m_Str{str}, m_Index{index}, m_CategoryCount{0} {
}

const std::string& CTokenListDataCategorizerBase::CTokenInfoItem::str() const {
    return m_Str;
}

void CTokenListDataCategorizerBase::CTokenInfoItem::debugMemoryUsage(
    const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CTokenInfoItem");
    core::CMemoryDebug::dynamicSize("m_Str", m_Str, mem);
}

std::size_t CTokenListDataCategorizerBase::CTokenInfoItem::memoryUsage() const {
    return core::CMemory::dynamicSize(m_Str);
}

std::size_t CTokenListDataCategorizerBase::CTokenInfoItem::index() const {
    return m_Index;
}

std::size_t CTokenListDataCategorizerBase::CTokenInfoItem::categoryCount() const {
    return m_CategoryCount;
}

void CTokenListDataCategorizerBase::CTokenInfoItem::categoryCount(std::size_t categoryCount) {
    m_CategoryCount = categoryCount;
}

void CTokenListDataCategorizerBase::CTokenInfoItem::incCategoryCount() {
    ++m_CategoryCount;
}

CTokenListDataCategorizerBase::CSizePairFirstElementEquals::CSizePairFirstElementEquals(std::size_t value)
    : m_Value(value) {
}

CTokenListDataCategorizerBase::SIdTranslater::SIdTranslater(const CTokenListDataCategorizerBase& categorizer,
                                                            const TSizeSizePrVec& tokenIds,
                                                            char separator)
    : s_Categorizer{categorizer}, s_TokenIds{tokenIds}, s_Separator{separator} {
}

std::ostream& operator<<(std::ostream& strm,
                         const CTokenListDataCategorizerBase::SIdTranslater& translator) {
    for (auto iter = translator.s_TokenIds.begin();
         iter != translator.s_TokenIds.end(); ++iter) {
        if (iter != translator.s_TokenIds.begin()) {
            strm << translator.s_Separator;
        }

        if (iter->first < translator.s_Categorizer.m_TokenIdLookup.size()) {
            strm << translator.s_Categorizer.m_TokenIdLookup[iter->first].str();
        } else {
            strm << "Out of bounds!";
        }
    }

    return strm;
}
}
}
