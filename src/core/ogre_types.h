// OGRE 14's vector, quaternion and angle maths as the port's desktop and Switch builds run them,
// for the gameplay code that left the renderer (core/world.h): the same operations in the same
// order, so a moved function computes the same floats it did. OGRE is MIT-licensed
// (external/ogre/LICENSE); this is a copy of the parts the game uses, from OgreVector.h,
// OgreQuaternion.{h,cpp}, OgreMatrix3.h and OgreMath.{h,cpp}. The console's own OGRE differs in
// places (its fused arithmetic: core/ogre_math.h); where a check shows the port must follow
// it, the gameplay code says so at the call.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace tl::core::ogre {

using Real = float;

class Degree;

// PORT: OGRE's `Radian`.
class Radian {
    float mRad;

public:
    explicit Radian(float r = 0) : mRad(r) {}
    Radian(const Degree& d);
    Radian& operator=(const float& f) {
        mRad = f;
        return *this;
    }
    Radian& operator=(const Degree& d);
    float valueDegrees() const;
    float valueRadians() const { return mRad; }
    const Radian& operator+() const { return *this; }
    Radian operator+(const Radian& r) const { return Radian(mRad + r.mRad); }
    Radian& operator+=(const Radian& r) {
        mRad += r.mRad;
        return *this;
    }
    Radian operator-() const { return Radian(-mRad); }
    Radian operator-(const Radian& r) const { return Radian(mRad - r.mRad); }
    Radian& operator-=(const Radian& r) {
        mRad -= r.mRad;
        return *this;
    }
    Radian operator*(float f) const { return Radian(mRad * f); }
    Radian operator*(const Radian& f) const { return Radian(mRad * f.mRad); }
    Radian& operator*=(float f) {
        mRad *= f;
        return *this;
    }
    Radian operator/(float f) const { return Radian(mRad / f); }
    Radian& operator/=(float f) {
        mRad /= f;
        return *this;
    }
    bool operator<(const Radian& r) const { return mRad < r.mRad; }
    bool operator<=(const Radian& r) const { return mRad <= r.mRad; }
    bool operator==(const Radian& r) const { return mRad == r.mRad; }
    bool operator!=(const Radian& r) const { return mRad != r.mRad; }
    bool operator>=(const Radian& r) const { return mRad >= r.mRad; }
    bool operator>(const Radian& r) const { return mRad > r.mRad; }
};

// PORT: OGRE's `Degree`.
class Degree {
    float mDeg;

public:
    explicit Degree(float d = 0) : mDeg(d) {}
    Degree(const Radian& r) : mDeg(r.valueDegrees()) {}
    Degree& operator=(const float& f) {
        mDeg = f;
        return *this;
    }
    Degree& operator=(const Radian& r) {
        mDeg = r.valueDegrees();
        return *this;
    }
    float valueDegrees() const { return mDeg; }
    float valueRadians() const;
    Degree operator-() const { return Degree(-mDeg); }
    Degree operator+(const Degree& d) const { return Degree(mDeg + d.mDeg); }
    Degree operator-(const Degree& d) const { return Degree(mDeg - d.mDeg); }
    Degree operator*(float f) const { return Degree(mDeg * f); }
    Degree operator/(float f) const { return Degree(mDeg / f); }
    bool operator<(const Degree& d) const { return mDeg < d.mDeg; }
    bool operator>(const Degree& d) const { return mDeg > d.mDeg; }
    bool operator==(const Degree& d) const { return mDeg == d.mDeg; }
};

