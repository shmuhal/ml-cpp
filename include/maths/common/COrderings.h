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

#ifndef INCLUDED_ml_maths_common_COrderings_h
#define INCLUDED_ml_maths_common_COrderings_h

#include <core/CNonInstantiatable.h>
#include <core/CStoredStringPtr.h>
#include <core/UnwrapRef.h>

#include <memory>
#include <optional>
#include <utility>

namespace ml {
namespace core {
template<typename VECTOR>
class CVectorRange;
}
namespace maths {
namespace common {
//! \brief A collection of useful functionality to order collections
//! of objects.
//!
//! DESCRIPTION:\n
//! This implements some generic commonly occurring ordering functionality.
//! In particular,
//!   -# Lexicographical compare for small collections of objects with
//!      distinct types (std::lexicographical_compare only supports a
//!      single type).
//!   -# Fast (partial) orderings on pairs which only compare the first
//!      or second element of the pair.
//!   -# Efficiently, O(N log(N)), simultaneously sorting multiple vectors
//!      using one of the vectors to provide the ordering.
class COrderings : private core::CNonInstantiatable {
public:
    //! \brief Orders two optional values such that non-null are
    //! less than null values.
    //! less than null values and otherwise compares using the type
    //! operator <.
    struct SOptionalLess {
        //! \note U and V must be convertible to T or optional<T>
        //! for some type T and T must support operator <.
        template<typename U, typename V>
        inline bool operator()(const U& lhs, const V& rhs) const {
            return less(lhs, rhs);
        }

        template<typename T>
        static inline bool less(const std::optional<T>& lhs, const std::optional<T>& rhs) {
            bool lInitialized(lhs);
            bool rInitialized(rhs);
            return lInitialized && rInitialized
                       ? core::unwrap_ref(*lhs) < core::unwrap_ref(*rhs)
                       : rInitialized < lInitialized;
        }
        template<typename T>
        static inline bool less(const T& lhs, const std::optional<T>& rhs) {
            return !rhs ? true : core::unwrap_ref(lhs) < core::unwrap_ref(*rhs);
        }
        template<typename T>
        static inline bool less(const std::optional<T>& lhs, const T& rhs) {
            return !lhs ? false : core::unwrap_ref(*lhs) < core::unwrap_ref(rhs);
        }
    };

    //! \brief Orders two optional values such that null are greater
    //! than non-null values and otherwise compares using the type
    //! operator >.
    struct SOptionalGreater {
        //! \note U and V must be convertible to T or optional<T>
        //! for some type T and T must support operator >.
        template<typename U, typename V>
        inline bool operator()(const U& lhs, const V& rhs) const {
            return greater(lhs, rhs);
        }

        template<typename T>
        static inline bool
        greater(const std::optional<T>& lhs, const std::optional<T>& rhs) {
            bool lInitialized(lhs);
            bool rInitialized(rhs);
            return lInitialized && rInitialized
                       ? core::unwrap_ref(*lhs) > core::unwrap_ref(*rhs)
                       : rInitialized > lInitialized;
        }
        template<typename T>
        static inline bool greater(const T& lhs, const std::optional<T>& rhs) {
            return !rhs ? false : core::unwrap_ref(lhs) > core::unwrap_ref(*rhs);
        }
        template<typename T>
        static inline bool greater(const std::optional<T>& lhs, const T& rhs) {
            return !lhs ? true : core::unwrap_ref(*lhs) > core::unwrap_ref(rhs);
        }
    };

    //! \brief Orders two pointers such that non-null are less
    //! than null values and otherwise compares using the type
    //! operator <.
    struct SPtrLess {
        template<typename T>
        inline bool operator()(const T* lhs, const T* rhs) const {
            return less(lhs, rhs);
        }

        template<typename T>
        static inline bool less(const T* lhs, const T* rhs) {
            bool lInitialized(lhs != nullptr);
            bool rInitialized(rhs != nullptr);
            return lInitialized && rInitialized
                       ? core::unwrap_ref(*lhs) < core::unwrap_ref(*rhs)
                       : rInitialized < lInitialized;
        }
    };

