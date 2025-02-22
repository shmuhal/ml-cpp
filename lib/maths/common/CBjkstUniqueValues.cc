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

#include <maths/common/CBjkstUniqueValues.h>

#include <core/CLogger.h>
#include <core/CMemoryDef.h>
#include <core/CPersistUtils.h>
#include <core/RestoreMacros.h>
#include <core/WindowsSafe.h>

#include <maths/common/CChecksum.h>

#include <boost/operators.hpp>

#include <algorithm>
#include <iterator>

namespace ml {
namespace maths {
namespace common {
namespace {
namespace detail {

using TUInt8Vec = std::vector<std::uint8_t>;
using TUInt8VecItr = TUInt8Vec::iterator;
using TUInt8VecCItr = TUInt8Vec::const_iterator;

//! Convert the decomposition of the hash into two 8 bit integers
//! bask into the original hash value.
inline uint16_t from8Bit(std::uint8_t leading, std::uint8_t trailing) {
    // The C++ standard says that arithmetic on types smaller than int may be
    // done by converting to int, so cast this way to avoid compiler warnings
    return static_cast<uint16_t>((static_cast<unsigned int>(leading) << 8) + trailing);
}

using TUInt8UInt8Pr = std::pair<std::uint8_t, std::uint8_t>;

//! \brief Random access iterator wrapper for B set iterator.
//!
//! DESCRIPTION:\n
//! This automatically iterates through and extracts the 16 bit hash
//! from the appropriate 8 bit unsigned integer values in the B set
//! for the BJKST algorithm. It assumes that the values in the vector
//! are layed out as follows:
//! \code
//!   |<-----8 bits---->|<-----8 bits---->|<-----8 bits---->|
//!   |(g(x) >> 8) % 256|    g(x) % 256   |    zeros(x)     |
//! \endcode
// clang-format off
class EMPTY_BASE_OPT CHashIterator final
    : private boost::less_than_comparable<CHashIterator,
              boost::addable<CHashIterator, std::ptrdiff_t,
              boost::subtractable<CHashIterator, std::ptrdiff_t>>> {
    // clang-format on
public:
    using iterator_category = std::random_access_iterator_tag;
    using value_type = uint16_t;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = void;

public:
    CHashIterator() = default;
    explicit CHashIterator(TUInt8VecItr itr) : m_Itr(itr) {}

    TUInt8VecItr base() const { return m_Itr; }

    bool operator==(CHashIterator other) const { return m_Itr == other.m_Itr; }
    bool operator!=(CHashIterator other) const { return m_Itr != other.m_Itr; }
    bool operator<(CHashIterator other) const { return m_Itr < other.m_Itr; }

    uint16_t operator*() const { return from8Bit(*m_Itr, *(m_Itr + 1)); }
    const CHashIterator& operator++() {
        m_Itr += 3;
        return *this;
    }
    const CHashIterator operator++(int) {
        CHashIterator result(m_Itr);
        m_Itr += 3;
        return result;
    }
    const CHashIterator& operator--() {
        m_Itr -= 3;
        return *this;
    }
    CHashIterator operator--(int) {
        CHashIterator result(m_Itr);
        m_Itr -= 3;
        return result;
    }
    uint16_t operator[](std::ptrdiff_t n) const {
        auto itr = m_Itr + 3 * n;
        return from8Bit(*itr, *(itr + 1));
    }
    const CHashIterator& operator+=(std::ptrdiff_t n) {
        m_Itr += 3 * n;
        return *this;
    }
    const CHashIterator& operator-=(std::ptrdiff_t n) {
        m_Itr -= 3 * n;
        return *this;
    }
    std::ptrdiff_t operator-(const CHashIterator& other) const {
        return (m_Itr - other.m_Itr) / 3;
    }

private:
    TUInt8VecItr m_Itr;
};

bool insert(TUInt8Vec& b, uint16_t g, std::uint8_t zeros) {
    // This uses the fact that the set "b" is laid out as follows:
    //  |<---8 bits--->|<---8 bits--->|<---8 bits--->|
    //  |(g >> 8) % 256|    g % 256   |    zeros     |

    CHashIterator lb =
        std::lower_bound(CHashIterator(b.begin()), CHashIterator(b.end()), g);
    if (lb.base() != b.end() && *lb == g) {
        // We've got this value in the set. Update the zeros,
        // which may have changed if the h hash has changed.
        *(lb.base() + 2) = zeros;
        return false;
    }

    // We need to insert the new values immediately before the
    // lower bound iterator. Note that it is more efficient to
    // insert space for all three values in one operation because
    // each insertion might require a reallocation and always
    // requires values after in b to be copied to their new
    // positions.

    std::ptrdiff_t i = lb.base() - b.begin();
    auto g1 = static_cast<std::uint8_t>(g >> 8);
    auto g2 = static_cast<std::uint8_t>(g);
    LOG_TRACE(<< "Adding g = " << g << " at " << i
              << " (g1 = " << static_cast<std::uint32_t>(g1)
              << ", g2 = " << static_cast<std::uint32_t>(g2) << ")");

    b.insert(lb.base(), 3u, std::uint8_t());
    b[i] = g1;
    b[i + 1] = g2;
    b[i + 2] = zeros;
    return true;
}

void remove(TUInt8Vec& b, uint16_t g) {
    // This uses the fact that the set "b" is laid out as follows:
    //  |<---8 bits--->|<---8 bits--->|<---8 bits--->|
    //  |(g >> 8) % 256|    g % 256   |    zeros     |

    CHashIterator lb =
        std::lower_bound(CHashIterator(b.begin()), CHashIterator(b.end()), g);
    if (lb.base() != b.end() && *lb == g) {
        // We've got this value in the set.
        b.erase(lb.base(), lb.base() + 3);
    }
}

void prune(TUInt8Vec& b, std::uint8_t z) {
    // This uses the fact that the set "b" is laid out as follows:
    //  |<---8 bits--->|<---8 bits--->|<---8 bits--->|
    //  |(g >> 8) % 256|    g % 256   |    zeros     |

    std::size_t j = 0;
    for (std::size_t i = 0; i < b.size(); i += 3) {
        if (b[i + 2] >= z) {
            b[j] = b[i];
            b[j + 1] = b[i + 1];
            b[j + 2] = b[i + 2];
            j += 3;
        } else {
            LOG_TRACE(<< "Removing " << from8Bit(b[i], b[i + 1])
                      << ", zeros =  " << static_cast<std::uint32_t>(b[i + 2])
                      << ", z = " << static_cast<std::uint32_t>(z));
        }
    }
    b.erase(b.begin() + j, b.end());
}

} // detail::

using TOptionalSize = std::optional<std::size_t>;

const char DELIMITER(':');
const char PAIR_DELIMITER(';');
const std::string MAX_SIZE_TAG("a");
const std::string NUMBER_HASHES_TAG("b");
const std::string VALUES_TAG("c");
const std::string SKETCH_TAG("d");

// Nested tags.
const std::string HASH_H_TAG("a");
const std::string HASH_G_TAG("b");
const std::string Z_TAG("c");
const std::string B_TAG("d");

//! Casting conversion to a string.
template<typename U>
class CToString {
public:
    template<typename V>
    std::string operator()(V value) const {
        return core::CStringUtils::typeToString(static_cast<U>(value));
    }
};

//! Casting initialization from string.
template<typename U>
class CFromString {
public:
    template<typename V>
    bool operator()(const std::string& token, V& value) const {
        U value_;
        if (core::CStringUtils::stringToType(token, value_)) {
            value = static_cast<V>(value_);
            return true;
        }
        return false;
    }
};
}

std::uint8_t CBjkstUniqueValues::trailingZeros(std::uint32_t value) {
    if (value == 0) {
        return 32;
    }

    // This is just doing a binary search for the first
    // non-zero bit.

    static const std::uint32_t MASKS[]{0xffff, 0xff, 0xf, 0x3, 0x1};
    static const std::uint8_t SHIFTS[]{16, 8, 4, 2, 1};

    std::uint8_t result = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        switch (value & MASKS[i]) {
        case 0:
            value >>= SHIFTS[i];
            result = static_cast<std::uint8_t>(result + SHIFTS[i]);
            break;
        default:
            break;
        }
    }