// PORT: OGRE's `Math`, the functions the game calls (no lookup tables).
struct Math {
    static constexpr Real PI = static_cast<Real>(3.14159265358979323846);
    static constexpr Real TWO_PI = Real(2.0 * PI);
    static constexpr Real HALF_PI = Real(0.5 * PI);
    static constexpr float fDeg2Rad = PI / Real(180.0);
    static constexpr float fRad2Deg = Real(180.0) / PI;
    static Real Abs(Real fValue) { return std::abs(fValue); }
    static Radian Abs(const Radian& rValue) { return Radian(std::abs(rValue.valueRadians())); }
    static Radian ACos(Real fValue) {
        if (-1.0 < fValue) {
            if (fValue < 1.0) return Radian(std::acos(fValue));
            return Radian(0.0);
        }
        return Radian(PI);
    }
    static Radian ASin(Real fValue) {
        if (-1.0 < fValue) {
            if (fValue < 1.0) return Radian(std::asin(fValue));
            return Radian(HALF_PI);
        }
        return Radian(-HALF_PI);
    }
    static Radian ATan(float fValue) { return Radian(std::atan(fValue)); }
    static Radian ATan2(float fY, float fX) { return Radian(std::atan2(fY, fX)); }
    static Real Sin(const Radian& fValue) { return std::sin(fValue.valueRadians()); }
    static Real Sin(Real fValue) { return std::sin(fValue); }
    static Real Cos(const Radian& fValue) { return std::cos(fValue.valueRadians()); }
    static Real Cos(Real fValue) { return std::cos(fValue); }
    static Real Tan(const Radian& fValue) { return std::tan(fValue.valueRadians()); }
    static Real Sqr(Real fValue) { return fValue * fValue; }
    static Real Sqrt(Real fValue) { return std::sqrt(fValue); }
    static Real InvSqrt(Real fValue) { return Real(1.) / std::sqrt(fValue); }
    static Real Floor(Real fValue) { return std::floor(fValue); }
    static float DegreesToRadians(float degrees) { return degrees * fDeg2Rad; }
    static float RadiansToDegrees(float radians) { return radians * fRad2Deg; }
    static bool RealEqual(Real a, Real b, Real tolerance) { return std::abs(b - a) <= tolerance; }
    template <typename T>
    static T Clamp(T val, T minval, T maxval) {
        return std::max(std::min(val, maxval), minval);
    }
};

inline Radian::Radian(const Degree& d) : mRad(d.valueRadians()) {}
inline Radian& Radian::operator=(const Degree& d) {
    mRad = d.valueRadians();
    return *this;
}
inline float Radian::valueDegrees() const { return Math::RadiansToDegrees(mRad); }
inline float Degree::valueRadians() const { return Math::DegreesToRadians(mDeg); }
inline Radian operator*(float a, const Radian& b) { return Radian(a * b.valueRadians()); }
inline Radian operator/(float a, const Radian& b) { return Radian(a / b.valueRadians()); }
inline Degree operator*(float a, const Degree& b) { return Degree(a * b.valueDegrees()); }

class Quaternion;

// PORT: OGRE's `Vector3` (`Vector<3, Real>`), its loops written out in their order.
struct Vector3 {
    Real x, y, z;

    // Left unset, as OGRE's.
    // cppcheck-suppress uninitMemberVar
    Vector3() = default;
    constexpr Vector3(Real _x, Real _y, Real _z) : x(_x), y(_y), z(_z) {}
    explicit Vector3(Real s) : x(s), y(s), z(s) {}

    // OGRE's own indexing over x, y, z, which lie together.
    // cppcheck-suppress objectIndex
    Real operator[](size_t i) const { return (&x)[i]; }
    // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
    Real& operator[](size_t i) { return (&x)[i]; }  // cppcheck-suppress objectIndex
    Real* ptr() { return &x; }
    const Real* ptr() const { return &x; }