    //! \brief Orders two pointers such that null are greater
    //! than non-null values and otherwise compares using
    //! the type operator >.
    struct SPtrGreater {
        template<typename T>
        inline bool operator()(const T* lhs, const T* rhs) const {
            return greater(lhs, rhs);
        }

        template<typename T>
        static inline bool greater(const T* lhs, const T* rhs) {
            bool lInitialized(lhs != nullptr);
            bool rInitialized(rhs != nullptr);
            return lInitialized && rInitialized
                       ? core::unwrap_ref(*lhs) > core::unwrap_ref(*rhs)
                       : rInitialized > lInitialized;
        }
    };

    //! \brief Orders two reference wrapped objects which are
    //! comparable with operator <.
    struct SReferenceLess {
        template<typename U, typename V>
        inline bool operator()(const U& lhs, const V& rhs) const {
            return less(lhs, rhs);
        }

        template<typename U, typename V>
        static inline bool less(const U& lhs, const V& rhs) {
            return core::unwrap_ref(lhs) < core::unwrap_ref(rhs);
        }
    };

    //! \brief Orders two reference wrapped objects which are
    //! comparable with operator >.
    struct SReferenceGreater {
        template<typename U, typename V>
        inline bool operator()(const U& lhs, const V& rhs) const {
            return greater(lhs, rhs);
        }

        template<typename U, typename V>
        static inline bool greater(const U& lhs, const V& rhs) {
            return core::unwrap_ref(lhs) > core::unwrap_ref(rhs);
        }
    };

    //! \name Mixed Type Lexicographical Comparison
    //!
    //! This is equivalent to std::lexicographical_compare but allows
    //! for the type of each value in the collection to be different.
    //! Each type must define operator<.
    //@{
    //! Lexicographical comparison of \p l1 and \p r1.
    template<typename T1, typename COMP>
    static bool lexicographical_compare(const T1& l1, const T1& r1, COMP comp) {
        return comp(l1, r1);
    }
    template<typename T1>
    static bool lexicographical_compare(const T1& l1, const T1& r1) {
        return lexicographical_compare(l1, r1, SReferenceLess());
    }
#define COMPARE(l, r)                                                          \
    if (comp(l, r)) {                                                          \
        return true;                                                           \
    } else if (comp(r, l)) {                                                   \
        return false;                                                          \
    }
    //! Lexicographical comparison of (\p l1, \p l2) and (\p r1, \p r2).
    template<typename T1, typename T2, typename COMP>
    static bool
    lexicographical_compare(const T1& l1, const T2& l2, const T1& r1, const T2& r2, COMP comp) {
        COMPARE(l1, r1);
        return comp(l2, r2);
    }
    template<typename T1, typename T2>
    static bool
    lexicographical_compare(const T1& l1, const T2& l2, const T1& r1, const T2& r2) {
        return lexicographical_compare(l1, l2, r1, r2, SReferenceLess());
    }
    //! Lexicographical comparison of (\p l1, \p l2, \p l3) and (\p r1, \p r2, \p r3).
    template<typename T1, typename T2, typename T3, typename COMP>
    static bool lexicographical_compare(const T1& l1,
                                        const T2& l2,
                                        const T3& l3,
                                        const T1& r1,
                                        const T2& r2,
                                        const T3& r3,
                                        COMP comp) {
        COMPARE(l1, r1);
        COMPARE(l2, r2);
        return comp(l3, r3);
    }
    template<typename T1, typename T2, typename T3>
    static bool lexicographical_compare(const T1& l1,
                                        const T2& l2,
                                        const T3& l3,
                                        const T1& r1,
                                        const T2& r2,
                                        const T3& r3) {
        return lexicographical_compare(l1, l2, l3, r1, r2, r3, SReferenceLess());
    }
    //! Lexicographical comparison of (\p l1, \p l2, \p l3, \p l4) and
    //! (\p r1, \p r2, \p r3, \p r4).
    template<typename T1, typename T2, typename T3, typename T4, typename COMP>
    static bool lexicographical_compare(const T1& l1,
                                        const T2& l2,
                                        const T3& l3,
                                        const T4& l4,
                                        const T1& r1,
                                        const T2& r2,
                                        const T3& r3,
                                        const T4& r4,
                                        COMP comp) {
        COMPARE(l1, r1);
        COMPARE(l2, r2);
        COMPARE(l3, r3);
        return comp(l4, r4);
    }
    template<typename T1, typename T2, typename T3, typename T4>
    static bool lexicographical_compare(const T1& l1,
                                        const T2& l2,
                                        const T3& l3,
                                        const T4& l4,
                                        const T1& r1,
                                        const T2& r2,
                                        const T3& r3,
                                        const T4& r4) {
        return lexicographical_compare(l1, l2, l3, l4, r1, r2, r3, r4, SReferenceLess());
    }
    //! Lexicographical comparison of (\p l1, \p l2, \p l3, \p l4, \p l5) and
    //! (\p r1, \p r2, \p r3, \p r4, \p r5).
    template<typename T1, typename T2, typename T3, typename T4, typename T5, typename COMP>
    static bool lexicographical_compare(const T1& l1,
                                        const T2& l2,
                                        const T3& l3,
                                        const T4& l4,
                                        const T5& l5,
                                        const T1& r1,
                                        const T2& r2,
                                        const T3& r3,
                                        const T4& r4,
                                        const T5& r5,
                                        COMP comp) {
        COMPARE(l1, r1);
        COMPARE(l2, r2);
        COMPARE(l3, r3);
        COMPARE(l4, r4);
        return comp(l5, r5);
    }
    template<typename T1, typename T2, typename T3, typename T4, typename T5>
    static bool lexicographical_compare(const T1& l1,
                                        const T2& l2,
                                        const T3& l3,
                                        const T4& l4,
                                        const T5& l5,
                                        const T1& r1,
                                        const T2& r2,
                                        const T3& r3,
                                        const T4& r4,
                                        const T5& r5) {
        return lexicographical_compare(l1, l2, l3, l4, l5, r1, r2, r3, r4, r5,
                                       SReferenceLess());
    }
#undef COMPARE
    //@}

