#include "mat4x4.h"

// ---- Mat × Mat (SSE column-broadcast trick) --------------------------------
Mat4x4 Mat4x4::operator*(const Mat4x4& o) const noexcept {
    Mat4x4 out{};
    for (int r = 0; r < 4; ++r) {
        __m128 row = LoadRow(r);
        __m128 r0  = _mm_mul_ps(_mm_shuffle_ps(row, row, 0x00), o.LoadRow(0));
        __m128 r1  = _mm_mul_ps(_mm_shuffle_ps(row, row, 0x55), o.LoadRow(1));
        __m128 r2  = _mm_mul_ps(_mm_shuffle_ps(row, row, 0xAA), o.LoadRow(2));
        __m128 r3  = _mm_mul_ps(_mm_shuffle_ps(row, row, 0xFF), o.LoadRow(3));
        out.StoreRow(r, _mm_add_ps(_mm_add_ps(r0, r1), _mm_add_ps(r2, r3)));
    }
    return out;
}

// ---- Transpose (SSE _MM_TRANSPOSE4_PS macro) --------------------------------
Mat4x4 Mat4x4::Transposed() const noexcept {
    __m128 row0 = LoadRow(0);
    __m128 row1 = LoadRow(1);
    __m128 row2 = LoadRow(2);
    __m128 row3 = LoadRow(3);
    _MM_TRANSPOSE4_PS(row0, row1, row2, row3);
    Mat4x4 out{};
    out.StoreRow(0, row0);
    out.StoreRow(1, row1);
    out.StoreRow(2, row2);
    out.StoreRow(3, row3);
    return out;
}

// ---- Inverse (cofactor / adjugate, stack-only) ------------------------------
Mat4x4 Mat4x4::Inverted() const noexcept {
    const float* a = &m[0][0];

    float s0 = a[0]*a[5]  - a[1]*a[4];
    float s1 = a[0]*a[6]  - a[2]*a[4];
    float s2 = a[0]*a[7]  - a[3]*a[4];
    float s3 = a[1]*a[6]  - a[2]*a[5];
    float s4 = a[1]*a[7]  - a[3]*a[5];
    float s5 = a[2]*a[7]  - a[3]*a[6];

    float c0 = a[8]*a[13] - a[9]*a[12];
    float c1 = a[8]*a[14] - a[10]*a[12];
    float c2 = a[8]*a[15] - a[11]*a[12];
    float c3 = a[9]*a[14] - a[10]*a[13];
    float c4 = a[9]*a[15] - a[11]*a[13];
    float c5 = a[10]*a[15]- a[11]*a[14];

    float det = s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0;
    if (__builtin_expect(det == 0.f, 0)) return {};

    float inv_det = 1.f / det;

    Mat4x4 out{};
    float* b = &out.m[0][0];

    b[0]  = ( a[5]*c5 - a[6]*c4 + a[7]*c3) * inv_det;
    b[1]  = (-a[1]*c5 + a[2]*c4 - a[3]*c3) * inv_det;
    b[2]  = ( a[13]*s5- a[14]*s4+ a[15]*s3) * inv_det;
    b[3]  = (-a[9]*s5 + a[10]*s4- a[11]*s3) * inv_det;

    b[4]  = (-a[4]*c5 + a[6]*c2 - a[7]*c1) * inv_det;
    b[5]  = ( a[0]*c5 - a[2]*c2 + a[3]*c1) * inv_det;
    b[6]  = (-a[12]*s5+ a[14]*s2- a[15]*s1) * inv_det;
    b[7]  = ( a[8]*s5 - a[10]*s2+ a[11]*s1) * inv_det;

    b[8]  = ( a[4]*c4 - a[5]*c2 + a[7]*c0) * inv_det;
    b[9]  = (-a[0]*c4 + a[1]*c2 - a[3]*c0) * inv_det;
    b[10] = ( a[12]*s4- a[13]*s2+ a[15]*s0) * inv_det;
    b[11] = (-a[8]*s4 + a[9]*s2 - a[11]*s0) * inv_det;

    b[12] = (-a[4]*c3 + a[5]*c1 - a[6]*c0) * inv_det;
    b[13] = ( a[0]*c3 - a[1]*c1 + a[2]*c0) * inv_det;
    b[14] = (-a[12]*s3+ a[13]*s1- a[14]*s0) * inv_det;
    b[15] = ( a[8]*s3 - a[9]*s1 + a[10]*s0) * inv_det;

    return out;
}