    bool operator==(const Vector3& v) const { return x == v.x && y == v.y && z == v.z; }
    bool operator!=(const Vector3& v) const { return !(*this == v); }
    bool positionEquals(const Vector3& rhs, Real tolerance = 1e-03f) const {
        return Math::RealEqual(x, rhs.x, tolerance) && Math::RealEqual(y, rhs.y, tolerance) &&
               Math::RealEqual(z, rhs.z, tolerance);
    }
    void makeFloor(const Vector3& cmp) {
        if (cmp.x < x) x = cmp.x;
        if (cmp.y < y) y = cmp.y;
        if (cmp.z < z) z = cmp.z;
    }
    void makeCeil(const Vector3& cmp) {
        if (cmp.x > x) x = cmp.x;
        if (cmp.y > y) y = cmp.y;
        if (cmp.z > z) z = cmp.z;
    }
    Real dotProduct(const Vector3& vec) const {
        Real ret = 0;
        ret += x * vec.x;
        ret += y * vec.y;
        ret += z * vec.z;
        return ret;
    }
    Real absDotProduct(const Vector3& vec) const {
        return Math::Abs(x * vec.x) + Math::Abs(y * vec.y) + Math::Abs(z * vec.z);
    }
    Real squaredLength() const { return dotProduct(*this); }
    bool isZeroLength() const { return squaredLength() < 1e-06 * 1e-06; }
    Real length() const { return Math::Sqrt(squaredLength()); }
    Real distance(const Vector3& rhs) const { return (*this - rhs).length(); }
    Real squaredDistance(const Vector3& rhs) const { return (*this - rhs).squaredLength(); }
    Real normalise() {
        const Real fLength = length();
        if (fLength > Real(0.0f)) {
            const Real fInvLength = 1.0f / fLength;
            x *= fInvLength;
            y *= fInvLength;
            z *= fInvLength;
        }
        return fLength;
    }
    Vector3 normalisedCopy() const {
        Vector3 ret = *this;
        ret.normalise();
        return ret;
    }
    Radian angleBetween(const Vector3& dest) const {
        Real lenProduct = length() * dest.length();
        if (lenProduct < 1e-6f) lenProduct = 1e-6f;
        Real f = dotProduct(dest) / lenProduct;
        f = Math::Clamp(f, (Real)-1.0, (Real)1.0);
        return Math::ACos(f);
    }
    Vector3 crossProduct(const Vector3& rkVector) const {
        return Vector3(y * rkVector.z - z * rkVector.y, z * rkVector.x - x * rkVector.z,
                       x * rkVector.y - y * rkVector.x);
    }
    Vector3 midPoint(const Vector3& vec) const {
        return Vector3((x + vec.x) * 0.5f, (y + vec.y) * 0.5f, (z + vec.z) * 0.5f);
    }
    Vector3 perpendicular() const {
        const Vector3 perp =
            Math::Abs(x) > Math::Abs(z) ? Vector3(-y, x, 0.0) : Vector3(0.0, -z, y);
        return perp.normalisedCopy();
    }
    Quaternion getRotationTo(const Vector3& dest, const Vector3& fallbackAxis = ZERO) const;

    Vector3& operator*=(Real s) {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }
    Vector3& operator/=(Real s) {
        const Real fInv = 1.0f / s;
        x *= fInv;
        y *= fInv;
        z *= fInv;
        return *this;
    }
    Vector3& operator+=(Real s) {
        x += s;
        y += s;
        z += s;
        return *this;
    }
    Vector3& operator-=(Real s) {
        x -= s;
        y -= s;
        z -= s;
        return *this;
    }
    Vector3& operator+=(const Vector3& b) {
        x += b.x;
        y += b.y;
        z += b.z;
        return *this;
    }
    Vector3& operator-=(const Vector3& b) {
        x -= b.x;
        y -= b.y;
        z -= b.z;
        return *this;
    }
    Vector3& operator*=(const Vector3& b) {
        x *= b.x;
        y *= b.y;
        z *= b.z;
        return *this;
    }
    Vector3& operator/=(const Vector3& b) {
        x /= b.x;
        y /= b.y;
        z /= b.z;
        return *this;
    }
    friend Vector3 operator*(Real s, Vector3 v) {
        v *= s;
        return v;
    }
    friend Vector3 operator+(Real s, Vector3 v) {
        v += s;
        return v;
    }
    friend Vector3 operator-(Real s, const Vector3& v) {
        return Vector3(s - v.x, s - v.y, s - v.z);
    }
    friend Vector3 operator/(Real s, const Vector3& v) {
        return Vector3(s / v.x, s / v.y, s / v.z);
    }
    Vector3 operator-() const { return -1 * *this; }
    const Vector3& operator+() const { return *this; }
    Vector3 operator*(Real s) const { return s * *this; }
    Vector3 operator/(Real s) const {
        const Real fInv = 1.0f / s;
        return fInv * *this;
    }
    Vector3 operator-(Real s) const { return -s + *this; }
    Vector3 operator+(Real s) const { return s + *this; }
    Vector3 operator+(const Vector3& b) const {
        Vector3 ret = *this;
        ret += b;
        return ret;
    }
    Vector3 operator-(const Vector3& b) const {
        Vector3 ret = *this;
        ret -= b;
        return ret;
    }
    Vector3 operator*(const Vector3& b) const {
        Vector3 ret = *this;
        ret *= b;
        return ret;
    }
    Vector3 operator/(const Vector3& b) const {
        Vector3 ret = *this;
        ret /= b;
        return ret;
    }

