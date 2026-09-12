#include "mat3x4.h"

Mat3x4 Mat3x4::Concat(const Mat3x4& o) const noexcept {
    Mat3x4 out;
    for (int r = 0; r < 3; ++r) {
        out.m[r][0] = m[r][0]*o.m[0][0] + m[r][1]*o.m[1][0] + m[r][2]*o.m[2][0];
        out.m[r][1] = m[r][0]*o.m[0][1] + m[r][1]*o.m[1][1] + m[r][2]*o.m[2][1];
        out.m[r][2] = m[r][0]*o.m[0][2] + m[r][1]*o.m[1][2] + m[r][2]*o.m[2][2];
        out.m[r][3] = m[r][0]*o.m[0][3] + m[r][1]*o.m[1][3] + m[r][2]*o.m[2][3] + m[r][3];
    }
    return out;
}
