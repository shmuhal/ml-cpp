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
#ifndef INCLUDED_ml_model_CTokenListDataCategorizer_h
#define INCLUDED_ml_model_CTokenListDataCategorizer_h

#include <core/CLogger.h>
#include <core/CMemoryDec.h>
#include <core/CStringSimilarityTester.h>
#include <core/CTimeUtils.h>
#include <core/CWordDictionary.h>

#include <model/CTokenListDataCategorizerBase.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace ml {
namespace model {

//! \brief
//! Concrete implementation class to categorise strings.
//!
//! DESCRIPTION:\n
//! Breaks a string down into terms and creates a feature vector.
//! Terms are [a-zA-Z0-9_]+ strings.  Terms that are pure hex
//! numbers are discarded.
//!
//! IMPLEMENTATION DECISIONS:\n
//! Configuration of exactly how to tokenise strings is taken in template
//! arguments so that the flags can be tested and dead code eliminated at
//! compile time based on the chosen configuration.  (Because there are
//! so many options this has been observed to give a performance boost of
//! more than 10% compared to having lots of flags being constantly
//! checked at runtime.)
//!
template<bool DO_WARPING = true,
         bool ALLOW_UNDERSCORE = true,
         bool ALLOW_DOT = true,
         bool ALLOW_DASH = true,
         bool ALLOW_FORWARD_SLASH = true,
         bool IGNORE_LEADING_DIGIT = true,
         bool IGNORE_HEX = true,
         bool IGNORE_DATE_WORDS = true,
         bool IGNORE_FIELD_NAMES = true,
         std::size_t MIN_DICTIONARY_LENGTH = 2,
         bool TRUNCATE_AT_NEWLINE = true,
         typename DICTIONARY_WEIGHT_FUNC = core::CWordDictionary::TWeightAll2>
class CTokenListDataCategorizer : public CTokenListDataCategorizerBase {
public:
    //! Create a data categorizer with threshold for how comparable categories are
    //! 0.0 means everything is the same category
    //! 1.0 means things have to match exactly to be the same category
    CTokenListDataCategorizer(CLimits& limits,
                              const TTokenListReverseSearchCreatorCPtr& reverseSearchCreator,
                              double threshold,
                              const std::string& fieldName)
        : CTokenListDataCategorizerBase{limits, reverseSearchCreator, threshold, fieldName},
          m_Dict{core::CWordDictionary::instance()} {}

    //! No copying allowed (because it would complicate the resource monitoring).
    CTokenListDataCategorizer(const CTokenListDataCategorizer&) = delete;
    CTokenListDataCategorizer& operator=(const CTokenListDataCategorizer&) = delete;

    //! Debug the memory used by this categorizer.
    void debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const override {
        mem->setName("CTokenListDataCategorizer");
        this->CTokenListDataCategorizerBase::debugMemoryUsage(mem->addChild());
        core::CMemoryDebug::dynamicSize("m_SimilarityTester", m_SimilarityTester, mem);
    }

    //! Get the memory used by this categorizer.
    std::size_t memoryUsage() const override {
        std::size_t mem = 0;
        mem += this->CTokenListDataCategorizerBase::memoryUsage();
        mem += core::CMemory::dynamicSize(m_SimilarityTester);
        return mem;
    }

    //! Get the static size of this object - used for virtual hierarchies
    std::size_t staticSize() const override { return sizeof(*this); }

protected:
    //! Split the string into a list of tokens.  The result of the
    //! tokenisation is returned in \p tokenIds, \p tokenUniqueIds and
    //! \p totalWeight.  Any previous content of these variables is wiped.
    void tokeniseString(const TStrStrUMap& fields,
                        const std::string& str,
                        TSizeSizePrVec& tokenIds,
                        TSizeSizeMap& tokenUniqueIds,
                        std::size_t& totalWeight,
                        std::size_t& minReweightedTotalWeight,
                        std::size_t& maxReweightedTotalWeight) override {
        tokenIds.clear();
        tokenUniqueIds.clear();
        totalWeight = 0;

        std::string temp;

        // TODO - make more efficient
        std::string::size_type nonHexPos(std::string::npos);
        for (const char curChar : str) {

            if (TRUNCATE_AT_NEWLINE && curChar == '\n') {
                break;
            }

            // Basically tokenise into [a-zA-Z0-9/]+ strings, possibly
            // allowing underscores, dots and dashes in the middle
            if (std::isalnum(static_cast<unsigned char>(curChar)) ||
                (!temp.empty() && ((ALLOW_UNDERSCORE && curChar == '_') ||
                                   (ALLOW_DOT && curChar == '.') ||
                                   (ALLOW_DASH && curChar == '-'))) ||
                (ALLOW_FORWARD_SLASH && curChar == '/')) {
                temp += curChar;
                if (IGNORE_HEX) {
                    // Count dots and dashes as numeric
                    if (!std::isxdigit(static_cast<unsigned char>(curChar)) &&
                        curChar != '.' && curChar != '-') {
                        nonHexPos = temp.length() - 1;
                    }
                }
            } else {
                if (!temp.empty()) {
                    this->considerToken(fields, nonHexPos, temp, tokenIds, tokenUniqueIds,
                                        totalWeight, minReweightedTotalWeight,
                                        maxReweightedTotalWeight);
                    temp.clear();
                }

                if (IGNORE_HEX) {
                    nonHexPos = std::string::npos;
                }
            }
        }

        if (!temp.empty()) {
            this->considerToken(fields, nonHexPos, temp, tokenIds, tokenUniqueIds, totalWeight,
                                minReweightedTotalWeight, maxReweightedTotalWeight);
        }

        LOG_TRACE(<< str << " tokenised to " << tokenIds.size() << " tokens with total weight "
                  << totalWeight << ": " << SIdTranslater(*this, tokenIds, ' '));

        m_DictionaryWeightFunc.reset();
    }