    static const Vector3 ZERO;
    static const Vector3 UNIT_X;
    static const Vector3 UNIT_Y;
    static const Vector3 UNIT_Z;
    static const Vector3 NEGATIVE_UNIT_X;
    static const Vector3 NEGATIVE_UNIT_Y;
    static const Vector3 NEGATIVE_UNIT_Z;
    static const Vector3 UNIT_SCALE;
};

inline const Vector3 Vector3::ZERO(0, 0, 0);
inline const Vector3 Vector3::UNIT_X(1, 0, 0);
inline const Vector3 Vector3::UNIT_Y(0, 1, 0);
inline const Vector3 Vector3::UNIT_Z(0, 0, 1);
inline const Vector3 Vector3::NEGATIVE_UNIT_X(-1, 0, 0);
inline const Vector3 Vector3::NEGATIVE_UNIT_Y(0, -1, 0);
inline const Vector3 Vector3::NEGATIVE_UNIT_Z(0, 0, -1);
inline const Vector3 Vector3::UNIT_SCALE(1, 1, 1);

// PORT: OGRE's `Matrix3`, as far as a quaternion goes through it.
class Matrix3 {
public:
    // Left unset, as OGRE's.
    // cppcheck-suppress uninitMemberVar
    Matrix3() = default;
    const Real* operator[](size_t iRow) const { return m[iRow]; }
    Real* operator[](size_t iRow) { return m[iRow]; }
    void FromAxes(const Vector3& xAxis, const Vector3& yAxis, const Vector3& zAxis) {
        SetColumn(0, xAxis);
        SetColumn(1, yAxis);
        SetColumn(2, zAxis);
    }
    void SetColumn(size_t iCol, const Vector3& vec) {
        m[0][iCol] = vec.x;
        m[1][iCol] = vec.y;
        m[2][iCol] = vec.z;
    }
    Vector3 GetColumn(size_t iCol) const { return Vector3(m[0][iCol], m[1][iCol], m[2][iCol]); }

private:
    Real m[3][3];
};

// PORT: OGRE's `Quaternion`.
class Quaternion {
public:
    Quaternion() : w(1), x(0), y(0), z(0) {}
    Quaternion(Real fW, Real fX, Real fY, Real fZ) : w(fW), x(fX), y(fY), z(fZ) {}
    explicit Quaternion(const Matrix3& rot) { FromRotationMatrix(rot); }
    Quaternion(const Radian& rfAngle, const Vector3& rkAxis) { FromAngleAxis(rfAngle, rkAxis); }
    Quaternion(const Vector3& xaxis, const Vector3& yaxis, const Vector3& zaxis) {
        FromAxes(xaxis, yaxis, zaxis);
    }

