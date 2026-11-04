#pragma once

// Private bounded feasibility helper; separate binary64 operations, never FMA.
namespace airack::product_state {
struct Pair { double h = 0.0, l = 0.0; };
struct Coefficients { double b0, b1, b2, a1, a2; };

inline Pair twoSum(double a, double b) noexcept {
    const double s = a + b;
    const double v = s - a;
    const double t = s - v;
    const double u = a - t;
    const double w = b - v;
    const double e = u + w;
    return { s, e };
}
inline Pair split(double a) noexcept {
    const double t = 134217729.0 * a;
    const double u = t - a;
    const double h = t - u;
    const double l = a - h;
    return { h, l };
}
inline Pair twoProd(double a, double b) noexcept {
    const double p = a * b;
    const Pair aa = split(a);
    const Pair bb = split(b);
    const double t0 = aa.h * bb.h;
    const double t1 = t0 - p;
    const double t2 = aa.h * bb.l;
    const double t3 = t1 + t2;
    const double t4 = aa.l * bb.h;
    const double t5 = t3 + t4;
    const double t6 = aa.l * bb.l;
    const double e = t5 + t6;
    return { p, e };
}
inline Pair add(Pair a, Pair b) noexcept {
    Pair se = twoSum(a.h, b.h);
    const Pair tf = twoSum(a.l, b.l);
    se.l = se.l + tf.h;
    se = twoSum(se.h, se.l);
    se.l = se.l + tf.l;
    se = twoSum(se.h, se.l);
    return se;
}
inline Pair subtract(Pair a, Pair b) noexcept {
    return add(a, { -b.h, -b.l });
}
inline Pair multiply(double c, Pair a) noexcept {
    Pair pe = twoProd(c, a.h);
    const double t = c * a.l;
    pe.l = pe.l + t;
    return twoSum(pe.h, pe.l);
}
struct State {
    Pair z1, z2, y;
    double step(double x, const Coefficients& c) noexcept {
        const Pair input { x, 0.0 };
        y = add(multiply(c.b0, input), z1);
        const Pair n1 = add(subtract(multiply(c.b1, input), multiply(c.a1, y)), z2);
        const Pair n2 = subtract(multiply(c.b2, input), multiply(c.a2, y));
        z1 = n1;
        z2 = n2;
        return y.h;
    }
};
}