    //! \brief Wrapper around various less than comparisons.
    struct SLess {
        template<typename T>
        bool operator()(const std::optional<T>& lhs, const std::optional<T>& rhs) const {
            return SOptionalLess::less(lhs, rhs);
        }

        template<typename T>
        bool operator()(const T* lhs, const T* rhs) const {
            return SPtrLess::less(lhs, rhs);
        }

        template<typename T>
        bool operator()(T* lhs, T* rhs) const {
            return SPtrLess::less(lhs, rhs);
        }

        template<typename U, typename V>
        bool operator()(const U& lhs, const V& rhs) const {
            return SReferenceLess::less(lhs, rhs);
        }

        bool operator()(const core::CStoredStringPtr& lhs, const core::CStoredStringPtr& rhs) {
            return SPtrLess::less(lhs.get(), rhs.get());
        }

        template<typename T>
        bool operator()(const std::shared_ptr<T>& lhs, const std::shared_ptr<T>& rhs) {
            return SPtrLess::less(lhs.get(), rhs.get());
        }

        template<typename T>
        bool operator()(const std::unique_ptr<T>& lhs, const std::unique_ptr<T>& rhs) {
            return SPtrLess::less(lhs.get(), rhs.get());
        }

        template<typename U, typename V>
        bool operator()(const std::pair<U, V>& lhs, const std::pair<U, V>& rhs) const {
            return lexicographical_compare(lhs.first, lhs.second, rhs.first,
                                           rhs.second, *this);
        }
    };

    //! \brief Wrapper around various less than comparisons.
    struct SGreater {
        template<typename T>
        bool operator()(const std::optional<T>& lhs, const std::optional<T>& rhs) const {
            return SOptionalGreater::greater(lhs, rhs);
        }

        template<typename T>
        bool operator()(const T* lhs, const T* rhs) const {
            return SPtrGreater::greater(lhs, rhs);
        }

        template<typename T>
        bool operator()(T* lhs, T* rhs) const {
            return SPtrGreater::greater(lhs, rhs);
        }

        template<typename U, typename V>
        bool operator()(const U& lhs, const V& rhs) const {
            return SReferenceGreater::greater(lhs, rhs);
        }

        bool operator()(const core::CStoredStringPtr& lhs, const core::CStoredStringPtr& rhs) {
            return SPtrGreater::greater(lhs.get(), rhs.get());
        }