    void FromRotationMatrix(const Matrix3& kRot) {
        Real fTrace = kRot[0][0] + kRot[1][1] + kRot[2][2];
        Real fRoot;
        if (fTrace > 0.0) {
            fRoot = Math::Sqrt(fTrace + 1.0f);
            w = 0.5f * fRoot;
            fRoot = 0.5f / fRoot;
            x = (kRot[2][1] - kRot[1][2]) * fRoot;
            y = (kRot[0][2] - kRot[2][0]) * fRoot;
            z = (kRot[1][0] - kRot[0][1]) * fRoot;
        } else {
            static const size_t s_iNext[3] = {1, 2, 0};
            size_t i = 0;
            if (kRot[1][1] > kRot[0][0]) i = 1;
            if (kRot[2][2] > kRot[i][i]) i = 2;
            const size_t j = s_iNext[i];
            const size_t k = s_iNext[j];
            fRoot = Math::Sqrt(kRot[i][i] - kRot[j][j] - kRot[k][k] + 1.0f);
            Real* apkQuat[3] = {&x, &y, &z};
            *apkQuat[i] = 0.5f * fRoot;
            fRoot = 0.5f / fRoot;
            w = (kRot[k][j] - kRot[j][k]) * fRoot;
            *apkQuat[j] = (kRot[j][i] + kRot[i][j]) * fRoot;
            *apkQuat[k] = (kRot[k][i] + kRot[i][k]) * fRoot;
        }
    }
    void ToRotationMatrix(Matrix3& kRot) const {
        const Real fTx = x + x;
        const Real fTy = y + y;
        const Real fTz = z + z;
        const Real fTwx = fTx * w;
        const Real fTwy = fTy * w;
        const Real fTwz = fTz * w;
        const Real fTxx = fTx * x;
        const Real fTxy = fTy * x;
        const Real fTxz = fTz * x;
        const Real fTyy = fTy * y;
        const Real fTyz = fTz * y;
        const Real fTzz = fTz * z;
        kRot[0][0] = 1.0f - (fTyy + fTzz);
        kRot[0][1] = fTxy - fTwz;
        kRot[0][2] = fTxz + fTwy;
        kRot[1][0] = fTxy + fTwz;
        kRot[1][1] = 1.0f - (fTxx + fTzz);
        kRot[1][2] = fTyz - fTwx;
        kRot[2][0] = fTxz - fTwy;
        kRot[2][1] = fTyz + fTwx;
        kRot[2][2] = 1.0f - (fTxx + fTyy);
    }
    void FromAngleAxis(const Radian& rfAngle, const Vector3& rkAxis) {
        const Radian fHalfAngle(0.5f * rfAngle.valueRadians());
        const Real fSin = Math::Sin(fHalfAngle);
        w = Math::Cos(fHalfAngle);
        x = fSin * rkAxis.x;
        y = fSin * rkAxis.y;
        z = fSin * rkAxis.z;
    }
    void ToAngleAxis(Radian& rfAngle, Vector3& rkAxis) const {
        const Real fSqrLength = x * x + y * y + z * z;
        if (fSqrLength > 0.0) {
            rfAngle = 2.0f * Math::ACos(w).valueRadians();
            const Real fInvLength = Math::InvSqrt(fSqrLength);
            rkAxis.x = x * fInvLength;
            rkAxis.y = y * fInvLength;
            rkAxis.z = z * fInvLength;
        } else {
            rfAngle = Radian(0.0);
            rkAxis.x = 1.0;
            rkAxis.y = 0.0;
            rkAxis.z = 0.0;
        }
    }
    void FromAxes(const Vector3& xaxis, const Vector3& yaxis, const Vector3& zaxis) {
        Matrix3 kRot;
        kRot.FromAxes(xaxis, yaxis, zaxis);
        FromRotationMatrix(kRot);
    }
    void ToAxes(Vector3& xaxis, Vector3& yaxis, Vector3& zaxis) const {
        Matrix3 kRot;
        ToRotationMatrix(kRot);
        xaxis = Vector3(kRot[0][0], kRot[1][0], kRot[2][0]);
        yaxis = Vector3(kRot[0][1], kRot[1][1], kRot[2][1]);
        zaxis = Vector3(kRot[0][2], kRot[1][2], kRot[2][2]);
    }
    Vector3 xAxis() const {
        const Real fTy = 2.0f * y;
        const Real fTz = 2.0f * z;
        const Real fTwy = fTy * w;
        const Real fTwz = fTz * w;
        const Real fTxy = fTy * x;
        const Real fTxz = fTz * x;
        const Real fTyy = fTy * y;
        const Real fTzz = fTz * z;
        return Vector3(1.0f - (fTyy + fTzz), fTxy + fTwz, fTxz - fTwy);
    }
    Vector3 yAxis() const {
        const Real fTx = 2.0f * x;
        const Real fTy = 2.0f * y;
        const Real fTz = 2.0f * z;
        const Real fTwx = fTx * w;
        const Real fTwz = fTz * w;
        const Real fTxx = fTx * x;
        const Real fTxy = fTy * x;
        const Real fTyz = fTz * y;
        const Real fTzz = fTz * z;
        return Vector3(fTxy - fTwz, 1.0f - (fTxx + fTzz), fTyz + fTwx);
    }
    Vector3 zAxis() const {
        const Real fTx = 2.0f * x;
        const Real fTy = 2.0f * y;
        const Real fTz = 2.0f * z;
        const Real fTwx = fTx * w;
        const Real fTwy = fTy * w;
        const Real fTxx = fTx * x;
        const Real fTxz = fTz * x;
        const Real fTyy = fTy * y;
        const Real fTyz = fTz * y;
        return Vector3(fTxz + fTwy, fTyz - fTwx, 1.0f - (fTxx + fTyy));
    }
    Quaternion operator+(const Quaternion& rkQ) const {
        return Quaternion(w + rkQ.w, x + rkQ.x, y + rkQ.y, z + rkQ.z);
    }
    Quaternion operator-(const Quaternion& rkQ) const {
        return Quaternion(w - rkQ.w, x - rkQ.x, y - rkQ.y, z - rkQ.z);
    }
    Quaternion operator*(const Quaternion& rkQ) const {
        return Quaternion(w * rkQ.w - x * rkQ.x - y * rkQ.y - z * rkQ.z,
                          w * rkQ.x + x * rkQ.w + y * rkQ.z - z * rkQ.y,
                          w * rkQ.y + y * rkQ.w + z * rkQ.x - x * rkQ.z,
                          w * rkQ.z + z * rkQ.w + x * rkQ.y - y * rkQ.x);
    }
    Quaternion operator*(Real s) const { return Quaternion(s * w, s * x, s * y, s * z); }
    friend Quaternion operator*(Real s, const Quaternion& q) { return q * s; }
    Quaternion operator-() const { return Quaternion(-w, -x, -y, -z); }
    bool operator==(const Quaternion& rhs) const {
        return (rhs.x == x) && (rhs.y == y) && (rhs.z == z) && (rhs.w == w);
    }
    bool operator!=(const Quaternion& rhs) const { return !operator==(rhs); }
    Real Dot(const Quaternion& rkQ) const { return w * rkQ.w + x * rkQ.x + y * rkQ.y + z * rkQ.z; }
    Real Norm() const { return Math::Sqrt(w * w + x * x + y * y + z * z); }
    Real normalise() {
        const Real len = Norm();
        *this = 1.0f / len * *this;
        return len;
    }
    Quaternion Inverse() const {
        const Real fNorm = w * w + x * x + y * y + z * z;
        if (fNorm > 0.0) {
            const Real fInvNorm = 1.0f / fNorm;
            return Quaternion(w * fInvNorm, -x * fInvNorm, -y * fInvNorm, -z * fInvNorm);
        }
        return ZERO;
    }
    Quaternion UnitInverse() const { return Quaternion(w, -x, -y, -z); }
    Vector3 operator*(const Vector3& v) const {
        Vector3 uv, uuv;
        const Vector3 qvec(x, y, z);
        uv = qvec.crossProduct(v);
        uuv = qvec.crossProduct(uv);
        uv *= (2.0f * w);
        uuv *= 2.0f;
        return v + uv + uuv;
    }
    Radian getYaw(bool reprojectAxis = true) const {
        if (reprojectAxis) {
            const Real fTx = 2.0f * x;
            const Real fTy = 2.0f * y;
            const Real fTz = 2.0f * z;
            const Real fTwy = fTy * w;
            const Real fTxx = fTx * x;
            const Real fTxz = fTz * x;
            const Real fTyy = fTy * y;
            return Math::ATan2(fTxz + fTwy, 1.0f - (fTxx + fTyy));
        }
        return Math::ASin(-2 * (x * z - w * y));
    }
    static Quaternion Slerp(Real fT, const Quaternion& rkP, const Quaternion& rkQ,
                            bool shortestPath = false) {
        Real fCos = rkP.Dot(rkQ);
        Quaternion rkT;
        if (fCos < 0.0f && shortestPath) {
            fCos = -fCos;
            rkT = -rkQ;
        } else {
            rkT = rkQ;
        }
        if (Math::Abs(fCos) < 1 - msEpsilon) {
            const Real fSin = Math::Sqrt(1 - Math::Sqr(fCos));
            const Radian fAngle = Math::ATan2(fSin, fCos);
            const Real fInvSin = 1.0f / fSin;
            const Real fCoeff0 = Math::Sin((1.0f - fT) * fAngle) * fInvSin;
            const Real fCoeff1 = Math::Sin(fT * fAngle) * fInvSin;
            return fCoeff0 * rkP + fCoeff1 * rkT;
        }
        Quaternion t = (1.0f - fT) * rkP + fT * rkT;
        t.normalise();
        return t;
    }

