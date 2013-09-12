/* The file is part of Snowman decompiler.             */
/* See doc/licenses.txt for the licensing information. */

#pragma once

#include <nc/config.h>

#include <algorithm> /* std::max */
#include <cassert>

#include <nc/common/BitTwiddling.h>
#include <nc/common/SizedValue.h>
#include <nc/common/Types.h>

namespace nc {
namespace core {
namespace ir {
namespace dflow {

/**
 * An integer value of a variable size with bits taking values from the power set of {0, 1}.
 */
class AbstractValue {
    SmallBitSize size_;         ///< Size of the abstract value.
    ConstantValue zeroBits_;    ///< Bit mask of positions that can be zero.
    ConstantValue oneBits_;     ///< Bit mask of positions that can be one.

    public:

    /**
     * Constructs a value of zero size.
     */
    AbstractValue(): size_(0), zeroBits_(0), oneBits_(0) {}

    /**
     * Constructor.
     *
     * \param size      Size of the abstract value.
     * \param zeroBits  Bit mask of positions that can be zero.
     * \param oneBits   Bit mask of positions that can be one.
     *
     * Given bit masks are truncated to the given size.
     */
    explicit
    AbstractValue(SmallBitSize size, ConstantValue zeroBits, ConstantValue oneBits) {
        assert(size >= 0);

        size_ = size;
        zeroBits_ = bitTruncate(zeroBits, size_);
        oneBits_  = bitTruncate(oneBits,  size_);
    }

    /**
     * Constructs a concrete abstract value from a sized value.
     *
     * \param x Sized value.
     */
    AbstractValue(const SizedValue &x):
        size_(x.size()), zeroBits_(x.value() ^ bitMask<ConstantValue>(size_)), oneBits_(x.value())
    {}

    /**
     * \return Size of the abstract value.
     */
    SmallBitSize size() const { return size_; }

    /**
     * Resizes the abstract value to the given size.
     *
     * \param size New size.
     *
     * \return *this.
     */
    AbstractValue &resize(SmallBitSize size) {
        if (size < size_) {
            zeroBits_ = bitTruncate(zeroBits_, size);
            oneBits_  = bitTruncate(oneBits_,  size);
        }
        size_ = size;
        return *this;
    }

    /**
     * \return Bit mask of bits that can be zero.
     */
    ConstantValue zeroBits() const { return zeroBits_; }

    /**
     * \return Bit mask of bits that can be one.
     */
    ConstantValue oneBits() const { return oneBits_; }

    /**
     * \return True if all (at least one) bits of the value are known to be either one or zero, false otherwise.
     */
    bool isConcrete() const { return size_ > 0 && (zeroBits_ ^ oneBits_) == bitMask<ConstantValue>(size_); }

    /**
     * \return True if the value has a bit that can be both zero and one, false otherwise.
     */
    bool isNondeterministic() const { return (zeroBits_ ^ oneBits_) != 0; }

    /**
     * \return Concrete value of this abstract value, if this value is concrete.
     *         Otherwise, the behavior is undefined.
     */
    SizedValue asConcrete() const { assert(isConcrete()); return SizedValue(size_, oneBits_); }

    /**
     * Shifts the value by the given number of bits.
     * If the number of bits is positive, the shift is to the left.
     * If the number of bits is negative, the shift is to the right.
     * Increases (decreases) the value size by the same amount of bits.
     * If the resulting size is less than zero, it is set to zero.
     *
     * \param nbits Number of bits.
     *
     * \return *this.
     */
    AbstractValue &shift(int nbits) {
        size_ = std::max(size_ + nbits, 0);
        zeroBits_ = bitShift(zeroBits_, nbits);
        oneBits_  = bitShift(oneBits_,  nbits);
        return *this;
    }

    /**
     * Componentwise merges *this with the given abstract value.
     *
     * \param that Abstract value.
     *
     * \return *this.
     */
    AbstractValue &merge(const AbstractValue &that) {
        size_      = std::max(size_, that.size_);
        zeroBits_ |= that.zeroBits_;
        oneBits_  |= that.oneBits_;
        return *this;
    }

    /**
     * Ands each component of the abstract value with the mask.
     *
     * \param mask Bit mask.
     *
     * \return *this.
     */
    AbstractValue &project(ConstantValue mask) {
        zeroBits_ &= mask;
        oneBits_  &= mask;
        return *this;
    }

    /**
     * Zero-extends *this to the given size.
     *
     * \param size New size.
     *
     * \return *this.
     */
    AbstractValue &zeroExtend(SmallBitSize size) {
        assert(size > size_);

        zeroBits_ |= shiftLeft(bitMask<ConstantValue>(size - size_), size_);
        size_ = size;

        return *this;
    }