    return result;
}

CBjkstUniqueValues::CBjkstUniqueValues(std::size_t numberHashes, std::size_t maxSize)
    : m_MaxSize(maxSize), m_NumberHashes(numberHashes), m_Sketch(TUInt32Vec()) {
}

CBjkstUniqueValues::CBjkstUniqueValues(core::CStateRestoreTraverser& traverser)
    : m_MaxSize(0), m_NumberHashes(0) {
    if (traverser.traverseSubLevel([this](auto& traverser_) {
            return this->acceptRestoreTraverser(traverser_);
        }) == false) {
        traverser.setBadState();
    }
}

void CBjkstUniqueValues::swap(CBjkstUniqueValues& other) noexcept {
    if (this == &other) {
        return;
    }

    std::swap(m_MaxSize, other.m_MaxSize);
    std::swap(m_NumberHashes, other.m_NumberHashes);

    try {
        TUInt32Vec* values = std::get_if<TUInt32Vec>(&m_Sketch);
        if (values != nullptr) {
            TUInt32Vec* otherValues = std::get_if<TUInt32Vec>(&other.m_Sketch);
            if (otherValues != nullptr) {
                values->swap(*otherValues);
            } else {
                auto& otherSketch = std::get<SSketch>(other.m_Sketch);
                TUInt32Vec tmp;
                tmp.swap(*values);
                m_Sketch = SSketch();
                std::get<SSketch>(m_Sketch).swap(otherSketch);
                other.m_Sketch = TUInt32Vec();
                std::get<TUInt32Vec>(other.m_Sketch).swap(tmp);
            }
        } else {
            auto& sketch = std::get<SSketch>(m_Sketch);
            SSketch* otherSketch = std::get_if<SSketch>(&other.m_Sketch);
            if (otherSketch != nullptr) {
                sketch.swap(*otherSketch);
            } else {
                auto& otherValues = std::get<TUInt32Vec>(other.m_Sketch);
                TUInt32Vec tmp;
                tmp.swap(otherValues);
                other.m_Sketch = SSketch();
                std::get<SSketch>(other.m_Sketch).swap(sketch);
                m_Sketch = TUInt32Vec();
                std::get<TUInt32Vec>(m_Sketch).swap(tmp);
            }
        }
    } catch (const std::exception& e) {
        LOG_ABORT(<< "Unexpected exception: " << e.what());
    }
}

bool CBjkstUniqueValues::acceptRestoreTraverser(core::CStateRestoreTraverser& traverser) {
    do {
        const std::string& name = traverser.name();
        RESTORE_BUILT_IN(MAX_SIZE_TAG, m_MaxSize)
        RESTORE_BUILT_IN(NUMBER_HASHES_TAG, m_NumberHashes)
        if (name == VALUES_TAG) {
            m_Sketch = TUInt32Vec();
            auto& values = std::get<TUInt32Vec>(m_Sketch);
            if (core::CPersistUtils::fromString(traverser.value(), values, DELIMITER) == false) {
                return false;
            }
            continue;
        }
        if (name == SKETCH_TAG) {
            m_Sketch = SSketch();
            auto& sketch = std::get<SSketch>(m_Sketch);
            sketch.s_G.reserve(m_NumberHashes);
            sketch.s_H.reserve(m_NumberHashes);
            sketch.s_Z.reserve(m_NumberHashes);
            sketch.s_B.reserve(m_NumberHashes);
            if (traverser.traverseSubLevel([&](auto& traverser_) {
                    return sketch.acceptRestoreTraverser(traverser_, m_NumberHashes);
                }) == false) {
                return false;
            }
            continue;
        }
    } while (traverser.next());

    this->checkRestoredInvariants();

    return true;
}

void CBjkstUniqueValues::checkRestoredInvariants() const {
    const SSketch* sketch = std::get_if<SSketch>(&m_Sketch);
    if (sketch != nullptr) {
        VIOLATES_INVARIANT(sketch->s_G.size(), !=, sketch->s_H.size());
        VIOLATES_INVARIANT(sketch->s_H.size(), !=, sketch->s_Z.size());
        VIOLATES_INVARIANT(sketch->s_Z.size(), !=, sketch->s_B.size());
        VIOLATES_INVARIANT(sketch->s_B.size(), !=, m_NumberHashes);
    }
}

void CBjkstUniqueValues::acceptPersistInserter(core::CStatePersistInserter& inserter) const {
    inserter.insertValue(MAX_SIZE_TAG, m_MaxSize);
    inserter.insertValue(NUMBER_HASHES_TAG, m_NumberHashes);

    const TUInt32Vec* values = std::get_if<TUInt32Vec>(&m_Sketch);
    if (values != nullptr) {
        inserter.insertValue(VALUES_TAG, core::CPersistUtils::toString(*values, DELIMITER));
    } else {
        try {
            const auto& sketch = std::get<SSketch>(m_Sketch);
            inserter.insertLevel(SKETCH_TAG, [&sketch](auto& inserter_) {
                sketch.acceptPersistInserter(inserter_);
            });
        } catch (const std::exception& e) {
            LOG_ABORT(<< "Unexpected exception: " << e.what());
        }
    }
}

void CBjkstUniqueValues::add(std::uint32_t value) {
    TUInt32Vec* values = std::get_if<TUInt32Vec>(&m_Sketch);
    if (values != nullptr) {
        auto i = std::lower_bound(values->begin(), values->end(), value);
        if (i == values->end() || *i != value) {
            values->insert(i, value);
        }
        this->sketch();
    } else {
        try {
            auto& sketch = std::get<SSketch>(m_Sketch);
            sketch.add(m_MaxSize, value);
        } catch (const std::exception& e) {
            LOG_ABORT(<< "Unexpected exception: " << e.what());
        }
    }
}

void CBjkstUniqueValues::remove(std::uint32_t value) {
    TUInt32Vec* values = std::get_if<TUInt32Vec>(&m_Sketch);
    if (values != nullptr) {
        auto i = std::lower_bound(values->begin(), values->end(), value);
        if (i != values->end() && *i == value) {
            values->erase(i);
        }
    } else {
        try {
            auto& sketch = std::get<SSketch>(m_Sketch);
            sketch.remove(value);
        } catch (const std::exception& e) {
            LOG_ABORT(<< "Unexpected exception: " << e.what());
        }
    }
}

std::uint32_t CBjkstUniqueValues::number() const {
    const TUInt32Vec* values = std::get_if<TUInt32Vec>(&m_Sketch);
    if (values == nullptr) {
        try {
            const auto& sketch = std::get<SSketch>(m_Sketch);
            return sketch.number();
        } catch (const std::exception& e) {
            LOG_ABORT(<< "Unexpected exception: " << e.what());
        }
    }
    return static_cast<std::uint32_t>(values->size());
}

std::uint64_t CBjkstUniqueValues::checksum(std::uint64_t seed) const {
    seed = CChecksum::calculate(seed, m_MaxSize);
    seed = CChecksum::calculate(seed, m_NumberHashes);
    const TUInt32Vec* values = std::get_if<TUInt32Vec>(&m_Sketch);
    if (values == nullptr) {
        try {
            const auto& sketch = std::get<SSketch>(m_Sketch);
            seed = CChecksum::calculate(seed, sketch.s_G);
            seed = CChecksum::calculate(seed, sketch.s_H);
            seed = CChecksum::calculate(seed, sketch.s_Z);
            return CChecksum::calculate(seed, sketch.s_B);
        } catch (const std::exception& e) {
            LOG_ABORT(<< "Unexpected exception: " << e.what());
        }
    }
    return CChecksum::calculate(seed, *values);
}

void CBjkstUniqueValues::debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const {
    mem->setName("CBjkstUniqueValues");
    const TUInt32Vec* values = std::get_if<TUInt32Vec>(&m_Sketch);
    if (values != nullptr) {
        core::CMemoryDebug::dynamicSize("values", *values, mem);
    } else {
        try {
            const auto& sketch = std::get<SSketch>(m_Sketch);
            mem->addItem("SSketch", sizeof(SSketch));
            core::CMemoryDebug::dynamicSize("sketch.s_G", sketch.s_G, mem);
            core::CMemoryDebug::dynamicSize("sketch.s_H", sketch.s_H, mem);
            core::CMemoryDebug::dynamicSize("sketch.s_Z", sketch.s_Z, mem);
            core::CMemoryDebug::dynamicSize("sketch.s_B", sketch.s_B, mem);
        } catch (const std::exception& e) {
            LOG_ABORT(<< "Unexpected exception: " << e.what());
        }
    }
}

std::size_t CBjkstUniqueValues::memoryUsage() const {
    std::size_t mem = 0;
    const TUInt32Vec* values = std::get_if<TUInt32Vec>(&m_Sketch);
    if (values != nullptr) {
        mem += core::CMemory::dynamicSize(*values);
    } else {
        try {
            const auto& sketch = std::get<SSketch>(m_Sketch);
            mem += sizeof(SSketch);
            mem += core::CMemory::dynamicSize(sketch.s_G);
            mem += core::CMemory::dynamicSize(sketch.s_H);
            mem += core::CMemory::dynamicSize(sketch.s_Z);
            mem += core::CMemory::dynamicSize(sketch.s_B);
        } catch (const std::exception& e) {
            LOG_ABORT(<< "Unexpected exception: " << e.what());
        }
    }
    return mem;
}

void CBjkstUniqueValues::sketch() {
    static const std::size_t UINT8_SIZE = sizeof(std::uint8_t);
    static const std::size_t UINT32_SIZE = sizeof(std::uint32_t);
    static const std::size_t HASH_SIZE =
        sizeof(core::CHashing::CUniversalHash::CUInt32UnrestrictedHash);
    static const std::size_t VEC8_SIZE = sizeof(TUInt8Vec);
    static const std::size_t VEC32_SIZE = sizeof(TUInt32Vec);
    static const std::size_t SKETCH_SIZE = sizeof(SSketch);

    TUInt32Vec* values = std::get_if<TUInt32Vec>(&m_Sketch);
    if (values != nullptr) {
        std::size_t valuesSize = VEC32_SIZE + UINT32_SIZE * values->capacity();
        std::size_t sketchSize =
            SKETCH_SIZE + m_NumberHashes * (2 * HASH_SIZE + 1 * UINT8_SIZE +
                                            1 * VEC8_SIZE + 3 * m_MaxSize * UINT8_SIZE);
        if (valuesSize > sketchSize) {
            if (values->capacity() > values->size() &&
                values->size() < (sketchSize - VEC32_SIZE) / UINT32_SIZE) {
                TUInt32Vec shrunk;
                shrunk.reserve((sketchSize - VEC32_SIZE) / UINT32_SIZE);
                shrunk.assign(values->begin(), values->end());
                values->swap(shrunk);
                return;
            }

            LOG_TRACE(<< "Sketching " << values->size() << " values");

            TUInt32Vec values_;
            values_.swap(*values);
            m_Sketch = SSketch(m_NumberHashes);
            for (std::size_t i = 0; i < values_.size(); ++i) {
                this->add(values_[i]);
            }
        }
    }
}

CBjkstUniqueValues::SSketch::SSketch() = default;

CBjkstUniqueValues::SSketch::SSketch(std::size_t numberHashes) {
    core::CHashing::CUniversalHash::generateHashes(numberHashes, s_G);
    core::CHashing::CUniversalHash::generateHashes(numberHashes, s_H);
    s_Z.resize(numberHashes, 0);
    s_B.resize(numberHashes, TUInt8Vec());
}

void CBjkstUniqueValues::SSketch::swap(SSketch& other) noexcept {
    s_G.swap(other.s_G);
    s_H.swap(other.s_H);
    s_Z.swap(other.s_Z);
    s_B.swap(other.s_B);
}

bool CBjkstUniqueValues::SSketch::acceptRestoreTraverser(core::CStateRestoreTraverser& traverser,
                                                         std::size_t numberHashes) {
    core::CHashing::CUniversalHash::CFromString hashFromString(PAIR_DELIMITER);
    do {
        const std::string& name = traverser.name();
        if (name == HASH_G_TAG) {
            if (core::CPersistUtils::fromString(traverser.value(), hashFromString,
                                                s_G, DELIMITER) == false ||
                s_G.size() != numberHashes) {
                LOG_ERROR(<< "Invalid hashes in " << traverser.value());
                return false;
            }
        } else if (name == HASH_H_TAG) {
            if (core::CPersistUtils::fromString(traverser.value(), hashFromString,
                                                s_H, DELIMITER) == false ||
                s_H.size() != numberHashes) {
                LOG_ERROR(<< "Invalid hashes in " << traverser.value());
                return false;
            }
        } else if (name == Z_TAG) {
            if (core::CPersistUtils::fromString(traverser.value(), CFromString<int>(),
                                                s_Z, DELIMITER) == false ||
                s_Z.size() != numberHashes) {
                LOG_ERROR(<< "Invalid zeros in " << traverser.value());
                return false;
            }
        } else if (name == B_TAG) {
            s_B.push_back(TUInt8Vec());
            if (core::CPersistUtils::fromString(traverser.value(), CFromString<int>(),
                                                s_B.back(), DELIMITER) == false) {
                LOG_ERROR(<< "Invalid values in " << traverser.value());
                return false;
            }
        }
    } while (traverser.next());

    if (s_B.size() != numberHashes) {
        LOG_ERROR(<< "Invalid number of rows " << s_B.size() << " expected " << numberHashes);
        return false;
    }

    return true;
}

void CBjkstUniqueValues::SSketch::acceptPersistInserter(core::CStatePersistInserter& inserter) const {
    core::CHashing::CUniversalHash::CToString hashToString(PAIR_DELIMITER);
    inserter.insertValue(
        HASH_G_TAG, core::CPersistUtils::toString(s_G, hashToString, DELIMITER));
    inserter.insertValue(
        HASH_H_TAG, core::CPersistUtils::toString(s_H, hashToString, DELIMITER));
    inserter.insertValue(
        Z_TAG, core::CPersistUtils::toString(s_Z, CToString<int>(), DELIMITER));
    for (std::size_t i = 0; i < s_B.size(); ++i) {
        inserter.insertValue(B_TAG, core::CPersistUtils::toString(
                                        s_B[i], CToString<int>(), DELIMITER));
    }
}

void CBjkstUniqueValues::SSketch::add(std::size_t maxSize, std::uint32_t value) {
    LOG_TRACE(<< "Adding " << value);
    for (std::size_t i = 0; i < s_Z.size(); ++i) {
        std::uint8_t zeros = trailingZeros((s_H[i])(value));
        if (zeros >= s_Z[i]) {
            TUInt8Vec& b = s_B[i];
            auto g = static_cast<uint16_t>((s_G[i])(value));
            LOG_TRACE(<< "g = " << g << ", zeros = " << static_cast<std::uint32_t>(zeros));
            if (detail::insert(b, g, zeros)) {
                while (b.size() >= 3 * maxSize) {
                    ++s_Z[i];
                    detail::prune(b, s_Z[i]);
                }
                if (b.capacity() >= 3 * maxSize) {
                    TUInt8Vec shrunk;
                    shrunk.reserve(3 * maxSize);
                    shrunk.assign(b.begin(), b.end());
                    b.swap(shrunk);
                }
                LOG_TRACE(<< "|B| = " << b.size()
                          << ", z = " << static_cast<std::uint32_t>(s_Z[i]));
            }
        }
    }
}

void CBjkstUniqueValues::SSketch::remove(std::uint32_t value) {
    for (std::size_t i = 0; i < s_Z.size(); ++i) {
        std::uint8_t zeros = trailingZeros((s_H[i])(value));
        if (zeros >= s_Z[i]) {
            TUInt8Vec& b = s_B[i];
            auto g = static_cast<uint16_t>((s_G[i])(value));
            LOG_TRACE(<< "g = " << g << ", zeros = " << static_cast<std::uint32_t>(zeros));
            detail::remove(b, g);
        }
    }
}

std::uint32_t CBjkstUniqueValues::SSketch::number() const {
    // This uses the median trick to reduce the error.
    TUInt32Vec estimates;
    estimates.reserve(s_Z.size());
    for (std::size_t i = 0; i < s_Z.size(); ++i) {
        LOG_TRACE(<< "|B| = " << s_B[i].size()
                  << ", z = " << static_cast<std::uint32_t>(s_Z[i]));
        estimates.push_back(static_cast<std::uint32_t>(s_B[i].size() / 3) * (1 << s_Z[i]));
    }

    LOG_TRACE(<< "estimates = " << estimates);

    std::size_t n = estimates.size();
    if (n % 2 == 0) {
        std::partial_sort(estimates.begin(), estimates.begin() + n / 2 + 1,
                          estimates.end());
        return (estimates[n / 2] + estimates[n / 2 - 1]) / 2;
    }

    std::nth_element(estimates.begin(), estimates.begin() + n / 2, estimates.end());
    return estimates[n / 2];
}
}
}
}