    static constexpr Real msEpsilon = 1e-03f;
    static const Quaternion ZERO;
    static const Quaternion IDENTITY;
    Real w, x, y, z;
};

inline const Quaternion Quaternion::ZERO(0, 0, 0, 0);
inline const Quaternion Quaternion::IDENTITY(1, 0, 0, 0);

inline Quaternion Vector3::getRotationTo(const Vector3& dest, const Vector3& fallbackAxis) const {
    const Real a = Math::Sqrt(squaredLength() * dest.squaredLength());
    Real b = a + dest.dotProduct(*this);
    if (Math::RealEqual(b, 2 * a, std::numeric_limits<Real>::epsilon()) || a == 0) {
        return Quaternion::IDENTITY;
    }
    Vector3 axis;
    if (b < (Real)1e-06 * a) {
        b = (Real)0.0;
        axis = fallbackAxis != Vector3::ZERO ? fallbackAxis
               : Math::Abs(x) > Math::Abs(z) ? Vector3(-y, x, (Real)0.0)
                                             : Vector3((Real)0.0, -z, y);
    } else {
        axis = crossProduct(dest);
    }
    Quaternion q(b, axis.x, axis.y, axis.z);
    q.normalise();
    return q;
}

/// OGRE 14's `Matrix4`, the part a collision mesh's placement uses: `makeTransform` (scale, then the
/// turn, then the move) and the projective `Matrix4 * Vector3`.
class Matrix4 {
public:
    const Real* operator[](size_t row) const { return m[row]; }
    Real* operator[](size_t row) { return m[row]; }