    //! Take a string token, convert it to a numeric ID and a weighting and
    //! add these to the provided data structures.
    void tokenToIdAndWeight(const std::string& token,
                            TSizeSizePrVec& tokenIds,
                            TSizeSizeMap& tokenUniqueIds,
                            std::size_t& totalWeight,
                            std::size_t& minReweightedTotalWeight,
                            std::size_t& maxReweightedTotalWeight) override {
        TSizeSizePr idWithWeight(this->idForToken(token), 1);

        if (token.length() >= MIN_DICTIONARY_LENGTH) {
            // Give more weighting to tokens that are dictionary words.
            idWithWeight.second += m_DictionaryWeightFunc(m_Dict.partOfSpeech(token));
        }

        tokenIds.push_back(idWithWeight);
        tokenUniqueIds[idWithWeight.first] += idWithWeight.second;
        totalWeight += idWithWeight.second;
        minReweightedTotalWeight +=
            m_DictionaryWeightFunc.minMatchingWeight(idWithWeight.second);
        maxReweightedTotalWeight +=
            m_DictionaryWeightFunc.maxMatchingWeight(idWithWeight.second);
    }

    void reset() override { m_DictionaryWeightFunc.reset(); }

    //! Compute similarity between two vectors
    double similarity(const TSizeSizePrVec& left,
                      std::size_t leftWeight,
                      const TSizeSizePrVec& right,
                      std::size_t rightWeight) const override {
        double similarity(1.0);

        std::size_t maxWeight(std::max(leftWeight, rightWeight));
        if (maxWeight > 0) {
            std::size_t diff(DO_WARPING ? m_SimilarityTester.weightedEditDistance(left, right)
                                        : this->compareNoWarp(left, right));

            similarity = 1.0 - double(diff) / double(maxWeight);
        }

        return similarity;
    }

private:
    //! Compare two vectors of tokens without doing any warping (this is an
    //! alternative to using the Levenshtein distance, which is a form of
    //! warping)
    std::size_t compareNoWarp(const TSizeSizePrVec& left, const TSizeSizePrVec& right) const {
        std::size_t minSize(std::min(left.size(), right.size()));
        std::size_t maxSize(std::max(left.size(), right.size()));

        std::size_t diff(0);

        for (std::size_t index = 0; index < minSize; ++index) {
            if (left[index].first != right[index].first) {
                diff += std::max(left[index].second, right[index].second);
            }
        }

        // Account for different length vector instances
        if (left.size() < right.size()) {
            for (std::size_t index = minSize; index < maxSize; ++index) {
                diff += right[index].second;
            }
        } else if (left.size() > right.size()) {
            for (std::size_t index = minSize; index < maxSize; ++index) {
                diff += left[index].second;
            }
        }

        return diff;
    }

    //! Consider adding a token to the data structures that will be used in
    //! the comparison.  The \p token argument must not be empty when this
    //! method is called.  This method may modify \p token.
    void considerToken(const TStrStrUMap& fields,
                       std::string::size_type nonHexPos,
                       std::string& token,
                       TSizeSizePrVec& tokenIds,
                       TSizeSizeMap& tokenUniqueIds,
                       std::size_t& totalWeight,
                       std::size_t& minReweightedTotalWeight,
                       std::size_t& maxReweightedTotalWeight) {
        if (IGNORE_LEADING_DIGIT && std::isdigit(static_cast<unsigned char>(token[0]))) {
            return;
        }

        // If configured, ignore pure hex numbers, with or without a 0x
        // prefix.
        if (IGNORE_HEX) {
            if (nonHexPos == std::string::npos) {
                // Implies hex without 0x prefix.
                return;
            }

            // This second hex test is redundant if we're ignoring tokens
            // with leading digits, and checking this first will cause the
            // check to be completely compiled away as IGNORE_LEADING_DIGIT
            // is a template argument
            if (!IGNORE_LEADING_DIGIT && nonHexPos == 1 &&
                token.compare(0, 2, "0x") == 0 && token.length() != 2) {
                // Implies hex with 0x prefix.
                return;
            }
        }

        // If the last character is not alphanumeric, strip it.
        while (!std::isalnum(static_cast<unsigned char>(token[token.length() - 1]))) {
            token.erase(token.length() - 1);
        }

        if (IGNORE_DATE_WORDS && core::CTimeUtils::isDateWord(token)) {
            return;
        }

        if (IGNORE_FIELD_NAMES && fields.find(token) != fields.end()) {
            return;
        }

        this->tokenToIdAndWeight(token, tokenIds, tokenUniqueIds, totalWeight,
                                 minReweightedTotalWeight, maxReweightedTotalWeight);
    }

private:
    //! Reference to a part-of-speech dictionary.
    const core::CWordDictionary& m_Dict;

    //! Used for determining the edit distance between two vectors of
    //! strings, i.e. how many insertions, deletions or changes would it
    //! take to convert one to the other
    core::CStringSimilarityTester m_SimilarityTester;

    //! Function used to increase weighting for dictionary words
    DICTIONARY_WEIGHT_FUNC m_DictionaryWeightFunc;
};
}
}

#endif // INCLUDED_ml_model_CTokenListDataCategorizer_h
