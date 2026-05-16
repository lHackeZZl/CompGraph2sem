#pragma once
// MathHelper.h  — Small math utilities (Frank Luna style, self-contained)

#include <DirectXMath.h>
#include <cmath>
#include <algorithm>

using namespace DirectX;

class MathHelper
{
public:
    static XMFLOAT4X4 Identity4x4()
    {
        static XMFLOAT4X4 I(
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1);
        return I;
    }

    template<typename T>
    static T Clamp(const T& x, const T& low, const T& high)
    {
        return x < low ? low : (x > high ? high : x);
    }

    static const float Pi;
};

inline const float MathHelper::Pi = 3.1415926535f;