    void makeTransform(const Vector3& position, const Vector3& scale,
                       const Quaternion& orientation) {
        Matrix3 rot3x3;
        orientation.ToRotationMatrix(rot3x3);
        m[0][0] = scale.x * rot3x3[0][0];
        m[0][1] = scale.y * rot3x3[0][1];
        m[0][2] = scale.z * rot3x3[0][2];
        m[0][3] = position.x;
        m[1][0] = scale.x * rot3x3[1][0];
        m[1][1] = scale.y * rot3x3[1][1];
        m[1][2] = scale.z * rot3x3[1][2];
        m[1][3] = position.y;
        m[2][0] = scale.x * rot3x3[2][0];
        m[2][1] = scale.y * rot3x3[2][1];
        m[2][2] = scale.z * rot3x3[2][2];
        m[2][3] = position.z;
        m[3][0] = 0;
        m[3][1] = 0;
        m[3][2] = 0;
        m[3][3] = 1;
    }

    Real m[4][4];
};

inline Vector3 operator*(const Matrix4& m, const Vector3& v) {
    Vector3 r;
    const Real fInvW = 1.0f / (m[3][0] * v.x + m[3][1] * v.y + m[3][2] * v.z + m[3][3]);
    r.x = (m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3]) * fInvW;
    r.y = (m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3]) * fInvW;
    r.z = (m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3]) * fInvW;
    return r;
}

}  // namespace tl::core::ogre
