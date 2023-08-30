#include "text_box.hpp"

#include "font.hpp"
#include "font_cache.hpp"
#include "rich_text.hpp"

#include <layout/ParagraphLayout.h>
#include <layout/RunArrays.h>

#include <unicode/utext.h>

static constexpr const UChar32 CH_LF = 0x000A;
static constexpr const UChar32 CH_CR = 0x000D;
static constexpr const UChar32 CH_LSEP = 0x2028;
static constexpr const UChar32 CH_PSEP = 0x2029;

void TextBox::render(Bitmap& target) {
	for (auto& rect : m_textRects) {
		target.blit_alpha(rect.texture, static_cast<int32_t>(m_position[0] + rect.x),
				static_cast<int32_t>(m_position[1] + rect.y), rect.color);
	}
}

void TextBox::recalc_text() {
	m_textRects.clear();

	if (!m_font) {
		return;
	}

	auto runs = m_richText ? RichText::parse(m_text, m_contentText, *m_font, m_textColor)
			: RichText::make_default_runs(m_text, m_contentText, *m_font, m_textColor);

	if (m_contentText.empty()) {
		return;
	}

	create_text_rects(runs);
}

void TextBox::create_text_rects(RichText::Result& textInfo) {
	RichText::TextRuns<const Font*> subsetFontRuns(textInfo.fontRuns.get_value_count());

	// FIXME: maintain line counter

	auto* start = textInfo.str.getBuffer();
	auto* end = start + textInfo.str.length();
	UText iter UTEXT_INITIALIZER;
	UErrorCode err{};
	utext_openUnicodeString(&iter, &textInfo.str, &err);

	float lineY = m_font->get_baseline();

	int32_t startIndex = 0;
	int32_t charIndex = 0;
	int32_t codepointIndex = 0;

	for (;;) {
		auto c = UTEXT_NEXT32(&iter);

		if (c == U_SENTINEL || c == CH_LF || c == CH_CR || c == CH_LSEP || c == CH_PSEP) {
			if (startIndex != charIndex) {
				subsetFontRuns.clear();
				textInfo.fontRuns.get_runs_subset(startIndex, charIndex - startIndex, subsetFontRuns);
				create_text_rects_for_paragraph(textInfo, subsetFontRuns, lineY, codepointIndex, startIndex,
						charIndex - startIndex);
			}
			else {
				lineY += m_font->get_line_height();
			}

			if (c == U_SENTINEL) {
				break;
			}
			else if (c == CH_CR && UTEXT_CURRENT32(&iter) == CH_LF) {
				UTEXT_NEXT32(&iter);
				++charIndex;
			}

			codepointIndex = UTEXT_GETNATIVEINDEX(&iter);
			startIndex = charIndex + 1;
		}

		++charIndex;
	}
}

void TextBox::create_text_rects_for_paragraph(RichText::Result& textInfo,
		const RichText::TextRuns<const Font*>& subsetFontRuns, float& lineY, int32_t codepointOffset,
		int32_t charOffset, int32_t length) {
	auto** ppFonts = const_cast<const Font**>(subsetFontRuns.get_values());
	icu::FontRuns fontRuns(reinterpret_cast<const icu::LEFontInstance**>(ppFonts), subsetFontRuns.get_limits(),
			subsetFontRuns.get_value_count());

	LEErrorCode err{};
	icu::ParagraphLayout pl(textInfo.str.getBuffer() + codepointOffset, length, &fontRuns, nullptr, nullptr,
			nullptr, UBIDI_DEFAULT_LTR, false, err);
	auto paragraphLevel = pl.getParagraphLevel();

	float lineX = 0.f;

	float lineWidth = m_size[0];
	float lineHeight = m_font->get_line_height();
	
	while (auto* line = pl.nextLine(lineWidth)) {
		if (paragraphLevel == UBIDI_RTL) {
			auto lastX = line->getWidth();
			lineX = lineWidth - lastX;
		}

		for (le_int32 runID = 0; runID < line->countRuns(); ++runID) {
			auto* run = line->getVisualRun(runID);
			auto* posData = run->getPositions();
			auto* pFont = static_cast<const Font*>(run->getFont());
			auto* pGlyphs = run->getGlyphs();
			auto* pGlyphChars = run->getGlyphToCharMap();

			for (le_int32 i = 0; i < run->getGlyphCount(); ++i) {
				auto pX = posData[2 * i];
				auto pY = posData[2 * i + 1];
				auto globalCharIndex = pGlyphChars[i] + charOffset;
				float glyphOffset[2]{};
				auto [glyphBitmap, hasColor] = pFont->get_glyph(pGlyphs[i], glyphOffset);
				auto textColor = hasColor ? Color{1.f, 1.f, 1.f, 1.f}
						: textInfo.colorRuns.get_value(globalCharIndex);

				if (textInfo.strikethroughRuns.get_value(globalCharIndex)) {
					auto height = static_cast<uint32_t>(pFont->get_strikethrough_thickness() + 0.5f);
					m_textRects.push_back({
						.x = lineX + pX + glyphOffset[0],
						.y = lineY + pY + pFont->get_strikethrough_position(),
						.texture = Bitmap(glyphBitmap.get_width(), height, {1.f, 1.f, 1.f, 1.f}),
						.color = textColor,
					});
				}

				if (textInfo.underlineRuns.get_value(globalCharIndex)) {
					auto height = static_cast<uint32_t>(pFont->get_underline_thickness() + 0.5f);
					m_textRects.push_back({
						.x = lineX + pX + glyphOffset[0],
						.y = lineY + pY + pFont->get_underline_position(),
						.texture = Bitmap(glyphBitmap.get_width(), height, {1.f, 1.f, 1.f, 1.f}),
						.color = textColor,
					});
				}

				m_textRects.push_back({
					.x = lineX + pX + glyphOffset[0],
					.y = lineY + pY + glyphOffset[1],
					.texture = std::move(glyphBitmap),
					.color = textColor,
				});
			}
		}

		delete line;
		lineY += lineHeight;
	}
}

// Setters

void TextBox::set_font(Font* font) {
	m_font = font;
	recalc_text();
}

void TextBox::set_text(std::string text) {
	m_text = std::move(text);
	recalc_text();
}

void TextBox::set_position(float x, float y) {
	m_position[0] = x;
	m_position[1] = y;
	recalc_text();
}

void TextBox::set_size(float width, float height) {
	m_size[0] = width;
	m_size[1] = height;
	recalc_text();
}

void TextBox::set_text_wrapped(bool wrapped) {
	m_textWrapped = wrapped;
	recalc_text();
}

void TextBox::set_rich_text(bool richText) {
	m_richText = richText;
	recalc_text();
}