        template<typename T>
        bool operator()(const std::shared_ptr<T>& lhs, const std::shared_ptr<T>& rhs) {
            return SPtrGreater::greater(lhs.get(), rhs.get());
        }

        template<typename T>
        bool operator()(const std::unique_ptr<T>& lhs, const std::unique_ptr<T>& rhs) {
            return SPtrLess::less(lhs.get(), rhs.get());
        }

        template<typename U, typename V>
        bool operator()(const std::pair<U, V>& lhs, const std::pair<U, V>& rhs) const {
            return lexicographical_compare(lhs.first, lhs.second, rhs.first,
                                           rhs.second, *this);
        }
    };

    //! Lexicographical comparison of various common types.
    //!
    //! IMPLEMENTATION DECISIONS:\n
    //! Although pair provides its own comparison operator it doesn't properly
    //! handle pairs of reference wrapped types.
    struct SLexicographicalCompare {
        template<typename T1, typename T2>
        inline bool operator()(const std::pair<T1, T2>& lhs,
                               const std::pair<T1, T2>& rhs) const {
            return lexicographical_compare(lhs.first, lhs.second, rhs.first,
                                           rhs.second, s_Less);
        }

        SLess s_Less;
    };

    //! \brief Partial ordering of std::pairs on smaller first element.
    //!
    //! \note That while this functionality can be implemented by boost
    //! bind, since it overloads the comparison operators, the resulting
    //! code is more than an order of magnitude slower than this version.
    struct SFirstLess {
        template<typename U, typename V>
        inline bool operator()(const std::pair<U, V>& lhs, const std::pair<U, V>& rhs) const {
            return s_Less(lhs.first, rhs.first);
        }

        template<typename U, typename V>
        inline bool operator()(const U& lhs, const std::pair<U, V>& rhs) const {
            return s_Less(lhs, rhs.first);
        }

        template<typename U, typename V>
        inline bool operator()(const std::pair<U, V>& lhs, const U& rhs) const {
            return s_Less(lhs.first, rhs);
        }

        SLess s_Less;
    };

    //! \brief Partial ordering of std::pairs based on larger first element.
    //!
    //! \note That while this functionality can be implemented by bind
    //! bind, since it overloads the comparison operators, the resulting
    //! code is more than an order of magnitude slower than this version.
    struct SFirstGreater {
        template<typename U, typename V>
        inline bool operator()(const std::pair<U, V>& lhs, const std::pair<U, V>& rhs) const {
            return s_Greater(lhs.first, rhs.first);
        }

        template<typename U, typename V>
        inline bool operator()(const U& lhs, const std::pair<U, V>& rhs) const {
            return s_Greater(lhs, rhs.first);
        }

        template<typename U, typename V>
        inline bool operator()(const std::pair<U, V>& lhs, const U& rhs) const {
            return s_Greater(lhs.first, rhs);
        }

        SGreater s_Greater;
    };

    //! \brief Partial ordering of pairs based on smaller second element.
    //!
    //! \note That while this functionality can be implemented by boost
    //! bind, since it overloads the comparison operators, the resulting
    //! code is more than an order of magnitude slower than this version.
    struct SSecondLess {
        template<typename U, typename V>
        inline bool operator()(const std::pair<U, V>& lhs, const std::pair<U, V>& rhs) const {
            return s_Less(lhs.second, rhs.second);
        }

        template<typename U, typename V>
        inline bool operator()(const V& lhs, const std::pair<U, V>& rhs) const {
            return s_Less(lhs, rhs.second);
        }

        template<typename U, typename V>
        inline bool operator()(const std::pair<U, V>& lhs, const V& rhs) const {
            return s_Less(lhs.second, rhs);
        }

        SLess s_Less;
    };

    //! \brief Partial ordering of pairs based on larger second element.
    //!
    //! \note That while this functionality can be implemented by boost
    //! bind, since it overloads the comparison operators, the resulting
    //! code is more than an order of magnitude slower than this version.
    struct SSecondGreater {
        template<typename U, typename V>
        inline bool operator()(const std::pair<U, V>& lhs, const std::pair<U, V>& rhs) const {
            return s_Greater(lhs.second, rhs.second);
        }