    /**
     * Sign-extends *this to given size.
     *
     * \param size New size.
     *
     * \return *this.
     */
    AbstractValue &signExtend(SmallBitSize size) {
        assert(size > size_);

        auto signBitMask = shiftLeft<ConstantValue>(1, size_ - 1);

        if (zeroBits_ & signBitMask) {
            zeroBits_ |= shiftLeft(bitMask<ConstantValue>(size - size_), size_);
        }
        if (oneBits_ & signBitMask) {
            oneBits_ |= shiftLeft(bitMask<ConstantValue>(size - size_), size_);
        }

        size_ = size;

        return *this;
    }
};

/**
 * Proxy class for performing signed versions of operations on abstract values.
 */
class SignedAbstractValue: public AbstractValue {
    public:

    SignedAbstractValue(const AbstractValue &that): AbstractValue(that) {}

    SignedAbstractValue(AbstractValue &&that): AbstractValue(std::move(that)) {}
};

/**
 * Proxy class for performing unsigned versions of operations on abstract values.
 */
class UnsignedAbstractValue: public AbstractValue {
    public:

    UnsignedAbstractValue(const AbstractValue &that): AbstractValue(that) {}

    UnsignedAbstractValue(AbstractValue &&that): AbstractValue(std::move(that)) {}
};

inline
AbstractValue operator&(const AbstractValue &a, const AbstractValue &b) {
    assert(a.size() == b.size());
    return AbstractValue(a.size(), a.zeroBits() | b.zeroBits(), a.oneBits() & b.oneBits());
}

inline
AbstractValue operator|(const AbstractValue &a, const AbstractValue &b) {
    assert(a.size() == b.size());
    return AbstractValue(a.size(), a.zeroBits() & b.zeroBits(), a.oneBits() | b.oneBits());
}

inline
AbstractValue operator^(const AbstractValue &a, const AbstractValue &b) {
    assert(a.size() == b.size());
    return AbstractValue(a.size(),
        (a.zeroBits() & b.zeroBits()) | (a.oneBits() & b.oneBits()),
        (a.oneBits() & b.zeroBits()) | (a.zeroBits() & b.oneBits()));
}

inline
AbstractValue operator~(const AbstractValue &a) {
    return AbstractValue(a.size(), ~a.zeroBits(), ~a.oneBits());
}

inline
AbstractValue operator<<(const AbstractValue &a, const AbstractValue &b) {
    if (b.isConcrete()) {
        auto nbits = b.asConcrete().value();
        return AbstractValue(a.size(),
            shiftLeft(a.zeroBits(), nbits) | bitMask<ConstantValue>(nbits),
            shiftLeft(a.oneBits(), nbits));
    } else {
        return AbstractValue(a.size(), -1, -1);
    }
}

inline
AbstractValue operator>>(const UnsignedAbstractValue &a, const AbstractValue &b) {
    if (b.isConcrete()) {
        auto nbits = b.asConcrete().value();

        return AbstractValue(a.size(),
            shiftRight(a.zeroBits(), nbits) | shiftLeft(bitMask<ConstantValue>(nbits), a.size() - nbits),
            shiftRight(a.oneBits(), nbits));
    } else {
        return AbstractValue(a.size(), -1, -1);
    }
}

inline
AbstractValue operator>>(const SignedAbstractValue &a, const AbstractValue &b) {
    if (b.isConcrete()) {
        auto nbits = b.asConcrete().value();

        auto zeroBits = shiftRight(a.zeroBits(), nbits);
        auto oneBits  = shiftRight(a.oneBits(), nbits);

        auto signBitMask = shiftLeft<ConstantValue>(1, a.size() - 1);
        auto signExtendMask = shiftLeft(bitMask<ConstantValue>(nbits), a.size() - nbits);

        if (a.zeroBits() & signBitMask) {
            zeroBits |= signExtendMask;
        }
        if (a.oneBits() & signBitMask) {
            oneBits |= signExtendMask;
        }

        return AbstractValue(a.size(), zeroBits, oneBits);
    } else {
        return AbstractValue(a.size(), -1, -1);
    }
}

inline
AbstractValue operator+(const AbstractValue &a, const AbstractValue &b) {
    assert(a.size() == b.size());

    if (a.isConcrete() && b.isConcrete()) {
        return SizedValue(a.size(), a.asConcrete().value() + b.asConcrete().value());
    } else if (b.isConcrete() && b.asConcrete().value() == 0) {
        return a;
    } else if (a.isConcrete() && a.asConcrete().value() == 0) {
        return b;
    } else {
        return AbstractValue(a.size(), -1, -1);
    }
}

inline
AbstractValue operator-(const AbstractValue &a) {
    return ~a + AbstractValue(a.size(), 0, 1);
}

inline
AbstractValue operator-(const AbstractValue &a, const AbstractValue &b) {
    assert(a.size() == b.size());

    if (a.isConcrete() && b.isConcrete()) {
        return SizedValue(a.size(), a.asConcrete().value() - b.asConcrete().value());
    } else if (b.isConcrete() && b.asConcrete().value() == 0) {
        return a;
    } else if (a.isConcrete() && a.asConcrete().value() == 0) {
        return -b;
    } else {
        return AbstractValue(a.size(), -1, -1);
    }
}

inline
AbstractValue operator*(const AbstractValue &a, const AbstractValue &b) {
    assert(a.size() == b.size());

    if (a.isConcrete() && b.isConcrete()) {
        return SizedValue(a.size(), a.asConcrete().value() * b.asConcrete().value());
    } else if (a.isConcrete() && a.asConcrete().value() == 0) {
        return a;
    } else if (b.isConcrete() && b.asConcrete().value() == 0) {
        return b;
    } else {
        return AbstractValue(a.size(), -1, -1);
    }
}

inline
AbstractValue operator/(const UnsignedAbstractValue &a, const UnsignedAbstractValue &b) {
    assert(a.size() == b.size());

    if (b.isConcrete() && b.asConcrete().value() == 0) {
        return AbstractValue();
    } else if (a.isConcrete() && b.isConcrete()) {
        return SizedValue(a.size(), a.asConcrete().value() / b.asConcrete().value());
    } else if (a.isConcrete() && a.asConcrete().value() == 0) {
        return a;
    } else {
        return AbstractValue(a.size(), -1, -1);
    }
}

inline
AbstractValue operator/(const SignedAbstractValue &a, const SignedAbstractValue &b) {
    assert(a.size() == b.size());

    if (b.isConcrete() && b.asConcrete().value() == 0) {
        return AbstractValue();
    } else if (a.isConcrete() && b.isConcrete()) {
        return SizedValue(a.size(), a.asConcrete().signedValue() / b.asConcrete().signedValue());
    } else if (a.isConcrete() && a.asConcrete().value() == 0) {
        return a;
    } else {
        return AbstractValue(a.size(), -1, -1);
    }
}

inline
AbstractValue operator%(const UnsignedAbstractValue &a, const UnsignedAbstractValue &b) {
    assert(a.size() == b.size());

    if (b.isConcrete() && b.asConcrete().value() == 0) {
        return AbstractValue();
    } else if (a.isConcrete() && b.isConcrete()) {
        return SizedValue(a.size(), a.asConcrete().value() % b.asConcrete().value());
    } else if (a.isConcrete() && a.asConcrete().value() == 0) {
        return a;
    } else if (b.isConcrete() && b.asConcrete().value() == 1) {
        return SizedValue(a.size(), 0);
    } else {
        return AbstractValue(a.size(), -1, -1);
    }
}

inline
AbstractValue operator%(const SignedAbstractValue &a, const SignedAbstractValue &b) {
    assert(a.size() == b.size());

    if (b.isConcrete() && b.asConcrete().value() == 0) {
        return AbstractValue();
    } else if (a.isConcrete() && b.isConcrete()) {
        return SizedValue(a.size(), a.asConcrete().signedValue() % b.asConcrete().signedValue());
    } else if (a.isConcrete() && a.asConcrete().value() == 0) {
        return a;
    } else if (b.isConcrete() && b.asConcrete().value() == 1) {
        return SizedValue(a.size(), 0);
    } else {
        return AbstractValue(a.size(), -1, -1);
    }
}

inline
AbstractValue operator!(const AbstractValue &a) {
    /*
     * Can be 0 if 'a' can be nonzero, i.e. there is a bit which can be one.
     * Can be 1 if 'a' can be zero, i.e. if there is no bit which is strictly one.
     */
    return AbstractValue(1, a.oneBits() != 0, (a.oneBits() & ~a.zeroBits()) == 0);
}

inline
AbstractValue operator==(const AbstractValue &a, const AbstractValue &b) {
    assert(a.size() == b.size());

    return AbstractValue(1,
        (a.oneBits() ^ b.oneBits()) || (a.zeroBits() ^ b.zeroBits()),
        ((a.zeroBits() & b.zeroBits()) | (a.oneBits() & b.oneBits())) == bitMask<ConstantValue>(a.size()));
}

inline
AbstractValue operator<(const SignedAbstractValue &a, const SignedAbstractValue &b) {
    if (a.isConcrete() && b.isConcrete()) {
        return SizedValue(1, a.asConcrete().signedValue() < b.asConcrete().signedValue());
    } else {
        return AbstractValue(1, 1, 1);
    }
}

inline
AbstractValue operator<=(const SignedAbstractValue &a, const SignedAbstractValue &b) {
    if (a.isConcrete() && b.isConcrete()) {
        return SizedValue(1, a.asConcrete().signedValue() <= b.asConcrete().signedValue());
    } else {
        return AbstractValue(1, 1, 1);
    }
}

inline
AbstractValue operator<(const UnsignedAbstractValue &a, const UnsignedAbstractValue &b) {
    if (a.isConcrete() && b.isConcrete()) {
        return SizedValue(1, a.asConcrete().value() < b.asConcrete().value());
    } else {
        return AbstractValue(1, 1, 1);
    }
}

inline
AbstractValue operator<=(const UnsignedAbstractValue &a, const UnsignedAbstractValue &b) {
    if (a.isConcrete() && b.isConcrete()) {
        return SizedValue(1, a.asConcrete().value() <= b.asConcrete().value());
    } else {
        return AbstractValue(1, 1, 1);
    }
}

} // namespace dflow
} // namespace ir
} // namespace core
} // namespace nc

/* vim:set et sts=4 sw=4: */
