#pragma once

#include <cstdint>

struct Color {
	float r;
	float g;
	float b;
	float a;

	static constexpr Color from_rgb(float r, float g, float b, float a = 255.f) {
		return {r / 255.f, g / 255.f, b / 255.f, a / 255.f};
	}

	static constexpr Color from_rgb_uint(uint32_t rgb) {
		return from_rgb(static_cast<float>(rgb >> 16), static_cast<float>((rgb >> 8) & 0xFFu),
				static_cast<float>(rgb & 0xFFu));
	}

	static constexpr Color blend(const Color& src, const Color& dst) {
		return src * src.a + dst * (1.f - src.a);
	}

	constexpr Color operator+(const Color& c) const {
		return {r + c.r, g + c.g, b + c.b, a + c.a};
	}

	constexpr Color operator*(const Color& c) const {
		return {r * c.r, g * c.g, b * c.b, a * c.a};
	}

	constexpr Color operator*(float s) const {
		return {r * s, g * s, b * s, a * s};
	}
};