        template<typename U, typename V>
        inline bool operator()(const V& lhs, const std::pair<U, V>& rhs) const {
            return s_Greater(lhs, rhs.second);
        }

        template<typename U, typename V>
        inline bool operator()(const std::pair<U, V>& lhs, const V& rhs) const {
            return s_Greater(lhs.second, rhs);
        }

        SGreater s_Greater;
    };

    //! \name Simultaneously Sort Multiple Vectors
    //!
    //! This simultaneously sorts a number of vectors based on ordering
    //! a collection of keys. For examples, the following code
    //! \code{cpp}
    //!   double someids[] = { 3.1, 2.2, 0.5, 1.5 };
    //!   std::string somenames[] =
    //!       {
    //!           std::string('a'),
    //!           std::string('b'),
    //!           std::string('c'),
    //!           std::string('d')
    //!       };
    //!   std::vector<double> ids(someids, someids + 4);
    //!   std::vector<std::string> names(somenames, somenames + 4);
    //!
    //!   maths::common::COrderings::simultaneousSort(ids, names);
    //!
    //!   for (std::size_t i = 0; i < 4; ++i)
    //!   {
    //!       std::cout << ids[i] << ' ' << names[i] << std::endl;
    //!   }
    //! \endcode
    //!
    //! Will produce the following output:
    //! <pre>
    //! 0.5 c
    //! 1.5 d
    //! 2.2 b
    //! 3.1 a
    //! </pre>
    //!
    //! These support simultaneously sorting up to 4 additional containers
    //! to the keys.
    //!
    //! \note The complexity is O(N log(N)) where N is the length of the
    //! containers.
    //! \warning All containers must have the same length.
    //@{
private:
    //! Orders a set of indices into an array based using the default
    //! comparison operator of the corresponding key type.
    template<typename KEY_VECTOR, typename COMP = std::less<typename KEY_VECTOR::value_type>>
    class CIndexLess {
    public:
        explicit CIndexLess(const KEY_VECTOR& keys, const COMP& comp = COMP())
            : m_Keys(&keys), m_Comp(comp) {}

        bool operator()(std::size_t lhs, std::size_t rhs) {
            return m_Comp((*m_Keys)[lhs], (*m_Keys)[rhs]);
        }

    private:
        const KEY_VECTOR* m_Keys;
        COMP m_Comp;
    };

public:
    //! Simultaneously sort \p keys and \p values using the \p comp
    //! order of \p keys.
    template<typename KEY_VECTOR, typename VALUE_VECTOR, typename COMP>
    static bool simultaneousSort(KEY_VECTOR& keys, VALUE_VECTOR& values, const COMP& comp);
    //! Overload for default operator< comparison.
    template<typename KEY_VECTOR, typename VALUE_VECTOR>
    static bool simultaneousSort(KEY_VECTOR& keys, VALUE_VECTOR& values) {
        return simultaneousSort(keys, values,
                                std::less<typename KEY_VECTOR::value_type>());
    }
    //! Overload for default operator< comparison.
    template<typename KEY_VECTOR, typename VALUE_VECTOR>
    static bool simultaneousSort(core::CVectorRange<KEY_VECTOR>& keys,
                                 core::CVectorRange<VALUE_VECTOR>& values) {
        return simultaneousSort(keys, values,
                                std::less<typename KEY_VECTOR::value_type>());
    }

    //! Simultaneously sort \p keys, \p values1 and \p values2
    //! using the \p comp order of \p keys.
    template<typename KEY_VECTOR, typename VALUE1_VECTOR, typename VALUE2_VECTOR, typename COMP>
    static bool simultaneousSort(KEY_VECTOR& keys,
                                 VALUE1_VECTOR& values1,
                                 VALUE2_VECTOR& values2,
                                 const COMP& comp);
    //! Overload for default operator< comparison.
    template<typename KEY_VECTOR, typename VALUE1_VECTOR, typename VALUE2_VECTOR>
    static bool
    simultaneousSort(KEY_VECTOR& keys, VALUE1_VECTOR& values1, VALUE2_VECTOR& values2) {
        return simultaneousSort(keys, values1, values2,
                                std::less<typename KEY_VECTOR::value_type>());
    }
    //! Overload for default operator< comparison.
    template<typename KEY_VECTOR, typename VALUE1_VECTOR, typename VALUE2_VECTOR>
    static bool simultaneousSort(core::CVectorRange<KEY_VECTOR> keys,
                                 core::CVectorRange<VALUE1_VECTOR> values1,
                                 core::CVectorRange<VALUE2_VECTOR> values2) {
        return simultaneousSort(keys, values1, values2,
                                std::less<typename KEY_VECTOR::value_type>());
    }

    //! Simultaneously sort \p keys, \p values1, \p values2
    //! and \p values3 using the \p comp order of \p keys.
    template<typename KEY_VECTOR, typename VALUE1_VECTOR, typename VALUE2_VECTOR, typename VALUE3_VECTOR, typename COMP>
    static bool simultaneousSort(KEY_VECTOR& keys,
                                 VALUE1_VECTOR& values1,
                                 VALUE2_VECTOR& values2,
                                 VALUE3_VECTOR& values3,
                                 const COMP& comp);
    //! Overload for default operator< comparison.
    template<typename KEY_VECTOR, typename VALUE1_VECTOR, typename VALUE2_VECTOR, typename VALUE3_VECTOR>
    static bool simultaneousSort(KEY_VECTOR& keys,
                                 VALUE1_VECTOR& values1,
                                 VALUE2_VECTOR& values2,
                                 VALUE3_VECTOR& values3) {
        return simultaneousSort(keys, values1, values2, values3,
                                std::less<typename KEY_VECTOR::value_type>());
    }
    //! Overload for default operator< comparison.
    template<typename KEY_VECTOR, typename VALUE1_VECTOR, typename VALUE2_VECTOR, typename VALUE3_VECTOR>
    static bool simultaneousSort(core::CVectorRange<KEY_VECTOR> keys,
                                 core::CVectorRange<VALUE1_VECTOR> values1,
                                 core::CVectorRange<VALUE2_VECTOR> values2,
                                 core::CVectorRange<VALUE3_VECTOR> values3) {
        return simultaneousSort(keys, values1, values2, values3,
                                std::less<typename KEY_VECTOR::value_type>());
    }

    //! Simultaneously sort \p keys, \p values1, \p values2,
    //! \p values3 and \p values4 using the \p comp order of
    //! \p keys.
    template<typename KEY_VECTOR, typename VALUE1_VECTOR, typename VALUE2_VECTOR, typename VALUE3_VECTOR, typename VALUE4_VECTOR, typename COMP>
    static bool simultaneousSort(KEY_VECTOR& keys,
                                 VALUE1_VECTOR& values1,
                                 VALUE2_VECTOR& values2,
                                 VALUE3_VECTOR& values3,
                                 VALUE4_VECTOR& values4,
                                 const COMP& comp);
    //! Overload for default operator< comparison.
    template<typename KEY_VECTOR, typename VALUE1_VECTOR, typename VALUE2_VECTOR, typename VALUE3_VECTOR, typename VALUE4_VECTOR>
    static bool simultaneousSort(KEY_VECTOR& keys,
                                 VALUE1_VECTOR& values1,
                                 VALUE2_VECTOR& values2,
                                 VALUE3_VECTOR& values3,
                                 VALUE4_VECTOR& values4) {
        return simultaneousSort(keys, values1, values2, values3, values4,
                                std::less<typename KEY_VECTOR::value_type>());
    }
    //! Overload for default operator< comparison.
    template<typename KEY_VECTOR, typename VALUE1_VECTOR, typename VALUE2_VECTOR, typename VALUE3_VECTOR, typename VALUE4_VECTOR>
    static bool simultaneousSort(core::CVectorRange<KEY_VECTOR> keys,
                                 core::CVectorRange<VALUE1_VECTOR> values1,
                                 core::CVectorRange<VALUE2_VECTOR> values2,
                                 core::CVectorRange<VALUE3_VECTOR> values3,
                                 core::CVectorRange<VALUE4_VECTOR> values4) {
        return simultaneousSort(keys, values1, values2, values3, values4,
                                std::less<typename KEY_VECTOR::value_type>());
    }
    //@}
};
}
}
}

#endif // INCLUDED_ml_maths_common_COrderings_h
